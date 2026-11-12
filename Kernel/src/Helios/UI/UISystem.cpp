#include "Pch.h"
#include "UISystem.h"

#include <cstdlib>

#include <RmlUi/Core.h>

#include "ElementJoystick.h"
#include "UICanvasHost.h"

#include "Helios/Application/Application.h"
#include "Helios/Core/Input.h"
#include "Helios/Profiler/RenderStatistics.h"
#include "Helios/Renderer/RenderView.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Scene.h"

namespace Helios
{
	namespace UI
	{
		namespace
		{
			/* 方向键推出的摇杆值：走 Input 的闸，Game 画面没焦点时读不到按键，
			 * 在属性面板打字不会顺带推动摇杆 */
			Rml::Vector2f ReadKeyAxis()
			{
				glm::vec2 direction(0.0f);
				if (Input::IsKeyPressed(Key::A) || Input::IsKeyPressed(Key::Left))
					direction.x -= 1.0f;
				if (Input::IsKeyPressed(Key::D) || Input::IsKeyPressed(Key::Right))
					direction.x += 1.0f;
				if (Input::IsKeyPressed(Key::W) || Input::IsKeyPressed(Key::Up))
					direction.y -= 1.0f;
				if (Input::IsKeyPressed(Key::S) || Input::IsKeyPressed(Key::Down))
					direction.y += 1.0f;

				/* 斜向不该比单键快 */
				const float length = glm::length(direction);
				if (length > 1.0f)
					direction /= length;
				return Rml::Vector2f(direction.x, direction.y);
			}

			std::string CanvasContextPrefix(entt::entity canvas)
			{
				return "canvas_" + std::to_string(static_cast<uint32_t>(canvas));
			}

			/* 屏幕给的是逻辑点：dp 按点走，元素在手机上的物理大小才和设计稿一致。
			 * 安全区换成物理像素 —— 画布一律按像素布局 */
			struct ScreenMetrics
			{
				float DensityRatio{ 1.0f };
				glm::vec4 SafeArea{ 0.0f };
			};

			ScreenMetrics ReadScreenMetrics()
			{
				ScreenMetrics metrics;
				Application* application = Application::Instance();
				if (application == nullptr)
					return metrics;

				const DeviceWindow& window = application->GetWindow();
				metrics.DensityRatio = std::max(window.GetContentScale(), 0.01f);
				metrics.SafeArea = window.GetSafeAreaInsets() * metrics.DensityRatio;
				return metrics;
			}
		}

		struct UISystem::CanvasRuntime
		{
			entt::entity Entity{ entt::null };
			std::string DocumentPath{};
			int32_t SortingOrder{ 0 };
			bool ReceiveInput{ true };

			/* Context 名字由宿主补上序号：编辑世界与运行世界的画布句柄一样，只按句柄命名会撞 */
			UICanvasHost Host{};

			/* 引擎侧变量：控件写、脚本读。键就是控件上的 name 属性 */
			std::map<std::string, float> Numbers{};
		};

		UISystem::UISystem(Scene& scene)
			: m_Scene(scene)
		{
			Initialise();
		}

		UISystem::~UISystem() = default;

		void UISystem::Tick()
		{
			if (!IsInitialised())
				return;

			/* 布局耗时进渲染统计：闸门卡的是这一段的毫秒数 */
			const auto tick_start = std::chrono::steady_clock::now();

			SyncCanvases();
			/* 半秒查一次文件时间戳：改 .rml 重建画布，改 .rcss 就地重排 */
			if (IsHotReloadEnabled() && ++m_HotReloadTicks >= 30)
			{
				m_HotReloadTicks = 0;
				CheckHotReload();
			}

			const ScreenMetrics metrics = ReadScreenMetrics();
			for (const UniquePtr<CanvasRuntime>& canvas : m_Canvases)
			{
				if (!canvas->Host.IsOpen())
					continue;

				canvas->Host.SetDensityRatio(metrics.DensityRatio);
				canvas->Host.SetSafeAreaInsets(metrics.SafeArea);
				SyncJoysticks(*canvas);
				SyncSliders(*canvas);
				/* 尺寸取上一帧渲染时的视图尺寸：布局比画面慢一帧，命中测试也用它 */
				canvas->Host.Update(m_ViewWidth, m_ViewHeight);
			}

			const std::chrono::duration<float, std::milli> elapsed = std::chrono::steady_clock::now() - tick_start;
			RenderStatistics::Instance().AddUIUpdateMs(elapsed.count());
		}

