cmake_minimum_required(VERSION 3.20)

project(Player LANGUAGES CXX)

# 玩家形态的应用：引擎里通用的外壳，跑的是 HELIOS_PROJECT_DIR 指向的那个项目。
# 应用名与包标识跟项目走，新项目不用改这里；桌面与 iOS 的入口差别都在 Kernel 的平台层
helios_add_app(Player SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" FOLDER "Applications")

# 包名取 Project.xml 的 Name，包标识默认 com.helios.<名字小写>，要换就配置时传 -DHELIOS_APP_BUNDLE_ID
get_filename_component(PLAYER_APP_NAME "${HELIOS_PROJECT_DIR}" NAME)
if(EXISTS "${HELIOS_PROJECT_DIR}/Project.xml")
    file(READ "${HELIOS_PROJECT_DIR}/Project.xml" PLAYER_PROJECT_XML)
    if(PLAYER_PROJECT_XML MATCHES "<Project[^>]* Name=\"([^\"]+)\"")
        set(PLAYER_APP_NAME "${CMAKE_MATCH_1}")
    endif()
endif()
if(HELIOS_APP_BUNDLE_ID)
    set(PLAYER_BUNDLE_ID "${HELIOS_APP_BUNDLE_ID}")
else()
    string(TOLOWER "${PLAYER_APP_NAME}" PLAYER_BUNDLE_SUFFIX)
    string(REGEX REPLACE "[^a-z0-9-]" "-" PLAYER_BUNDLE_SUFFIX "${PLAYER_BUNDLE_SUFFIX}")
    set(PLAYER_BUNDLE_ID "com.helios.${PLAYER_BUNDLE_SUFFIX}")
endif()

set_target_properties(Player PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "${PLAYER_APP_NAME}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${PLAYER_BUNDLE_ID}"
)

# iOS 不允许加载没签名的代码，项目脚本只能随包编进主二进制：
# 源文件直接进 Player 目标，注册靠脚本末尾静态对象在启动时跑一遍。
if(HELIOS_TARGET_PLATFORM STREQUAL "iOS")
    file(GLOB_RECURSE PLAYER_SCRIPT_SOURCES CONFIGURE_DEPENDS
         "${HELIOS_PROJECT_DIR}/Scripts/*.cpp")
    if(PLAYER_SCRIPT_SOURCES)
        target_sources(Player PRIVATE ${PLAYER_SCRIPT_SOURCES})
        target_include_directories(Player PRIVATE "${HELIOS_PROJECT_DIR}/Scripts")
        # 脚本不写 #include "Pch.h"，引擎底座由编辑器那套预编译头带进来，这里照办
        set_source_files_properties(${PLAYER_SCRIPT_SOURCES} PROPERTIES
            COMPILE_FLAGS "-include ${HELIOS_ENGINE_ROOT}/Kernel/src/Helios/Script/ScriptPch.h")
    endif()

    set_target_properties(Player PROPERTIES
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    )

    # 随包资源：默认不拷，跑打包形态时打开。
    # Cache 是生成物，不随包；工程里其余目录跟着包走，运行期在可执行文件旁找 Project。
    option(HELIOS_BUNDLE_ASSETS "Copy engine and project assets into the app bundle" OFF)
    if(HELIOS_BUNDLE_ASSETS)
        set(PLAYER_BUNDLE_DIR "$<TARGET_BUNDLE_DIR:Player>")

        add_custom_command(TARGET Player POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${ENGINE_RES_PATH}"
                    "${PLAYER_BUNDLE_DIR}/EngineRes"
            COMMENT "Bundling engine resources"
            VERBATIM
        )

        # 工程的资源目录整个随包，只排除生成物与本机状态。
        # 用 glob 而不是写死清单：以后加资源目录不用再改这里。
        file(GLOB PLAYER_PROJECT_DIRS CONFIGURE_DEPENDS LIST_DIRECTORIES true
             RELATIVE "${HELIOS_PROJECT_DIR}" "${HELIOS_PROJECT_DIR}/*")
        set(PLAYER_BUNDLE_PROJECT_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E make_directory "${PLAYER_BUNDLE_DIR}/Project"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HELIOS_PROJECT_DIR}/Project.xml" "${PLAYER_BUNDLE_DIR}/Project/Project.xml"
        )
        foreach(entry IN LISTS PLAYER_PROJECT_DIRS)
            if(entry STREQUAL "Cache" OR entry STREQUAL ".helios")
                continue()
            endif()
            if(IS_DIRECTORY "${HELIOS_PROJECT_DIR}/${entry}")
                list(APPEND PLAYER_BUNDLE_PROJECT_COMMANDS
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                            "${HELIOS_PROJECT_DIR}/${entry}" "${PLAYER_BUNDLE_DIR}/Project/${entry}")
            endif()
        endforeach()

        # 有 .astc 的贴图不再随包递 PNG：两份都带等于包体翻倍
        file(GLOB_RECURSE PLAYER_PROJECT_PNGS RELATIVE "${HELIOS_PROJECT_DIR}"
             "${HELIOS_PROJECT_DIR}/*.png")
        set(PLAYER_STALE_PNGS "")
        foreach(png IN LISTS PLAYER_PROJECT_PNGS)
            string(REGEX REPLACE "\\.png$" ".astc" baked "${png}")
            if(EXISTS "${HELIOS_PROJECT_DIR}/${baked}")
                list(APPEND PLAYER_STALE_PNGS "${PLAYER_BUNDLE_DIR}/Project/${png}")
            endif()
        endforeach()
        if(PLAYER_STALE_PNGS)
            list(APPEND PLAYER_BUNDLE_PROJECT_COMMANDS
                COMMAND ${CMAKE_COMMAND} -E rm -f ${PLAYER_STALE_PNGS})
        endif()

        add_custom_command(TARGET Player POST_BUILD
            ${PLAYER_BUNDLE_PROJECT_COMMANDS}
            COMMENT "Bundling project assets"
            VERBATIM
        )
    endif()
endif()
