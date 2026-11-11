#include "Pch.h"
#include "TimingChart.h"
#include "EditorUI/EditorIcons.h"
#include "EditorUI/EditorTheme.h"
#include "EditorUI/ImGuiExtensions.h"

namespace Helios
{
	namespace TimingChart
	{
	void SquarifyInto(const std::vector<float>& weights, float x, float y, float width, float height,
		std::vector<ImVec2>& out_pos, std::vector<ImVec2>& out_size)
	{
		const size_t count = weights.size();
		out_pos.assign(count, ImVec2(0.0f, 0.0f));
		out_size.assign(count, ImVec2(0.0f, 0.0f));
		if (count == 0 || width <= 0.0f || height <= 0.0f)
			return;

		float total = 0.0f;
		for (const float weight : weights)
			total += weight > 0.5f ? weight : 0.0f;
		if (total <= 0.0f)
			return;

		std::vector<size_t> order(count);
		for (size_t index = 0; index < count; ++index)
			order[index] = index;
		std::stable_sort(order.begin(), order.end(),
			[&weights](size_t left, size_t right) { return weights[left] > weights[right]; });

		/* 试排 [start..end] 成一行的最差长宽比（squarified 贪心判据）：宽矩形下
		 * 行为左侧竖条、矮矩形下行为顶部横条 */
		const auto worst_aspect = [&weights, &order](size_t start, size_t end, float row_sum,
			float rest, float rw, float rh)
			{
				const float row_area = row_sum / rest * rw * rh;
				float worst = 0.0f;
				for (size_t index = start; index <= end; ++index)
				{
					const float item_area = weights[order[index]] / rest * rw * rh;
					if (rw >= rh)
					{
						const float row_w = ImMax(row_area / rh, 0.0001f);
						const float item_h = ImMax(item_area / row_w, 0.0001f);
						worst = ImMax(worst, ImMax(row_w / item_h, item_h / row_w));
					}
					else
					{
						const float row_h = ImMax(row_area / rw, 0.0001f);
						const float item_w = ImMax(item_area / row_h, 0.0001f);
						worst = ImMax(worst, ImMax(item_w / row_h, row_h / item_w));
					}
				}
				return worst;
			};

		float rx = x;
		float ry = y;
		float rw = width;
		float rh = height;
		float rest = total;
		size_t start = 0;
		while (start < count && rest > 0.0f && rw > 0.5f && rh > 0.5f)
		{
			/* 行贪心：逐项试加、长宽比变差就封行 */
			size_t end = start;
			float row_sum = 0.0f;
			float best_worst = FLT_MAX;
			while (end < count && weights[order[end]] > 0.5f)
			{
				const float candidate_sum = row_sum + weights[order[end]];
				const float worst = worst_aspect(start, end, candidate_sum, rest, rw, rh);
				if (end > start && worst > best_worst)
					break;
				row_sum = candidate_sum;
				best_worst = worst;
				++end;
			}
			if (end == start)
				break;

			/* 铺行：末块补满剩余行程（吸收浮点误差，块之间不留缝） */
			const float row_area = row_sum / rest * rw * rh;
			if (rw >= rh)
			{
				const float row_w = row_area / rh;
				float cursor = ry;
				for (size_t index = start; index < end; ++index)
				{
					float item_h = weights[order[index]] / rest * rw * rh / row_w;
					if (index + 1 == end)
						item_h = ry + rh - cursor;
					out_pos[order[index]] = ImVec2(rx, cursor);
					out_size[order[index]] = ImVec2(row_w, item_h);
					cursor += item_h;
				}
				rx += row_w;
				rw -= row_w;
			}
			else
			{
				const float row_h = row_area / rw;
				float cursor = rx;
				for (size_t index = start; index < end; ++index)
				{
					float item_w = weights[order[index]] / rest * rw * rh / row_h;
					if (index + 1 == end)
						item_w = rx + rw - cursor;
					out_pos[order[index]] = ImVec2(cursor, ry);
					out_size[order[index]] = ImVec2(item_w, row_h);
					cursor += item_w;
				}
				ry += row_h;
				rh -= row_h;
			}

			rest -= row_sum;
			start = end;
		}
	}

