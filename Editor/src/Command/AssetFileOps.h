#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace Helios
{
	/* 资源文件的大类：决定资源浏览器里画哪枚图标、"按类型筛选"能筛出什么。
	 * 枚举值顺序 = 筛选下拉里的顺序（面板那边的 TypeFilter 除 All 外与它一一对应）。 */
	enum class AssetFileKind : uint8_t
	{
		Other = 0,   /* 认不出的后缀：通用文件图标，只在「All types」下出现 */
		Image,       /* .png / .jpg / .jpeg / .bmp / .tga / .psd / .gif / .hdr / .dds */
		Scene,       /* .scn */
		MtlGraph,    /* .mtlgraph */
		Shader,      /* .glsl（源） / .metal（Metal 目标，编译产物） */
		Model,       /* .obj（引擎自己导的） / .mesh（网格缓存） / .fbx / .dae / .stl */
		Material,    /* .mtl（材质资产：独立单条目 / 模型伴生的槽表） */
		Probe,       /* .probe（反射探针的烘焙缓存：属性面板里直接预览烘焙结果） */
	};

	/* 后缀 -> 大类。extension 形如 ".glsl"，大小写不敏感（内部转大写再比）。
	 * 后缀与大类的对应关系只在这一处（表在 .cpp 里）：加一种资源类型 = 枚举加一项 + 表里加几行，
	 * 图标与类型筛选都跟着走，不必各自再列一遍后缀。 */
	AssetFileKind AssetFileKindOf(const std::string& extension);

	/* 资源浏览器里"当前选中"的一项（跨面板摘要：资源浏览器 → 属性面板）。
	 * 只带展示所需的字段，不暴露浏览器内部的节点结构 —— 面板之间只交换值；
	 * 路径与浏览器的 FilePath 同口径（相对 Assets）。 */
	struct AssetSelectionEntry
	{
		std::string   Name;                          /* 文件 / 文件夹名 */
		std::string   Path;                          /* 相对 Assets 的路径 */
		AssetFileKind Kind{ AssetFileKind::Other };  /* 文件大类；文件夹看 IsFolder（不走它） */
		bool          IsFolder{ false };
		uintmax_t     SizeBytes{ 0 };                /* 单个文件的字节数；文件夹不累计（0） */
		size_t        ChildCount{ 0 };               /* 文件夹直接子项数；文件为 0 */
	};

	/* 资源改动（新建 / 重命名 / 删除，含撤销重做）的观察者：文件命令只管改磁盘，界面靠回调同步
	 * "选中项 / 当前目录"（不然撤销一次重命名，选中项会先凭空消失）。传的是相对 Assets 的路径：
	 * from 空 = 新建，to 空 = 删除，都有 = 改名 / 移动。 */
	class IAssetChangeSink
	{
	public:
		virtual ~IAssetChangeSink() = default;

		virtual void OnAssetPathChanged(const std::string& from, const std::string& to) = 0;
	};

	/* 删除资源的去处：项目根的 `.helios-trash`（Assets 的兄弟目录）。不用系统回收站（撤销要精确
	 * 搬回，而 Windows 回收站会把文件改名成 $R… 拿不到落点，三平台行为还不一致）；项目内回收站
	 * 撤销精确、跨平台一致、文件也能手工找回。 */
	std::filesystem::path AssetTrashRoot();

	/* 绝对路径 → 相对 Assets 的路径（不在 Assets 内就原样返回），与面板的 FilePath 同口径 */
	std::string AssetRelativePath(const std::filesystem::path& path);

	/* 校验用户输入的资源名：合法返回空串，否则返回给人看的原因 */
	std::string AssetNameError(const std::string& name);

	/* 拖拽移动的裁决：把 from 移进 target_dir 可不可行。合法返回空串，否则返回原因（跟
	 * AssetNameError 同一契约）。只判"能不能"：源存在、目标是目录、不是原位、不会自嵌套成环、
	 * 不覆盖同名项。 */
	std::string AssetMoveError(const std::filesystem::path& from, const std::filesystem::path& target_dir);

/* 在 dir 下取一个不冲突的路径：被占了就依次试 "name 2"、"name 3"…
 * 扩展名待在它该在的位置（`.png` 在序号之后）。新建文件夹与回收站落点都用它。 */
std::filesystem::path MakeUniquePath(const std::filesystem::path& dir, const std::string& file_name);

/* 新建资源文件的初始内容：按扩展名给一份最小可用模板（认不出的就给空文件）。模板是
 * "能被现有加载器读进去"的最简形态（空场景 / 空材质图）。extension 形如 ".scn"，空串 = 无扩展名。 */
std::string AssetFileTemplate(const std::string& extension);

	/* 移到回收站：落到 <trash_root>/<扁平化的相对路径>[ 序号]。
	 * 失败（源不存在 / 建不了回收站 / 移动失败）返回 false 并写原因。 */
	bool MoveAssetToTrash(const std::filesystem::path& trash_root, const std::filesystem::path& target,
		std::filesystem::path& out_trashed, std::string& out_error);

	/* 从回收站搬回 target（父目录不存在会一并建出来，撤销"删除一个文件夹"时用得上） */
	bool RestoreAssetFromTrash(const std::filesystem::path& trashed, const std::filesystem::path& target,
		std::string& out_error);
}
