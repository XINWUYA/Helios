#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include "Helios/VirtualDevice/DeviceTexture.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "LayerMask.h"
#include "Camera.h"
#include "Mesh.h"
#include "Model.h"
#include "SkeletonModel.h"
#include "Light.h"
#include "ReflectionProbe.h"
#include "AnimationClip.h"
#include "GrassField.h"
#include "TerrainField.h"

namespace Helios
{
	class Scene;
	class Entity;
	class Script;

	/* 组件基类 */
	struct HELIOS_API ComponentBase
	{
		virtual ~ComponentBase() = default;

		virtual void OnAdded(const Scene& scene, Entity& entity) {}
		virtual void OnRemoved(const Scene& scene, Entity& entity) {}
	};

	/* 实体名称组件 */
	struct NameComponent : ComponentBase
	{
		std::string m_Name;

		NameComponent() = default;
		NameComponent(const NameComponent&) = default;
		NameComponent(std::string name)
			: m_Name(std::move(name))
		{
		}
	};

	/* 空间变换组件 */
	struct TransformComponent : ComponentBase
	{
		glm::vec3 m_Position{ 0.0f, 0.0f, 0.0f };
		glm::vec3 m_Rotation{ 0.0f, 0.0f, 0.0f };
		glm::vec3 m_Scale{ 1.0f, 1.0f, 1.0f };

		TransformComponent() = default;
		TransformComponent(const TransformComponent&) = default;
		TransformComponent(const glm::vec3& position)
			: m_Position(position)
		{
		}

		glm::mat4 GetTransform() const
		{
			const glm::mat4 rotation_mat = glm::toMat4(glm::quat(m_Rotation));
			return glm::translate(glm::mat4(1.0f), m_Position)
				* rotation_mat
				* glm::scale(glm::mat4(1.0f), m_Scale);
		}
	};

	/* 父子层级：只有子实体持有。父子关系只有这一处数据源，子节点列表按需从注册表遍历出来
	 * （不额外维护）；根节点不挂。Transform 存相对父节点的局部变换，世界变换沿父链累积，
	 * 读写一律走 Scene 的层级接口。 */
	struct ParentComponent : ComponentBase
	{
		entt::entity m_Parent{ entt::null };

		ParentComponent() = default;
		ParentComponent(const ParentComponent&) = default;
		ParentComponent(entt::entity parent)
			: m_Parent(parent)
		{
		}
	};

	/* 可见性组件：实体自身的显隐开关（层级行右端的眼睛）。「隐藏」= 挂组件且 m_Visible == false
	 * （缺席 = 可见）；沿父链传染，判定收口在 Scene::IsEntityVisible。隐藏的模型不进可见列表、
	 * 光源不参与光照、探针不烘焙；相机不受影响。 */
	struct VisibilityComponent : ComponentBase
	{
		bool m_Visible{ true };

		VisibilityComponent() = default;
		VisibilityComponent(const VisibilityComponent&) = default;
		VisibilityComponent(bool visible)
			: m_Visible(visible)
		{
		}
	};

	/* 渲染层组件：对象属于哪些层（位掩码）。相机按 CullingMask 取交集决定画不画，
	 * 缺席 = 只在默认层。层只筛渲染收集（含阴影投射），不影响光照与层级。 */
	struct LayerComponent : ComponentBase
	{
		LayerMask m_Mask{ Layers::Default };

		LayerComponent() = default;
		LayerComponent(const LayerComponent&) = default;
		LayerComponent(LayerMask mask)
			: m_Mask(mask)
		{
		}
	};

	/* 场景相机组件 */
	struct CameraComponent : ComponentBase
	{
		SharedPtr<Camera> m_Camera{ nullptr };
		bool m_IsPrimary{ true };
		bool m_IsFixedAspectRatio{ false };

		CameraComponent()
		{
			m_Camera = CreateSharedPtr<Camera>();
		}
		CameraComponent(const CameraComponent&) = default;
		CameraComponent(const SharedPtr<Camera>& camera)
		{
			if (camera)
				m_Camera = camera;
			else
				m_Camera = CreateSharedPtr<Camera>();

		}

	};

