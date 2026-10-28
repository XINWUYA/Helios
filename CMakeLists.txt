cmake_minimum_required(VERSION 3.20)

project(Helios VERSION 1.0.0 LANGUAGES C CXX)

# 引擎根目录：既支持引擎自己当顶层工程（现有用法），
# 也支持被项目工程 add_subdirectory 引入 —— 那种情况下 CMAKE_SOURCE_DIR 是项目根，
# 引擎内的路径必须相对自己算，不能跟着变。
if(NOT DEFINED HELIOS_ENGINE_ROOT)
    set(HELIOS_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# Enable folder organization in IDEs (Xcode, Visual Studio, etc.)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build configurations
set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Shipping" CACHE STRING "" FORCE)

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# Output directory format
set(OUTPUT_DIR "${CMAKE_BUILD_TYPE}-${CMAKE_SYSTEM_NAME}-x64")

# Output directories
# Windows 按 exe 所在目录找 DLL，所有 DLL 和可执行文件必须落在同一个目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/${OUTPUT_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Vulkan SDK (Windows only)
if(WIN32)
    set(VULKAN_SDK $ENV{VULKAN_SDK})
else()
    set(VULKAN_SDK "")
endif()

# Include directories for third-party libraries
set(INCLUDE_DIRS
    spdlog        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/spdlog/include
    debugbreak    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/debugbreak
    GLFW          ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glfw/include
    Glad          ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glad/include
    ImGui         ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/imgui
    glm           ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glm
    stb           ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/stb
    entt          ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/entt/single_include
    tinyxml2      ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/tinyxml2
    xxHash        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/xxHash
    magic_enum    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/magic_enum/include
    assimp        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/assimp/include
    tracy         ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/tracy/public
    spirv-cross   ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/spirv-cross
    glslang       ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glslang
    glslang-SPIRV ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glslang/SPIRV
)

if(VULKAN_SDK)
    list(APPEND INCLUDE_DIRS VulkanSDK ${VULKAN_SDK}/Include)
endif()

# Compiler-specific settings
if(MSVC)
    # Multi-processor compilation
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")

    # Disable iterator debugging to avoid stdext::checked_array_iterator issues
    # with the bundled fmt/spdlog on newer MSVC STL versions (C++20).
    # All project libraries are built from source, so this stays consistent.
    add_compile_definitions(_ITERATOR_DEBUG_LEVEL=0)

    # Disable specific warnings
    add_compile_options(
        /wd4244
        /wd4267
        /wd4312
        /wd4819
        # 导出类里带 STL 成员或继承未导出基类，各模块共用同一套 CRT，可以忽略
        /wd4251
        /wd4275
    )

    # Dynamic runtime library：Windows 下 Kernel 是 DLL，Editor 与脚本 DLL 都要跨模块
    # 传对象（SharedPtr / std::string），静态 CRT 会让每个模块各有一个堆，delete 直接崩。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    # Common GCC/Clang flags
    add_compile_options(-Wall -Wextra)
endif()

# 深度约定：全局启用 GLM 的 ZO（z ∈ [0,1]）投影，与 Metal/Vulkan 的裁剪空间一致。
# 引擎在此基础上统一采用 Reversed-Z（近平面→1，远平面→0），见 Common/Math.h。
# 必须在所有 glm include 之前生效，因此放在全局编译定义中。
add_compile_definitions(GLM_FORCE_DEPTH_ZERO_TO_ONE)

# Platform-specific definitions
if(WIN32)
    add_compile_definitions(PLATFORM_WINDOWS)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    add_compile_definitions(PLATFORM_MACOS)
    # Fix spdlog consteval issue on AppleClang
    # AppleClang has incomplete support for consteval in C++20
    # This forces fmt library to use constexpr instead of consteval
    add_compile_definitions(FMT_CONSTEVAL=)

    # macOS 要求可执行文件至少带有 ad-hoc 签名：未签名的 arm64 二进制会在启动
    # 瞬间被内核以 SIGKILL 终止（表现为“无任何输出地立即退出”，退出码 137）。
    # Xcode 生成器在链接阶段传入了 -no_adhoc_codesign（期望之后由 Xcode 签名），
    # 但本工程没有设置 CODE_SIGN* 构建设置，Xcode 的 CodeSign 阶段不会执行，
    # 于是产物处于未签名状态。此处在各 .app 构建完成后统一补一次 ad-hoc 签名，
    # 对 Makefile/Ninja 等单配置生成器同样是幂等的（链接器已签过则重新签）。
    function(helios_sign_macos_bundle target)
        find_program(HELIOS_CODESIGN_EXECUTABLE codesign)
        if(NOT HELIOS_CODESIGN_EXECUTABLE)
            message(WARNING "codesign 未找到，${target}.app 可能在 Apple Silicon 上无法启动")
            return()
        endif()

        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${HELIOS_CODESIGN_EXECUTABLE} --force --sign - --timestamp=none
                    "$<TARGET_BUNDLE_DIR:${target}>"
            COMMENT "Ad-hoc signing ${target}.app"
            VERBATIM
        )
    endfunction()
