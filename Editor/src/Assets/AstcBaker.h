#pragma once
#include <cstdint>
#include <string>

namespace Helios
{
	/* 贴图属性面板的 ASTC 烘焙：按块尺寸与目标边长重烘当前贴图，
	 * 产物是引擎直接读的同名 .astc（16 字节头 + 全 mip 链）。
	 * 编码走工程内编好的 astcenc 静态库，作为低优先级后台任务跑，面板每帧取一次状态。 */
	namespace AstcBake
	{
		struct Options
		{
			int BlockX{ 6 };
			int BlockY{ 6 };
			int MaxSize{ 0 };         /* 长边目标像素；0 = 保持源尺寸 */
			bool FlipVertical{ true }; /* 与贴图设置的 Flip V 一致，烘出来才和 PNG 路径同向 */
		};

		struct Status
		{
			enum class State : uint8_t { Idle, Running, Succeeded, Failed };

			State Current{ State::Idle };
			std::string SourcePath;
			std::string OutputPath;
			std::string Message;  /* 失败原因 */
			int Level{ 0 };       /* 已编码的 mip 级数 */
			int LevelCount{ 0 };
			int Width{ 0 };       /* 产物尺寸 */
			int Height{ 0 };
			long long Bytes{ 0 }; /* 产物字节数 */
		};

		/* 源图尺寸（只读文件头）；取不到返回 false */
		bool SourceSize(const std::string& source_path, int& width, int& height);

		/* 已经烘过的同名 .astc：存在与否，以及头里的块尺寸、尺寸、字节数 */
		struct BakedInfo
		{
			bool Exists{ false };
			bool Stale{ false }; /* 产物比源图旧，源图改过还没重烘 */
			int BlockX{ 0 };
			int BlockY{ 0 };
			int Width{ 0 };
			int Height{ 0 };
			int Levels{ 0 };    /* 全 mip 链的级数（容器没有这个字段，按尺寸推） */
			long long Bytes{ 0 };
		};
		BakedInfo InspectBaked(const std::string& source_path);

		/* 直接看一个 .astc 文件（资源面板与属性卡预览块数据用），Stale 恒为 false */
		BakedInfo InspectAstc(const std::string& astc_path);

		/* 按块尺寸与尺寸估算容器字节数（含全 mip 链），界面用来给个体积预览 */
		long long EstimateBytes(int width, int height, int block_x, int block_y);

		/* 起一次后台烘焙（同一时间只跑一个任务），已在跑时返回 false。
		 * 产物写源图旁边的同名 .astc，完成后由调用方重载贴图。 */
		bool Start(const std::string& source_path, const Options& options);

		/* 当前状态快照 */
		Status Poll();
	}
}
