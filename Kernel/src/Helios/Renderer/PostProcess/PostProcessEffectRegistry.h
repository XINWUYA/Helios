#pragma once
#include <cstdint>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include "Helios/Renderer/FrameGraph/FrameGraphResourceHandle.h"

namespace Helios
{
	class Entity;
	class RenderView;
	struct FrameGraphTexture;

	/* 后处理框架（效果注册表驱动）：
	 * 新增一个效果 = 效果自己的文件（设置 / 组件 / Build / 自含登记）+ 注册清单一行；
	 * 框架流程（入口 / 场景解析 / 视图状态）对效果零感知 —— 见《Bloom后处理方案》第三节。 */
	namespace PostProcess
	{
		/* 效果链上的相位：先按 Phase、同相位按注册序（入口遍历顺序即链序） */
		enum class Phase : uint8_t
		{
			Scene = 0,       /* 场景色修饰（Bloom 等）：天空之后、其它后处理之前 */
			ToneMapping = 1, /* 色调映射（HDR 二期） */
			Overlay = 2,     /* LDR 叠加（Vignette / 颗粒等） */
		};

		/* 每帧解析出的设置快照：按设置类型存取（type-erased，沿用 ComponentSnapshot 的模式）。
		 * 解析期由各效果的 Capture 写入；渲染期由各效果的 Build 读取 —— 两侧都只认自己的设置类型。 */
		class PostProcessState
		{
		public:
			template <typename T>
			void Set(const T& value) { m_Values[std::type_index(typeid(T))] = std::make_shared<T>(value); }

			template <typename T>
			[[nodiscard]] const T* Get() const
			{
				const auto it = m_Values.find(std::type_index(typeid(T)));
				return (it != m_Values.end()) ? static_cast<const T*>(it->second.get()) : nullptr;
			}

			[[nodiscard]] bool IsEmpty() const { return m_Values.empty(); }

		private:
			std::unordered_map<std::type_index, std::shared_ptr<void>> m_Values;
		};

		/* 效果注册项 */
		struct EffectDesc
		{
			const char* Name{ nullptr };  /* 注册名（调试 / 断言用） */
			Phase Stage{};                /* 链上相位 */
			/* 从「后处理对象」收集本效果的设置进快照（实体没挂该组件 = 不写入） */
			void (*Capture)(const Entity& owner, PostProcessState& state){ nullptr };
			/* 该效果在快照里是否会真的生效（默认目标路径据此决定"旁路到纹理"与否；
			 * 未提供视为不生效 —— 新效果请一并给出，否则默认目标路径上不会被触发） */
			bool (*IsEnabled)(const PostProcessState& state){ nullptr };
			/* 链的一步：把本效果的 Pass 加进渲染图；未启用原样返回 input */
			FrameGraphResourceHandleTyped<FrameGraphTexture> (*Build)(
				RenderView& view, FrameGraphResourceHandleTyped<FrameGraphTexture> input){ nullptr };
		};

		/* 效果注册表：登记时维护有序序列（先 Phase、同 Phase 注册序） */
		class PostProcessEffectRegistry
		{
		public:
			static PostProcessEffectRegistry& Instance();

			void Add(EffectDesc desc);
			[[nodiscard]] const std::vector<EffectDesc>& All() const { return m_Effects; }

		private:
			PostProcessEffectRegistry() = default;

			std::vector<EffectDesc> m_Effects;
		};
	}
}
