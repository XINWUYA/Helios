cmake_minimum_required(VERSION 3.20)

project(Helios VERSION 1.0.0 LANGUAGES C CXX)

# Enable folder organization in IDEs (Xcode, Visual Studio, etc.)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Build configurations
set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Shipping" CACHE STRING "" FORCE)

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# Output directory format
set(OUTPUT_DIR "${CMAKE_BUILD_TYPE}-${CMAKE_SYSTEM_NAME}-x64")

# Vulkan SDK (Windows only)
if(WIN32)
    set(VULKAN_SDK $ENV{VULKAN_SDK})
else()
    set(VULKAN_SDK "")
endif()

# Include directories for third-party libraries
set(INCLUDE_DIRS
    spdlog        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/spdlog/include
    debugbreak    ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/debugbreak
    GLFW          ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glfw/include
    Glad          ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glad/include
    ImGui         ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/imgui
    glm           ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glm
    stb           ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/stb
    entt          ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/entt/single_include
    tinyxml2      ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/tinyxml2
    ImGuizmo      ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/ImGuizmo
    xxHash        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/xxHash
    tinyobjloader ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/tinyobjloader
    magic_enum    ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/magic_enum/include
    assimp        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/assimp/include
    tracy         ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/tracy/public
    spirv-cross   ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/spirv-cross
    glslang       ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glslang
    glslang-SPIRV ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glslang/SPIRV
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
    )

    # Static runtime library
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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

# Assets path definition - generate config header
set(ASSETS_PATH "${CMAKE_SOURCE_DIR}/Assets")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/HeliosConfig.h.in"
    "${CMAKE_BINARY_DIR}/HeliosConfig.h"
    @ONLY
)

# Add third-party libraries
add_subdirectory(Libraries/ThirdParty)

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

# Print configuration summary
message(STATUS "")
message(STATUS "Helios Build Configuration:")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Generator: ${CMAKE_GENERATOR}")
message(STATUS "  Vulkan SDK: ${VULKAN_SDK}")
message(STATUS "")