	template <typename NodeT, typename BlockT>
	void LayoutTreemapImpl(const NodeT& root, float width, float height, float inset,
		std::vector<BlockT>& out_blocks)
	{
		using Traits = TimingNodeTraits<NodeT>;
		out_blocks.clear();

		std::function<void(const NodeT&, float, float, float, float, int32_t, uint32_t)> layout_node;
		layout_node = [&layout_node, &out_blocks, inset](
			const NodeT& node, float x, float y, float w, float h,
			int32_t parent_block, uint32_t depth)
			{
				if (w < 1.0f || h < 1.0f)
					return;

				BlockT block{};
				block.Node = &node;
				block.X = x;
				block.Y = y;
				block.Width = w;
				block.Height = h;
				block.ParentBlock = parent_block;
				const int32_t block_index = static_cast<int32_t>(out_blocks.size());
				out_blocks.emplace_back(block);

				/* 内缩区域：太深 / 太小不再细分（防病理树） */
				if (depth >= 10)
					return;
				const float inner_x = x + inset;
				const float inner_y = y + inset;
				const float inner_w = w - inset * 2.0f;
				const float inner_h = h - inset * 2.0f;
				if (inner_w < 4.0f || inner_h < 4.0f)
					return;

				std::vector<const NodeT*> children;
				children.reserve(node.Children.size());
				for (const auto& child : node.Children)
				{
					if (!Traits::Label(child)[0])
						continue;
					children.push_back(&child);
				}

				/* 权重表：[0] = 自身 self（无子节点时不铺 —— 节点块本身就是它）+ 子节点 Total */
				const bool has_children = !children.empty();
				std::vector<float> weights;
				weights.reserve(children.size() + 1);
				weights.emplace_back(has_children ? static_cast<float>(Traits::Self(node)) : 0.0f);
				double children_total = 0.0;
				for (const NodeT* child : children)
				{
					weights.emplace_back(static_cast<float>(Traits::Total(*child)));
					children_total += Traits::Total(*child);
				}

				std::vector<ImVec2> item_pos;
				std::vector<ImVec2> item_size;
				SquarifyInto(weights, inner_x, inner_y, inner_w, inner_h, item_pos, item_size);

				/* self 块：有子节点、自身与子耗时都够量才铺 —— 纯自身节点（子耗时趋零）不
				 * 单独铺（节点块本身就是它，不然会多一张同名卡） */
				if (has_children && weights[0] > 0.5f && children_total > 0.5
					&& item_size[0].x > 0.5f && item_size[0].y > 0.5f)
				{
					BlockT self_block{};
					self_block.Node = &node;
					self_block.IsSelf = true;
					self_block.X = item_pos[0].x;
					self_block.Y = item_pos[0].y;
					self_block.Width = item_size[0].x;
					self_block.Height = item_size[0].y;
					self_block.ParentBlock = block_index;
					out_blocks.emplace_back(self_block);
				}

				for (size_t index = 0; index < children.size(); ++index)
				{
					const size_t slot = index + 1;
					if (item_size[slot].x <= 0.5f || item_size[slot].y <= 0.5f)
						continue;
					layout_node(*children[index], item_pos[slot].x, item_pos[slot].y,
						item_size[slot].x, item_size[slot].y, block_index, depth + 1);
				}
			};

		layout_node(root, 0.0f, 0.0f, width, height, -1, 0);
	}

	template <typename NodeT, typename BlockT>
	void LayoutFlameImpl(const NodeT& root, const std::unordered_set<std::string>* collapsed,
		const std::string& root_path, std::vector<BlockT>& out_blocks)
	{
		using Traits = TimingNodeTraits<NodeT>;
		out_blocks.clear();

		std::function<void(const NodeT&, const std::string&, uint32_t, double, double, int32_t)> collect;
		collect = [&collect, &out_blocks, collapsed](
			const NodeT& node, const std::string& path, uint32_t depth,
			double x0, double x1, int32_t parent_block)
			{
				if (Traits::Label(node)[0] == '\0')
					return;

				BlockT block{};
				block.Node = &node;
				block.Depth = depth;
				block.X0 = x0;
				block.X1 = x1;
				block.ParentBlock = parent_block;
				block.Collapsed = collapsed != nullptr && depth > 0 && collapsed->count(path) > 0;
				const int32_t block_index = static_cast<int32_t>(out_blocks.size());
				out_blocks.emplace_back(block);

				/* 收起：子树不再下钻（本块照留、位置不变） */
				if (block.Collapsed)
					return;

				/* 子块：零耗时 / 空名的不铺；宽度按总耗时占比（末块补满吸收浮点误差） */
				std::vector<const NodeT*> ordered;
				ordered.reserve(node.Children.size());
				double children_total = 0.0;
				for (const auto& child : node.Children)
				{
					if (Traits::Label(child)[0] == '\0' || Traits::Total(child) <= 0.0)
						continue;
					children_total += Traits::Total(child);
					ordered.push_back(&child);
				}
				if (children_total <= 0.0)
					return;
				if constexpr (Traits::TimeOrdered)
				{
					std::stable_sort(ordered.begin(), ordered.end(),
						[](const NodeT* left, const NodeT* right)
							{ return Traits::Begin(*left) < Traits::Begin(*right); });
				}

				const double span = x1 - x0;
				double cursor = x0;
				for (size_t index = 0; index < ordered.size(); ++index)
				{
					const NodeT& child = *ordered[index];
					double child_width = span * (Traits::Total(child) / children_total);
					if (index + 1 == ordered.size())
						child_width = x1 - cursor;   /* 末块补满（吸收浮点误差） */
					const std::string child_path = path.empty()
						? Traits::Label(child) : path + "/" + Traits::Label(child);
					collect(child, child_path, depth + 1, cursor, cursor + child_width, block_index);
					cursor += child_width;
				}
			};
		collect(root, root_path, 0, 0.0, 1.0, -1);
	}

