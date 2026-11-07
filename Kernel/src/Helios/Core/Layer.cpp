#include "Pch.h"
#include "Layer.h"

namespace Helios
{
	ILayer::ILayer(const std::string& name, int priority)
		: m_Name(name)
		, m_Priority(priority)
	{
	}
}