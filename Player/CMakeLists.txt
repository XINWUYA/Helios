cmake_minimum_required(VERSION 3.20)

project(Player LANGUAGES CXX)

# 玩家形态的应用：引擎里通用的外壳，跑的是 HELIOS_PROJECT_DIR 指向的那个项目
# 应用名、包标识、项目脚本与随包资源都跟着项目走，换项目不用改这里
helios_add_app(Player SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" FOLDER "Applications")
helios_apply_project_identity(Player)
helios_embed_project_scripts(Player)
helios_bundle_assets(Player)