		/* 布局入口（CPU / GPU 各一份重载，实现只有一份） */
		void LayoutTreemap(const CPUTimeScopeNode& root, float width, float height,
			float inset, std::vector<TreemapBlock>& out_blocks)
		{
			LayoutTreemapImpl(root, width, height, inset, out_blocks);
		}

		void LayoutTreemap(const GPUTimeScopeNode& root, float width, float height,
			float inset, std::vector<GpuTreemapBlock>& out_blocks)
		{
			LayoutTreemapImpl(root, width, height, inset, out_blocks);
		}

		void LayoutFlame(const CPUTimeScopeNode& root,
			const std::unordered_set<std::string>* collapsed, const std::string& root_path,
			std::vector<FlameBlock>& out_blocks)
		{
			LayoutFlameImpl(root, collapsed, root_path, out_blocks);
		}

		void LayoutFlame(const GPUTimeScopeNode& root,
			const std::unordered_set<std::string>* collapsed, const std::string& root_path,
			std::vector<GpuFlameBlock>& out_blocks)
		{
			LayoutFlameImpl(root, collapsed, root_path, out_blocks);
		}
	}

	namespace TimingChart
	{
	/* 图表条目的耗时文本（火焰图条目标签用）：≥1000us 转 ms（单行要短） */
	void FormatBlockTime(double us, char* out, size_t out_size)
	{
		if (us >= 1000.0)
			std::snprintf(out, out_size, "%.2f ms", us / 1000.0);
		else
			std::snprintf(out, out_size, "%.1f us", us);
	}

	/* 热力强度（0..1）：按 log10(µs) 分档 —— 跨帧稳定、跨函数可比（深色主题越热越亮） */
	float HeatMixForUs(double total_us)
	{
		const double clamped = total_us < 1.0 ? 1.0 : total_us;
		return static_cast<float>(ImClamp((std::log10(clamped) - 2.0) / 2.5, 0.0, 1.0));
	}

	ImU32 HeatFillForUs(double total_us)
	{
		return ImGui::GetColorU32(ImLerp(EditorTheme::Token::CardBg, EditorTheme::Token::Accent,
			HeatMixForUs(total_us)));
	}

	/* 热力色块上的文字：亮底（后半段）用深字、暗底用正文色 */
	ImU32 HeatTextForUs(double total_us)
	{
		return ImGui::GetColorU32(HeatMixForUs(total_us) >= 0.55f
			? EditorTheme::Token::Neutral0 : EditorTheme::Token::Text);
	}

	/* Treemap 视图（CPU / GPU 同款）：面积 = 耗时占比（叶层全是 self 块、铺满）、颜色 = 热力；
	 * 悬停读数、点击下钻（面包屑返回）。下钻路径按 label 链对位 —— 换帧后失效段自动截断。
	 * show_duration = 条目标签在名字下附一行耗时（弹窗 / 最大化用 —— 地方大）。 */
	template <typename NodeT>
	void DrawTreemapImpl(const NodeT& root, ViewState& state,
		float width, float height, bool show_duration)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);
		using Traits = ::Helios::TimingChart::TimingNodeTraits<NodeT>;

		/* 下钻路径解析（按 label 链；换帧 / 换数据源后截断失效段） */
		const NodeT* zoom_root = &root;
		{
			size_t valid = 0;
			const NodeT* node = &root;
			for (; valid < state.TreemapZoom.size(); ++valid)
			{
				const NodeT* next = nullptr;
				for (const auto& child : node->Children)
				{
					if (child.Label == state.TreemapZoom[valid])
					{
						next = &child;
						break;
					}
				}
				if (next == nullptr)
					break;
				node = next;
			}
			if (valid < state.TreemapZoom.size())
				state.TreemapZoom.resize(valid);
			zoom_root = node;
		}

