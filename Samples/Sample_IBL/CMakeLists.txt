cmake_minimum_required(VERSION 3.20)

project(Sample_IBL LANGUAGES CXX)

# Executable
add_executable(Sample_IBL)

# Source files
file(GLOB_RECURSE SAMPLE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

target_sources(Sample_IBL PRIVATE ${SAMPLE_SOURCES})

# Organize source files in Visual Studio by directory structure
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${SAMPLE_SOURCES})

# Include directories
target_include_directories(Sample_IBL
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/Kernel/src
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/spdlog/include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/debugbreak
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glfw/include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glad/include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/imgui
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/glm
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/entt/single_include
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/tinyxml2
        ${CMAKE_SOURCE_DIR}/Libraries/ThirdParty/magic_enum/include
)

# Link libraries
target_link_libraries(Sample_IBL
    PRIVATE
        Kernel
)

# Build configuration specific settings
target_compile_definitions(Sample_IBL
    PRIVATE
        $<$<CONFIG:Debug>:HELIOS_DEBUG>
        $<$<CONFIG:Release>:HELIOS_RELEASE>
        $<$<CONFIG:Shipping>:HELIOS_SHIPPING>
)

# Output directory
set_target_properties(Sample_IBL PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${OUTPUT_DIR}"
    FOLDER "Samples"
)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(Sample_IBL PRIVATE PLATFORM_WINDOWS)
elseif(APPLE)
    set_target_properties(Sample_IBL PROPERTIES
        MACOSX_BUNDLE ON
    )

    helios_sign_macos_bundle(Sample_IBL)
endif()
