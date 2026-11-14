#ifndef SHADING_MODEL_GLSL
#define SHADING_MODEL_GLSL

/* G-Buffer 里的着色模型编号：决定 Texture3 那张自定义数据怎么解释。
 * 0 留给没写 G-Buffer 的像素（清屏值），8 位通道按整数存，最多 255 种 */
#define SHADING_MODEL_NONE     0
#define SHADING_MODEL_STANDARD 1
#define SHADING_MODEL_FOLIAGE  2
#define SHADING_MODEL_UNLIT    3

float EncodeShadingModel(int id)
{
	return float(id) / 255.0f;
}

int DecodeShadingModel(float value)
{
	return int(round(value * 255.0f));
}

#endif // SHADING_MODEL_GLSL
