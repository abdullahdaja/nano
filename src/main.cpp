#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <cstring>
#include <atomic>
#include <random>
#include <chrono>
#include <netdb.h>
#include <sstream>

// 1. Single-header Audio Engine
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

// 2. Opus Codec
#include <opus/opus.h>

// 3. ImGui & Windowing
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

// 4. Platform Networking (UDP)
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    typedef int SOCKET;
    #define INVALID_SOCKET -1
    #define closesocket close
#endif

// Audio & Codec Parameters
constexpr uint32_t SAMPLE_RATE = 48000;
constexpr uint32_t CHANNELS = 1;               // Mono for voice optimization
constexpr uint32_t FRAME_SIZE = 960;            // 20ms at 48kHz
constexpr uint32_t MAX_PACKET_SIZE = 4000;      // Max size for Opus packet
static constexpr size_t PLAYBACK_RING_BUFFER_FRAMES = FRAME_SIZE * 16; // ~320 ms total
static constexpr size_t PLAYBACK_RING_BUFFER_SAMPLES = PLAYBACK_RING_BUFFER_FRAMES * CHANNELS;
static constexpr size_t PLAYBACK_WARMUP_FRAMES = FRAME_SIZE * 3; // buffer before playback starts
static constexpr size_t PLAYBACK_WARMUP_SAMPLES = PLAYBACK_WARMUP_FRAMES * CHANNELS;

struct VoiceEngine {
    ma_device captureDevice;
    ma_device playbackDevice;
    OpusEncoder* encoder = nullptr;
    OpusDecoder* decoder = nullptr;
    
    SOCKET udpSocket = INVALID_SOCKET;
    sockaddr_in targetAddr{};
    
    std::mutex audioBufferMutex;
    std::vector<int16_t> playbackBuffer{PLAYBACK_RING_BUFFER_SAMPLES};
    size_t playbackReadIndex = 0;
    size_t playbackWriteIndex = 0;
    size_t playbackSamplesAvailable = 0;
    bool playbackWarmupCompleted = false;
    
    std::atomic<bool> isMuted{false};
    std::atomic<bool> isConnected{false};
    // Signaling / room state
    std::string roomId;
    std::string peerName;
    bool useStun = true;
};

VoiceEngine g_Engine;

// Audio Capture Callback: Invoked when soundcard hardware has new mic PCM data
void audioCaptureCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    if (g_Engine.isMuted || !g_Engine.isConnected) return;

    const int16_t* pcmInput = static_cast<const int16_t*>(pInput);
    unsigned char opusPacket[MAX_PACKET_SIZE];

    // Encode raw PCM to compressed Opus frame
    opus_int32 encodedBytes = opus_encode(
        g_Engine.encoder, 
        pcmInput, 
        FRAME_SIZE, 
        opusPacket, 
        sizeof(opusPacket)
    );

    if (encodedBytes > 0 && g_Engine.udpSocket != INVALID_SOCKET) {
        // Send directly over UDP to target IP/Port
        sendto(g_Engine.udpSocket, reinterpret_cast<const char*>(opusPacket), encodedBytes, 0,
               reinterpret_cast<struct sockaddr*>(&g_Engine.targetAddr), sizeof(g_Engine.targetAddr));
    }
}

// Audio Playback Callback: Invoked when soundcard asks for PCM data to output
void audioPlaybackCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    int16_t* pcmOutput = static_cast<int16_t*>(pOutput);
    std::lock_guard<std::mutex> lock(g_Engine.audioBufferMutex);

    size_t samplesRequested = frameCount * CHANNELS;
    size_t samplesAvailable = g_Engine.playbackSamplesAvailable;

    if (!g_Engine.playbackWarmupCompleted) {
        if (samplesAvailable >= PLAYBACK_WARMUP_SAMPLES) {
            g_Engine.playbackWarmupCompleted = true;
        } else {
            std::memset(pcmOutput, 0, samplesRequested * sizeof(int16_t));
            return;
        }
    }

    size_t samplesToCopy = std::min(samplesRequested, samplesAvailable);
    size_t firstChunk = std::min(samplesToCopy, PLAYBACK_RING_BUFFER_SAMPLES - g_Engine.playbackReadIndex);
    std::memcpy(pcmOutput, g_Engine.playbackBuffer.data() + g_Engine.playbackReadIndex, firstChunk * sizeof(int16_t));
    if (firstChunk < samplesToCopy) {
        std::memcpy(pcmOutput + firstChunk, g_Engine.playbackBuffer.data(), (samplesToCopy - firstChunk) * sizeof(int16_t));
    }

    g_Engine.playbackReadIndex = (g_Engine.playbackReadIndex + samplesToCopy) % PLAYBACK_RING_BUFFER_SAMPLES;
    g_Engine.playbackSamplesAvailable -= samplesToCopy;

    if (samplesToCopy < samplesRequested) {
        std::memset(pcmOutput + samplesToCopy, 0, (samplesRequested - samplesToCopy) * sizeof(int16_t));
        g_Engine.playbackWarmupCompleted = false;
    }
}

