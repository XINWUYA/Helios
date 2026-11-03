#include "Pch.h"
#include "TerrainSculptCommand.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/TerrainField.h"

namespace Helios
{
	TerrainSculptCommand::TerrainSculptCommand(SharedPtr<Scene> scene, Entity entity, uint32_t resolution,
		const glm::uvec2& min, const glm::uvec2& max,
		std::vector<float> before, std::vector<float> after)
		: m_Scene(std::move(scene))
		, m_Entity(entity)
		, m_Resolution(resolution)
		, m_Min(min)
		, m_Max(max)
		, m_Before(std::move(before))
		, m_After(std::move(after))
	{
	}

	void TerrainSculptCommand::Do()
	{
		Apply(m_After);
	}

	void TerrainSculptCommand::Undo()
	{
		Apply(m_Before);
	}

	void TerrainSculptCommand::Apply(const std::vector<float>& values)
	{
		if (m_Scene == nullptr || !m_Scene->IsEntityValid(m_Entity))
			return;
		if (!m_Entity.HasComponent<TerrainComponent>())
			return;

		auto& terrain = m_Entity.GetComponent<TerrainComponent>();
		if (terrain.m_Field == nullptr || terrain.m_Field->GetResolution() != m_Resolution)
			return;

		terrain.m_Field->WriteHeightRect(m_Min, m_Max, values);
	}
}
