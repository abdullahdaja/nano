cmake_minimum_required(VERSION 3.16)
project(NanoVoice LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include Directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends
)

# Platform Dependencies
if(WIN32)
    # Windows: vcpkg provides native CMake targets
    find_package(glfw3 CONFIG REQUIRED)
    find_package(Opus CONFIG REQUIRED)
    find_package(OpenGL REQUIRED)
    find_package(Threads REQUIRED)
else()
    # Linux/Unix: Use system pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPUS REQUIRED opus)

    find_package(glfw3 CONFIG QUIET)
    if(glfw3_FOUND)
        set(GLFW_LINK_TARGET glfw)
    else()
        pkg_check_modules(GLFW3 QUIET glfw3)
        if(GLFW3_FOUND)
            set(GLFW_INCLUDE_DIRS ${GLFW3_INCLUDE_DIRS})
            set(GLFW_LINK_TARGET ${GLFW3_LIBRARIES})
        else()
            message(FATAL_ERROR "glfw3 not found. Install libglfw3-dev.")
        endif()
    endif()

    find_package(OpenGL REQUIRED)
    find_package(Threads REQUIRED)

    include_directories(
        ${OPUS_INCLUDE_DIRS}
        ${GLFW_INCLUDE_DIRS}
    )
endif()

# ImGui Source Files
set(IMGUI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_opengl3.cpp
)

# Executable Setup
add_executable(NanoVoice 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp 
    ${IMGUI_SOURCES}
)

# Link Libraries
if(WIN32)
    target_link_libraries(NanoVoice PRIVATE 
        glfw 
        Opus::opus 
        OpenGL::GL 
        Threads::Threads 
        ws2_32
    )
else()
    target_link_libraries(NanoVoice PRIVATE 
        ${OPUS_LIBRARIES} 
        ${GLFW_LINK_TARGET} 
        OpenGL::GL 
        Threads::Threads
    )
endif()