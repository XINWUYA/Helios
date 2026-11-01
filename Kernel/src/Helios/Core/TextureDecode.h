#pragma once
#include <string>
#include <vector>
#include "Helios/Common/Export.h"

namespace Helios
{
	/* 解码完的贴图原始像素：后端只管建纹理和上传，格式只看 IsFloat */
	struct HELIOS_API DecodedImage
	{
		void* Pixels = nullptr; /* stb 分配的缓冲，析构时释放 */
		int Width = 0;
		int Height = 0;
		bool IsFloat = false;   /* HDR：像素是 float 的 RGBA */

		DecodedImage() = default;
		~DecodedImage();
		DecodedImage(DecodedImage&& other) noexcept;
		DecodedImage& operator=(DecodedImage&& other) noexcept;
		DecodedImage(const DecodedImage&) = delete;
		DecodedImage& operator=(const DecodedImage&) = delete;

		[[nodiscard]] bool IsValid() const { return Pixels != nullptr; }
	};

	/* 贴图预解码：材质加载前把路径提交进来，多张图在后台同时解，加载时直接取。
	 * 解码不碰图形 API，翻转走 stb 的线程局部开关，不会打扰主线程。 */
	namespace TextureDecode
	{
		/* 提交预解码；同一路径重复提交只解一次 */
		HELIOS_API void Prefetch(const std::string& path);
		HELIOS_API void Prefetch(const std::vector<std::string>& paths);

		/* 读材质文件里的贴图参数再提交：要在材质真正加载之前调，才并得起来 */
		HELIOS_API void PrefetchMaterialTextures(const std::string& material_asset_path);

		/* 取结果：后台还在解就等它；没提交过返回 false，调用方自己 Decode */
		HELIOS_API bool Take(const std::string& path, DecodedImage& out);

		/* 同步解码，HDR / 翻转按贴图设置解析 */
		HELIOS_API bool Decode(const std::string& path, DecodedImage& out);

		/* 换工程或退出：丢掉还在飞的结果 */
		HELIOS_API void Clear();
	}
}