		/* 面包屑（下钻中）：一键回根 + 返回上级 + 路径（逐级可点、超框自动换行 ——
		 * 点第 k 级直接跳回那一层；末级亮显、祖先暗显，" / " 作分隔、换行处省略分隔符） */
		if (!state.TreemapZoom.empty())
		{
			const float back_size = ImGui::GetFrameHeight();
			/* 换行右界（屏幕坐标）：行首 + 整行可用宽（本行起点处的剩余区域宽） */
			const float wrap_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
			if (Icons::IconButton(Icons::Id::ReturnRoot, ImVec2(back_size, back_size), false,
				"Back to root"))
				state.TreemapZoom.clear();
			ImGui::SameLine();
			if (Icons::IconButton(Icons::Id::Return, ImVec2(back_size, back_size), false,
				"Back to parent"))
				state.TreemapZoom.pop_back();

			/* 路径逐级可点 + 超框换行：每段 = 与图标钮同尺寸的 InvisibleButton（命中 /
			 * 悬停 / 点击），悬停底与文字用 draw_list 按几何中线画 —— Selectable 的基线
			 * 偏移会被下一段 SameLine 继承而漂移，不用。 */
			ImDrawList* crumb_draw_list = ImGui::GetWindowDrawList();
			int jump_to = -1;
			for (size_t index = 0; index < state.TreemapZoom.size(); ++index)
			{
				const std::string& label = state.TreemapZoom[index];
				const float spacing = ImGui::GetStyle().ItemSpacing.x;
				const float label_text_width = ImGui::CalcTextSize(label.c_str()).x;
				const float sep_width = ImGui::CalcTextSize("/").x;
				const float text_y_offset = (back_size - ImGui::GetTextLineHeight()) * 0.5f;

				if (index == 0)
				{
					ImGui::SameLine();
				}
				else if (ImGui::GetItemRectMax().x + sep_width + spacing * 2.0f + label_text_width
					<= wrap_right)
				{
					/* 行内继续：分隔符 + 段 */
					ImGui::SameLine();
					ImGui::PushID(static_cast<int>(index));
					ImGui::InvisibleButton("##crumb_sep", ImVec2(sep_width, back_size));
					const ImVec2 sep_min = ImGui::GetItemRectMin();
					crumb_draw_list->AddText(ImVec2(sep_min.x, sep_min.y + text_y_offset),
						ImGui::GetColorU32(EditorTheme::Token::TextDim), "/");
					ImGui::PopID();
					ImGui::SameLine();
				}

				ImGui::PushID(static_cast<int>(index));
				ImGui::InvisibleButton("##crumb_seg", ImVec2(label_text_width, back_size));
				const ImVec2 seg_min = ImGui::GetItemRectMin();
				const bool seg_hovered = ImGui::IsItemHovered();
				if (seg_hovered)
					crumb_draw_list->AddRectFilled(seg_min,
						ImVec2(seg_min.x + label_text_width, seg_min.y + back_size),
						ImGui::GetColorU32(EditorTheme::Token::Neutral5), 3.0f);
				crumb_draw_list->AddText(ImVec2(seg_min.x, seg_min.y + text_y_offset),
					ImGui::GetColorU32(index + 1 == state.TreemapZoom.size()
						? EditorTheme::Token::Text : EditorTheme::Token::TextDim), label.c_str());
				if (ImGui::IsItemClicked())
					jump_to = static_cast<int>(index);
				if (seg_hovered)
					ImGui::SetTooltip("Jump to this level");
				ImGui::PopID();
			}
			if (jump_to >= 0)
				state.TreemapZoom.resize(static_cast<size_t>(jump_to) + 1);
		}

		const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
		const ImVec2 canvas_max(canvas_min.x + width, canvas_min.y + height);
		ImGui::InvisibleButton("##TimingTreemapCanvas", ImVec2(width, height));
		const bool hovered = ImGui::IsItemHovered();
		const bool clicked = ImGui::IsItemClicked();

		std::vector<::Helios::TimingChart::TimingTreemapBlock<NodeT>> blocks;
		::Helios::TimingChart::LayoutTreemap(*zoom_root, width, height, 2.0f, blocks);

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		draw_list->AddRectFilled(canvas_min, canvas_max,
			ImGui::GetColorU32(EditorTheme::Token::Neutral1), 3.0f);

		const ImVec2 mouse = ImGui::GetIO().MousePos;
		int hover_block = -1;

