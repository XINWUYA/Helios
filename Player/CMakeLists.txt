cmake_minimum_required(VERSION 3.20)

project(Player LANGUAGES CXX)

# 玩家形态的应用：桌面与 iOS 共用同一份源码。
# 桌面由 main() 起 while 循环，iOS 由 UIApplicationMain 接管 run loop，
# 帧由窗口的 display link 推 —— 差别全在平台层，这一层只负责载入场景与出图。
add_executable(Player)

file(GLOB_RECURSE PLAYER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.mm"
)

target_sources(Player PRIVATE ${PLAYER_SOURCES})

# iOS 入口是 Objective-C++；桌面这一份会被平台宏预处理成空文件
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/IOSMain.mm" PROPERTIES
    COMPILE_FLAGS "-x objective-c++"
    SKIP_PRECOMPILE_HEADERS ON
)

target_include_directories(Player
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${HELIOS_ENGINE_ROOT}/Kernel/src
        # 引擎头文件依赖的第三方（glm / entt / spdlog 等），与脚本模块同一份清单
        ${HELIOS_ENGINE_INCLUDE_DIRS}
)

target_link_libraries(Player PRIVATE Kernel)

target_compile_definitions(Player
    PRIVATE
        $<$<CONFIG:Debug>:HELIOS_DEBUG>
        $<$<CONFIG:Release>:HELIOS_RELEASE>
        $<$<CONFIG:Shipping>:HELIOS_SHIPPING>
)

set_target_properties(Player PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${OUTPUT_DIR}"
    FOLDER "Applications"
)

if(HELIOS_TARGET_PLATFORM STREQUAL "iOS")
    set_target_properties(Player PROPERTIES
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Player"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.helios.player"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    )
elseif(APPLE)
    set_target_properties(Player PROPERTIES
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_BUNDLE_NAME "Player"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.helios.player"
    )

    helios_set_app_icon(Player)
    helios_sign_macos_bundle(Player)
endif()
