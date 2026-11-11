#pragma once
#include <cstdint>
#include <cstddef>

#include "Helios/Common/Common.h"

namespace Helios
{
	class DeviceShader;
	class DeviceTexture;
	class DeviceVertexArray;
	class DeviceVertexBuffer;
	class IndexBuffer;

	namespace UI
	{
		class UIRenderInterface;

		/* 录制结果的落地：着色器、白图与三槽轮转缓冲都在这里。
		 * 一帧只设一次状态（预乘混合、正交投影、正交原点左上），逐条命令只换纹理与裁剪。
		 * 调用方负责渲染目标与视口，本类只往当前目标画。 */
		class UIFrameRenderer final
		{
		public:
			/* 建着色器与白图。Rml::Initialise 之后调一次 */
			void Init();
			void Shutdown();

			void Draw(const UIRenderInterface& recorder, uint32_t target_width, uint32_t target_height);

		private:
			/* 顶点 / 索引缓冲按三槽轮转：GPU 可能还有两帧在读旧槽 */
			static constexpr int kFramesInFlight = 3;

			void EnsureBufferCapacity(size_t vertex_count, size_t index_count);
			void ReleaseBuffers();

			SharedPtr<DeviceShader> m_Shader{};
			/* 无纹理几何（纯色块）统一采白图：省掉着色器里分两套分支 */
			SharedPtr<DeviceTexture> m_WhiteTexture{};

			SharedPtr<DeviceVertexBuffer> m_VertexBuffers[kFramesInFlight]{};
			SharedPtr<IndexBuffer> m_IndexBuffers[kFramesInFlight]{};
			SharedPtr<DeviceVertexArray> m_VertexArrays[kFramesInFlight]{};
			size_t m_VertexCapacity{ 0 };
			size_t m_IndexCapacity{ 0 };
			int m_FrameSlot{ 0 };
		};
	}
}
