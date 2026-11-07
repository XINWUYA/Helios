#include "Pch.h"
#include "LayerStack.h"

namespace Helios
{
	LayerStack::~LayerStack()
	{
		Release();
	}

	void LayerStack::Release()
	{
		for (auto& layer : m_Layers)
		{
			layer->OnDetached();
			layer.reset();
		}
		m_Layers.clear();
		m_LayerInsertIndex = 0;
	}

	/* 两种入栈只差插在哪个区：普通层占 [0, m_LayerInsertIndex)，覆盖层在其后。
	 * 区内按优先级升序，数字大的更靠后（更上层）；同优先级保持调用先后。 */
	void LayerStack::PushLayer(const SharedPtr<ILayer>& layer)
	{
		auto it = m_Layers.begin() + m_LayerInsertIndex;
		while (it != m_Layers.begin() && (*(it - 1))->GetPriority() > layer->GetPriority())
			--it;

		m_Layers.emplace(it, layer);
		m_LayerInsertIndex++;
		layer->OnAttached();
	}

	void LayerStack::PushOverlay(const SharedPtr<ILayer>& layer)
	{
		auto it = m_Layers.end();
		while (it != m_Layers.begin() + m_LayerInsertIndex && (*(it - 1))->GetPriority() > layer->GetPriority())
			--it;

		m_Layers.emplace(it, layer);
		layer->OnAttached();
	}

	void LayerStack::PopLayer(const SharedPtr<ILayer>& layer)
	{
	auto it = std::find(begin(), begin() + m_LayerInsertIndex, layer);
	if (it != begin() + m_LayerInsertIndex)
	{
		layer->OnDetached();
		m_Layers.erase(it);
		m_LayerInsertIndex--;
	}
		else
		{
			CORE_LOG_WARN("Can't find layer ({0}) to pop up.", layer->GetName());
		}
	}

	void LayerStack::PopOverlay(const SharedPtr<ILayer>& layer)
	{
	auto it = std::find(begin() + m_LayerInsertIndex, end(), layer);
	if (it != end())
	{
		layer->OnDetached();
		m_Layers.erase(it);
	}
		else
		{
			CORE_LOG_WARN("Can't find layer ({0}) to pop up.", layer->GetName());
		}
	}

	/* Get Layer */
	SharedPtr<ILayer> LayerStack::GetLayerByName(const std::string& name)
	{
		for (auto& layer : m_Layers)
		{
			if (strcmp(layer->GetName().c_str(), name.c_str()) == 0)
				return layer;
		}

		return {};
	}
}
