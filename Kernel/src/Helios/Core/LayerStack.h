#pragma once
#include "Layer.h"

namespace Helios
{
	class LayerStack
	{
	public:
		LayerStack() = default;
		~LayerStack();
		void Release();

		void PushLayer(const SharedPtr<ILayer>& layer);
		void PushOverlay(const SharedPtr<ILayer>& layer);
		void PopLayer(const SharedPtr<ILayer>& layer);
		void PopOverlay(const SharedPtr<ILayer>& layer);

		/* Get Layer（按值返回：未命中时返回空 SharedPtr，避免返回绑定到临时量的悬垂引用） */
		SharedPtr<ILayer> GetLayerByName(const std::string& name);

		std::vector<SharedPtr<ILayer>>::iterator begin() { return m_Layers.begin(); }
		std::vector<SharedPtr<ILayer>>::iterator end() { return m_Layers.end(); }
		std::vector<SharedPtr<ILayer>>::reverse_iterator rbegin() { return m_Layers.rbegin(); }
		std::vector<SharedPtr<ILayer>>::reverse_iterator rend() { return m_Layers.rend(); }
		[[nodiscard]]
		std::vector<SharedPtr<ILayer>>::const_iterator begin() const { return m_Layers.begin(); }
		[[nodiscard]]
		std::vector<SharedPtr<ILayer>>::const_iterator end() const { return m_Layers.end(); }
		[[nodiscard]]
		std::vector<SharedPtr<ILayer>>::const_reverse_iterator rbegin() const { return m_Layers.rbegin(); }
		[[nodiscard]]
		std::vector<SharedPtr<ILayer>>::const_reverse_iterator rend() const { return m_Layers.rend(); }

	private:
		std::vector<SharedPtr<ILayer>> m_Layers{};
		uint32_t m_LayerInsertIndex{ 0 };
	};

}