	/* 图片精灵组件 */
	struct SpriteComponent : ComponentBase
	{
		SharedPtr<DeviceTexture> m_Texture{ nullptr };
		glm::vec4 m_BaseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
		float m_TilingFactor{ 1.0f };

		SpriteComponent() = default;
		SpriteComponent(const SpriteComponent&) = default;
		SpriteComponent(const glm::vec4& basecolor)
			: m_BaseColor(basecolor)
		{
		}
	};

	/* 材质槽的实体级绑定（三态）：条目不存在 = 用模型槽表的默认；IsInstance=false = 覆盖
	 * （挂另一份材质资产，共享）；IsInstance=true = 实例（克隆出来的实体级独立材质）。
	 * 解析链见 ResolveSlotMaterial。 */
	struct MaterialSlotOverride
	{
		SharedPtr<Material> pMaterial; /* 覆盖（共享资产）或实例（独立） */
		std::string SourceAssetPath;   /* 来源材质文件（"重置回来源"用；空 = 无来源） */
		bool IsInstance{ false };      /* 实例：参数独立、序列化全量内嵌 */
	};

	/* 单个材质槽的值语义：实例在拷贝时深拷贝一份 —— 组件撤销快照靠这条把改过的参数
	 * 一起存 / 还原；引用形态是共享资产，保持浅拷贝。模型槽是多槽表，深拷贝写在
	 * ModelComponent 的拷贝构造里，两者口径一致。 */
	struct InstanceMaterialSlot : MaterialSlotOverride
	{
		InstanceMaterialSlot() = default;
		InstanceMaterialSlot(const InstanceMaterialSlot& other)
			: MaterialSlotOverride(other)
		{
			if (IsInstance && pMaterial != nullptr)
				pMaterial = pMaterial->Clone();
		}
		InstanceMaterialSlot& operator=(const InstanceMaterialSlot& other)
		{
			if (this == &other)
				return *this;

			MaterialSlotOverride copy(other);
			if (copy.IsInstance && copy.pMaterial != nullptr)
				copy.pMaterial = copy.pMaterial->Clone();
			MaterialSlotOverride::operator=(std::move(copy));
			return *this;
		}
	};

	/* 模型组件 */
	struct ModelComponent : ComponentBase
	{
		SharedPtr<Model> m_Model{ nullptr };
		/* 槽名 → 实体级绑定（"覆盖"与"实例"，见 MaterialSlotOverride） */
		std::unordered_map<std::string, MaterialSlotOverride> m_SlotOverrides;
		/* 投影：开启后本模型写入阴影图（见 ShadowMap 的投射体提交）；
		 * 接受阴影：关闭后本模型不采样阴影（见 GBuffer / 前向着色器的 u_ReceiveShadow）。 */
		bool m_CastShadow{ true };
		bool m_ReceiveShadow{ true };

		/* 骨骼蒙皮开关（存盘）：关掉就按绑定姿态画静态网格。
		 * 模型本身是 SkeletonModel 时才有意义（模型从 Model::Create 出来，按 .mesh 有没有骨骼段定类型） */
		bool m_EnableSkinning{ true };

		ModelComponent() = default;

		/* 值语义：拷贝组件时，实例材质必须深拷贝 ——
		 * 组件的撤销快照（Capture/Restore）靠这条把实例参数一起保存 / 还原；
		 * "覆盖 / 默认"是共享资产，SharedPtr 浅拷贝保持共享。 */
		ModelComponent(const ModelComponent& other)
			: ComponentBase(other), m_Model(other.m_Model), m_SlotOverrides(other.m_SlotOverrides),
			  m_CastShadow(other.m_CastShadow), m_ReceiveShadow(other.m_ReceiveShadow),
			  m_EnableSkinning(other.m_EnableSkinning)
		{
			for (auto& entry : m_SlotOverrides)
			{
				auto& slot_override = entry.second;
				if (slot_override.IsInstance && slot_override.pMaterial != nullptr)
					slot_override.pMaterial = slot_override.pMaterial->Clone();
			}
		}

