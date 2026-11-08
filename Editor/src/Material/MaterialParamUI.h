#pragma once
/* 材质参数编辑的共享控件：Shader 选择行 + 按 Shader 反射列出的参数行。
 * 属性面板的材质卡（实体槽位 / .mtl 资产）与模型编辑器共用这一份 —— 两处的行控件、
 * 反射口径、Shader 菜单都保持同一种语言。改动只落调用方给的 write_back / 材质本身。 */

#include <any>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Helios/Scene/Material.h"
#include "Helios/Scene/SceneCommon.h"

namespace Helios::MaterialParamUI
{
	/* 材质没有 Shader 时按钮上的占位文案（菜单照常能开，选一个即可补上） */
	inline constexpr const char* kNoShaderOption = "(no shader)";

	/* 反射参数 → 显示行：材质还没有这条时用它撑起一行 ——
	 * 默认值取 Shader 源码声明的（如 u_AlbedoTilingOffset = vec4(1,1,0,0)），
	 * 没有就按类型给零值；贴图没有"默认"，显示成空槽（拖入才落材质）。 */
	MaterialParamInfo MakeDefaultMaterialParamRow(const ReflectedMaterialParam& reflected);

	/* 一行 = 当前值 + 这条属性的面板规范（显示名 / 控件类型 / 范围 / 条件） */
	struct MaterialParamRow
	{
		MaterialParamInfo Param;
		MaterialPropertySpec Spec;
	};

	/* 一条 @property 声明的预处理结果：名字的 ID、缺省行、@showif 依赖都提前算好 */
	struct PropertyLayoutEntry
	{
		const ReflectedMaterialParam* Reflected{ nullptr };
		uint32_t ParamId{ 0 };
		MaterialParamInfo DefaultRow;
		uint32_t ShowIfId{ 0 };
		const ReflectedMaterialParam* ShowIfDeclared{ nullptr };
	};

	/* Shader 的属性清单（只含 @property，按名字排好）：按 Shader 实例 + 热重载版本缓存，
	 * 面板每帧取行时不再过滤反射、算 ID、排序。只在 UI 线程用。 */
	const std::vector<PropertyLayoutEntry>& PropertyLayoutOf(const SharedPtr<DeviceShader>& shader);

	/* @showif 求值：拿材质里的当前值比（没有就用反射默认值）。开关不是 int / float、
	 * 或者那个参数压根没声明时按可见处理 —— 别把行藏没了。 */
	bool IsLayoutEntryVisible(const Material& material, const PropertyLayoutEntry& entry);

	/* 材质参数行的显示清单：Shader 反射声明里标了 @property 的才列（反射是参数集合的单一来源；
	 * 没标的是引擎逐绘制给的值或库内部声明），再按 @showif 过滤掉当前不该露面的；值优先取材质里
	 * 已有的，没有的就显示反射默认值。反射不到就返回空、不回退旧参数表（让调用方画报错）。 */
	std::vector<MaterialParamRow> BuildMaterialParamRows(const Material& material);

	using ParamWriter = std::function<void(ParamType, const std::string&, const std::any&)>;
	/* 同名分组合并为一个折叠标题，未分组参数保持原行布局。 */
	void DrawMaterialParams(const Material& material, const std::function<const Material*()>& resolve_live,
		const std::function<void(const std::string&)>& reveal_asset, const ParamWriter& write_back);

	/* 一组参数 / 字段的折叠标题（属性面板的材质卡、组件卡与地形面板共用一套版式）：
	 * 返回 true = 展开，调用方紧接着画组内内容。suffix 是标题后的补充说明（弱化色，
	 * 如「3 slots, 1 instance」），放不下时按行宽裁剪。 */
	bool BeginPropertyGroup(const char* id, const char* title, bool default_open = true,
		const char* suffix = nullptr);

	/* 参数行空时的提示行：没 Shader 或反射拿不到声明（后端不支持 / 声明识别不出）用 Danger；
	 * 扫过源码但没有一条标 @property（材质本来就没东西可改，天空吃 uniform 块就是这种）弱化提示。 */
	void DrawMaterialReflectionNotice(const Material& material);

	/* Shader 菜单树：按目录分层的节点（文件夹 = Children 非空；文件 = Path 为完整相对路径） */
	struct ShaderMenuNode
	{
		std::string Name;                      /* 显示名（文件 / 文件夹名） */
		std::string Path;                      /* 文件 = 完整相对路径；文件夹 = 空 */
		std::vector<ShaderMenuNode> Children;  /* 文件夹的子项 */

		[[nodiscard]] bool IsFolder() const { return Path.empty(); }
	};

	/* 把"可选 Shader 的相对路径清单"装成一棵按目录分层的菜单树（同名文件夹归并）。
	 * 清单本身是字典序：同一层级里文件夹与文件各自保持排好的顺序 ——
	 * 画的时候文件夹先、文件后（见 DrawShaderMenuLevel）。 */
	std::vector<ShaderMenuNode> BuildShaderMenuTree(const std::vector<std::string>& options);

	/* 画一层 Shader 菜单：文件夹先（悬停展开子菜单）、文件后（点一条 = 选中）。
	 * 当前 Shader 打一枚对勾（Accent，行右缘）；选中后把整条菜单链收起。 */
	void DrawShaderMenuLevel(const std::vector<ShaderMenuNode>& nodes, const std::string& current,
		std::string& out_picked);

	/* Shader 选择行（标签 + 下拉按钮 + 按目录分组的选择菜单）：
	 * 按钮上显示当前 Shader 的文件名、右缘一枚朝下的箭头；悬停看全路径；
	 * 当前 Shader 不在清单里（资产外路径之类）时树顶补一条。
	 * 返回 true = 选中了新的 Shader（已 SetShader，调用方按需热更）。 */
	std::string DrawMaterialShaderPicker(const Material& material);

	/* 直接改材质的 Shader：资产编辑缓冲这类"就地写"的场合用 */
	bool DrawMaterialShaderRow(Material& material);

	/* 当前参数驱动的变体：只列非 0 维度，全默认显示 (default)；没有参数维度时不画 */
	void DrawMaterialVariantRow(const Material& material);

	/* 渲染队列行：小于 2500 走不透明批次，2500 起走透明批次（与引擎的判据同源）。
	 * 返回 true = 有改动（新值写回传入的引用）。 */
	bool DrawMaterialRenderQueueRow(int32_t& render_queue);

	/* 一条材质参数行：值画在临时缓冲上，改动经 write_back 落进目标材质（槽位 → 自动实例化
	 * 写回；资产编辑缓冲 / 模型编辑器 → 直接写）。行对齐：标签跟 frame 高控件垂直居中。
	 * 贴图行画缩略图加一枚下拉（点击定位、拖入替换、悬停看大图、下拉选引擎内置贴图）。
	 * spec 决定标签文案与控件形态（@property 显示名 / @type / @range）：标了 @type(color) 的
	 * vec3 / vec4 画取色器 —— 色块点开是取色盘，右边照旧能填分量（float 显示，0 到 1）。 */
	void DrawMaterialParamRow(const MaterialParamInfo& param_info, const MaterialPropertySpec& spec,
		const std::function<void(const std::string&)>& reveal_asset,
		const std::function<void(ParamType, const std::string&, const std::any&)>& write_back);
}
