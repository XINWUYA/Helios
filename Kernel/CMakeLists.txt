cmake_minimum_required(VERSION 3.20)

project(Kernel LANGUAGES CXX)

# Static library
add_library(Kernel STATIC)

# Source files
# CONFIGURE_DEPENDS 让新增/删除源文件后无需手动重新配置即可被自动收录
file(GLOB_RECURSE KERNEL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

# Precompiled header header path
set(PCH_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/Pch.h")

# Copy TracyClient.cpp before build and insert Pch.h
set(TRACY_CLIENT_SRC "${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/tracy/public/TracyClient.cpp")
set(TRACY_CLIENT_DST "${CMAKE_CURRENT_SOURCE_DIR}/src/Helios/Tracy/TracyClient.cpp")

add_custom_command(
    OUTPUT ${TRACY_CLIENT_DST}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TRACY_CLIENT_SRC} ${TRACY_CLIENT_DST}
    COMMENT "Copying TracyClient.cpp"
)

# Add sources
target_sources(Kernel PRIVATE
    ${KERNEL_SOURCES}
    ${TRACY_CLIENT_DST}
)

# Organize source files in Visual Studio by directory structure
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${KERNEL_SOURCES} ${TRACY_CLIENT_DST})


# Precompiled header (CMake 3.20+ handles /Yc automatically)
if(MSVC)
    target_precompile_headers(Kernel PRIVATE
        <Pch.h>
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    target_precompile_headers(Kernel PRIVATE
        <Pch.h>
    )
endif()

# Include directories
target_include_directories(Kernel
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}  # For HeliosConfig.h
    PRIVATE
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/spdlog/include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/debugbreak
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glfw/include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glad/include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/imgui
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glm
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/stb
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/entt/single_include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/tinyxml2
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/xxHash
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/tinyobjloader
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/magic_enum/include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/assimp/include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/tracy/public
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/spirv-cross
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glslang
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glslang/SPIRV
)

if(APPLE)
    target_include_directories(Kernel PRIVATE
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/metal-cpp  # Metal C++ wrapper
    )
endif()

if(VULKAN_SDK)
    target_include_directories(Kernel PRIVATE ${VULKAN_SDK}/Include)
endif()

# Compile definitions
target_compile_definitions(Kernel
    PRIVATE
        GLFW_INCLUDE_NONE
        # TRACY_ENABLE
)

# Link libraries
target_link_libraries(Kernel
    PUBLIC
        GLFW
        Glad
        ImGui
        tinyxml2
        tinyobjloader
        assimp
        xxHash
        spirv-cross
        glslang
        SPIRV
        glslang-default-resource-limits
)

# Platform-specific linking
if(WIN32)
    target_link_libraries(Kernel PRIVATE opengl32)
    # System libraries required on Windows:
    #   - ws2_32 : Winsock2, used by Tracy (closesocket / WSAStartup / ...)
    #   - dbghelp: Tracy callstack capture (SymFromAddr / StackWalk64 / ...)
    #   - imm32  : Win32 IME API, used by ImGui GLFW/Win32 backends
    #              (ImmGetContext / ImmSetCompositionWindow / ...)
    # MSVC auto-links these via `#pragma comment(lib, ...)`, but MinGW/g++
    # doesn't recognize those pragmas, leading to errors like
    # "undefined reference to __imp_closesocket" or "ImmSetCompositionWindow".
    # Link them explicitly to support both MSVC and MinGW.
    target_link_libraries(Kernel PRIVATE ws2_32 dbghelp imm32)
    if(VULKAN_SDK)
        target_link_libraries(Kernel PRIVATE ${VULKAN_SDK}/Lib/shaderc_shared.lib)
    endif()
elseif(APPLE)
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    target_link_libraries(Kernel PRIVATE ${OPENGL_FRAMEWORK} ${COCOA_FRAMEWORK} ${METAL_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
elseif(UNIX)
    target_link_libraries(Kernel PRIVATE OpenGL::GL)
endif()

# Build configuration specific settings
target_compile_definitions(Kernel
    PRIVATE
        $<$<CONFIG:Debug>:HELIOS_DEBUG>
        $<$<CONFIG:Release>:HELIOS_RELEASE>
        $<$<CONFIG:Shipping>:HELIOS_SHIPPING>
)

# Output directory
set_target_properties(Kernel PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/${OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/${OUTPUT_DIR}"
    FOLDER "Engine"
)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(Kernel PRIVATE PLATFORM_WINDOWS)
elseif(APPLE)
    target_compile_definitions(Kernel PRIVATE PLATFORM_MACOS)
    # Files that need to be compiled as Objective-C++ on macOS
    # (FileDialog uses Cocoa NSOpenPanel; ImGuiLayer/MetalQuery use Metal APIs)
    set(KERNEL_OBJCXX_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Helios/Application/FileDialog.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Helios/ImGui/ImGuiLayer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/GraphicsAPI/Metal/MetalQuery.cpp"
    )
    set_source_files_properties(${KERNEL_OBJCXX_SOURCES} PROPERTIES
        COMPILE_FLAGS "-x objective-c++"
        SKIP_PRECOMPILE_HEADERS ON
    )
elseif(UNIX)
    target_compile_definitions(Kernel PRIVATE PLATFORM_LINUX)
endif()
