#pragma once
#include <filesystem>
#include <string>

namespace Helios
{
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

	/* 在 dir 下取一个不冲突的路径：被占了就依次试 "name 2"、"name 3"…
	 * 扩展名待在它该在的位置（`.png` 在序号之后）。新建文件夹与回收站落点都用它。 */
	std::filesystem::path MakeUniquePath(const std::filesystem::path& dir, const std::string& file_name);

	/* 移到回收站：落到 <trash_root>/<扁平化的相对路径>[ 序号]。
	 * 失败（源不存在 / 建不了回收站 / 移动失败）返回 false 并写原因。 */
	bool MoveAssetToTrash(const std::filesystem::path& trash_root, const std::filesystem::path& target,
		std::filesystem::path& out_trashed, std::string& out_error);

	/* 从回收站搬回 target（父目录不存在会一并建出来，撤销"删除一个文件夹"时用得上） */
	bool RestoreAssetFromTrash(const std::filesystem::path& trashed, const std::filesystem::path& target,
		std::string& out_error);
}