// Background thread that continuously reads UDP packets from connected peer
void networkReceiveLoop() {
    unsigned char buffer[MAX_PACKET_SIZE];
    int16_t pcmOut[FRAME_SIZE];

    while (g_Engine.isConnected) {
        int bytesReceived = recv(g_Engine.udpSocket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (bytesReceived > 0) {
            // Decode incoming Opus packet back into PCM
            int decodedSamples = opus_decode(
                g_Engine.decoder, 
                buffer, 
                bytesReceived, 
                pcmOut, 
                FRAME_SIZE, 0
            );

            if (decodedSamples > 0) {
                std::lock_guard<std::mutex> lock(g_Engine.audioBufferMutex);
                size_t samplesDecoded = decodedSamples * CHANNELS;
                size_t freeSpace = PLAYBACK_RING_BUFFER_SAMPLES - g_Engine.playbackSamplesAvailable;
                if (samplesDecoded > freeSpace) {
                    size_t samplesToDrop = samplesDecoded - freeSpace;
                    g_Engine.playbackReadIndex = (g_Engine.playbackReadIndex + samplesToDrop) % PLAYBACK_RING_BUFFER_SAMPLES;
                    g_Engine.playbackSamplesAvailable -= samplesToDrop;
                }

                size_t firstChunk = std::min(samplesDecoded, PLAYBACK_RING_BUFFER_SAMPLES - g_Engine.playbackWriteIndex);
                std::memcpy(g_Engine.playbackBuffer.data() + g_Engine.playbackWriteIndex, pcmOut, firstChunk * sizeof(int16_t));
                if (firstChunk < samplesDecoded) {
                    std::memcpy(g_Engine.playbackBuffer.data(), pcmOut + firstChunk, (samplesDecoded - firstChunk) * sizeof(int16_t));
                }

                g_Engine.playbackWriteIndex = (g_Engine.playbackWriteIndex + samplesDecoded) % PLAYBACK_RING_BUFFER_SAMPLES;
                g_Engine.playbackSamplesAvailable += samplesDecoded;
            }
        }
    }
}

// Minimal STUN Binding Request to discover public IP/port
// Returns true on success and fills out publicIp/publicPort
bool stun_get_public_endpoint(const char* stunHost, const char* stunPort, std::string &publicIp, uint16_t &publicPort) {
    const uint32_t MAGIC_COOKIE = 0x2112A442;
    unsigned char req[20];
    memset(req, 0, sizeof(req));
    // Message Type: Binding Request (0x0001)
    req[0] = 0x00; req[1] = 0x01;
    // Message Length = 0
    // Magic cookie
    req[4] = (MAGIC_COOKIE >> 24) & 0xFF;
    req[5] = (MAGIC_COOKIE >> 16) & 0xFF;
    req[6] = (MAGIC_COOKIE >> 8) & 0xFF;
    req[7] = MAGIC_COOKIE & 0xFF;
    // Transaction ID: 12 random bytes
    std::random_device rd;
    for (int i = 8; i < 20; ++i) req[i] = static_cast<unsigned char>(rd() & 0xFF);

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(stunHost, stunPort, &hints, &res) != 0 || !res) return false;

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return false; }

    // set recv timeout
    timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    ssize_t sent = sendto(s, reinterpret_cast<const char*>(req), sizeof(req), 0, res->ai_addr, res->ai_addrlen);
    if (sent != (ssize_t)sizeof(req)) { closesocket(s); freeaddrinfo(res); return false; }

    unsigned char resp[512];
    ssize_t recvd = recv(s, reinterpret_cast<char*>(resp), sizeof(resp), 0);
    if (recvd <= 0) { closesocket(s); freeaddrinfo(res); return false; }

    // Parse response: iterate attributes to find XOR-MAPPED-ADDRESS (0x0020)
    if (recvd < 20) { closesocket(s); freeaddrinfo(res); return false; }
    // verify transaction id
    bool match = true;
    for (int i = 8; i < 20; ++i) if (resp[i] != req[i]) { match = false; break; }
    if (!match) { closesocket(s); freeaddrinfo(res); return false; }

    size_t offset = 20;
    while (offset + 4 <= (size_t)recvd) {
        uint16_t attrType = (resp[offset] << 8) | resp[offset+1];
        uint16_t attrLen = (resp[offset+2] << 8) | resp[offset+3];
        offset += 4;
        if (offset + attrLen > (size_t)recvd) break;

        if (attrType == 0x0020 && attrLen >= 8) {
            // first byte 0, family, port (2), addr (4)
            unsigned char family = resp[offset+1];
            if (family == 0x01) { // IPv4
                uint16_t xport = (resp[offset+2] << 8) | resp[offset+3];
                uint32_t xaddr = (resp[offset+4] << 24) | (resp[offset+5] << 16) | (resp[offset+6] << 8) | resp[offset+7];
                uint16_t port = xport ^ (MAGIC_COOKIE >> 16);
                uint32_t addr = xaddr ^ MAGIC_COOKIE;
                char ipbuf[INET_ADDRSTRLEN];
                in_addr a; a.s_addr = htonl(addr);
                inet_ntop(AF_INET, &a, ipbuf, sizeof(ipbuf));
                publicIp = ipbuf;
                publicPort = port;
                closesocket(s); freeaddrinfo(res);
                return true;
            }
        }

        offset += ((attrLen + 3) / 4) * 4; // padded
    }

    closesocket(s); freeaddrinfo(res);
    return false;
}

