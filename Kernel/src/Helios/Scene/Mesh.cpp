#include "Pch.h"
#include "Mesh.h"

namespace Helios
{
	MeshSegment::MeshSegment(const std::string& name, const MeshPrimitive& mesh_primitive, int slot_index)
		: m_DebugName(name), m_MeshPrimitive(mesh_primitive), m_SlotIndex(slot_index)
	{
	}

	SharedPtr<MeshSegment> MeshSegment::Create(const std::string& name, const MeshPrimitive& mesh_primitive, int slot_index)
	{
		return CreateSharedPtr<MeshSegment>(name, mesh_primitive, slot_index);
	}
}
