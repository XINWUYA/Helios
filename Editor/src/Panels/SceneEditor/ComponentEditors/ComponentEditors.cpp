#include "Pch.h"
#include "ComponentEditors.h"

namespace Helios
{
	void InstallComponentEditors()
	{
		/* 只装一次：换工程会重建面板，重复回填没有意义 */
		static const bool installed = []
		{
			RegisterModelComponentEditor();
			RegisterScriptComponentEditor();
			RegisterReflectionProbeComponentEditor();
			RegisterAnimatorComponentEditor();
			RegisterSpriteComponentEditor();
			RegisterPostProcessComponentEditor();
			RegisterGrassComponentEditor();
			RegisterTerrainComponentEditor();
			return true;
		}();
		(void)installed;
	}
}