		for (size_t index = 0; index < blocks.size(); ++index)
		{
			const auto& block = blocks[index];
			const ImVec2 min(canvas_min.x + block.X, canvas_min.y + block.Y);
			const ImVec2 max(min.x + block.Width, min.y + block.Height);

			const double value = block.IsSelf ? Traits::Self(*block.Node) : Traits::Total(*block.Node);
			draw_list->AddRectFilled(min, max, HeatFillForUs(value), 1.5f);
			draw_list->AddRect(min, max, ImGui::GetColorU32(EditorTheme::Token::Neutral0),
				1.5f, 0, 1.0f);

			/* 标签：块内自动回车（按块宽折行）—— 垂直只放整行（不切半个字），窄到连一行
			 * 都放不下就不画。真裁剪必须走 draw_list 的 clip：RenderText 的 clip_rect 只做
			 * 粗剔除（整字形在框外才跳过），跨边界的字形会照画出去。 */
			const float line_height = ImGui::GetTextLineHeight();
			const int max_lines = static_cast<int>((block.Height - 4.0f) / line_height);
			if (block.Width >= 34.0f && max_lines >= 1)
			{
				char label[160];
				if (show_duration)
				{
					char time_text[32];
					FormatBlockTime(value, time_text, sizeof(time_text));
					snprintf(label, sizeof(label), "%s\n%s", Traits::Label(*block.Node), time_text);
				}
				else
				{
					snprintf(label, sizeof(label), "%s", Traits::Label(*block.Node));
				}

				const ImVec2 text_pos(min.x + 4.0f, min.y + 2.0f);
				const ImVec2 clip_max(max.x, ImMin(max.y,
					text_pos.y + static_cast<float>(max_lines) * line_height));
				draw_list->PushClipRect(min, clip_max, true);
				ImGui::GetFont()->RenderText(draw_list, ImGui::GetFontSize(), text_pos,
					HeatTextForUs(value), ImVec4(min.x, min.y, clip_max.x, clip_max.y),
					label, nullptr, block.Width - 8.0f, false);
				draw_list->PopClipRect();
			}

			/* 命中（正序后覆盖 = 更深的块优先） */
			if (hovered && mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y)
				hover_block = static_cast<int>(index);
		}

		/* 悬停：高亮描边 + 读数 */
		if (hover_block >= 0)
		{
			const auto& block = blocks[hover_block];
			const ImVec2 min(canvas_min.x + block.X, canvas_min.y + block.Y);
			const ImVec2 max(min.x + block.Width, min.y + block.Height);
			draw_list->AddRect(min, max, ImGui::GetColorU32(EditorTheme::Token::AccentHover),
				1.5f, 0, 2.0f);

			const NodeT& node = *block.Node;
			const double view_total = Traits::Total(*zoom_root) > 0.0 ? Traits::Total(*zoom_root) : 1.0;
			const double share = block.IsSelf ? Traits::Self(node) : Traits::Total(node);
			ImGui::BeginTooltip();
			ImGui::Text("%s%s", Traits::Label(node), block.IsSelf ? " (self)" : "");
			ImGui::Text("total %.2f us   self %.2f us", Traits::Total(node), Traits::Self(node));
			Traits::DrawHoverExtra(node, share / view_total);
			if (!node.Children.empty())
				ImGui::TextDisabled("Click to zoom in");
			ImGui::EndTooltip();
		}

