#pragma once
#include <string>
#include <vector>
#include "Helios/Common/Common.h"
#include "Helios/Renderer/RenderCommon.h"

namespace Helios
{
	class DeviceShader;

	/*
	 * 一笔绘制的变体取值来源。
	 * External 由绘制端逐笔给（调试档位这类），View 由视图给（换视图才变）；
	 * Global 来源走 ShaderVariantGlobals，进程内配置一次。
	 */
	struct ShaderVariantContext
	{
		const ShaderVariantDefines* External{ nullptr };
		const ShaderVariantDefines* View{ nullptr };
	};

	/* 进程级变体取值：平台能力、质量档这类换进程才变的东西 */
	class ShaderVariantGlobals
	{
	public:
		static void Set(const std::string& name, int value);
		static void Clear();
		[[nodiscard]] static const ShaderVariantDefines& All();

		/* 同一份源码的同一组差异只让上层报一次 */
		static bool ReportFallback(const std::string& path, const std::string& diff);

	private:
		static ShaderVariantDefines s_Values;
	};

	/* 变体匹配结果：候选中跟请求最接近的一份 */
	struct ShaderVariantMatch
	{
		SharedPtr<DeviceShader> Shader{ nullptr };
		int Score{ 0 };
		int FullScore{ 0 };
		/* 与请求不一致的维度，拼成 "DEBUG_VIEW 1→0"；空串表示完全一致 */
		std::string Diff{};

		[[nodiscard]] bool Valid() const { return Shader != nullptr; }
		[[nodiscard]] bool Exact() const { return Valid() && Diff.empty(); }
	};

	/* 逐维度打分：相等给满分，候选值更小按比例给分（越接近越高），
	 * 更大或候选缺这一维不给分。方向取"宁可用弱一档"——回退出来的画面偏暗比偏亮安全。 */
	int ScoreVariant(const ShaderVariantDefines& requested, const ShaderVariantDefines& candidate,
		std::string* diff = nullptr);

	/* 在候选里挑分数最高的一个；候选为空返回空匹配 */
	ShaderVariantMatch FindBestVariant(const std::vector<SharedPtr<DeviceShader>>& candidates,
		const ShaderVariantDefines& requested);
}
