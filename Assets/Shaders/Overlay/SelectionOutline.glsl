#type vertex
#version 410 core

layout(location = 0) in vec4 a_Position;
layout(location = 0) out vec2 v_TexCoord;

void main()
{
	/* 全屏三角形：z=1 即近平面（Reversed-Z），本 Pass 不测深度 */
	gl_Position = a_Position;

	v_TexCoord = a_Position.xy * 0.5f + 0.5f;
}


#type fragment
#version 410 core

layout(location = 0) out vec4 OutFragColor;
layout(location = 0) in vec2 v_TexCoord;

/* 场景的实体 id 附件（与拾取同一张），空像素为 -1 */
layout(binding = 0) uniform isampler2D u_ObjectId;
/* 选中实体 id：描边只落在它外侧的一圈 */
layout(location = 0) uniform int u_SelectedId;
layout(location = 1) uniform vec4 u_OutlineColor;
/* 描边宽度（像素），配合固定的采样半径 4 使用 */
layout(location = 2) uniform float u_Thickness;

const int kKernelRadius = 4;

void main()
{
	const ivec2 size = textureSize(u_ObjectId, 0);
	const ivec2 pixel = clamp(ivec2(v_TexCoord * vec2(size)), ivec2(0), size - ivec2(1));

	/* 选中像素自己不落笔：轮廓是它外侧的一圈 */
	if (texelFetch(u_ObjectId, pixel, 0).r == u_SelectedId)
	{
		OutFragColor = vec4(0.0f);
		return;
	}

	/* 到最近选中像素的距离：贴着的一圈实心，再往外一像素淡出，边缘不带锯齿 */
	float nearest = 1.0e6f;
	for (int y = -kKernelRadius; y <= kKernelRadius; ++y)
	{
		for (int x = -kKernelRadius; x <= kKernelRadius; ++x)
		{
			const ivec2 sample_pixel = pixel + ivec2(x, y);
			if (sample_pixel.x < 0 || sample_pixel.y < 0
				|| sample_pixel.x >= size.x || sample_pixel.y >= size.y)
				continue;
			if (texelFetch(u_ObjectId, sample_pixel, 0).r != u_SelectedId)
				continue;

			nearest = min(nearest, length(vec2(x, y)));
		}
	}

	/* 被别的物体挡住的部分本来就没有选中 id，自然不会描边 */
	const float alpha = clamp(u_Thickness + 0.5f - nearest, 0.0f, 1.0f);
	OutFragColor = vec4(u_OutlineColor.rgb, u_OutlineColor.a * alpha);
}
