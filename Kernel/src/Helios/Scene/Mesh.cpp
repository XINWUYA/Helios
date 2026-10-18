#include "Pch.h"
#include "Mesh.h"
#include "Helios/VirtualDevice/DeviceBuffer.h"
#include "Helios/VirtualDevice/DeviceVertexArray.h"

namespace Helios
{
	uint32_t MeshSegment::GetVertexStrideFloats() const
	{
		const auto& vertex_buffers = m_MeshPrimitive.VertexArray->GetVertexBuffers();
		if (vertex_buffers.empty())
			return 0;

		uint32_t stride = 0;
		for (const BufferElement& element : vertex_buffers.front()->GetLayout())
			stride += element.GetComponentCount();
		return stride;
	}

	MeshSegment::MeshSegment(const std::string& name, const MeshPrimitive& mesh_primitive, int slot_index)
		: m_DebugName(name), m_MeshPrimitive(mesh_primitive), m_SlotIndex(slot_index)
	{
	}

	SharedPtr<MeshSegment> MeshSegment::Create(const std::string& name, const MeshPrimitive& mesh_primitive, int slot_index)
	{
		return CreateSharedPtr<MeshSegment>(name, mesh_primitive, slot_index);
	}
}