// Very small signaling client: TCP to signalingHost:port using a tiny line protocol
// JOIN <room> <name> <public_ip> <public_port>\n
// Server responds with one or more lines: PEER <ip> <port>\n and then END\n
bool signaling_join(const char* serverHost, int serverPort, const std::string &room, const std::string &name, const std::string &publicIp, uint16_t publicPort, std::string &outPeerIp, uint16_t &outPeerPort) {
    // resolve
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    char portbuf[8]; snprintf(portbuf, sizeof(portbuf), "%d", serverPort);
    if (getaddrinfo(serverHost, portbuf, &hints, &res) != 0 || !res) return false;
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return false; }
    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) { closesocket(s); freeaddrinfo(res); return false; }

    std::string joinLine = "JOIN "+room+" "+name+" "+publicIp+" "+std::to_string(publicPort)+"\n";
    send(s, joinLine.c_str(), (int)joinLine.size(), 0);

    // read lines until END or first PEER
    char buf[512];
    int n = recv(s, buf, sizeof(buf)-1, 0);
    if (n <= 0) { closesocket(s); freeaddrinfo(res); return false; }
    buf[n] = '\0';
    std::string resp(buf);
    // look for PEER line
    size_t pos = resp.find("PEER ");
    if (pos != std::string::npos) {
        size_t eol = resp.find('\n', pos);
        std::string line = resp.substr(pos, eol==std::string::npos? std::string::npos : eol-pos);
        // parse
        std::istringstream iss(line);
        std::string tag; iss >> tag; iss >> outPeerIp; iss >> outPeerPort;
        closesocket(s); freeaddrinfo(res);
        return true;
    }

    closesocket(s); freeaddrinfo(res);
    return false;
}

