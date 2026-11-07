#include "Pch.h"
#include "ProfilerExport.h"

namespace Helios
{
	namespace ProfilerExport
	{
std::string BuildRecordingCsv(const GPUTimeScopeRecorder& recorder)
	{
		const uint32_t count = recorder.GetFrameCount();
		if (count == 0)
			return {};

		/* 一、收集：节点列顺序（首现先序）+ 每帧的 (节点下标 → 总 / 顶点耗时之和) */
		struct NodeValues
		{
			double Total = 0.0;
			double Vertex = 0.0;
		};

		std::vector<std::string> nodes;
		std::unordered_map<std::string, size_t> node_index;
		std::vector<std::unordered_map<size_t, NodeValues>> frame_values(count);

		std::function<void(const GPUTimeScopeNode&, const std::string&, std::unordered_map<size_t, NodeValues>&)> collect;
		collect = [&node_index, &nodes, &collect](const GPUTimeScopeNode& node, const std::string& parent_path,
			std::unordered_map<size_t, NodeValues>& values)
			{
				for (const GPUTimeScopeNode& child : node.Children)
				{
					/* 空标签节点不做列，用父路径继续向子层递归 */
					if (child.Label.empty())
					{
						collect(child, parent_path, values);
						continue;
					}

					const std::string path = parent_path.empty() ? child.Label : parent_path + "/" + child.Label;
					const auto [entry, inserted] = node_index.try_emplace(path, nodes.size());
					if (inserted)
						nodes.emplace_back(path);
					if (child.HasData)
					{
						NodeValues& value = values[entry->second]; /* 同路径重复出现 = 求和 */
						value.Total += child.TotalUs;
						value.Vertex += child.StageVertexUs;
					}

					collect(child, path, values);
				}
			};

		for (uint32_t index = 0; index < count; ++index)
			collect(recorder.GetFrame(index), {}, frame_values[index]);

		/* 二、写表：UTF-8 BOM（Excel 打开中文实体名不乱码）→ 表头 → 每帧一行 */
		std::string csv;
		csv.reserve(256 + static_cast<size_t>(count) * (32 + nodes.size() * 24));

		const auto append_field = [&csv](const std::string& field)
			{
				/* 含分隔符 / 引号 / 换行 / 首尾空格 → RFC 4180 引号包裹（引号翻倍） */
				const bool needs_quotes = field.find_first_of(",\"\n\r") != std::string::npos
					|| (!field.empty() && (field.front() == ' ' || field.back() == ' '));
				if (!needs_quotes)
				{
					csv += field;
					return;
				}

				csv += '"';
				for (const char ch : field)
				{
					if (ch == '"')
						csv += '"';
					csv += ch;
				}
				csv += '"';
			};

		csv += "\xEF\xBB\xBF";
		append_field("frame");
		csv += ',';
		append_field("total_us");
		csv += ',';
		append_field("vertex_us");
		csv += ',';
		append_field("fragment_us");
		for (const std::string& node : nodes)
		{
			csv += ',';
			append_field(node + ".total_us");
			csv += ',';
			append_field(node + ".vertex_us");
		}
		csv += '\n';

		char cell[32];
		for (uint32_t index = 0; index < count; ++index)
		{
			const GPUTimeScopeNode& frame = recorder.GetFrame(index);

			snprintf(cell, sizeof(cell), "%u", recorder.GetFrameId(index));
			csv += cell;

			if (frame.HasData)
			{
				snprintf(cell, sizeof(cell), ",%.3f,%.3f,%.3f",
					frame.TotalUs, frame.StageVertexUs, frame.StageFragmentUs);
				csv += cell;
			}
			else
			{
				csv += ",,,"; /* 无数据帧：占位留空（写 0 会被当成真实零耗时） */
			}

			for (size_t node = 0; node < nodes.size(); ++node)
			{
				csv += ',';
				const auto value = frame_values[index].find(node);
				if (value != frame_values[index].end())
				{
					snprintf(cell, sizeof(cell), "%.3f", value->second.Total);
					csv += cell;
				}

				csv += ',';
				if (value != frame_values[index].end())
				{
					snprintf(cell, sizeof(cell), "%.3f", value->second.Vertex);
					csv += cell;
				}
			}
			csv += '\n';
		}

		return csv;
	}

	/* 导出落盘的路径收尾：系统保存框不保证补后缀 —— 名称没以 ".csv"（大小写不敏感）结尾
	 * 就补上，已有则不重复；空路径（用户取消）原样返回。纯函数：headless 直接断言。 */
	std::string EnsureRecordingExtension(const std::string& path)
	{
		constexpr const char* kExtension = ".csv";
		constexpr size_t kExtensionLength = 4;

		if (path.size() >= kExtensionLength)
		{
			const auto ascii_lower = [](char ch)
				{ return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; };

			bool has_extension = true;
			for (size_t index = 0; index < kExtensionLength; ++index)
			{
				if (ascii_lower(path[path.size() - kExtensionLength + index]) != kExtension[index])
				{
					has_extension = false;
					break;
				}
			}
			if (has_extension)
				return path;
		}

		return path.empty() ? path : path + kExtension;
	}	}
}
