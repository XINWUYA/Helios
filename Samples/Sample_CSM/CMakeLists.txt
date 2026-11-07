cmake_minimum_required(VERSION 3.20)

project(Sample_CSM LANGUAGES CXX)

# Executable
add_executable(Sample_CSM)

# Source files
file(GLOB_RECURSE SAMPLE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

target_sources(Sample_CSM PRIVATE ${SAMPLE_SOURCES})

# Organize source files in Visual Studio by directory structure
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${SAMPLE_SOURCES})

# Include directories
target_include_directories(Sample_CSM
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${HELIOS_ENGINE_ROOT}/Kernel/src
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/spdlog/include
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/debugbreak
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glfw/include
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glad/include
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/imgui
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/glm
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/entt/single_include
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/tinyxml2
        ${HELIOS_ENGINE_ROOT}/Libraries/ThirdParty/magic_enum/include
)

# Link libraries
target_link_libraries(Sample_CSM
    PRIVATE
        Kernel
)

# Build configuration specific settings
target_compile_definitions(Sample_CSM
    PRIVATE
        $<$<CONFIG:Debug>:HELIOS_DEBUG>
        $<$<CONFIG:Release>:HELIOS_RELEASE>
        $<$<CONFIG:Shipping>:HELIOS_SHIPPING>
        # 打开性能插桩（HELIOS_PROFILE）：解开后 Main.cpp 里的 PROFILER_BEGIN/END_SESSION
        # 会话生效、写 PerfettoWriter-*.json（chrome://tracing 可开）；不写就注释掉会话
        # HELIOS_PROFILE=1
)

# Output directory
set_target_properties(Sample_CSM PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${OUTPUT_DIR}"
    FOLDER "Samples"
)

helios_set_app_icon(Sample_CSM)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(Sample_CSM PRIVATE PLATFORM_WINDOWS)
elseif(APPLE)
    set_target_properties(Sample_CSM PROPERTIES
        MACOSX_BUNDLE ON
    )

    helios_sign_macos_bundle(Sample_CSM)
endif()
