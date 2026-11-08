#include "Pch.h"
#include "ShaderVariants.h"

#include <algorithm>
#include <set>

#include "Helios/VirtualDevice/DeviceShader.h"

namespace Helios
{
	ShaderVariantDefines ShaderVariantGlobals::s_Values;
	bool ShaderVariantGlobals::s_Fallback = false;

	void ShaderVariantGlobals::Set(const std::string& name, int value)
	{
		s_Values[name] = std::clamp(value, 0, ShaderVariantMaxValue);
	}

	void ShaderVariantGlobals::Clear()
	{
		s_Values.clear();
	}

	const ShaderVariantDefines& ShaderVariantGlobals::All()
	{
		return s_Values;
	}

	void ShaderVariantGlobals::SetFallbackEnabled(bool enabled)
	{
		s_Fallback = enabled;
	}

	bool ShaderVariantGlobals::FallbackEnabled()
	{
		return s_Fallback;
	}

	bool ShaderVariantGlobals::ReportFallback(const std::string& path, const std::string& diff)
	{
		static std::set<std::string> reported;
		return reported.insert(path + "#" + diff).second;
	}

	int ScoreVariant(const ShaderVariantDefines& requested, const ShaderVariantDefines& candidate,
		std::string* diff)
	{
		/* 每维满分，降级按比例给分：差一档和差到底必须能分出高下，
		 * 否则"请求 2、候选 0 和 1"会打平，挑出来的候选就看遍历顺序 */
		constexpr int kExact = 8;

		int score = 0;
		for (const auto& [name, wanted] : requested)
		{
			const auto it = candidate.find(name);
			if (it == candidate.end())
				continue;

			if (it->second == wanted)
			{
				score += kExact;
				continue;
			}

			if (it->second < wanted)
				score += wanted == 0 ? 0 : (it->second * kExact) / wanted;

			if (diff != nullptr)
				*diff += (diff->empty() ? "" : " ") + name + " " + std::to_string(wanted)
					+ "→" + std::to_string(it->second);
		}
		return score;
	}

	ShaderVariantMatch FindBestVariant(const std::vector<SharedPtr<DeviceShader>>& candidates,
		const ShaderVariantDefines& requested)
	{
		ShaderVariantMatch best;
		best.FullScore = static_cast<int>(requested.size()) * 8;

		for (const SharedPtr<DeviceShader>& candidate : candidates)
		{
			if (candidate == nullptr)
				continue;

			std::string diff;
			const int score = ScoreVariant(requested, candidate->GetVariantDefines(), &diff);
			/* 同分取先到的：候选表已按 define 排序，结果跟遍历顺序无关 */
			if (!best.Valid() || score > best.Score)
			{
				best.Shader = candidate;
				best.Score = score;
				best.Diff = std::move(diff);
			}
		}
		return best;
	}
}
