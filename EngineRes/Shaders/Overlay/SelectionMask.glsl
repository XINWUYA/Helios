#type vertex
#version 410 core

layout(location = 0) in vec4 a_Position;
out vec2 v_TexCoord;

void main()
{
	/* 全屏三角形：z=1 即近平面（Reversed-Z），本 Pass 不测深度 */
	gl_Position = a_Position;

	v_TexCoord = a_Position.xy * 0.5f + 0.5f;
}


#type fragment
#version 410 core

layout(location = 0) out vec4 OutFragColor;
in vec2 v_TexCoord;

/* 场景的实体 id 附件（与拾取、描边同一张），空像素为 -1 */
uniform sampler2D u_ObjectId;
/* 选中实体 id */
uniform int u_SelectedId;
/* 一块掩码覆盖的边长（像素），由 SelectionOutlinePass 给 */
uniform int u_MaskTile;

/* 粗筛：把实体 id 附件按 tile 归约成一张低分辨率掩码，标出"这块里有选中像素"。
 * 描边只在掩码命中的 tile 附近精算，其余像素查一下就能退出，省掉全屏的 9x9 邻域。 */
void main()
{
	const ivec2 object_size = textureSize(u_ObjectId, 0);
	const ivec2 origin = ivec2(gl_FragCoord.xy) * u_MaskTile;

	for (int y = 0; y < u_MaskTile; ++y)
	{
		for (int x = 0; x < u_MaskTile; ++x)
		{
			const ivec2 pixel = origin + ivec2(x, y);
			if (pixel.x >= object_size.x || pixel.y >= object_size.y)
				continue;
			if (int(floor(texelFetch(u_ObjectId, pixel, 0).r + 0.5f)) == u_SelectedId)
			{
				OutFragColor = vec4(1.0f);
				return;
			}
		}
	}

	OutFragColor = vec4(0.0f);
}
