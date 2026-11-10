#ifndef SHADING_MODEL_GLSL
#define SHADING_MODEL_GLSL

/* G-Buffer 里的着色模型编号：决定 Texture3 那张自定义数据怎么解释。
 * 编号写进 8 位通道，0 与 1 之间留足间隔，读取时按半档判断。 */
#define SHADING_MODEL_STANDARD 0.0
#define SHADING_MODEL_FOLIAGE  1.0

bool ShadingModelIs(float value, float expected)
{
	return abs(value - expected) < 0.5f;
}

#endif // SHADING_MODEL_GLSL
