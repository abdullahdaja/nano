cmake_minimum_required(VERSION 3.16)
project(NanoVoice LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find System Dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED opus)

find_package(glfw3 CONFIG QUIET)
if(glfw3_FOUND)
    set(GLFW_LINK_TARGET glfw)
elseif(NOT glfw3_FOUND)
    pkg_check_modules(GLFW3 QUIET glfw3)
    if(GLFW3_FOUND)
        set(GLFW_INCLUDE_DIRS ${GLFW3_INCLUDE_DIRS})
        set(GLFW_LINK_TARGET ${GLFW3_LIBRARIES})
    endif()
endif()

if(NOT glfw3_FOUND AND NOT GLFW3_FOUND)
    message(FATAL_ERROR "glfw3 not found. Install the GLFW development package (for Ubuntu/Debian: libglfw3-dev) or set CMAKE_PREFIX_PATH/glfw3_DIR to the GLFW install directory.")
endif()

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# Include Directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends
    ${OPUS_INCLUDE_DIRS}
    ${GLFW_INCLUDE_DIRS}
)

# ImGui Source Files
set(IMGUI_SOURCES
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp
)

# Executable Setup
add_executable(NanoVoice 
    src/main.cpp 
    ${IMGUI_SOURCES}
)

# Platform-specific socket libraries
if(WIN32)
    target_link_libraries(NanoVoice PRIVATE ws2_32)
endif()

target_link_libraries(NanoVoice PRIVATE 
    ${OPUS_LIBRARIES} 
    ${GLFW_LINK_TARGET} 
    OpenGL::GL 
    Threads::Threads
)