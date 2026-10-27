#include "TestApp.h"
#include "SampleIBL.h"

namespace Helios
{
TestApp::TestApp() : Application("Sample_IBL", 1920, 1080, false, {}, "Sample_IBL")
{
	PushLayer(CreateSharedPtr<SampleIBLLayer>());
}

UniquePtr<Application> CreateApplication()
{
	return CreateUniquePtr<TestApp>();
}

}