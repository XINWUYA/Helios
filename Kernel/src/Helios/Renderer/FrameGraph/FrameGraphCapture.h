#pragma once
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <Helios/Common/Common.h>
#include <Helios/Renderer/RenderCommon.h>

namespace Helios
{
	class DeviceTexture;

	/* FrameGraph 的逐 Pass 抓取设施：每个 Pass 执行完当刻把附件（颜色 + 深度）拷到常驻预览纹理
	 * （给 Frame Graph 页呈现中间结果）。用快照而不是直接引用：叠加型 Pass 之后资源还会被改、
	 * transient 归还缓存后引用会干扰轮转。支持 2D / 2D 数组附件；禁用期间不清快照（面板冻结）。 */
	class FrameGraphCapture
	{
	public:
		/* 某个 Pass 抓取到的一路输出 */
		struct Output
		{
			std::string Name;                 /* 资源名（= FrameGraph 中的纹理资源名） */
			SharedPtr<DeviceTexture> Preview; /* 常驻预览纹理（与源同尺寸同格式） */
			uint64_t Sequence{ 0 };           /* 帧内抓取序号（判断"某资源最后被写入的当刻"用） */
			/* 行序 = 顶行在前（后端上报：Metal 的默认目标快照为 true；离屏纹理与
			 * OpenGL 的默认目标快照为底行在前）—— 采样方据此翻转 V
			 * （面板按它选 (0,0)/(1,1) 或 (0,1)/(1,0)） */
			bool TopDown{ false };
		};

		void SetEnabled(bool enabled) { m_IsEnabled = enabled; }
		[[nodiscard]] bool IsEnabled() const { return m_IsEnabled; }

		/* 帧边界（由 FrameGraph::Execute 驱动）：开始收集本帧出现的 Pass；帧末淘汰
		 * 本帧未出现的 Pass（其预览纹理一并释放）。禁用时为空操作。 */
		void BeginFrame();
		void EndFrame();

		/* 抓取一个 Pass 的附件（在该 Pass 执行完、资源销毁之前调用）。
		 * outputs 为 (资源名, 纹理) 列表（颜色附件按序在前、深度附件在后）；
		 * 不满足预览条件的项（多重采样 / 立方图 / 3D / 整数等不可显示格式）在内部过滤。 */
		void CapturePass(const std::string& pass_name,
			const std::vector<std::pair<std::string, SharedPtr<DeviceTexture>>>& outputs);

		/* 抓取"默认目标快照"作为某 Pass 的一路输出（渲染到窗口默认目标、没有附件纹理
		 * 的 Pass）—— 纹理由 RenderAPI 持有并跨帧复用，这里只做登记。
		 * top_down = 快照的行序（后端上报：Metal 顶行在前、OpenGL 底行在前） */
		void CaptureSnapshot(const std::string& pass_name, const std::string& resource_name,
			const SharedPtr<DeviceTexture>& texture, bool top_down);

		/* 查询某 Pass 最近一次抓取到的输出（按附件顺序）；没有则返回 nullptr */
		[[nodiscard]] const std::vector<Output>* GetPassOutputs(const std::string& pass_name) const;

		/* 查询某个资源最近一次被抓取到的快照（帧内执行顺序里的最后一次写入 ——
		 * 资源节点用它显示"该资源的最终内容"，如 GBuffer 各通道）；无则 nullptr */
		[[nodiscard]] const Output* GetResourcePreview(const std::string& resource_name) const;

		/* 已抓取输出总数（诊断 / 验证用） */
		[[nodiscard]] size_t GetCapturedOutputCount() const;

	private:
		/* 判定一张纹理能否作为预览源：2D / 2D 数组采样类型 + 非多重采样 + 可显示格式 */
		[[nodiscard]] static bool IsPreviewable(const SharedPtr<DeviceTexture>& texture);
		/* 显示格式白名单：能被 ImGui 渲染路径正常显示的格式（深度格式走深度变体） */
		[[nodiscard]] static bool IsDisplayableFormat(TextureFormat format);
		/* 预览纹理与源纹理的尺寸 / 格式是否一致（一致才可复用） */
		[[nodiscard]] static bool PreviewMatches(const SharedPtr<DeviceTexture>& preview,
			const SharedPtr<DeviceTexture>& source);
		/* 为一路输出创建预览纹理 */
		[[nodiscard]] SharedPtr<DeviceTexture> CreatePreview(const std::string& pass_name,
			const std::string& resource_name, const SharedPtr<DeviceTexture>& source);

		struct PassEntry
		{
			std::vector<Output> Outputs;
			bool SeenThisFrame{ false };
		};

		bool m_IsEnabled{ false };
		uint64_t m_SequenceCounter{ 0 }; /* 帧内抓取序号（单调递增，跨帧保持） */
		std::unordered_map<std::string, PassEntry> m_Passes;
	};
}
