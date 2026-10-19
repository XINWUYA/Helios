cmake_minimum_required(VERSION 3.20)

project(Editor LANGUAGES CXX)

# Executable
add_executable(Editor)

# Source files
# CONFIGURE_DEPENDS：新增/删除源文件时自动重新 glob，避免"新文件没进构建"导致链接报未定义符号
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

target_sources(Editor PRIVATE ${EDITOR_SOURCES})

# Organize source files in Visual Studio by directory structure
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${EDITOR_SOURCES})


# Precompiled header (CMake 3.20+ handles /Yc automatically)
if(MSVC)
    target_precompile_headers(Editor PRIVATE
        <Pch.h>
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    target_precompile_headers(Editor PRIVATE
        <Pch.h>
    )
endif()

# Include directories
target_include_directories(Editor
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/Kernel/src
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/spdlog/include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/debugbreak
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glfw/include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glad/include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/imgui
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glm
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/stb
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/entt/single_include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/tinyxml2
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/ImGuizmo
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/magic_enum/include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/assimp/include
)

# Link libraries
target_link_libraries(Editor
    PRIVATE
        Kernel
        ImGuizmo
)

# 脚本模块在运行期调用引擎函数，静态库默认只把「链接期被引用」的 .o 拉进可执行文件，
# 脚本用到但编辑器自己没用过的符号会缺失（dlopen 报 symbol not found in flat namespace）。
# 这里强制整包链进来；打包形态把脚本改成静态编译，不依赖这条。
if(APPLE)
    target_link_options(Editor PRIVATE "LINKER:-force_load,$<TARGET_LINKER_FILE:Kernel>")
elseif(WIN32)
    target_link_options(Editor PRIVATE "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:Kernel>")
endif()

# Build configuration specific settings
target_compile_definitions(Editor
    PRIVATE
        $<$<CONFIG:Debug>:HELIOS_DEBUG>
        $<$<CONFIG:Release>:HELIOS_RELEASE>
        $<$<CONFIG:Shipping>:HELIOS_SHIPPING>
        # 编辑器默认开启性能插桩（HELIOS_PROFILE）；写追踪文本是运行期全局开关 ——
        # 性能面板 CPU Timings 的 "Trace Text"（不用重编）；Sample 等其它目标不定义
        HELIOS_PROFILE=1
)

# Output directory
set_target_properties(Editor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${OUTPUT_DIR}"
    FOLDER "Applications"
    # 脚本模块 dlopen 时要解析引擎符号。macOS 上可执行文件默认导出全局符号，
    # 但开了 dead_strip 就未必 —— 显式导出，别让 Release 构建在加载脚本时挂掉。
    ENABLE_EXPORTS ON
)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(Editor PRIVATE PLATFORM_WINDOWS)
elseif(APPLE)
    set_target_properties(Editor PROPERTIES
        MACOSX_BUNDLE ON
    )

    helios_sign_macos_bundle(Editor)
endif()

# Copy assets to output directory (optional)
if(WIN32)
    add_custom_command(TARGET Editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:Editor>/Assets"
        COMMENT "Copying Assets to output directory"
    )
endif()
