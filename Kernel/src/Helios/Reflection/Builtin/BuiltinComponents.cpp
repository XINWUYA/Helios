#include "Pch.h"
#include "BuiltinComponents.h"

namespace Helios
{
	void RegisterBuiltinComponents()
	{
		RegisterCoreComponents();
		RegisterLayerComponent();
		RegisterSpriteComponent();
		RegisterCameraComponent();
		RegisterModelComponent();
		RegisterLightComponent();
		RegisterReflectionProbeComponent();
		RegisterAnimatorComponent();
		RegisterPostProcessComponents();
	}
}