		ModelComponent& operator=(const ModelComponent& other)
		{
			if (this != &other)
			{
				ModelComponent copy(other); /* 走深拷贝构造 */
				std::swap(m_Model, copy.m_Model);
				std::swap(m_SlotOverrides, copy.m_SlotOverrides);
				std::swap(m_CastShadow, copy.m_CastShadow);
				std::swap(m_ReceiveShadow, copy.m_ReceiveShadow);
				std::swap(m_EnableSkinning, copy.m_EnableSkinning);
			}
			return *this;
		}
	};

	/* 解析一个 MeshSegment 的最终材质（全引擎唯一入口）：
	 * 实体绑定（覆盖 / 实例） ?: 模型槽表默认 ?: 内置白模 */
	inline SharedPtr<Material> ResolveSlotMaterial(
		const Model& model, int slot_index,
		const std::unordered_map<std::string, MaterialSlotOverride>* slot_overrides)
	{
		const MaterialSlot* slot = model.GetSlotByIndex(slot_index);
		if (slot == nullptr)
			return Material::BuiltinWhite();

		if (slot_overrides != nullptr)
		{
			const auto iter = slot_overrides->find(slot->Name);
			if (iter != slot_overrides->end() && iter->second.pMaterial != nullptr)
				return iter->second.pMaterial;
		}

		return (slot->pDefault != nullptr) ? slot->pDefault : Material::BuiltinWhite();
	}

	/* 光源组件 */
	struct LightComponent : ComponentBase
	{
		SharedPtr<Light> m_Light{ nullptr };
		LightType m_Type{ LightType::Point };

		LightComponent() = default;
		LightComponent(const LightComponent&) = default;
		LightComponent(LightType type)
			: m_Type(type)
		{
			m_Light = Light::Create(type);
		}
	};

	/* 反射探针组件 */
	struct HELIOS_API ReflectionProbeComponent : ComponentBase
	{
		SharedPtr<ReflectionProbe> m_ReflectionProbe;

		ReflectionProbeComponent()
		{
			m_ReflectionProbe = CreateSharedPtr<ReflectionProbe>();
		}
		ReflectionProbeComponent(const ReflectionProbeComponent&) = default;

		/* 加入场景时注册到反射探针管理器 */
		void OnAdded(const Scene& scene, Entity& entity) override;
		/* 移除出场景时从反射探针管理器注销 */
		void OnRemoved(const Scene& scene, Entity& entity) override;
	};

	/* 动画播放器：Clip 是资产引用，其余是作者数据；Playing / Time 只在运行期有意义
	 * （不序列化、不进字段表）。Loop 是实例级开关，覆盖 clip 文件里的默认值。 */
	struct AnimatorComponent : ComponentBase
	{
		/* 播放速度的编辑范围（属性卡与模型编辑器的动画卡共用一份口径） */
		static constexpr float kSpeedMin = 0.0f;
		static constexpr float kSpeedMax = 10.0f;
		static constexpr float kSpeedStep = 0.01f;

		SharedPtr<AnimationClip> m_Clip{ nullptr };
		float m_Speed{ 1.0f };
		float m_Time{ 0.0f };
		bool m_Loop{ true };
		bool m_AutoPlay{ false };
		bool m_Playing{ false };
		bool m_Scrubbing{ false };  /* 拖播放头：定格在 m_Time，不推进 */

		/* 过渡请求：Play() 只写这两个字段，运行时接住后开两层混合；运行期状态，不存盘 */
		SharedPtr<AnimationClip> m_PendingClip{ nullptr };
		float m_PendingFade{ 0.0f };

		/* 切到新片段：fade_seconds > 0 时旧片段按权重淡出，两层每帧混合 */
		void Play(SharedPtr<AnimationClip> clip, float fade_seconds = 0.0f)
		{
			m_PendingClip = std::move(clip);
			m_PendingFade = (fade_seconds > 0.0f) ? fade_seconds : 0.0f;
		}

		AnimatorComponent() = default;
		AnimatorComponent(const AnimatorComponent&) = default;
		AnimatorComponent& operator=(const AnimatorComponent&) = default;
	};

