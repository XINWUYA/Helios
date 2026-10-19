#pragma once

namespace Helios
{
	/* 内置组件注册清单：Builtin/ 下每个文件负责一个组件的全部注册内容（字段表、自定义绘制、
	 * 序列化补充、可添加形态）。新增内置组件 = 新建一个注册文件，再到清单补一行。
	 * 注册顺序决定 Inspector 卡片顺序、Add 菜单顺序与场景元素写出顺序。 */

	void RegisterCoreComponents();           /* Name / Transform / Visibility */
	void RegisterLayerComponent();           /* Layer */
	void RegisterSpriteComponent();
	void RegisterCameraComponent();
	void RegisterModelComponent();
	void RegisterLightComponent();
	void RegisterReflectionProbeComponent();
	void RegisterAnimatorComponent();
	void RegisterScriptComponent();
	void RegisterPostProcessComponents();    /* PostProcess / Bloom */

	/* 按清单顺序注册全部内置组件（由 ComponentRegistry 启动时调用一次） */
	void RegisterBuiltinComponents();
}
