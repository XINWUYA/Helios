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

	void LayerStack::PushLayer(const SharedPtr<ILayer>& layer)
	{
		m_Layers.emplace(begin() + m_LayerInsertIndex, layer);
		m_LayerInsertIndex++;
		layer->OnAttached();
	}

	void LayerStack::PushOverlay(const SharedPtr<ILayer>& layer)
	{
		m_Layers.emplace_back(layer);
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
