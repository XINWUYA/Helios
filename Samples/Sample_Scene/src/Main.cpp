#include "Pch.h"
#include "SampleScene.h"
#include "Helios/Common/AssetPath.h"

namespace Helios
{
	class SceneApp final : public Application
	{
	public:
		SceneApp() : Application("Sample_Scene", 1920, 1080, false, {}, "Sample_Scene")
		{
			PushLayer(CreateSharedPtr<SampleSceneLayer>());
		}
	};

	UniquePtr<Application> CreateApplication()
	{
		return CreateUniquePtr<SceneApp>();
	}
}

int main(int argc, char** argv)
{
	Helios::Logger::Init();
	Helios::ParseProjectArgument(argc, argv);

	Helios::CreateApplication()->Run();

	Helios::Logger::Shutdown();
	return 0;
}
