/* Debug：脚本模板。OnUpdate 每帧跑一次，Runtime 模式下生效。 */
#include "Helios/Script/ScriptRegistry.h"
#include "Helios/Scene/Components.h"

namespace
{
	class Debug : public Helios::Script
	{
	public:
		void OnUpdate(float delta_time) override
		{
			(void)delta_time;
		}

		float m_Speed{ 1.0f };
	};
}

HELIOS_SCRIPT(Debug,
	HELIOS_SCRIPT_FIELD(&Debug::m_Speed, "Speed", Helios::FieldType::Float))
