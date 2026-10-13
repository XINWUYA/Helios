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
	 * transient 归还缓存后引用会干扰轮转。支持 2D / 2D 数组附件（数组默认取第 0 层，注册过
	 * 逐层显示名的按层展开，见 SetLayerNames）；禁用期间不清快照（面板冻结）。深度输出额外
	 * 按帧刷新逐层显示范围（写过像素的 [min,max]），供深度预览逐层归一化显示（对齐 Xcode 抓帧）。 */
	class FrameGraphCapture
	{
	public:
		/* 某个 Pass 抓取到的一路输出 */
		struct Output
		{
			std::string Name;                 /* 显示名（常规 = 资源名；逐层展开 = 层名，如 "Cascade 0"） */
			std::string ResourceName;         /* 所属 FrameGraph 资源名（逐层展开的层卡 = 数组资源名，
			                                   * 供面板按依赖图身份做连线 / 聚焦；常规输出与 Name 相同） */
			SharedPtr<DeviceTexture> Preview; /* 常驻预览纹理（与源同尺寸同格式） */
			uint64_t Sequence{ 0 };           /* 帧内抓取序号（判断"某资源最后被写入的当刻"用） */
			/* 行序 = 顶行在前（后端上报：Metal 默认目标快照 / 离屏渲染目标为 true；
			 * OpenGL 均为底行在前）—— 采样方据此翻转 V
			 * （面板按它选 (0,0)/(1,1) 或 (0,1)/(1,0)） */
			bool TopDown{ false };
			/* 深度预览的显示范围（逐层归一化的基准）：写过像素（> 0）的 [min, max]。
			 * 空层 / 常量层保持无效（max <= min，预览端回退提亮曲线）。由 CapturePass
			 * 按帧龄节流挂起刷新（BeginFrame 里 CPU 回读上一帧已提交的拷贝），并同步
			 * 写入预览纹理（ImGui 深度变体按纹理上的登记值设置 uniform）。 */
			float DisplayMin{ 1.0f };
			float DisplayMax{ 0.0f };
			uint64_t RangeFrame{ 0 };         /* 最近一次范围刷新的帧号（节流用） */
			bool HasRange{ false };           /* 是否已刷新出现过内容（空层重试的终止条件） */
			uint8_t EmptyRetries{ 0 };        /* 连续刷到空内容的次数（到上限后只走帧龄节流） */
			/* 范围回读的复用缓冲：随输出跨帧迁移，只在首帧 / 尺寸变化时真正分配
			 * —— 避免每次重建全幅（Debug 下 50ms+） */
			std::vector<uint8_t> RangeReadback;
		};

		void SetEnabled(bool enabled) { m_IsEnabled = enabled; }
		[[nodiscard]] bool IsEnabled() const { return m_IsEnabled; }

		/* 注册一张数组纹理的"逐层显示名"（下标 = 层号）：它被抓取时按层展开成多条输出
		 * —— 每层一条、名字取注册值（缺省 "资源名 #层号"），各配独立预览纹理；而不是
		 * 只取第 0 层。阴影图（级联 / 点光面共用同一张数组）用它把每层单独送进可视化面板。
		 * 未注册的资源、或层数 = 1 的纹理，维持"整幅取 slice 0"的原有语义。 */
		void SetLayerNames(const std::string& resource_name, std::vector<std::string> layer_names);

		/* 帧边界（由 FrameGraph::Execute 驱动）：开始收集本帧出现的 Pass；帧末淘汰
		 * 本帧未出现的 Pass（其预览纹理一并释放）。禁用时为空操作。 */
		void BeginFrame();
		void EndFrame();

		/* 抓取一个 Pass 的附件（在该 Pass 执行完、资源销毁之前调用）。
		 * outputs 为 (资源名, 纹理) 列表（颜色附件按序在前、深度附件在后）；
		 * 不满足预览条件的项（多重采样 / 立方图 / 3D / 整数等不可显示格式）在内部过滤；
		 * 注册过逐层显示名的 2D 数组按层展开成多条输出（见 SetLayerNames）。 */
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

		/* 刷新一路深度输出的显示范围：CPU 回读预览、取写过像素（> 0）的 [min, max]，
		 * 写入输出字段与预览纹理属性；空层 / 常量层落无效范围（回退提亮曲线）。
		 * 由 BeginFrame 消费上一帧挂起的请求调用（此刻上一帧的命令已提交、内容可回读）。 */
		void RefreshDisplayRange(Output& output);

		struct PassEntry
		{
			std::vector<Output> Outputs;
			bool SeenThisFrame{ false };
			/* 待刷新显示范围的输出名（名字存活到下一帧；跨帧淘汰的 Pass 一并丢弃） */
			std::string PendingRangeOutput;
		};

		bool m_IsEnabled{ false };
		uint64_t m_SequenceCounter{ 0 }; /* 帧内抓取序号（单调递增，跨帧保持） */
		uint64_t m_FrameSerial{ 0 };     /* 帧号（BeginFrame 递增；显示范围的帧龄节流用） */
		std::unordered_map<std::string, PassEntry> m_Passes;
		/* 逐层展开的显示名（资源名 → 各层名字；注册后长期有效，跨帧保留） */
		std::unordered_map<std::string, std::vector<std::string>> m_LayerNames;
	};
}
