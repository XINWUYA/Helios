#include "Pch.h"
#include "Svg.h"
#include "Helios/Common/PathUtils.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <tinyxml2.h>

namespace Helios::Svg
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		struct PathToken
		{
			char Command = 0;
			float Number = 0.0f;
			bool IsCommand = false;
		};

		std::vector<PathToken> TokenizePath(const char* data)
		{
			std::vector<PathToken> tokens;
			const char* cursor = data;
			while (*cursor != '\0')
			{
				if ((*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z'))
				{
					tokens.push_back({ *cursor++, 0.0f, true });
					continue;
				}
				if (*cursor == ',' || *cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t')
				{
					++cursor;
					continue;
				}
				char* end = nullptr;
				const float number = std::strtof(cursor, &end);
				if (end == cursor)
				{
					++cursor;
					continue;
				}
				tokens.push_back({ 0, number, false });
				cursor = end;
			}
			return tokens;
		}

		/* 椭圆弧（A 指令）按端点到段的换算展开成折线点 */
		void AppendArc(std::vector<ImVec2>& points, const ImVec2& from,
		               float rx, float ry, float rotation, bool large_arc, bool sweep,
		               const ImVec2& to)
		{
			rx = std::fabs(rx);
			ry = std::fabs(ry);
			if (rx <= 0.0f || ry <= 0.0f || (std::fabs(from.x - to.x) < 0.0001f && std::fabs(from.y - to.y) < 0.0001f))
			{
				points.push_back(to);
				return;
			}

			const float phi = rotation * (kPi / 180.0f);
			const float cos_phi = std::cos(phi);
			const float sin_phi = std::sin(phi);
			const float dx = (from.x - to.x) * 0.5f;
			const float dy = (from.y - to.y) * 0.5f;
			const float x1p = cos_phi * dx + sin_phi * dy;
			const float y1p = -sin_phi * dx + cos_phi * dy;
			const float lambda = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
			if (lambda > 1.0f)
			{
				const float scale = std::sqrt(lambda);
				rx *= scale;
				ry *= scale;
			}

			const float rx2 = rx * rx;
			const float ry2 = ry * ry;
			const float numerator = std::max(0.0f, rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p);
			const float denominator = rx2 * y1p * y1p + ry2 * x1p * x1p;
			const float sign = large_arc == sweep ? -1.0f : 1.0f;
			const float factor = denominator > 0.0f ? sign * std::sqrt(numerator / denominator) : 0.0f;
			const float cxp = factor * rx * y1p / ry;
			const float cyp = factor * -ry * x1p / rx;
			const float cx = cos_phi * cxp - sin_phi * cyp + (from.x + to.x) * 0.5f;
			const float cy = sin_phi * cxp + cos_phi * cyp + (from.y + to.y) * 0.5f;

			const float ux = (x1p - cxp) / rx;
			const float uy = (y1p - cyp) / ry;
			const float vx = (-x1p - cxp) / rx;
			const float vy = (-y1p - cyp) / ry;
			float start = std::atan2(uy, ux);
			float delta = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
			if (!sweep && delta > 0.0f)
				delta -= 2.0f * kPi;
			else if (sweep && delta < 0.0f)
				delta += 2.0f * kPi;

			const int segments = std::max(4, static_cast<int>(std::ceil(std::fabs(delta) * 12.0f / kPi)));
			for (int i = 1; i <= segments; ++i)
			{
				const float angle = start + delta * (static_cast<float>(i) / segments);
				const float x = rx * std::cos(angle);
				const float y = ry * std::sin(angle);
				points.emplace_back(cx + cos_phi * x - sin_phi * y, cy + sin_phi * x + cos_phi * y);
			}
		}

		bool IsPointInsideTriangle(const ImVec2& p, const ImVec2& a, const ImVec2& b, const ImVec2& c)
		{
			const auto edge = [](const ImVec2& p0, const ImVec2& p1, const ImVec2& p2)
			{
				return (p2.x - p0.x) * (p1.y - p0.y) - (p1.x - p0.x) * (p2.y - p0.y);
			};
			const float d1 = edge(p, a, b);
			const float d2 = edge(p, b, c);
			const float d3 = edge(p, c, a);
			const bool negative = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
			const bool positive = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
			return !(negative && positive);
		}

		/* ---- 预览：颜色与样式解析 ---- */

		Paint ParsePaint(const char* value, const Paint& fallback)
		{
			if (value == nullptr)
				return fallback;

			if (std::strcmp(value, "none") == 0)
				return Paint{};
			if (std::strcmp(value, "black") == 0)
				return Paint{ false, IM_COL32(0, 0, 0, 255) };
			if (std::strcmp(value, "white") == 0)
				return Paint{ false, IM_COL32(255, 255, 255, 255) };

			/* #rgb / #rrggbb / #rrggbbaa —— 解析不了就保持继承（不猜颜色） */
			if (value[0] == '#')
			{
				const char* hex = value + 1;
				const size_t digits = std::strlen(hex);
				const auto nibble = [](char c) -> int
				{
					if (c >= '0' && c <= '9') return c - '0';
					if (c >= 'a' && c <= 'f') return c - 'a' + 10;
					if (c >= 'A' && c <= 'F') return c - 'A' + 10;
					return -1;
				};

				unsigned rgba[4] = { 0, 0, 0, 255 };
				if (digits == 3 || digits == 4)
				{
					for (size_t i = 0; i < digits; ++i)
					{
						const int v = nibble(hex[i]);
						if (v < 0)
							return fallback;
						rgba[i] = static_cast<unsigned>(v * 17);
					}
					if (digits == 3)
						rgba[3] = 255;
					return Paint{ false, IM_COL32(rgba[0], rgba[1], rgba[2], rgba[3]) };
				}
				if (digits == 6 || digits == 8)
				{
					for (size_t i = 0; i < digits / 2; ++i)
					{
						const int high = nibble(hex[i * 2]);
						const int low = nibble(hex[i * 2 + 1]);
						if (high < 0 || low < 0)
							return fallback;
						rgba[i] = static_cast<unsigned>(high * 16 + low);
					}
					return Paint{ false, IM_COL32(rgba[0], rgba[1], rgba[2], rgba[3]) };
				}
			}

			return fallback;
		}

		Style InheritStyle(const tinyxml2::XMLElement* element, Style style)
		{
			style.Stroke = ParsePaint(element->Attribute("stroke"), style.Stroke);
			style.Fill = ParsePaint(element->Attribute("fill"), style.Fill);
			if (const char* width = element->Attribute("stroke-width"))
				style.StrokeWidth = std::strtof(width, nullptr);
			if (const char* cap = element->Attribute("stroke-linecap"))
				style.RoundCap = std::strcmp(cap, "round") == 0;
			if (const char* join = element->Attribute("stroke-linejoin"))
				style.RoundJoin = std::strcmp(join, "round") == 0;
			if (const char* dash = element->Attribute("stroke-dasharray"))
			{
				style.DashOn = std::strtof(dash, nullptr);
				const char* separator = dash;
				while (*separator != '\0' && *separator != ',' && *separator != ' ' && *separator != '\t')
					++separator;
				while (*separator == ',' || *separator == ' ' || *separator == '\t')
					++separator;
				style.DashOff = std::strtof(separator, nullptr);
			}
			return style;
		}

		/* 根默认值按 SVG 规范：fill 黑、stroke 无 —— 图标源每个图形都显式声明，
		 * 通用 SVG 也能按"没写就是黑填充"正常显示。 */
		Style RootStyle()
		{
			Style style;
			style.Fill.None = false;
			style.Fill.Color = IM_COL32(0, 0, 0, 255);
			return style;
		}

		void ParseElements(const tinyxml2::XMLElement* element, const Style& inherited, Image& image)
		{
			for (const tinyxml2::XMLElement* child = element->FirstChildElement(); child != nullptr;
				child = child->NextSiblingElement())
			{
				const char* tag = child->Name();
				const Style style = InheritStyle(child, inherited);
				if (std::strcmp(tag, "g") == 0)
				{
					ParseElements(child, style, image);
					continue;
				}

				Shape shape;
				shape.Style = style;
				if (std::strcmp(tag, "path") == 0)
				{
					shape.Kind = Shape::Type::Path;
					const char* data = child->Attribute("d");
					if (data != nullptr)
						shape.Contours = ParsePath(data);
				}
				else if (std::strcmp(tag, "circle") == 0)
				{
					shape.Kind = Shape::Type::Circle;
					shape.X = child->FloatAttribute("cx");
					shape.Y = child->FloatAttribute("cy");
					shape.Radius = child->FloatAttribute("r");
				}
				else if (std::strcmp(tag, "rect") == 0)
				{
					shape.Kind = Shape::Type::Rect;
					shape.X = child->FloatAttribute("x");
					shape.Y = child->FloatAttribute("y");
					shape.Width = child->FloatAttribute("width");
					shape.Height = child->FloatAttribute("height");
					shape.Radius = child->FloatAttribute("rx");
				}
				else
					continue;
				image.Shapes.push_back(std::move(shape));
			}
		}

		/* viewBox 优先，其次退到 width/height（认数值前缀，忽略 px 等单位）；
		 * 两样都没有就画不了 —— 返回 false，调用方按"预览不了"处理。 */
		bool ParseViewport(const tinyxml2::XMLElement* root, Image& image)
		{
			const auto read_numbers = [](const char* text, float* values, int count) -> int
			{
				int read = 0;
				const char* cursor = text;
				while (read < count)
				{
					while (*cursor == ',' || *cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r')
						++cursor;
					char* end = nullptr;
					const float value = std::strtof(cursor, &end);
					if (end == cursor)
						break;
					values[read++] = value;
					cursor = end;
				}
				return read;
			};

			if (const char* box = root->Attribute("viewBox"))
			{
				float values[4] = {};
				if (read_numbers(box, values, 4) == 4 && values[2] > 0.0f && values[3] > 0.0f)
				{
					image.ViewBoxMin = ImVec2(values[0], values[1]);
					image.ViewBoxSize = ImVec2(values[2], values[3]);
					return true;
				}
			}

			float values[2] = {};
			const char* width = root->Attribute("width");
			const char* height = root->Attribute("height");
			if (width != nullptr && height != nullptr
				&& read_numbers(width, values, 1) == 1 && read_numbers(height, values + 1, 1) == 1
				&& values[0] > 0.0f && values[1] > 0.0f)
			{
				image.ViewBoxMin = ImVec2(0.0f, 0.0f);
				image.ViewBoxSize = ImVec2(values[0], values[1]);
				return true;
			}

			return false;
		}

		SharedPtr<const Image> ParseImageFile(const std::filesystem::path& path)
		{
			tinyxml2::XMLDocument document;
			if (document.LoadFile(PathToUtf8(path).c_str()) != tinyxml2::XML_SUCCESS)
				return nullptr;

			const tinyxml2::XMLElement* root = document.RootElement();
			if (root == nullptr || std::strcmp(root->Name(), "svg") != 0)
				return nullptr;

			auto image = CreateSharedPtr<Image>();
			if (!ParseViewport(root, *image))
				return nullptr;

			ParseElements(root, RootStyle(), *image);
			return image;
		}
	}

	std::vector<Contour> ParsePath(const char* data)
	{
		const std::vector<PathToken> tokens = TokenizePath(data);
		std::vector<Contour> contours;
		Contour contour;
		ImVec2 current(0.0f, 0.0f);
		ImVec2 start(0.0f, 0.0f);
		ImVec2 previous_cubic(0.0f, 0.0f);
		ImVec2 previous_quadratic(0.0f, 0.0f);
		char command = 0;
		size_t index = 0;
		bool previous_was_cubic = false;
		bool previous_was_quadratic = false;

		const auto flush = [&]()
		{
			if (contour.Points.size() > 1)
				contours.push_back(std::move(contour));
			contour = Contour{};
		};
		const auto read_number = [&](float& value) -> bool
		{
			if (index >= tokens.size() || tokens[index].IsCommand)
				return false;
			value = tokens[index++].Number;
			return true;
		};
		const auto read_point = [&](bool relative, ImVec2& point) -> bool
		{
			float x = 0.0f, y = 0.0f;
			if (!read_number(x) || !read_number(y))
				return false;
			point = relative ? ImVec2(current.x + x, current.y + y) : ImVec2(x, y);
			return true;
		};

		while (index < tokens.size())
		{
			if (tokens[index].IsCommand)
				command = tokens[index++].Command;
			if (command == 0)
				break;

			const bool relative = command >= 'a' && command <= 'z';
			const char op = relative ? static_cast<char>(command - 'a' + 'A') : command;
			if (op == 'Z')
			{
				current = start;
				contour.Closed = true;
				flush();
				previous_was_cubic = false;
				previous_was_quadratic = false;
				command = 0;
				continue;
			}
			if (index >= tokens.size() || tokens[index].IsCommand)
				continue;

			if (op == 'M' || op == 'L')
			{
				ImVec2 point;
				if (!read_point(relative, point))
					break;
				if (op == 'M')
				{
					flush();
					current = point;
					start = point;
					contour.Points.push_back(point);
					command = relative ? 'l' : 'L';
				}
				else
				{
					current = point;
					contour.Points.push_back(point);
				}
				previous_was_cubic = false;
				previous_was_quadratic = false;
				continue;
			}
			if (op == 'H' || op == 'V')
			{
				float value = 0.0f;
				if (!read_number(value))
					break;
				current = op == 'H' ? ImVec2(relative ? current.x + value : value, current.y)
				                    : ImVec2(current.x, relative ? current.y + value : value);
				contour.Points.push_back(current);
				previous_was_cubic = false;
				previous_was_quadratic = false;
				continue;
			}
			if (op == 'C')
			{
				ImVec2 c1, c2, end;
				if (!read_point(relative, c1) || !read_point(relative, c2) || !read_point(relative, end))
					break;
				const ImVec2 from = current;
				for (int step = 1; step <= 12; ++step)
				{
					const float t = static_cast<float>(step) / 12.0f;
					const float u = 1.0f - t;
					contour.Points.emplace_back(u*u*u*from.x + 3*u*u*t*c1.x + 3*u*t*t*c2.x + t*t*t*end.x,
						u*u*u*from.y + 3*u*u*t*c1.y + 3*u*t*t*c2.y + t*t*t*end.y);
				}
				current = end;
				previous_cubic = c2;
				previous_was_cubic = true;
				previous_was_quadratic = false;
				continue;
			}
			if (op == 'S')
			{
				ImVec2 c1, c2, end;
				c1 = previous_was_cubic
					? ImVec2(2.0f * current.x - previous_cubic.x, 2.0f * current.y - previous_cubic.y)
					: current;
				if (!read_point(relative, c2) || !read_point(relative, end))
					break;
				const ImVec2 from = current;
				for (int step = 1; step <= 12; ++step)
				{
					const float t = static_cast<float>(step) / 12.0f;
					const float u = 1.0f - t;
					contour.Points.emplace_back(u*u*u*from.x + 3*u*u*t*c1.x + 3*u*t*t*c2.x + t*t*t*end.x,
						u*u*u*from.y + 3*u*u*t*c1.y + 3*u*t*t*c2.y + t*t*t*end.y);
				}
				current = end;
				previous_cubic = c2;
				previous_was_cubic = true;
				previous_was_quadratic = false;
				continue;
			}
			if (op == 'Q' || op == 'T')
			{
				ImVec2 control, end;
				if (op == 'Q')
				{
					if (!read_point(relative, control) || !read_point(relative, end))
					break;
				}
				else
				{
					control = previous_was_quadratic
						? ImVec2(2.0f * current.x - previous_quadratic.x, 2.0f * current.y - previous_quadratic.y)
						: current;
					if (!read_point(relative, end))
					break;
				}
				const ImVec2 from = current;
				for (int step = 1; step <= 10; ++step)
				{
					const float t = static_cast<float>(step) / 10.0f;
					const float u = 1.0f - t;
					contour.Points.emplace_back(u*u*from.x + 2*u*t*control.x + t*t*end.x,
						u*u*from.y + 2*u*t*control.y + t*t*end.y);
				}
				current = end;
				previous_quadratic = control;
				previous_was_quadratic = true;
				previous_was_cubic = false;
				continue;
			}
			if (op == 'A')
			{
				float rx = 0.0f, ry = 0.0f, rotation = 0.0f, large = 0.0f, sweep = 0.0f;
				ImVec2 end;
				if (!read_number(rx) || !read_number(ry) || !read_number(rotation)
					|| !read_number(large) || !read_number(sweep) || !read_point(relative, end))
					break;
				AppendArc(contour.Points, current, rx, ry, rotation, large != 0.0f, sweep != 0.0f, end);
				current = end;
				previous_was_cubic = false;
				previous_was_quadratic = false;
				continue;
			}
			break;
		}
		flush();
		return contours;
	}

	void FillPolygon(ImDrawList* draw_list, const std::vector<ImVec2>& points, ImU32 color)
	{
		if (points.size() < 3)
			return;
		std::vector<int> indices(points.size());
		for (size_t i = 0; i < indices.size(); ++i)
			indices[i] = static_cast<int>(i);
		float area = 0.0f;
		for (size_t i = 0; i < points.size(); ++i)
		{
			const ImVec2& a = points[i];
			const ImVec2& b = points[(i + 1) % points.size()];
			area += a.x * b.y - b.x * a.y;
		}
		const float orientation = area >= 0.0f ? 1.0f : -1.0f;
		int guard = static_cast<int>(points.size() * points.size());
		while (indices.size() > 3 && guard-- > 0)
		{
			bool clipped = false;
			for (size_t i = 0; i < indices.size(); ++i)
			{
				const int ia = indices[(i + indices.size() - 1) % indices.size()];
				const int ib = indices[i];
				const int ic = indices[(i + 1) % indices.size()];
				const ImVec2& a = points[ia];
				const ImVec2& b = points[ib];
				const ImVec2& c = points[ic];
				const float cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
				if (cross * orientation <= 0.00001f)
					continue;
				bool contains = false;
				for (int index : indices)
				{
					if (index == ia || index == ib || index == ic)
						continue;
					if (IsPointInsideTriangle(points[index], a, b, c))
					{
						contains = true;
						break;
					}
				}
				if (contains)
					continue;
				draw_list->AddTriangleFilled(a, b, c, color);
				indices.erase(indices.begin() + static_cast<ptrdiff_t>(i));
				clipped = true;
				break;
			}
			if (!clipped)
				break;
		}
		if (indices.size() == 3)
			draw_list->AddTriangleFilled(points[indices[0]], points[indices[1]], points[indices[2]], color);
	}

	void StrokePolyline(ImDrawList* draw_list, const std::vector<ImVec2>& points, bool closed,
		ImU32 color, float width, float dash_on, float dash_off, bool round_cap, bool round_join)
	{
		if (points.size() < 2 || width <= 0.0f)
			return;

		const bool dashed = dash_on > 0.0f && dash_off > 0.0f;
		const size_t segment_count = points.size() - 1 + (closed ? 1 : 0);

		if (!dashed)
		{
			draw_list->PathClear();
			for (const ImVec2& point : points)
				draw_list->PathLineTo(point);
			draw_list->PathStroke(color, closed ? ImDrawFlags_Closed : 0, width);
		}

		for (size_t i = 0; i < segment_count; ++i)
		{
			const ImVec2& a = points[i];
			const ImVec2& b = points[(i + 1) % points.size()];
			if (!dashed)
				continue;

			const float dx = b.x - a.x;
			const float dy = b.y - a.y;
			const float length = std::sqrt(dx * dx + dy * dy);
			const float cycle = dash_on + dash_off;
			if (length <= 0.001f || cycle <= 0.001f)
				continue;
			for (float offset = 0.0f; offset < length; offset += cycle)
			{
				const float end = std::min(offset + dash_on, length);
				const ImVec2 p0(a.x + dx * (offset / length), a.y + dy * (offset / length));
				const ImVec2 p1(a.x + dx * (end / length), a.y + dy * (end / length));
				draw_list->AddLine(p0, p1, color, width);
				if (round_cap)
				{
					const float radius = width * 0.5f;
					draw_list->AddCircleFilled(p0, radius, color, 8);
					draw_list->AddCircleFilled(p1, radius, color, 8);
				}
			}
		}

		if (!dashed && round_cap && !closed)
		{
			const float radius = width * 0.5f;
			draw_list->AddCircleFilled(points.front(), radius, color, 8);
			draw_list->AddCircleFilled(points.back(), radius, color, 8);
		}
		if (round_join)
		{
			const float radius = width * 0.5f;
			const size_t point_count = closed ? points.size() : points.size() - 2;
			for (size_t i = 0; i < point_count; ++i)
			{
				const size_t point_index = closed ? i : i + 1;
				draw_list->AddCircleFilled(points[point_index], radius, color, 8);
			}
		}
	}

	SharedPtr<const Image> GetImage(const std::filesystem::path& path)
	{
		/* 只服务 UI 帧（资源浏览器与属性面板都在同一线程取图）；
		 * 缓存按文件写入时间失效 —— 外部改了文件，下一帧就重新解析。 */
		struct CacheEntry
		{
			std::filesystem::file_time_type WriteTime{};
			SharedPtr<const Image> Parsed;
		};
		static std::unordered_map<std::string, CacheEntry> s_Cache;

		std::error_code error;
		const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
		if (error)
		{
			s_Cache.erase(PathToUtf8(path));
			return nullptr;
		}

		const std::string key = PathToUtf8(path);
		const auto found = s_Cache.find(key);
		if (found != s_Cache.end() && found->second.WriteTime == write_time)
			return found->second.Parsed;

		SharedPtr<const Image> parsed = ParseImageFile(path);
		s_Cache[key] = CacheEntry{ write_time, parsed };
		return parsed;
	}

	void DrawImage(ImDrawList* draw_list, const Image& image, const ImVec2& rect_min, const ImVec2& rect_max)
	{
		if (draw_list == nullptr || image.ViewBoxSize.x <= 0.0f || image.ViewBoxSize.y <= 0.0f || image.Shapes.empty())
			return;

		/* contain + 居中：viewBox 直接映到屏幕，两边 y 都朝下 */
		const float scale = ImMin((rect_max.x - rect_min.x) / image.ViewBoxSize.x,
		                          (rect_max.y - rect_min.y) / image.ViewBoxSize.y);
		if (scale <= 0.0f)
			return;

		const ImVec2 center((rect_min.x + rect_max.x) * 0.5f, (rect_min.y + rect_max.y) * 0.5f);
		const ImVec2 view_center(image.ViewBoxMin.x + image.ViewBoxSize.x * 0.5f,
		                         image.ViewBoxMin.y + image.ViewBoxSize.y * 0.5f);
		const auto map = [&](const ImVec2& point)
		{
			return ImVec2(center.x + (point.x - view_center.x) * scale,
				center.y + (point.y - view_center.y) * scale);
		};

		for (const Shape& shape : image.Shapes)
		{
			const bool filled = !shape.Style.Fill.None;
			const bool stroked = !shape.Style.Stroke.None;
			const float width = ImMax(shape.Style.StrokeWidth * scale, 1.0f);
			const float dash_on = shape.Style.DashOn * scale;
			const float dash_off = shape.Style.DashOff * scale;

			if (shape.Kind == Shape::Type::Path)
			{
				std::vector<ImVec2> points;
				for (const Contour& contour : shape.Contours)
				{
					if (contour.Points.size() < 2)
						continue;

					points.clear();
					points.reserve(contour.Points.size());
					for (const ImVec2& point : contour.Points)
						points.push_back(map(point));

					/* 图标源的填充面常不带 Z：有填充就按闭合画（与图标渲染同一口径） */
					const bool closed = contour.Closed || filled;

					if (filled && points.size() >= 3)
						FillPolygon(draw_list, points, shape.Style.Fill.Color);

					if (stroked)
						StrokePolyline(draw_list, points, closed, shape.Style.Stroke.Color, width,
							dash_on, dash_off, shape.Style.RoundCap, shape.Style.RoundJoin);
				}
				continue;
			}

			if (shape.Kind == Shape::Type::Circle)
			{
				const ImVec2 circle_center = map(ImVec2(shape.X, shape.Y));
				const float radius = shape.Radius * scale;
				if (filled && radius > 0.0f)
					draw_list->AddCircleFilled(circle_center, radius, shape.Style.Fill.Color);

				if (stroked && radius > 0.0f)
				{
					if (dash_on > 0.0f && dash_off > 0.0f)
					{
						const float circumference = 2.0f * kPi * radius;
						const float cycle = dash_on + dash_off;
						if (cycle > 0.001f)
						{
							for (float offset = 0.0f; offset < circumference; offset += cycle)
							{
								const float a0 = offset / radius;
								const float a1 = std::min(offset + dash_on, circumference) / radius;
								draw_list->PathArcTo(circle_center, radius, a0, a1, 4);
								draw_list->PathStroke(shape.Style.Stroke.Color, 0, width);
							}
						}
					}
					else
						draw_list->AddCircle(circle_center, radius, shape.Style.Stroke.Color, 0, width);
				}
				continue;
			}

			const ImVec2 min = map(ImVec2(shape.X, shape.Y));
			const ImVec2 max = map(ImVec2(shape.X + shape.Width, shape.Y + shape.Height));
			const float rounding = shape.Radius * scale;
			if (filled)
				draw_list->AddRectFilled(min, max, shape.Style.Fill.Color, rounding);
			if (stroked)
				draw_list->AddRect(min, max, shape.Style.Stroke.Color, rounding, 0, width);
		}
	}
}
