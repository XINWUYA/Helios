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
        ${HELIOS_ENGINE_ROOT}/Kernel/src
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/spdlog/include
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/debugbreak
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glfw/include
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glad/include
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/imgui
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glm
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/stb
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/entt/single_include
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/tinyxml2
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/magic_enum/include
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/assimp/include
)

# Link libraries
target_link_libraries(Editor
    PRIVATE
        Kernel
)

# 脚本模块在运行期调用引擎函数：Kernel 是动态库，符号由链接器解析，
# 不需要整包拉进可执行文件，也不需要额外导出开关。
if(APPLE)
    # 让可执行文件按 @rpath 找同一次构建出来的 libKernel.dylib
    set_target_properties(Editor PROPERTIES BUILD_RPATH "${CMAKE_BINARY_DIR}/lib/${OUTPUT_DIR}")
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
        # 编辑器重编脚本时用同一个配置（ScriptModule::Compile）
        "HELIOS_BUILD_CONFIG=\"$<CONFIG>\""
)

# Output directory
set_target_properties(Editor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${OUTPUT_DIR}"
    FOLDER "Applications"
)

helios_set_app_icon(Editor)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(Editor PRIVATE PLATFORM_WINDOWS)
    # 文件对话框：GetOpenFileName / ShellExecute 来自这两个系统库；
    # 选目录的 SHBrowseForFolder 要 CoInitializeEx，配对 ole32
    target_link_libraries(Editor PRIVATE comdlg32 shell32 ole32)
elseif(APPLE)
    set_target_properties(Editor PROPERTIES
        MACOSX_BUNDLE ON
    )

    # 文件对话框直接用 Cocoa 的 NSOpenPanel / NSSavePanel
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    target_link_libraries(Editor PRIVATE ${COCOA_FRAMEWORK})

    # 文件对话框走 Cocoa 的 NSOpenPanel，要按 Objective-C++ 编
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/Common/FileDialog.cpp"
        PROPERTIES
            COMPILE_FLAGS "-x objective-c++"
            SKIP_PRECOMPILE_HEADERS ON
    )

    helios_sign_macos_bundle(Editor)
endif()

# 输出目录自带一份引擎资源：脱离源码树运行时把它放到可执行文件旁边即可
if(WIN32)
    add_custom_command(TARGET Editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${ENGINE_RES_PATH}"
        "$<TARGET_FILE_DIR:Editor>/EngineRes"
        COMMENT "Copying engine resources to output directory"
    )
endif()