elseif(UNIX)
    add_compile_definitions(PLATFORM_LINUX)
endif()

# 资源根：引擎内置资源所有项目共享且只读；项目资源各自一份、可写。
# 默认项目可用 -DHELIOS_PROJECT_DIR=... 换，运行期还能用 --project 覆盖。
set(ENGINE_RES_PATH "${HELIOS_ENGINE_ROOT}/EngineRes")
set(HELIOS_PROJECT_DIR "${HELIOS_ENGINE_ROOT}/Projects/Sample" CACHE PATH "Default project directory")
get_filename_component(HELIOS_PROJECT_DIR "${HELIOS_PROJECT_DIR}" ABSOLUTE BASE_DIR "${HELIOS_ENGINE_ROOT}")

configure_file(
    "${HELIOS_ENGINE_ROOT}/cmake/HeliosConfig.h.in"
    "${CMAKE_BINARY_DIR}/HeliosConfig.h"
    @ONLY
)

# Add third-party libraries
add_subdirectory(Libraries/ThirdParty)

# 任务调度库：只由 Kernel 链接，其它模块走 Helios/Core/Jobs.h
add_subdirectory(Libraries/HeliosJobs)

# 引擎源码与脚本模块共用的头文件目录。脚本要 include 引擎头，两边必须看到同一套
# 第三方库与同一份配置头，否则类型定义对不上（脚本库是运行期 dlopen 的，编译器不会替我们查）。
set(HELIOS_ENGINE_INCLUDE_DIRS
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/spdlog/include
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/debugbreak
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glfw/include
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glad/include
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/imgui
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glm
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/stb
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/entt/single_include
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/tinyxml2
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/xxHash
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/magic_enum/include
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/assimp/include
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/tracy/public
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/spirv-cross
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glslang
    ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glslang/SPIRV
)
if(APPLE)
    list(APPEND HELIOS_ENGINE_INCLUDE_DIRS ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/metal-cpp)
endif()
if(VULKAN_SDK)
    list(APPEND HELIOS_ENGINE_INCLUDE_DIRS ${VULKAN_SDK}/Include)
endif()

# Add Kernel
add_subdirectory(Kernel)

# Add applications
# 说明：旧的 ModelEditor 独立编辑器已废弃（其场景/模型编辑能力已并入 Editor 的
# MainEditor / SceneEditor / ModelEditor 三个面板层），源目录与 target 一并移除。
add_subdirectory(Editor)

# Add samples (optional)
option(BUILD_SAMPLES "Build sample projects" ON)  # Disabled for macOS compatibility
if(BUILD_SAMPLES)
    add_subdirectory(Samples/Sample_SkyBox)
    add_subdirectory(Samples/Sample_CSM)
    add_subdirectory(Samples/Sample_IBL)
endif()

# 项目脚本模板
# 编辑器打开任意位置的项目时，用这份模板套项目自己的 Scripts 现场生成一个小工程，
# 产物落在项目的 Cache 下。引擎构建目录不需要知道项目在哪，换盘换目录都不用重配。
set(HELIOS_SCRIPT_KERNEL_DIR "${CMAKE_BINARY_DIR}/lib/${OUTPUT_DIR}")
if(WIN32)
    set(HELIOS_SCRIPT_PLATFORM_DEFINE "PLATFORM_WINDOWS")
elseif(APPLE)
    set(HELIOS_SCRIPT_PLATFORM_DEFINE "PLATFORM_MACOS")
else()
    set(HELIOS_SCRIPT_PLATFORM_DEFINE "PLATFORM_LINUX")
endif()

set(HELIOS_SCRIPT_INCLUDE_BLOCK "")
foreach(HELIOS_SCRIPT_INCLUDE_DIR IN LISTS HELIOS_ENGINE_INCLUDE_DIRS)
    string(APPEND HELIOS_SCRIPT_INCLUDE_BLOCK "    \"${HELIOS_SCRIPT_INCLUDE_DIR}\"\n")
endforeach()

configure_file(
    "${HELIOS_ENGINE_ROOT}/cmake/HeliosScriptTemplate.cmake.in"
    "${CMAKE_BINARY_DIR}/HeliosScriptTemplate.cmake"
    @ONLY
)

# Print configuration summary
message(STATUS "")
message(STATUS "Helios Build Configuration:")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Generator: ${CMAKE_GENERATOR}")
message(STATUS "  Vulkan SDK: ${VULKAN_SDK}")
message(STATUS "")