	/* 后处理对象（场景级总开关）：挂在实体上表示「这是后处理配置对象」——
	 * 各效果的设置组件（BloomComponent 等）挂在同一实体上，由 Scene 每帧解析分发；
	 * 总开关关闭 = 整个对象不参与解析（效果设置原样保留，与实体隐藏同源）。 */
	struct PostProcessComponent : ComponentBase
	{
		bool m_Enabled{ true };

		PostProcessComponent() = default;
		PostProcessComponent(const PostProcessComponent&) = default;
	};

	/* 属性面板里组件卡的显示顺序（编辑器排版用）：存组件名，没记到的按注册顺序排在后面。
	 * 不注册、不上属性面板，随场景一起存（见 Scene::Serializer）。 */
	struct ComponentOrderComponent : ComponentBase
	{
		std::vector<std::string> m_Components;

		ComponentOrderComponent() = default;
		ComponentOrderComponent(std::vector<std::string> components)
			: m_Components(std::move(components))
		{
		}
	};

	/* 脚本参数：名字、类型、文本值。编辑器只存文本，类缺失时原样保留，
	 * 等脚本库补上（或字段类型改了）再按字段表重新解释。 */
	struct ScriptProperty
	{
		std::string Name;
		FieldType   Type{ FieldType::Float };
		std::string Value;
		/* 枚举的可选名字：面板画下拉用，不序列化（类缺失时为空，退回按数字编辑） */
		std::vector<std::string> Options;

		ScriptProperty() = default;
		ScriptProperty(std::string name, FieldType type, std::string value)
			: Name(std::move(name)), Type(type), Value(std::move(value))
		{
		}
	};

	/* 一个脚本挂载点：类名 + 参数，实例只在 Runtime 模式下存在 */
	struct HELIOS_API ScriptSlot
	{
		std::string                 ClassName;
		std::vector<ScriptProperty> Properties;
		/* 全局执行顺序：小的先跑，同值按实体与挂载顺序。只影响 tick 顺序，不影响 OnCreate 之外的东西 */
		int                         m_Order{ 0 };
		Script*                     Instance{ nullptr };

		ScriptSlot() = default;
		ScriptSlot(const ScriptSlot&) = default;
		ScriptSlot& operator=(const ScriptSlot&) = default;
	};

	/* 脚本组件：一个实体可以挂多个脚本，按添加顺序执行。
	 * 只记类名与参数，实例进 Runtime 才建，退出就销毁。 */
	struct HELIOS_API ScriptComponent : ComponentBase
	{
		std::vector<ScriptSlot> m_Scripts;

		/* 面板画实体引用时要列场景里的实体，进场景时把所属场景记下来。
		 * 不序列化、不参与比较：同场景内快照拷贝拿到的仍是同一个指针。 */
		const Scene* m_pOwnerScene{ nullptr };

		ScriptComponent() = default;
		ScriptComponent(const ScriptComponent&) = default;
		ScriptComponent& operator=(const ScriptComponent&) = default;

		void OnAdded(const Scene& scene, Entity& entity) override { m_pOwnerScene = &scene; }
	};

	/* 草地组件：实体局部空间的一块区域，叶片按密度程序化摆放。
	 * 外观（颜色、透射、风参数默认值）由材质决定，这里只管范围、密度与这一片的风。
	 * 实例数据挂在 m_Field 上，场景每帧按变换与参数是否变化决定要不要重建。 */
	struct HELIOS_API GrassComponent : ComponentBase
	{
		glm::vec2 m_Extent{ 12.0f, 12.0f };      /* 局部空间的半宽半长（米） */
		float m_Density{ 55.0f };                /* 每平米株数（近档） */
		float m_Height{ 0.5f };
		float m_Width{ 0.045f };
		/* 同一区域共享朝向与高宽趋势；0 关闭，不改变实例与剔除块。 */
		float m_ClumpSize{ 1.5f };
		float m_ClumpStrength{ 0.0f };

		/* 距离抽稀：StartCullDistance 以内满密度，之后逐株变稀，EndCullDistance 处归零。
		 * 名字跟 UE 的 Grass Variety 一致，好对照 */
		float m_StartCullDistance{ 25.0f };
		float m_EndCullDistance{ 90.0f };
		float m_MinDensityScale{ 0.12f };   /* 远端密度下限，改曲线形状用的，不是性能旋钮 */

