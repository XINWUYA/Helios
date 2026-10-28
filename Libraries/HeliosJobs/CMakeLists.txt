cmake_minimum_required(VERSION 3.20)

# 任务调度库：内核取自 Filament JobSystem，只依赖标准库，不引引擎头文件。
# 只允许 Kernel 链接它，其它模块走 Helios/Core/Jobs.h，否则 Windows 上会各有一份线程池。
add_library(HeliosJobs STATIC)

target_sources(HeliosJobs PRIVATE
	include/HeliosJobs/JobSystem.h
	include/HeliosJobs/ThreadQueue.h
	include/HeliosJobs/WorkStealingDequeue.h
	src/JobSystem.cpp
	src/ThreadQueue.cpp
)

target_include_directories(HeliosJobs PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 静态库要进 Kernel 动态库，必须位置无关
set_target_properties(HeliosJobs PROPERTIES
	POSITION_INDEPENDENT_CODE ON
	FOLDER "Libraries"
)

find_package(Threads REQUIRED)
target_link_libraries(HeliosJobs PUBLIC Threads::Threads)