		void UISystem::Render(RenderView& view)
		{
			if (m_Canvases.empty() || !IsInitialised())
				return;

			const ViewportRegion& region = view.GetViewportRegion();
			if (region.Width == 0 || region.Height == 0)
				return;
			m_ViewWidth = static_cast<int>(region.Width);
			m_ViewHeight = static_cast<int>(region.Height);

			const auto render_start = std::chrono::steady_clock::now();
			for (const UniquePtr<CanvasRuntime>& canvas : m_Canvases)
				canvas->Host.Render(m_ViewWidth, m_ViewHeight);

			const std::chrono::duration<float, std::milli> elapsed = std::chrono::steady_clock::now() - render_start;
			RenderStatistics::Instance().AddUIRenderMs(elapsed.count());
		}

		void UISystem::SyncCanvases()
		{
			auto& registry = m_Scene.GetRegistry();

			/* 先拆：实体没了、组件删了、文档路径改了 */
			m_Canvases.erase(std::remove_if(m_Canvases.begin(), m_Canvases.end(),
				[&registry](const UniquePtr<CanvasRuntime>& canvas)
				{
					const CanvasComponent* component = registry.valid(canvas->Entity)
						? registry.try_get<CanvasComponent>(canvas->Entity) : nullptr;
					return component == nullptr || component->m_DocumentPath != canvas->DocumentPath;
				}), m_Canvases.end());

			/* 再建：新挂上的画布开宿主；已在册的跟着组件的排序与输入开关走 */
			for (const entt::entity entity : registry.view<CanvasComponent>())
			{
				const CanvasComponent& component = registry.get<CanvasComponent>(entity);
				if (component.m_DocumentPath.empty())
					continue;

				if (CanvasRuntime* existing = FindCanvas(entity))
				{
					existing->SortingOrder = component.m_SortingOrder;
					existing->ReceiveInput = component.m_ReceiveInput;
					continue;
				}

				auto runtime = CreateUniquePtr<CanvasRuntime>();
				runtime->Entity = entity;
				runtime->DocumentPath = component.m_DocumentPath;
				runtime->SortingOrder = component.m_SortingOrder;
				runtime->ReceiveInput = component.m_ReceiveInput;

				UICanvasHost::Options options;
				options.ContextPrefix = CanvasContextPrefix(entity);
				options.Width = m_ViewWidth;
				options.Height = m_ViewHeight;
				options.DensityRatio = ReadScreenMetrics().DensityRatio;
				/* 编辑器里同目录的样式也盯：改哪一份都能就地重排 */
				options.WatchSiblingStyles = IsHotReloadEnabled();

				if (!runtime->Host.Open(component.m_DocumentPath, options))
				{
					/* 打不开也留在表里：文档路径不变就只在下次改动时再试，别每帧刷日志 */
					CORE_LOG_ERROR("Failed to load UI document '{}'", component.m_DocumentPath);
				}
				m_Canvases.push_back(std::move(runtime));
			}

			/* 排序号小的先画；后画的压在上面，问输入时也从后往前 */
			std::stable_sort(m_Canvases.begin(), m_Canvases.end(),
				[](const UniquePtr<CanvasRuntime>& lhs, const UniquePtr<CanvasRuntime>& rhs)
				{
					return lhs->SortingOrder < rhs->SortingOrder;
				});
		}

