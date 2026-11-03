#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "Command.h"
#include "Helios/Scene/Entity.h"
#include "Helios/Scene/Scene.h"

namespace Helios
{
	/* 一笔地形笔刷：只存改动过的那块纹素矩形，撤销重做按矩形写回高度。
	 * 一笔（按下到松开）算一条历史，拖多久都不会把栈撑爆 */
	class TerrainSculptCommand final : public ICommand
	{
	public:
		TerrainSculptCommand(SharedPtr<Scene> scene, Entity entity, const glm::uvec2& min,
			const glm::uvec2& max, std::vector<float> before, std::vector<float> after);

		void Do() override;
		void Undo() override;

		[[nodiscard]] const char* GetLabel() const override { return "Sculpt Terrain"; }

	private:
		void Apply(const std::vector<float>& values);

		SharedPtr<Scene> m_Scene;
		Entity m_Entity;
		glm::uvec2 m_Min{ 0 };
		glm::uvec2 m_Max{ 0 };
		std::vector<float> m_Before;
		std::vector<float> m_After;
	};
}
