#pragma once

namespace Helios
{
	/* 组件自定义编辑器：schema 画不出的那部分（资源引用、材质槽、脚本参数、烘焙详情）。
	 * 实现归编辑器，按组件类型装配进 ComponentRegistry —— 引擎只留数据与元数据。 */

	/* 装配全部自定义绘制（属性面板构造时调用一次，重复调用无副作用） */
	void InstallComponentEditors();

	void RegisterModelComponentEditor();
	void RegisterScriptComponentEditor();
	void RegisterReflectionProbeComponentEditor();
	void RegisterAnimatorComponentEditor();
	void RegisterSpriteComponentEditor();
	void RegisterPostProcessComponentEditor();
	void RegisterGrassComponentEditor();
}