		void UISystem::CheckHotReload()
		{
			for (auto it = m_Canvases.begin(); it != m_Canvases.end();)
			{
				CanvasRuntime& canvas = **it;
				if (canvas.Host.ReloadIfChanged() != UICanvasHost::Change::Document)
				{
					++it;
					continue;
				}

				/* 结构变了：拆掉宿主，下一帧 SyncCanvases 按新文档重建 */
				CORE_LOG_INFO("UI document changed, reloading '{}'", canvas.DocumentPath);
				it = m_Canvases.erase(it);
			}
		}

		UISystem::CanvasRuntime* UISystem::FindCanvas(entt::entity canvas)
		{
			for (const UniquePtr<CanvasRuntime>& runtime : m_Canvases)
			{
				if (runtime->Entity == canvas)
					return runtime.get();
			}
			return nullptr;
		}

		const UISystem::CanvasRuntime* UISystem::FindCanvas(entt::entity canvas) const
		{
			for (const UniquePtr<CanvasRuntime>& runtime : m_Canvases)
			{
				if (runtime->Entity == canvas)
					return runtime.get();
			}
			return nullptr;
		}

		void UISystem::SyncJoysticks(CanvasRuntime& canvas)
		{
			Rml::ElementDocument* document = canvas.Host.GetDocument();
			if (document == nullptr)
				return;

			Rml::ElementList elements;
			document->GetElementsByTagName(elements, "joystick");
			if (elements.empty())
				return;

			/* 键盘是第二路输入：拖拽期间控件自己以指针为准，这里只负责把键值递进去。
			 * 焦点在文本框里时键盘归打字，摇杆不跟着动 */
			const Rml::Vector2f key_axis = (canvas.ReceiveInput && !canvas.Host.HasTextFocus())
				? ReadKeyAxis() : Rml::Vector2f(0.0f, 0.0f);

			for (Rml::Element* element : elements)
			{
				auto* joystick = dynamic_cast<ElementJoystick*>(element);
				if (joystick == nullptr)
					continue;

				joystick->SetKeyAxis(key_axis);
				const std::string name = joystick->GetControlName().c_str();
				canvas.Numbers[name + "_x"] = joystick->GetAxis().x;
				canvas.Numbers[name + "_y"] = joystick->GetAxis().y;
			}
		}

		void UISystem::SyncSliders(CanvasRuntime& canvas)
		{
			Rml::ElementDocument* document = canvas.Host.GetDocument();
			if (document == nullptr)
				return;

			Rml::ElementList elements;
			document->GetElementsByTagName(elements, "input");
			for (Rml::Element* element : elements)
			{
				if (element->GetAttribute<Rml::String>("type", "") != "range")
					continue;

				const std::string name = element->GetAttribute<Rml::String>("name", "");
				auto* input = dynamic_cast<Rml::ElementFormControlInput*>(element);
				if (name.empty() || input == nullptr)
					continue;

				canvas.Numbers[name] = std::strtof(input->GetValue().c_str(), nullptr);
			}
		}

		bool UISystem::ProcessMouseMove(float x, float y)
		{
			bool handled = false;
			for (const UniquePtr<CanvasRuntime>& canvas : m_Canvases)
			{
				if (canvas->ReceiveInput)
					handled |= canvas->Host.ProcessMouseMove(x, y);
			}
			return handled;
		}

		bool UISystem::ProcessMouseButtonDown(int button)
		{
			bool handled = false;
			/* 交互事件从最上层往下问：上面消费掉就不再往下传 */
			for (auto it = m_Canvases.rbegin(); it != m_Canvases.rend() && !handled; ++it)
			{
				CanvasRuntime& canvas = **it;
				if (canvas.ReceiveInput)
					handled = canvas.Host.ProcessMouseButtonDown(button);
			}
			return handled;
		}

		bool UISystem::ProcessMouseButtonUp(int button)
		{
			bool handled = false;
			/* 交互事件从最上层往下问：上面消费掉就不再往下传 */
			for (auto it = m_Canvases.rbegin(); it != m_Canvases.rend() && !handled; ++it)
			{
				CanvasRuntime& canvas = **it;
				if (canvas.ReceiveInput)
					handled = canvas.Host.ProcessMouseButtonUp(button);
			}
			return handled;
		}

