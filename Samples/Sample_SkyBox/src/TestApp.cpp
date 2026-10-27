#include "TestApp.h"
#include "SampleSkyBox.h"

namespace Helios
{
TestApp::TestApp() : Application("Sample_SkyBox", 1920, 1080, false, {}, "Sample_SkyBox")
{
	PushLayer(CreateSharedPtr<SampleSkyBox>());
}

UniquePtr<Application> CreateApplication()
{
	return CreateUniquePtr<TestApp>();
}

}