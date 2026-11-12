#include "Platform/Platform.h"

#if HELIOS_PLATFORM_APPLE

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include "Metal/Metal.hpp"
#include "QuartzCore/QuartzCore.hpp"

#endif