// Send several small UDP packets to peer to punch holes
void punch_hole_to_peer(const std::string &peerIp, uint16_t peerPort, int count = 10) {
    sockaddr_in peerAddr{};
    peerAddr.sin_family = AF_INET;
    peerAddr.sin_port = htons(peerPort);
    inet_pton(AF_INET, peerIp.c_str(), &peerAddr.sin_addr);

    const char payload[] = "PING";
    for (int i = 0; i < count; ++i) {
        sendto(g_Engine.udpSocket, payload, sizeof(payload), 0, reinterpret_cast<struct sockaddr*>(&peerAddr), sizeof(peerAddr));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}



int main() {
    // 1. Initialize Sockets
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    g_Engine.udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    // 2. Initialize Opus Encoder and Decoder
    int error;
    g_Engine.encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP, &error);
    g_Engine.decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);
    opus_encoder_ctl(g_Engine.encoder, OPUS_SET_BITRATE(32000)); // 32 kbps voice target

    // 3. Initialize Miniaudio
    ma_device_config captureConfig = ma_device_config_init(ma_device_type_capture);
    captureConfig.capture.format   = ma_format_s16;
    captureConfig.capture.channels = CHANNELS;
    captureConfig.sampleRate       = SAMPLE_RATE;
    captureConfig.performanceProfile = ma_performance_profile_low_latency;
    captureConfig.periodSizeInFrames = FRAME_SIZE;
    captureConfig.periods = 2;
    captureConfig.dataCallback     = audioCaptureCallback;
    ma_device_init(NULL, &captureConfig, &g_Engine.captureDevice);

    ma_device_config playbackConfig = ma_device_config_init(ma_device_type_playback);
    playbackConfig.playback.format   = ma_format_s16;
    playbackConfig.playback.channels = CHANNELS;
    playbackConfig.sampleRate       = SAMPLE_RATE;
    playbackConfig.performanceProfile = ma_performance_profile_low_latency;
    playbackConfig.periodSizeInFrames = FRAME_SIZE;
    playbackConfig.periods = 2;
    playbackConfig.dataCallback     = audioPlaybackCallback;
    ma_device_init(NULL, &playbackConfig, &g_Engine.playbackDevice);

    ma_device_start(&g_Engine.captureDevice);
    ma_device_start(&g_Engine.playbackDevice);

    // 4. Initialize GLFW & OpenGL Window
    if (!glfwInit()) return -1;

    GLFWwindow* window = glfwCreateWindow(380, 220, "NanoVoice C++ Client", NULL, NULL);
    if (!window) return -1;
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable VSync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    char targetIP[64] = "127.0.0.1";
    int targetPort = 9001;
    bool isMutedUI = false;

    // Room / signaling UI state
    char signalingHost[128] = "127.0.0.1";
    int signalingPort = 9000;
    char roomId[64] = "";
    char peerName[64] = "peer";
    bool useStunUI = true;

    // Main App Render Loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Build UI Interface
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("NanoVoice Controls", nullptr, 
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        ImGui::Text("NanoVoice C++ (RAM: ~8 MB)");
        ImGui::Separator();

        ImGui::InputText("Target IP", targetIP, sizeof(targetIP));
        ImGui::InputInt("Target Port", &targetPort);

        ImGui::Separator();
        ImGui::Text("Room / Signaling (optional)");
        ImGui::InputText("Signaling Host", signalingHost, sizeof(signalingHost));
        ImGui::InputInt("Signaling Port", &signalingPort);
        ImGui::InputText("Room ID", roomId, sizeof(roomId));
        ImGui::InputText("Your Name", peerName, sizeof(peerName));
        ImGui::Checkbox("Use STUN (for public IP)", &useStunUI);

        if (!g_Engine.isConnected) {
            if (ImGui::Button("Join Room / Connect", ImVec2(-1, 30))) {
                // Bind local UDP socket to chosen port
                sockaddr_in localAddr{};
                localAddr.sin_family = AF_INET;
                localAddr.sin_port = htons(targetPort);
                localAddr.sin_addr.s_addr = INADDR_ANY;
                bind(g_Engine.udpSocket, reinterpret_cast<struct sockaddr*>(&localAddr), sizeof(localAddr));

                // Discover public endpoint (optional STUN)
                std::string publicIp = "";
                uint16_t publicPort = (uint16_t)targetPort;
                if (useStunUI) {
                    bool ok = stun_get_public_endpoint("stun.l.google.com", "19302", publicIp, publicPort);
                    if (!ok) {
                        // fallback to local address
                        publicIp = "0.0.0.0";
                    }
                } else {
                    publicIp = "0.0.0.0";
                }

                // If a room id and signaling host are provided, use signaling
                std::string peerIp;
                uint16_t peerPort = 0;
                if (strlen(roomId) > 0) {
                    bool sj = signaling_join(signalingHost, signalingPort, roomId, peerName, publicIp, publicPort, peerIp, peerPort);
                    if (sj) {
                        // punch holes to peer
                        std::thread(punch_hole_to_peer, peerIp, peerPort, 10).detach();
                        // set target
                        g_Engine.targetAddr.sin_family = AF_INET;
                        g_Engine.targetAddr.sin_port = htons(peerPort);
                        inet_pton(AF_INET, peerIp.c_str(), &g_Engine.targetAddr.sin_addr);
                        g_Engine.isConnected = true;
                        std::thread(networkReceiveLoop).detach();
                    } else {
                        // signaling failed, leave socket bound but don't connect
                        std::cerr << "Signaling join failed" << std::endl;
                    }
                } else {
                    // manual connect (unchanged behavior)
                    g_Engine.targetAddr.sin_family = AF_INET;
                    g_Engine.targetAddr.sin_port = htons(targetPort);
                    inet_pton(AF_INET, targetIP, &g_Engine.targetAddr.sin_addr);
                    g_Engine.isConnected = true;
                    std::thread(networkReceiveLoop).detach();
                }
            }
        } else {
            if (ImGui::Button("Disconnect", ImVec2(-1, 30))) {
                g_Engine.isConnected = false;
            }
        }

        ImGui::Separator();
        if (ImGui::Checkbox("Mute Microphone", &isMutedUI)) {
            g_Engine.isMuted = isMutedUI;
        }

        ImGui::End();

        // Render Frame
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup Resources
    g_Engine.isConnected = false;
    ma_device_uninit(&g_Engine.captureDevice);
    ma_device_uninit(&g_Engine.playbackDevice);
    opus_encoder_destroy(g_Engine.encoder);
    opus_decoder_destroy(g_Engine.decoder);
    
    if (g_Engine.udpSocket != INVALID_SOCKET) {
        closesocket(g_Engine.udpSocket);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}