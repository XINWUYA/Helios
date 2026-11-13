# 引擎应用的公共配置：Player、示例和项目自己的应用都走这里，别再各抄一份

# 建可执行目标：收源码、链 Kernel、按平台打图标、签名与包信息
#   helios_add_app(<target> SOURCE_DIR <dir> [FOLDER <IDE 分组>])
function(helios_add_app target)
    cmake_parse_arguments(ARG "" "SOURCE_DIR;FOLDER" "" ${ARGN})

    add_executable(${target})

    file(GLOB_RECURSE app_sources CONFIGURE_DEPENDS
        "${ARG_SOURCE_DIR}/*.cpp"
        "${ARG_SOURCE_DIR}/*.h"
    )
    target_sources(${target} PRIVATE ${app_sources})
    source_group(TREE "${ARG_SOURCE_DIR}" FILES ${app_sources})

    target_include_directories(${target} PRIVATE "${ARG_SOURCE_DIR}" ${HELIOS_ENGINE_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE Kernel)
    target_compile_definitions(${target} PRIVATE
        $<$<CONFIG:Debug>:HELIOS_DEBUG>
        $<$<CONFIG:Release>:HELIOS_RELEASE>
        $<$<CONFIG:Shipping>:HELIOS_SHIPPING>
    )

    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${OUTPUT_DIR}"
        FOLDER "${ARG_FOLDER}"
    )

    if(HELIOS_TARGET_PLATFORM STREQUAL "macOS")
        set_target_properties(${target} PROPERTIES MACOSX_BUNDLE ON)
        helios_set_app_icon(${target})
        # 签名必须排在最后一个 POST_BUILD，之后再往包里拷东西签名就失效了
        helios_sign_macos_bundle(${target})
    elseif(HELIOS_TARGET_PLATFORM STREQUAL "iOS")
        # 包信息走公共模板，任何 iOS 应用都能复用
        set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE ON
            MACOSX_BUNDLE_INFO_PLIST "${HELIOS_ENGINE_ROOT}/cmake/AppInfo.plist.in"
            MACOSX_BUNDLE_BUNDLE_VERSION "1"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        )
    elseif(HELIOS_TARGET_PLATFORM STREQUAL "Windows")
        helios_set_app_icon(${target})
    endif()
endfunction()

# 显示名取 Project.xml 的 Name，包标识默认 com.helios.<名字小写>
# 要换就配置时传 -DHELIOS_APP_BUNDLE_ID
function(helios_apply_project_identity target)
    get_filename_component(app_name "${HELIOS_PROJECT_DIR}" NAME)
    if(EXISTS "${HELIOS_PROJECT_DIR}/Project.xml")
        file(READ "${HELIOS_PROJECT_DIR}/Project.xml" project_xml)
        if(project_xml MATCHES "<Project[^>]* Name=\"([^\"]+)\"")
            set(app_name "${CMAKE_MATCH_1}")
        endif()
    endif()

    if(HELIOS_APP_BUNDLE_ID)
        set(bundle_id "${HELIOS_APP_BUNDLE_ID}")
    else()
        string(TOLOWER "${app_name}" suffix)
        string(REGEX REPLACE "[^a-z0-9-]" "-" suffix "${suffix}")
        set(bundle_id "com.helios.${suffix}")
    endif()

    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "${app_name}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_id}"
    )
endfunction()

# iOS 不允许加载没签名的代码：项目脚本编进主二进制，
# 注册靠脚本末尾的静态对象在启动时跑一遍
function(helios_embed_project_scripts target)
    if(NOT HELIOS_TARGET_PLATFORM STREQUAL "iOS")
        return()
    endif()

    file(GLOB_RECURSE script_sources CONFIGURE_DEPENDS "${HELIOS_PROJECT_DIR}/Scripts/*.cpp")
    if(NOT script_sources)
        return()
    endif()

    target_sources(${target} PRIVATE ${script_sources})
    target_include_directories(${target} PRIVATE "${HELIOS_PROJECT_DIR}/Scripts")
    # 脚本不写 #include "Pch.h"，引擎底座由这份预编译头带进来
    set_source_files_properties(${script_sources} PROPERTIES
        COMPILE_FLAGS "-include ${HELIOS_ENGINE_ROOT}/Kernel/src/Helios/Script/ScriptPch.h")
endfunction()

# 随包资源：引擎资源与工程目录拷进包，运行期在可执行文件旁找。
# 只在 iOS 有意义，默认不拷，跑打包形态时传 -DHELIOS_BUNDLE_ASSETS=ON
function(helios_bundle_assets target)
    if(NOT HELIOS_TARGET_PLATFORM STREQUAL "iOS")
        return()
    endif()

    option(HELIOS_BUNDLE_ASSETS "Copy engine and project assets into the app bundle" OFF)
    if(NOT HELIOS_BUNDLE_ASSETS)
        return()
    endif()

    set(bundle_dir "$<TARGET_BUNDLE_DIR:${target}>")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${ENGINE_RES_PATH}" "${bundle_dir}/EngineRes"
        COMMENT "Bundling engine resources"
        VERBATIM
    )

    # 工程目录整个随包，只排除生成物与本机状态；用 glob 免得以后加资源目录还要改这里
    file(GLOB project_dirs CONFIGURE_DEPENDS LIST_DIRECTORIES true
         RELATIVE "${HELIOS_PROJECT_DIR}" "${HELIOS_PROJECT_DIR}/*")
    set(bundle_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory "${bundle_dir}/Project"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HELIOS_PROJECT_DIR}/Project.xml" "${bundle_dir}/Project/Project.xml"
    )
    foreach(entry IN LISTS project_dirs)
        if(entry STREQUAL "Cache" OR entry STREQUAL ".helios")
            continue()
        endif()
        if(IS_DIRECTORY "${HELIOS_PROJECT_DIR}/${entry}")
            list(APPEND bundle_commands
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${HELIOS_PROJECT_DIR}/${entry}" "${bundle_dir}/Project/${entry}")
        endif()
    endforeach()

    # 有 .astc 的贴图不再随包递 PNG：两份都带等于包体翻倍
    file(GLOB_RECURSE project_pngs RELATIVE "${HELIOS_PROJECT_DIR}" "${HELIOS_PROJECT_DIR}/*.png")
    set(stale_pngs "")
    foreach(png IN LISTS project_pngs)
        string(REGEX REPLACE "\\.png$" ".astc" baked "${png}")
        if(EXISTS "${HELIOS_PROJECT_DIR}/${baked}")
            list(APPEND stale_pngs "${bundle_dir}/Project/${png}")
        endif()
    endforeach()
    if(stale_pngs)
        list(APPEND bundle_commands COMMAND ${CMAKE_COMMAND} -E rm -f ${stale_pngs})
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        ${bundle_commands}
        COMMENT "Bundling project assets"
        VERBATIM
    )
endfunction()