		/* 风场：风向是水平方向，强度是叶尖的最大偏移（米） */
		glm::vec2 m_WindDirection{ 1.0f, 0.35f };
		float m_WindStrength{ 0.12f };
		float m_WindSpeed{ 1.6f };
		float m_WindSpatialScale{ 0.25f };
		/* 超过这个距离不再算风场与压弯（省顶点），0 是始终算 */
		float m_WindCullDistance{ 0.0f };

		bool m_ReceiveShadow{ true };
		/* 关闭后不投草影；最远距离由阴影范围或 ShadowFadeDistance 限制。 */
		bool m_CastShadow{ true };
		/* 草影最远距离：0 = 跟随阴影距离（ShadowFar），大于 0 时取它和阴影距离的较小值 */
		float m_ShadowFadeDistance{ 0.0f };
		/* 被踩过的草弹回原状要多久（秒） */
		float m_BendRecover{ 6.0f };
		int m_Seed{ 7 };

		/* 贴合地形：打开后叶根落到覆盖本片草地的地形表面，块底面也随之停画 */
		bool m_ConformToTerrain{ true };
		/* 按地表草层的混合权重决定哪里长草；旧场景默认保持原分布 */
		bool m_UseTerrainGrowth{ false };
		/* 叶片姿态对齐坡面的程度：0 = 始终竖直，1 = 完全垂直于坡面 */
		float m_SurfaceAlign{ 1.0f };
		/* 长草的最大坡度（度）：超过它的地方退化不画，0 = 不限 */
		float m_MaxSlope{ 42.0f };
		/* 跟随地形：打开后范围自动取覆盖它的地形，块网格与地形最细节点对齐，
		 * 组件上的 Extent 不再生效 */
		bool m_FollowTerrain{ false };
		/* 总株数预算：跟随地形时按块数分摊，避免范围一大就炸 */
		uint32_t m_MaxInstances{ 500000 };
		/* 叶片下面每块垫一张草色平面，叶子稀的地方不露底；贴着地形时本来就不画 */
		bool m_DrawFlatUnderlay{ true };

		/* 运行期：已经提示过"开了贴地但没找到地形"，避免每帧刷日志（不序列化） */
		bool m_WarnedMissingTerrain{ false };

		/* 材质槽：pMaterial 是生效材质；IsInstance / SourceAssetPath 的含义与模型槽一致 ——
		 * 面板上改参数时从资产克隆一份，改动只落在这片草地上，Reset 回资产 */
		InstanceMaterialSlot m_MaterialSlot;
		SharedPtr<GrassField> m_Field{ nullptr };

		[[nodiscard]] const SharedPtr<Material>& GetMaterial() const { return m_MaterialSlot.pMaterial; }

		/* 缺材质时的内建外观：场景没写 Material、或者从面板新加组件都落到这里。
		 * 没有它就出现「组件在、草不在」这种完全没有提示的空状态 */
		[[nodiscard]] static SharedPtr<Material> GetDefaultMaterial();
		void OnAdded(const Scene& scene, Entity& entity) override;

		GrassComponent() = default;
		GrassComponent(const GrassComponent&) = default;
		GrassComponent& operator=(const GrassComponent&) = default;
	};

	/* 压倒草的物体：以实体世界位置为心、按半径把叶片朝外压下去。
	 * 用在角色 / 石头这类会站在草里的东西上，多个物体可以同时生效。 */
	struct HELIOS_API GrassBendComponent : ComponentBase
	{
		float m_Radius{ 1.2f };   /* 影响半径（米，按实体缩放的垂直分量算） */
		bool m_Enabled{ true };

		GrassBendComponent() = default;
		GrassBendComponent(const GrassBendComponent&) = default;
		GrassBendComponent& operator=(const GrassBendComponent&) = default;
	};