		bool UISystem::ProcessMouseWheel(float delta)
		{
			bool handled = false;
			/* 交互事件从最上层往下问：上面消费掉就不再往下传 */
			for (auto it = m_Canvases.rbegin(); it != m_Canvases.rend() && !handled; ++it)
			{
				CanvasRuntime& canvas = **it;
				if (canvas.ReceiveInput)
					handled = canvas.Host.ProcessMouseWheel(delta);
			}
			return handled;
		}

		void UISystem::ProcessMouseLeave()
		{
			for (const UniquePtr<CanvasRuntime>& canvas : m_Canvases)
			{
				if (canvas->ReceiveInput)
					canvas->Host.ProcessMouseLeave();
			}
		}

		bool UISystem::IsPointerOverControl() const
		{
			/* 画布按叠放顺序喂同一份指针位置，任一画布命中控件就算落在 UI 上 */
			for (auto it = m_Canvases.rbegin(); it != m_Canvases.rend(); ++it)
			{
				if ((*it)->ReceiveInput && (*it)->Host.IsPointerOverControl())
					return true;
			}
			return false;
		}

		bool UISystem::IsPointOverControl(float x, float y) const
		{
			for (auto it = m_Canvases.rbegin(); it != m_Canvases.rend(); ++it)
			{
				if ((*it)->ReceiveInput && (*it)->Host.IsPointOverControl(x, y))
					return true;
			}
			return false;
		}

		bool UISystem::ProcessKeyDown(KeyCode key)
		{
			bool handled = false;
			/* 交互事件从最上层往下问：上面消费掉就不再往下传 */
			for (auto it = m_Canvases.rbegin(); it != m_Canvases.rend() && !handled; ++it)
			{
				CanvasRuntime& canvas = **it;
				if (canvas.ReceiveInput)
					handled = canvas.Host.ProcessKeyDown(key);
			}
			return handled;
		}

		bool UISystem::ProcessKeyUp(KeyCode key)
		{
			bool handled = false;
			/* 交互事件从最上层往下问：上面消费掉就不再往下传 */
			for (auto it = m_Canvases.rbegin(); it != m_Canvases.rend() && !handled; ++it)
			{
				CanvasRuntime& canvas = **it;
				if (canvas.ReceiveInput)
					handled = canvas.Host.ProcessKeyUp(key);
			}
			return handled;
		}

		bool UISystem::ProcessTextInput(uint32_t codepoint)
		{
			bool handled = false;
			/* 交互事件从最上层往下问：上面消费掉就不再往下传 */
			for (auto it = m_Canvases.rbegin(); it != m_Canvases.rend() && !handled; ++it)
			{
				CanvasRuntime& canvas = **it;
				if (canvas.ReceiveInput)
					handled = canvas.Host.ProcessTextInput(codepoint);
			}
			return handled;
		}

		float UISystem::GetNumber(entt::entity canvas, const std::string& name, float fallback) const
		{
			const CanvasRuntime* runtime = FindCanvas(canvas);
			if (runtime == nullptr)
				return fallback;

			const auto it = runtime->Numbers.find(name);
			return it == runtime->Numbers.end() ? fallback : it->second;
		}

		void UISystem::SetText(entt::entity canvas, const std::string& element_id, const std::string& text)
		{
			const CanvasRuntime* runtime = FindCanvas(canvas);
			if (runtime == nullptr || !runtime->Host.IsOpen())
				return;

			Rml::Element* element = runtime->Host.GetDocument()->GetElementById(element_id.c_str());
			if (element == nullptr || element->GetInnerRML() == text.c_str())
				return;

			element->SetInnerRML(text.c_str());
		}

		bool UISystem::ConsumeClick(entt::entity canvas, const std::string& element_id)
		{
			CanvasRuntime* runtime = FindCanvas(canvas);
			return runtime != nullptr && runtime->Host.ConsumeClick(element_id);
		}
	}
}
