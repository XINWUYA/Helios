#include "Pch.h"
#include "ShaderVariantCatalog.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "Helios/Core/AssetManager.h"
#include "Helios/Core/Logger.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/VirtualDevice/DeviceShader.h"

namespace Helios
{
	namespace
	{
		/* 清单里存相对 EngineRes 的路径，换机器、换打包目录都能用 */
		std::string ToRelativePath(const std::string& path)
		{
			const std::string marker = "/EngineRes/";
			const size_t at = path.find(marker);
			return at == std::string::npos ? path : path.substr(at + marker.size());
		}

		std::string FormatDefines(const ShaderVariantDefines& defines)
		{
			if (defines.empty())
				return "-";

			std::string text;
			for (const auto& [name, value] : defines)
			{
				if (!text.empty())
					text += ',';
				text += name + "=" + std::to_string(value);
			}
			return text;
		}

		bool ParseDefines(const std::string& text, ShaderVariantDefines& out)
		{
			out.clear();
			if (text.empty() || text == "-")
				return true;

			size_t begin = 0;
			while (begin <= text.size())
			{
				const size_t end = text.find(',', begin);
				const std::string token = text.substr(begin,
					end == std::string::npos ? std::string::npos : end - begin);
				const size_t equals = token.find('=');
				if (equals == std::string::npos)
					return false;
				out[token.substr(0, equals)] = std::atoi(token.c_str() + equals + 1);
				if (end == std::string::npos)
					break;
				begin = end + 1;
			}
			return true;
		}
	}

	ShaderVariantCatalog& ShaderVariantCatalog::Instance()
	{
		static ShaderVariantCatalog instance;
		return instance;
	}

	std::vector<ShaderVariantItem> ShaderVariantCatalog::CollectLoaded()
	{
		std::vector<ShaderVariantItem> items;
		for (const SharedPtr<DeviceShader>& shader : AssetManager::Instance().GetAll<DeviceShader>())
		{
			if (shader == nullptr || shader->GetPath().empty())
				continue;

			ShaderVariantItem item;
			item.Path = ToRelativePath(shader->GetPath());
			item.Defines = shader->GetVariantDefines();
			items.push_back(std::move(item));
		}

		std::sort(items.begin(), items.end(), [](const ShaderVariantItem& a, const ShaderVariantItem& b)
			{
				if (a.Path != b.Path)
					return a.Path < b.Path;
				return a.Defines < b.Defines;
			});
		return items;
	}

	bool ShaderVariantCatalog::Save(const std::string& filepath, const std::vector<ShaderVariantItem>& items)
	{
		std::ofstream file(filepath, std::ios::out | std::ios::trunc);
		if (!file.is_open())
			return false;

		file << "# path|A=1,B=0\n";
		for (const ShaderVariantItem& item : items)
			file << item.Path << '|' << FormatDefines(item.Defines) << '\n';
		return file.good();
	}

	std::vector<ShaderVariantItem> ShaderVariantCatalog::Load(const std::string& filepath)
	{
		std::vector<ShaderVariantItem> items;

		std::ifstream file(filepath);
		if (!file.is_open())
			return items;

		std::string line;
		while (std::getline(file, line))
		{
			if (line.empty() || line[0] == '#')
				continue;

			const size_t bar = line.find('|');
			if (bar == std::string::npos)
				continue;

			ShaderVariantItem item;
			item.Path = line.substr(0, bar);
			if (!ParseDefines(line.substr(bar + 1), item.Defines))
			{
				CORE_LOG_WARN("[Shader] 变体清单行认不出来，跳过：{}", line);
				continue;
			}
			items.push_back(std::move(item));
		}
		return items;
	}

	void ShaderVariantCatalog::Enqueue(const std::vector<ShaderVariantItem>& items)
	{
		for (const ShaderVariantItem& item : items)
		{
			if (item.Path.empty())
				continue;

			/* 已经编过的跳过：清单可能整份重放，重复排队只会白占帧预算 */
			const std::string absolute = ABSOLUTE_PATH(item.Path);
			if (AssetManager::Instance().Find<DeviceShader>(absolute, ShaderVariantConfig{ item.Defines }) != nullptr)
				continue;

			m_Pending.push_back(item);
		}
	}

	void ShaderVariantCatalog::Tick()
	{
		for (uint32_t i = 0; i < m_FrameBudget && !m_Pending.empty(); ++i)
		{
			const ShaderVariantItem item = std::move(m_Pending.front());
			m_Pending.pop_front();
			AssetManager::Instance().GetOrLoad<DeviceShader>(ABSOLUTE_PATH(item.Path),
				ShaderVariantConfig{ item.Defines });
			++m_Completed;
		}
	}
}
