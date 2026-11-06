#pragma once
#include "Helios/Common/Export.h"

namespace Helios
{
	class RenderView;

	/* 草地提交：草不是 MeshSegment，不进可见对象列表，由各 Pass 在常规对象之后补一趟 */
	namespace GrassDraw
	{
		/* GPU 剔除：注册一个计算 Pass，按相机与各档级联的视锥把可见实例压实，
		 * 顺带写好间接参数。要排在所有渲染 Pass 之前注册 —— 计算派发会把
		 * 当前渲染编码器收掉，夹在渲染通道中间会把它们切碎 */
		HELIOS_API void AddIndirectCullPass(RenderView& view);
	}
}
