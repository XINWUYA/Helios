#include "Pch.h"
#include "BuiltinComponents.h"

namespace Helios
{
	void RegisterBuiltinComponents()
	{
		RegisterCoreComponents();
		RegisterSpriteComponent();
		RegisterCameraComponent();
		RegisterModelComponent();
		RegisterLightComponent();
		RegisterReflectionProbeComponent();
		RegisterPostProcessComponents();
	}
}