	/* 地形组件：一块高度场地形。高度由程序化噪声生成，顶点不落地 ——
	 * 所有 patch 共用同一个 33×33 模板网格，位置与 LOD 走实例属性，高度在顶点着色器里采样。
	 * 外观（草色 / 岩色 / 粗糙度）由材质决定，这里只管范围、起伏与 LOD。 */
	struct HELIOS_API TerrainComponent : ComponentBase
	{
		/* 高度数据来自 .terrain 资产（绝对路径；空 = 按下面的参数程序化生成）。
		 * 绑了资产时分辨率 / 范围 / 起伏幅度都由资产说了算，这几项只当程序化模式的输入 */
		std::string m_AssetPath;

		glm::vec2 m_Extent{ 256.0f, 256.0f }; /* 局部空间半宽半长（米） */
		uint32_t m_Resolution{ 512 };          /* 高度图边长（纹素） */
		float m_HeightScale{ 80.0f };          /* 起伏幅度（米）：最低点落在实体位置，往上乘这个数 */
		int m_Seed{ 1337 };

		/* 节点边长与相机距离的比值：小于它就继续细分，越大越激进 */
		float m_LodRatio{ 6.0f };
		/* 最细节点边长（米）：应不小于高度图 32 个纹素的跨度 */
		uint32_t m_MinNodeSize{ 64 };

		bool m_ReceiveShadow{ true };
		bool m_CastShadow{ true };

		/* 材质槽：与模型 / 草地同一套三态语义（覆盖 / 实例 / 默认） */
		InstanceMaterialSlot m_MaterialSlot;
		SharedPtr<TerrainField> m_Field{ nullptr };

		[[nodiscard]] const SharedPtr<Material>& GetMaterial() const { return m_MaterialSlot.pMaterial; }

		/* 缺材质时的内建外观：面板新加组件落到这里，否则组件在、地形不在 */
		[[nodiscard]] static SharedPtr<Material> GetDefaultMaterial();
		void OnAdded(const Scene& scene, Entity& entity) override;

		TerrainComponent() = default;
		TerrainComponent(const TerrainComponent&) = default;
		TerrainComponent& operator=(const TerrainComponent&) = default;
	};

	/* 天空与昼夜（Time of Day）：场景级配置，挂在哪个实体上无所谓（与后处理对象同层语义）。
	 * 每帧由场景换算成 SkyState 推给各视图；场景里的方向光按实体顺序绑定 ——
	 * 第一个当太阳、第二个当月亮，方向 / 颜色 / 强度都由这里驱动。 */
	struct HELIOS_API SkyComponent : ComponentBase
	{
		/* 当地真太阳时（0-24 小时） */
		float m_Hour{ 10.0f };
		/* 自动推进：每帧按 DayLength 走，编辑器里直接看一整天 */
		bool m_AutoAdvance{ false };
		/* 走完 24 小时要的真实秒数（600 = 十分钟一天） */
		float m_DayLengthSeconds{ 600.0f };

		/* 观测点：纬度决定太阳轨迹的倾斜，正北朝向绕 Y 轴转整个天球 */
		float m_Latitude{ 30.0f };
		float m_NorthAngle{ 0.0f };
		/* 一年中的第几天，决定太阳赤纬（春分 80、夏至 172、冬至 355） */
		int m_DayOfYear{ 172 };
		/* 月相：0 新月、0.25 上弦、0.5 满月、0.75 下弦 */
		float m_MoonPhase{ 0.5f };

		/* 日月星强度：太阳强度同时写进方向光，天空散射按同一份换算 */
		float m_SunIntensity{ 5.0f };
		float m_MoonIntensity{ 0.12f };
		float m_StarIntensity{ 1.0f };
		/* 雾霾（米氏散射强度缩放）与天空曝光 */
		float m_Haze{ 1.0f };
		float m_Exposure{ 1.5f };

		SkyComponent() = default;
		SkyComponent(const SkyComponent&) = default;
		SkyComponent& operator=(const SkyComponent&) = default;
	};

	/* 场景中需要接入on_construct/on_destroy信号分发的全部组件类型 */
	using SceneComponentList = std::tuple<
		NameComponent,
		TransformComponent,
		LayerComponent,
		SpriteComponent,
		CameraComponent,
		ModelComponent,
		LightComponent,
		ReflectionProbeComponent,
		AnimatorComponent,
		ScriptComponent,
		GrassComponent,
		GrassBendComponent,
		TerrainComponent,
		SkyComponent
	>;
}
