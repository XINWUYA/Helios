cmake_minimum_required(VERSION 3.20)

project(Kernel LANGUAGES CXX)

# 脚本模块在运行期要调用引擎里任意函数，所以 Kernel 一律编成动态库：
#   脚本 DLL / bundle 链接它，符号由链接器解析，三个平台走同一套机制。
# Windows 只导出标了 HELIOS_API 的符号（见 Common/Export.h），漏标就是链接错误。
add_library(Kernel SHARED)

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

# 着色器是引擎和各 App 共用的资产，挂这里方便在 IDE 里直接查看和编辑：
# 只进导航不参与编译，运行期仍按 ASSETS_PATH 读源文件，改完重跑即生效。
file(GLOB_RECURSE HELIOS_SHADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Assets/Shaders/*.glsl"
)
set_source_files_properties(${HELIOS_SHADER_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(Kernel PRIVATE ${HELIOS_SHADER_SOURCES})
source_group(TREE "${CMAKE_SOURCE_DIR}/Assets/Shaders" PREFIX "Shaders" FILES ${HELIOS_SHADER_SOURCES})

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
        ${HELIOS_ENGINE_INCLUDE_DIRS}
)

if(VULKAN_SDK)
    target_include_directories(Kernel PRIVATE ${VULKAN_SDK}/Include)
endif()

# Compile definitions
target_compile_definitions(Kernel
    PRIVATE
        GLFW_INCLUDE_NONE
        # ImGui 后端源码直接编进 Kernel（见 ImGuiContext.cpp），不跟着 IMGUI_API 走导入
        IMGUI_IMPL_API=
        # TRACY_ENABLE
)

# Link libraries
target_link_libraries(Kernel
    PUBLIC
        GLFW
        Glad
        ImGui
        tinyxml2
        assimp
        xxHash
        spirv-cross
        spdlog
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
# DLL / dylib 放到可执行文件旁边，Windows 与 macOS 都按 exe 附近找。
set_target_properties(Kernel PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/${OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/${OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${OUTPUT_DIR}"
    FOLDER "Engine"
)

if(NOT WIN32)
    # macOS / Linux 默认全导出，标不标 HELIOS_API 都能跑。打开这个选项可以模拟 Windows 的
    # 约束（没标的符号不给外部用），用来检查标注有没有漏 —— 漏了就是链接错误。
    # 只做链接检查别拿来跑：glm 这类类型的 RTTI 会被当成模块私有，跨模块 any_cast 必然失败。
    option(HELIOS_HIDDEN_VISIBILITY "Hide Kernel symbols not marked HELIOS_API" OFF)
    if(HELIOS_HIDDEN_VISIBILITY)
        set_target_properties(Kernel PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
        )
        # 模板显式实例化的符号继承模板定义处的可见性，Clang 又不认 template class 上的
        # attribute；这个 TU 里有 AssetStore 的显式实例化，单独放回默认可见性
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/src/Helios/Core/AssetManager.cpp"
            PROPERTIES COMPILE_OPTIONS "-fvisibility=default"
        )
    endif()
endif()

# Platform-specific settings
if(WIN32)
    target_compile_definitions(Kernel PRIVATE PLATFORM_WINDOWS)
    # 让 Export.h 里的 HELIOS_API 展开成 dllexport（链接方看到的是 dllimport）
    target_compile_definitions(Kernel PRIVATE HELIOS_KERNEL_BUILD)
    # stb_image 的实现编在 Kernel 里，它的默认 extern 在 DLL 上不导出符号，
    # Editor 侧要按 dllimport 声明才能链到 Kernel.dll 里那一份
    target_compile_definitions(Kernel
        PRIVATE "STBIDEF=__declspec(dllexport)"
        INTERFACE "STBIDEF=__declspec(dllimport)"
    )
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
