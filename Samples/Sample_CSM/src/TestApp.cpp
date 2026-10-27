#include "TestApp.h"
#include "SampleCSM.h"

namespace Helios
{
TestApp::TestApp() : Application("Sample_CSM", 1920, 1080, false, {}, "Sample_CSM")
{
	PushLayer(CreateSharedPtr<SampleCSM>());
}

UniquePtr<Application> CreateApplication()
{
	return CreateUniquePtr<TestApp>();
}

}