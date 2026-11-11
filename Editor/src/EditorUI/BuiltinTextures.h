#pragma once
#include <string>
#include <vector>

namespace Helios
{
	/* 引擎自带的贴图：EngineRes/Textures 下的图片。资源浏览器只列项目根，那边看不到这些图，
	 * 贴图槽靠这里的下拉把它们选进来。 */
	namespace BuiltinTextures
	{
		struct Entry
		{
			std::string Name;          /* 显示名，取文件名词干，如 White */
			std::string RelativePath;  /* 相对引擎根，如 Textures/White.png；存进材质也是这个口径 */
		};

		/* 引擎资源只读，装好就不变，第一次用到时扫一遍留着 */
		const std::vector<Entry>& List();

		/* 当前贴图是不是内置贴图。内置图不在资源浏览器里，定位过去没有落点 */
		bool Contains(const std::string& relative_path);

		/* 贴图槽用的下拉：点开是带预览缩略图和搜索框的内置贴图清单，选中返回 true 并把
		 * 相对路径写进 picked。current 是当前贴图的相对路径，空表示槽位还没有贴图，用来打勾。 */
		bool DrawPicker(const char* id, const std::string& current, std::string& picked);
	}
}
