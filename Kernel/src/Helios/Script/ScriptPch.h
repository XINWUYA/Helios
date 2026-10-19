#pragma once
/* 脚本模块的预编译头：只放引擎公共底座。宏与 Kernel 的 Pch.h 保持一套，
 * 但不拉渲染后端与资产库那些大头，脚本 TU 才编得快（一个脚本约 1.6 秒）。 */
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

#include "HeliosConfig.h"
#include "Helios/Common/Utils.h"
#include "Helios/Common/Common.h"
#include "Helios/Common/Assert.h"
#include "Helios/Core/Logger.h"