		/* 点击下钻：任何节点都可点进去（含叶子 —— 叶子成为独立视图：单块铺满 + 路径延伸），
		 * 路径 =「显示根 → 命中节点」的 label 链。例外 = 显示根本身（或其 self 块）：空操作
		 *（否则路径尾追加重复段，下一帧解析失败又自截断） */
		if (clicked && hover_block >= 0)
		{
			const auto& block = blocks[hover_block];
			if (block.Node != nullptr && block.Node != zoom_root)
			{
				std::vector<std::string> chain;
				for (int32_t cursor = hover_block; cursor >= 0; cursor = blocks[cursor].ParentBlock)
				{
					if (!blocks[cursor].IsSelf)
						chain.emplace_back(Traits::Label(*blocks[cursor].Node));
				}
				/* chain = [目标 ... 显示根]（逆序）—— 去掉显示根再翻正接上 */
				for (size_t index = chain.size(); index-- > 1;)
					state.TreemapZoom.emplace_back(chain[index - 1]);
			}
		}
	}

	/* 火焰图视图：横向 = 子块铺满父块，纵向 = 调用深度。条目上标耗时；悬停读数、
	 * 单击收起 / 展开；滚轮缩放锚在光标、拖拽平移、右键复位；缩放后右下角浮出缩略图。 */
	template <typename NodeT>
	void DrawFlameImpl(const NodeT& root, ViewState& state,
		float width, float height)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);
		using Traits = ::Helios::TimingChart::TimingNodeTraits<NodeT>;
		using FlameBlockT = ::Helios::TimingChart::TimingFlameBlock<NodeT>;

		/* 布局（收起键 = 显示根起的绝对 label 链） */
		std::vector<FlameBlockT> blocks;
		::Helios::TimingChart::LayoutFlame(root, &state.FlameCollapsed, Traits::Label(root), blocks);

		/* 画布 + 视图窗口映射（归一化 x → 画布 x；缩放 / 平移见下） */
		const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
		const ImVec2 canvas_max(canvas_min.x + width, canvas_min.y + height);
		ImGui::InvisibleButton("##TimingFlameCanvas", ImVec2(width, height));
		const bool hovered = ImGui::IsItemHovered();
		const ImGuiIO& io = ImGui::GetIO();

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		draw_list->AddRectFilled(canvas_min, canvas_max,
			ImGui::GetColorU32(EditorTheme::Token::Neutral1), 3.0f);

		const float plot_width = ImMax(width - 4.0f, 1.0f);
		const double view_w = 1.0 / static_cast<double>(state.FlameZoom);
		const auto view_clamp = [&state, view_w]()
			{
				state.FlamePan = static_cast<float>(
					ImClamp(static_cast<double>(state.FlamePan), 0.0, 1.0 - view_w));
			};
		const auto time_x = [&](double x01)
			{
				return canvas_min.x + 2.0f
					+ static_cast<float>((x01 - static_cast<double>(state.FlamePan))
						/ view_w) * plot_width;
			};
		/* 右下角缩略图几何（仅缩放超出画布时显示；小浮块，不撑满底栏）：
		 * 微型火焰缩画 + 高亮框标当前视图窗口位置 */
		const bool show_mini = state.FlameZoom > 1.0f;
		const float mini_w = ImMin(plot_width * 0.30f, 260.0f);
		const float mini_h = ImMin(64.0f, height * 0.32f);
		const float mini_x1 = canvas_max.x - 8.0f;
		const float mini_y1 = canvas_max.y - 8.0f;
		const float mini_x0 = mini_x1 - mini_w;
		const float mini_y0 = mini_y1 - mini_h;
		const bool mouse_in_mini = show_mini && hovered
			&& io.MousePos.x >= mini_x0 && io.MousePos.y >= mini_y0;
		/* 缩略图内的视图窗口高亮几何（x0, 宽）：位置 = pan 在整段数据里的比例；最小 12px 可抓 */
		const auto mini_thumb = [&]() -> ImVec2
			{
				float thumb_x0 = mini_x0 + static_cast<float>(state.FlamePan) * mini_w;
				float thumb_w = static_cast<float>(view_w) * mini_w;
				if (thumb_w < 12.0f)
					thumb_w = 12.0f;
				return ImVec2(thumb_x0, thumb_w);
			};

		/* 交互（与 FrameGraph 面板同一套）：滚轮缩放锚在光标；拖拽平移（位移过阈值才算拖，
		 * 未过阈值松开 = 单击）；右键复位。SetItemUsingMouseWheel：光标在画布上时吃掉滚轮，
		 * 不让面板窗口跟着滚（imgui 按"上一帧的 hover 标记"拦滚动，所以 hover 帧帧登记）。 */
		if (hovered)
			ImGui::SetItemUsingMouseWheel();
		if (hovered && io.MouseWheel != 0.0f)
		{
			const float new_zoom = static_cast<float>(ImClamp(
				static_cast<double>(state.FlameZoom) * std::pow(1.15, static_cast<double>(io.MouseWheel)),
				1.0, 64.0));
			if (new_zoom != state.FlameZoom)
			{
				const float anchor_ratio = ImClamp(
					(io.MousePos.x - (canvas_min.x + 2.0f)) / plot_width, 0.0f, 1.0f);
				const double anchor = static_cast<double>(state.FlamePan)
					+ static_cast<double>(anchor_ratio) * view_w;
				state.FlameZoom = new_zoom;
				state.FlamePan = static_cast<float>(
					anchor - static_cast<double>(anchor_ratio) / static_cast<double>(state.FlameZoom));
				view_clamp();
			}
		}
		if (ImGui::IsItemActivated())
		{
			state.FlameDraggedDistance = 0.0f;
			state.FlameScrollGrab = mouse_in_mini;
			if (state.FlameScrollGrab)
			{
				/* 点缩略图（高亮框之外）= 跳到该处：视图中心对准点击位置 */
				const ImVec2 thumb = mini_thumb();
				const bool in_thumb = io.MousePos.x >= thumb.x && io.MousePos.x <= thumb.x + thumb.y;
				if (!in_thumb)
				{
					const float track_frac = ImClamp((io.MousePos.x - mini_x0) / mini_w, 0.0f, 1.0f);
					state.FlamePan = static_cast<float>(
						static_cast<double>(track_frac) - view_w * 0.5);
					view_clamp();
				}
			}
		}
		if (ImGui::IsItemActive())
		{
			if (state.FlameScrollGrab)
			{
				/* 拖高亮框：把手跟手（缩略图像素 → 归一化） */
				state.FlamePan = static_cast<float>(static_cast<double>(state.FlamePan)
					+ io.MouseDelta.x / mini_w);
				view_clamp();
			}
			else
			{
				state.FlameDraggedDistance += ImFabs(io.MouseDelta.x);
				if (state.FlameDraggedDistance > 3.0f)
				{
					state.FlamePan = static_cast<float>(static_cast<double>(state.FlamePan)
						- io.MouseDelta.x / plot_width * view_w);
					view_clamp();
				}
			}
		}
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			state.FlameZoom = 1.0f;
			state.FlamePan = 0.0f;
		}

		/* 行高布局：一行一深度（装不下的深度不画） */
		constexpr float kRowHeight = 15.0f;
		constexpr float kRowGap = 1.0f;

		const FlameBlockT* hover_block = nullptr;
		ImVec2 hover_min{ 0.0f, 0.0f };
		ImVec2 hover_max{ 0.0f, 0.0f };

		for (const FlameBlockT& block : blocks)
		{
			const float row_y = canvas_min.y + 2.0f
				+ static_cast<float>(block.Depth) * (kRowHeight + kRowGap);
			if (row_y + kRowHeight > canvas_max.y)
				continue;

			float x0 = time_x(block.X0);
			float x1 = time_x(block.X1);
			if (x1 <= canvas_min.x || x0 >= canvas_max.x)
				continue;   /* 滚出视图窗口的块不画（也不参与命中） */
			if (x1 - x0 < 1.0f)
				x1 = x0 + 1.0f;
			const ImVec2 min(x0, row_y);
			const ImVec2 max(x1, row_y + kRowHeight);

			const double block_total = Traits::Total(*block.Node);
			draw_list->AddRectFilled(min, max, HeatFillForUs(block_total), 1.5f);
			draw_list->AddRect(min, max, ImGui::GetColorU32(EditorTheme::Token::Neutral0),
				1.5f, 0, 1.0f);

			/* 标签：名字左对齐；耗时右对齐（收起中的条目给「＋」让位；两者相撞就不画耗时） */
			if (max.x - min.x >= 44.0f)
			{
				draw_list->PushClipRect(min, max, true);
				draw_list->AddText(ImVec2(min.x + 4.0f, min.y + 1.0f),
					HeatTextForUs(block_total), Traits::Label(*block.Node));
				draw_list->PopClipRect();
			}
			if (max.x - min.x >= 72.0f)
			{
				char time_text[32];
				FormatBlockTime(block_total, time_text, sizeof(time_text));
				const float time_width = ImGui::CalcTextSize(time_text).x;
				const float name_width = ImGui::CalcTextSize(Traits::Label(*block.Node)).x;
				const float right_pad = block.Collapsed ? 16.0f : 4.0f;
				if (min.x + 4.0f + name_width + 10.0f < max.x - right_pad - time_width)
				{
					draw_list->AddText(ImVec2(max.x - right_pad - time_width, min.y + 1.0f),
						HeatTextForUs(block_total), time_text);
				}
			}

			/* 收起中的条目：右端一个小「＋」（点条目 = 展开） */
			if (block.Collapsed)
			{
				const ImU32 fold_color = HeatTextForUs(block_total);
				const float fold_x = max.x - 6.0f;
				const float fold_y = min.y + kRowHeight * 0.5f;
				draw_list->AddLine(ImVec2(fold_x - 3.0f, fold_y), ImVec2(fold_x + 3.0f, fold_y),
					fold_color, 1.4f);
				draw_list->AddLine(ImVec2(fold_x, fold_y - 3.0f), ImVec2(fold_x, fold_y + 3.0f),
					fold_color, 1.4f);
			}

			if (hovered && io.MousePos.x >= min.x && io.MousePos.x < max.x
				&& io.MousePos.y >= min.y && io.MousePos.y < max.y)
			{
				hover_block = &block;
				hover_min = min;
				hover_max = max;
			}
		}

		/* 鼠标竖线：指向位置参考（hover 时全高一根细线） */
		if (hovered)
		{
			draw_list->AddLine(ImVec2(io.MousePos.x, canvas_min.y + 2.0f),
				ImVec2(io.MousePos.x, canvas_max.y - 2.0f),
				ImGui::GetColorU32(EditorTheme::Token::TextLabel), 1.0f);
		}

		/* 右下角缩略图（仅缩放超出画布时）：整幅火焰按深度缩画（同热量配色）+ 当前窗口
		 * 区间高亮（半透明 accent 底 + 亮边；拖它 / 点缩略图滑 —— 命中逻辑见交互段） */
		if (show_mini)
		{
			draw_list->AddRectFilled(ImVec2(mini_x0, mini_y0), ImVec2(mini_x1, mini_y1),
				ImGui::GetColorU32(EditorTheme::Token::Neutral0), 3.0f);
			draw_list->AddRect(ImVec2(mini_x0, mini_y0), ImVec2(mini_x1, mini_y1),
				ImGui::GetColorU32(EditorTheme::Token::Border), 3.0f);

			uint32_t mini_max_depth = 0;
			for (const FlameBlockT& block : blocks)
				mini_max_depth = ImMax(mini_max_depth, block.Depth);
			const float mini_row_h = ImMax(
				(mini_y1 - mini_y0) / static_cast<float>(mini_max_depth + 1), 1.0f);
			for (const FlameBlockT& block : blocks)
			{
				const float block_x0 = mini_x0 + static_cast<float>(block.X0) * mini_w;
				const float block_x1 = mini_x0 + static_cast<float>(block.X1) * mini_w;
				const float block_y = mini_y0 + static_cast<float>(block.Depth) * mini_row_h;
				draw_list->AddRectFilled(ImVec2(block_x0, block_y),
					ImVec2(ImMax(block_x1, block_x0 + 1.0f), ImMin(block_y + mini_row_h, mini_y1)),
					HeatFillForUs(Traits::Total(*block.Node)));
			}

			const ImVec2 thumb = mini_thumb();
			ImVec4 highlight = EditorTheme::Token::AccentHover;
			highlight.w *= 0.28f;
			draw_list->AddRectFilled(ImVec2(thumb.x, mini_y0), ImVec2(thumb.x + thumb.y, mini_y1),
				ImGui::GetColorU32(highlight), 3.0f);
			draw_list->AddRect(ImVec2(thumb.x, mini_y0), ImVec2(thumb.x + thumb.y, mini_y1),
				ImGui::GetColorU32(EditorTheme::Token::AccentHover), 3.0f, 0, 1.5f);
		}

		if (hover_block != nullptr)
		{
			const NodeT& node = *hover_block->Node;
			draw_list->AddRect(hover_min, hover_max,
				ImGui::GetColorU32(EditorTheme::Token::AccentHover), 1.5f, 0, 2.0f);
			const double view_total = Traits::Total(root) > 0.0 ? Traits::Total(root) : 1.0;
			ImGui::BeginTooltip();
			ImGui::Text("%s", Traits::Label(node));
			ImGui::Text("total %.2f us   self %.2f us", Traits::Total(node), Traits::Self(node));
			Traits::DrawHoverExtra(node, Traits::Total(node) / view_total);
			if (!node.Children.empty() && hover_block->ParentBlock >= 0)
				ImGui::TextDisabled(hover_block->Collapsed ? "Click to expand" : "Click to collapse");
			ImGui::TextDisabled("Wheel: zoom · Drag: pan · Right-click: reset");
			ImGui::EndTooltip();
		}

		/* 单击条目（位移未过阈值就松开）：收起 / 展开子树（键 = 「显示根 → 条目」的绝对
		 * label 链；显示根不响应）。滑动条上的按下不算单击（ScrollGrab）。 */
		if (ImGui::IsItemDeactivated() && state.FlameDraggedDistance <= 3.0f
			&& !state.FlameScrollGrab && hover_block != nullptr
			&& !hover_block->Node->Children.empty() && hover_block->ParentBlock >= 0)
		{
			std::string path;
			for (int32_t cursor = static_cast<int32_t>(hover_block - blocks.data()); cursor >= 0;
				cursor = blocks[cursor].ParentBlock)
			{
				if (!path.empty())
					path.insert(0, "/");
				path.insert(0, Traits::Label(*blocks[cursor].Node));
			}
			if (state.FlameCollapsed.count(path) > 0)
				state.FlameCollapsed.erase(path);
			else
				state.FlameCollapsed.insert(path);
		}

		/* 滑动条抓取标记在收起判定之后清掉（判定用其快照） */
		if (ImGui::IsItemDeactivated())
			state.FlameScrollGrab = false;
	}

	/* 绘制入口（CPU / GPU 各一份重载，实现只有一份） */
	void DrawTreemap(const CPUTimeScopeNode& root, ViewState& state, float width, float height,
		bool show_duration)
	{
		DrawTreemapImpl(root, state, width, height, show_duration);
	}

	void DrawTreemap(const GPUTimeScopeNode& root, ViewState& state, float width, float height,
		bool show_duration)
	{
		DrawTreemapImpl(root, state, width, height, show_duration);
	}

	void DrawFlame(const CPUTimeScopeNode& root, ViewState& state, float width, float height)
	{
		DrawFlameImpl(root, state, width, height);
	}

	void DrawFlame(const GPUTimeScopeNode& root, ViewState& state, float width, float height)
	{
		DrawFlameImpl(root, state, width, height);
	}
	}
}
