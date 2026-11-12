# 示例：每个目录的 src 就是全部源码，入口统一走 Common/SampleMain.h
foreach(sample IN ITEMS Sample_SkyBox Sample_CSM Sample_IBL Sample_Scene)
    helios_add_app(${sample} SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${sample}/src" FOLDER "Samples")
    target_include_directories(${sample} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Common")
endforeach()
