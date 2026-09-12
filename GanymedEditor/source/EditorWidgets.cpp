#include "EditorWidgets.h"
#include "EditorFonts.h"
#include "EditorIcons.h"
#include "EditorTheme.h"

#include <imgui/imgui_internal.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace GanymedE::EditorUI {

	namespace {

		constexpr float kHitRadius = 8.0f;
		constexpr float kKeyRadius = 4.5f;
		constexpr float kTimeMin = 0.0f;
		constexpr float kTimeMax = 1.0f;

		ImVec2 CanvasToScreen(const ImVec2& canvas, const ImVec2& size, float t, float v,
			float yMin, float yMax)
		{
			const float x = canvas.x + (t - kTimeMin) / (kTimeMax - kTimeMin) * size.x;
			const float y = canvas.y + (1.0f - (v - yMin) / (yMax - yMin)) * size.y;
			return { x, y };
		}

		void ScreenToCurve(const ImVec2& canvas, const ImVec2& size, const ImVec2& mouse,
			float yMin, float yMax, float& t, float& v)
		{
			t = kTimeMin + (mouse.x - canvas.x) / size.x * (kTimeMax - kTimeMin);
			v = yMin + (1.0f - (mouse.y - canvas.y) / size.y) * (yMax - yMin);
			t = std::clamp(t, kTimeMin, kTimeMax);
			v = std::clamp(v, yMin, yMax);
		}

		int HitKey(const ImVec2& canvas, const ImVec2& size, const ImVec2& mouse,
			float yMin, float yMax, const std::vector<FloatKey>& keys)
		{
			int best = -1;
			float bestDist = kHitRadius;
			for (int i = 0; i < (int)keys.size(); i++)
			{
				const ImVec2 p = CanvasToScreen(canvas, size, keys[i].Time, keys[i].Value, yMin, yMax);
				const float dx = mouse.x - p.x;
				const float dy = mouse.y - p.y;
				const float d = std::sqrt(dx * dx + dy * dy);
				if (d <= bestDist)
				{
					bestDist = d;
					best = i;
				}
			}
			return best;
		}

		int HitGradientKey(const ImVec2& canvas, float width, float height, const ImVec2& mouse,
			const std::vector<ColorKey>& keys)
		{
			int best = -1;
			float bestDist = kHitRadius;
			const float midY = canvas.y + height * 0.5f;
			for (int i = 0; i < (int)keys.size(); i++)
			{
				const float x = canvas.x + std::clamp(keys[i].Time, kTimeMin, kTimeMax) * width;
				const float dx = mouse.x - x;
				const float dy = mouse.y - midY;
				const float d = std::sqrt(dx * dx + dy * dy);
				if (d <= bestDist)
				{
					bestDist = d;
					best = i;
				}
			}
			return best;
		}

		int FindFloatKey(const FloatCurve& curve, float t, float v)
		{
			const auto& keys = curve.Keys();
			int best = 0;
			float bestD = 1.0e30f;
			for (int i = 0; i < (int)keys.size(); i++)
			{
				const float d = std::abs(keys[i].Time - t) + std::abs(keys[i].Value - v);
				if (d < bestD)
				{
					bestD = d;
					best = i;
				}
			}
			return best;
		}

		int FindColorKey(const ColorGradient& gradient, float t)
		{
			const auto& keys = gradient.Keys();
			int best = 0;
			float bestD = 1.0e30f;
			for (int i = 0; i < (int)keys.size(); i++)
			{
				const float d = std::abs(keys[i].Time - t);
				if (d < bestD)
				{
					bestD = d;
					best = i;
				}
			}
			return best;
		}

		ImU32 ColorU32(const glm::vec4& c)
		{
			return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
		}

	}

	bool CurveEditor(const char* label, FloatCurve& curve, float minY, float maxY, ImVec2 size)
	{
		ImGui::PushID(label);
		ImGui::TextUnformatted(label);

		if (size.x <= 0.0f)
			size.x = ImGui::GetContentRegionAvail().x;
		if (size.y <= 0.0f)
			size.y = 80.0f;

		float yMin = minY;
		float yMax = maxY;
		for (const FloatKey& key : curve.Keys())
		{
			yMin = std::min(yMin, key.Value);
			yMax = std::max(yMax, key.Value);
		}
		if (yMax - yMin < 1.0e-4f)
			yMax = yMin + 1.0f;

		const ImVec2 canvas = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##canvas", size);
		const bool hovered = ImGui::IsItemHovered();
		const bool active = ImGui::IsItemActive();
		const ImVec2 mouse = ImGui::GetIO().MousePos;

		ImGuiStorage* storage = ImGui::GetStateStorage();
		const ImGuiID dragId = ImGui::GetID("drag");
		int dragIndex = storage->GetInt(dragId, -1);

		bool edited = false;
		const auto& keys = curve.Keys();

		if (active)
		{
			if (dragIndex < 0)
			{
				// Acquire only this frame. Applying ScreenToCurve here would snap the key
				// to the cursor on click-off-center and mint an undo command for a
				// grab-and-release with no motion.
				dragIndex = HitKey(canvas, size, mouse, yMin, yMax, keys);
				storage->SetInt(dragId, dragIndex);
			}
			else if (dragIndex < (int)keys.size())
			{
				float t, v;
				ScreenToCurve(canvas, size, mouse, yMin, yMax, t, v);
				const FloatKey before = keys[(size_t)dragIndex];
				if (std::abs(before.Time - t) > 1.0e-5f || std::abs(before.Value - v) > 1.0e-5f)
				{
					curve.SetKey((size_t)dragIndex, t, v);
					dragIndex = FindFloatKey(curve, t, v);
					storage->SetInt(dragId, dragIndex);
					edited = true;
				}
			}
		}
		else
		{
			storage->SetInt(dragId, -1);
			dragIndex = -1;
		}

		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			if (HitKey(canvas, size, mouse, yMin, yMax, curve.Keys()) < 0)
			{
				float t, v;
				ScreenToCurve(canvas, size, mouse, yMin, yMax, t, v);
				curve.AddKey(t, v);
				edited = true;
			}
		}

		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			const int hit = HitKey(canvas, size, mouse, yMin, yMax, curve.Keys());
			if (hit >= 0 && curve.RemoveKey((size_t)hit))
				edited = true;
		}

		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 canvasMax(canvas.x + size.x, canvas.y + size.y);
		const EditorTheme& theme = Theme();
		draw->PushClipRect(canvas, canvasMax, true);
		draw->AddRectFilled(canvas, canvasMax, theme.ChromeBg);
		draw->AddRect(canvas, canvasMax, theme.GrabBg);

		for (int i = 0; i <= 4; i++)
		{
			const float t = kTimeMin + (kTimeMax - kTimeMin) * (float)i / 4.0f;
			const float x = CanvasToScreen(canvas, size, t, yMin, yMin, yMax).x;
			draw->AddLine(ImVec2(x, canvas.y), ImVec2(x, canvasMax.y), WithAlpha(theme.GrabBg, 0.55f));
		}
		if (0.0f >= yMin && 0.0f <= yMax)
		{
			const float y = CanvasToScreen(canvas, size, 0.0f, 0.0f, yMin, yMax).y;
			draw->AddLine(ImVec2(canvas.x, y), ImVec2(canvasMax.x, y), WithAlpha(theme.FieldMixed, 0.40f));
		}

		const auto& drawn = curve.Keys();
		for (size_t i = 1; i < drawn.size(); i++)
		{
			const ImVec2 a = CanvasToScreen(canvas, size, drawn[i - 1].Time, drawn[i - 1].Value, yMin, yMax);
			const ImVec2 b = CanvasToScreen(canvas, size, drawn[i].Time, drawn[i].Value, yMin, yMax);
			draw->AddLine(a, b, theme.FieldMixed, 1.5f);
		}

		const int hoverIndex = hovered ? HitKey(canvas, size, mouse, yMin, yMax, drawn) : -1;
		for (int i = 0; i < (int)drawn.size(); i++)
		{
			const ImVec2 p = CanvasToScreen(canvas, size, drawn[i].Time, drawn[i].Value, yMin, yMax);
			const bool hot = i == dragIndex || i == hoverIndex;
			const ImU32 keyFill = hot
				? ImGui::ColorConvertFloat4ToU32(ColorHover(theme.FieldMixed))
				: theme.FieldMixed;
			draw->AddCircleFilled(p, hot ? kKeyRadius + 1.5f : kKeyRadius, keyFill);
			draw->AddCircle(p, hot ? kKeyRadius + 1.5f : kKeyRadius, theme.ChromeBg);
		}
		draw->PopClipRect();

		if (hovered && !active)
			ImGui::SetTooltip("Double-click to add a key, right-click to delete");

		ImGui::PopID();
		return edited;
	}

	bool GradientEditor(const char* label, ColorGradient& gradient)
	{
		ImGui::PushID(label);
		ImGui::TextUnformatted(label);

		const float width = ImGui::GetContentRegionAvail().x;
		const float height = 22.0f;
		const ImVec2 canvas = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##bar", ImVec2(width, height));
		const bool hovered = ImGui::IsItemHovered();
		const bool active = ImGui::IsItemActive();
		const ImVec2 mouse = ImGui::GetIO().MousePos;

		ImGuiStorage* storage = ImGui::GetStateStorage();
		const ImGuiID dragId = ImGui::GetID("drag");
		const ImGuiID selId = ImGui::GetID("sel");
		int dragIndex = storage->GetInt(dragId, -1);
		int selected = storage->GetInt(selId, 0);

		bool edited = false;
		const auto& keys = gradient.Keys();
		if (selected < 0 || selected >= (int)keys.size())
			selected = 0;

		if (active)
		{
			if (dragIndex < 0)
			{
				dragIndex = HitGradientKey(canvas, width, height, mouse, keys);
				storage->SetInt(dragId, dragIndex);
				if (dragIndex >= 0)
				{
					selected = dragIndex;
					storage->SetInt(selId, selected);
				}
			}
			else if (dragIndex < (int)keys.size())
			{
				float t = (mouse.x - canvas.x) / width;
				t = std::clamp(t, kTimeMin, kTimeMax);
				const ColorKey before = keys[(size_t)dragIndex];
				if (std::abs(before.Time - t) > 1.0e-5f)
				{
					gradient.SetKey((size_t)dragIndex, t, before.Value);
					dragIndex = FindColorKey(gradient, t);
					selected = dragIndex;
					storage->SetInt(dragId, dragIndex);
					storage->SetInt(selId, selected);
					edited = true;
				}
			}
		}
		else
		{
			storage->SetInt(dragId, -1);
			dragIndex = -1;
		}

		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			const int hit = HitGradientKey(canvas, width, height, mouse, gradient.Keys());
			if (hit >= 0)
			{
				selected = hit;
				storage->SetInt(selId, selected);
			}
		}

		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			if (HitGradientKey(canvas, width, height, mouse, gradient.Keys()) < 0)
			{
				const float t = std::clamp((mouse.x - canvas.x) / width, kTimeMin, kTimeMax);
				gradient.AddKey(t, gradient.Sample(t));
				selected = FindColorKey(gradient, t);
				storage->SetInt(selId, selected);
				edited = true;
			}
		}

		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			const int hit = HitGradientKey(canvas, width, height, mouse, gradient.Keys());
			if (hit >= 0 && gradient.RemoveKey((size_t)hit))
			{
				edited = true;
				selected = std::min(selected, (int)gradient.Keys().size() - 1);
				storage->SetInt(selId, selected);
			}
		}

		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 canvasMax(canvas.x + width, canvas.y + height);
		draw->PushClipRect(canvas, canvasMax, true);
		const int samples = std::max(2, (int)width);
		for (int i = 0; i < samples; i++)
		{
			const float t0 = (float)i / (float)samples;
			const float t1 = (float)(i + 1) / (float)samples;
			const glm::vec4 c = gradient.Sample(t0);
			draw->AddRectFilled(
				ImVec2(canvas.x + t0 * width, canvas.y),
				ImVec2(canvas.x + t1 * width, canvasMax.y),
				ColorU32(c));
		}
		draw->AddRect(canvas, canvasMax, Theme().GrabBg);

		const auto& drawn = gradient.Keys();
		const int hoverIndex = hovered ? HitGradientKey(canvas, width, height, mouse, drawn) : -1;
		for (int i = 0; i < (int)drawn.size(); i++)
		{
			const float x = canvas.x + std::clamp(drawn[i].Time, kTimeMin, kTimeMax) * width;
			const float y = canvas.y + height * 0.5f;
			const bool hot = i == dragIndex || i == hoverIndex || i == selected;
			draw->AddCircleFilled(ImVec2(x, y), hot ? kKeyRadius + 1.5f : kKeyRadius, ColorU32(drawn[i].Value));
			draw->AddCircle(ImVec2(x, y), hot ? kKeyRadius + 1.5f : kKeyRadius,
				hot ? Theme().TextPrimary : Theme().ChromeBg);
		}
		draw->PopClipRect();

		if (selected >= 0 && selected < (int)drawn.size())
		{
			glm::vec4 color = drawn[(size_t)selected].Value;
			if (ImGui::ColorEdit4("Key Color", &color.x))
			{
				gradient.SetKey((size_t)selected, drawn[(size_t)selected].Time, color);
				edited = true;
			}
		}

		if (hovered && !active)
			ImGui::SetTooltip("Double-click to add a key, right-click to delete");

		ImGui::PopID();
		return edited;
	}

	// Returns true when a widget in this row actually edited `values` - the ground truth the
	// undo commit boundary is built on. It used to return void, which meant the caller had to
	// diff the value instead, and a value diff cannot tell a real edit from a float that came
	// back changed through a degrees/radians round-trip.
	bool DrawVec3Control(const std::string& label, glm::vec3& values, float resetValue,
		float columnWidth, float speed)
	{
		bool edited = false;
		ImFont* headerFont = EditorFonts::Header();

		ImGui::PushID(label.c_str());

		ImGui::Columns(2);
		ImGui::SetColumnWidth(0, columnWidth);
		ImGui::Text(label.c_str());
		ImGui::NextColumn();

		ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ 0, 0 });

		float lineHeight = GImGui->Font->FontSize + GImGui->Style.FramePadding.y * 2.0f;
		ImVec2 buttonSize = { lineHeight + 3.0f, lineHeight };

		auto pushAxis = [](ImU32 c)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, Color(c));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorHover(c));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, Color(c));
		};
		auto pushHeader = [headerFont]()
		{
			if (headerFont)
				ImGui::PushFont(headerFont);
		};
		auto popHeader = [headerFont]()
		{
			if (headerFont)
				ImGui::PopFont();
		};

		pushAxis(Theme().AxisX);
		pushHeader();
		if (ImGui::Button("X", buttonSize))
		{
			values.x = resetValue;
			edited = true;
		}

		popHeader();
		ImGui::PopStyleColor(3);

		ImGui::SameLine();
		edited |= ImGui::DragFloat("##X", &values.x, speed, 0.0f, 0.0f, "%.2f");
		ImGui::PopItemWidth();
		ImGui::SameLine();

		pushAxis(Theme().AxisY);
		pushHeader();
		if (ImGui::Button("Y", buttonSize))
		{
			values.y = resetValue;
			edited = true;
		}

		popHeader();
		ImGui::PopStyleColor(3);

		ImGui::SameLine();
		edited |= ImGui::DragFloat("##Y", &values.y, speed, 0.0f, 0.0f, "%.2f");
		ImGui::PopItemWidth();
		ImGui::SameLine();

		pushAxis(Theme().AxisZ);
		pushHeader();
		if (ImGui::Button("Z", buttonSize))
		{
			values.z = resetValue;
			edited = true;
		}

		popHeader();
		ImGui::PopStyleColor(3);

		ImGui::SameLine();
		edited |= ImGui::DragFloat("##Z", &values.z, speed, 0.0f, 0.0f, "%.2f");
		ImGui::PopItemWidth();

		ImGui::PopStyleVar();

		ImGui::Columns(1);

		ImGui::PopID();
		return edited;
	}

	bool IconButton(const char* icon, const char* tooltip, bool active)
	{
		const ImVec2 size(24.0f, 24.0f);
		ImGui::PushID(icon);
		const bool clicked = ImGui::InvisibleButton("##ib", size);
		const bool hovered = ImGui::IsItemHovered();
		const bool held = ImGui::IsItemActive();

		const EditorTheme& theme = Theme();
		ImU32 fill = 0;
		ImU32 glyph = theme.TextPrimary;
		if (active || held)
		{
			fill = held ? theme.AccentActive : theme.Accent;
			glyph = theme.TextOnAccent;
		}
		else if (hovered)
		{
			fill = theme.AccentHover;
			glyph = theme.TextOnAccent;
		}

		const ImVec2 p0 = ImGui::GetItemRectMin();
		const ImVec2 p1 = ImGui::GetItemRectMax();
		ImDrawList* draw = ImGui::GetWindowDrawList();
		if (fill)
			draw->AddRectFilled(p0, p1, fill);

		const ImVec2 textSize = ImGui::CalcTextSize(icon);
		draw->AddText(
			ImVec2(p0.x + (size.x - textSize.x) * 0.5f, p0.y + (size.y - textSize.y) * 0.5f),
			glyph, icon);

		if (tooltip)
			ImGui::SetItemTooltip("%s", tooltip);

		ImGui::PopID();
		return clicked;
	}

	void ToolbarSeparator()
	{
		constexpr float kHeight = 24.0f;
		constexpr float kMargin = 4.0f;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(kMargin * 2.0f + 1.0f, kHeight));
		ImGui::GetWindowDrawList()->AddRectFilled(
			ImVec2(p.x + kMargin, p.y + kMargin),
			ImVec2(p.x + kMargin + 1.0f, p.y + kHeight - kMargin),
			Theme().Border);
	}

	bool BeginPanel(const char* name, bool* open, ImGuiWindowFlags flags)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		const bool visible = ImGui::Begin(name, open, flags);
		ImGui::PopStyleVar();
		return visible;
	}

	void EndPanel()
	{
		ImGui::End();
	}

	void BeginPanelBody()
	{
		// Parent BeginPanel captured WindowPadding 0. The child reads the live style
		// (ImGui's 8,8 — ApplyTheme does not change it) and AlwaysUseWindowPadding
		// applies it even without a child border.
		ImGui::BeginChild("##PanelBody", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
	}

	void EndPanelBody()
	{
		ImGui::EndChild();
	}

	bool PanelToolbarRow(const char* id, float height)
	{
		if (height <= 0.0f)
			height = 44.0f;

		ImGui::PushStyleColor(ImGuiCol_ChildBg, Color(Theme().SurfaceBg));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
		const bool open = ImGui::BeginChild(id, ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);

		const float y = (ImGui::GetContentRegionAvail().y - 24.0f) * 0.5f;
		if (y > 0.0f)
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + y);
		return open;
	}

	void EndPanelToolbarRow()
	{
		const ImVec2 wp = ImGui::GetWindowPos();
		const ImVec2 ws = ImGui::GetWindowSize();
		ImGui::GetWindowDrawList()->AddLine(
			ImVec2(wp.x, wp.y + ws.y - 1.0f),
			ImVec2(wp.x + ws.x, wp.y + ws.y - 1.0f),
			Theme().Border);
		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();
	}

	bool SearchField(const char* id, char* buf, size_t bufSize, const char* hint)
	{
		const EditorTheme& theme = Theme();
		ImGui::PushID(id);

		const float h = ImGui::GetFrameHeight();
		ImGuiContext& g = *GImGui;
		const float width = (g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth)
			? ImGui::CalcItemWidth()
			: ImGui::GetContentRegionAvail().x;
		const bool hasText = buf && buf[0] != '\0';
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(width, h));
		const ImVec2 after = ImGui::GetCursorScreenPos();

		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + h), theme.SurfaceSunken);

		const ImVec2 searchSize = ImGui::CalcTextSize(ICON_LC_SEARCH);
		draw->AddText(
			ImVec2(p0.x + 6.0f, p0.y + (h - searchSize.y) * 0.5f),
			theme.TextDim, ICON_LC_SEARCH);

		const float clearSlot = hasText ? h : 0.0f;
		const float textX = p0.x + 6.0f + searchSize.x + 4.0f;
		const float textW = ImMax(8.0f, width - (textX - p0.x) - clearSlot - 4.0f);

		ImGui::SetCursorScreenPos(ImVec2(textX, p0.y));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_TextDisabled, Color(theme.TextDim));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
		ImGui::SetNextItemWidth(textW);
		bool changed = ImGui::InputTextWithHint("##s", hint ? hint : "Search...", buf, bufSize);
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(4);

		if (hasText)
		{
			ImGui::SetCursorScreenPos(ImVec2(p0.x + width - h, p0.y));
			if (ImGui::InvisibleButton("##clear", ImVec2(h, h)))
			{
				buf[0] = '\0';
				changed = true;
			}
			const ImVec2 ts = ImGui::CalcTextSize(ICON_LC_X);
			const ImVec2 r0 = ImGui::GetItemRectMin();
			const ImU32 glyph = ImGui::IsItemHovered() ? theme.TextPrimary : theme.TextDim;
			draw->AddText(
				ImVec2(r0.x + (h - ts.x) * 0.5f, r0.y + (h - ts.y) * 0.5f),
				glyph, ICON_LC_X);
			ImGui::SetItemTooltip("Clear");
		}

		ImGui::SetCursorScreenPos(after);
		ImGui::PopID();
		return changed;
	}

	void ColumnHeaderRow(std::initializer_list<ColumnSpec> columns)
	{
		const EditorTheme& theme = Theme();
		const float height = theme.ColumnHeaderHeight;
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		const float width = ImGui::GetContentRegionAvail().x;
		ImGui::GetWindowDrawList()->AddRectFilled(
			p0, ImVec2(p0.x + width, p0.y + height), theme.SurfaceSunken);
		ImGui::Dummy(ImVec2(width, height));

		float fixed = 0.0f;
		int flex = 0;
		for (const ColumnSpec& col : columns)
		{
			if (col.width > 0.0f)
				fixed += col.width;
			else
				flex++;
		}
		const float flexW = flex > 0 ? ImMax(0.0f, width - fixed) / (float)flex : 0.0f;

		if (ImFont* small = EditorFonts::Small())
			ImGui::PushFont(small);

		float x = p0.x;
		const float textY = p0.y + (height - ImGui::GetFontSize()) * 0.5f;
		ImDrawList* draw = ImGui::GetWindowDrawList();
		for (const ColumnSpec& col : columns)
		{
			const float w = col.width > 0.0f ? col.width : flexW;
			if (col.label && col.label[0])
			{
				const ImVec2 ts = ImGui::CalcTextSize(col.label);
				const float tx = col.width > 0.0f
					? x + w - ts.x - 6.0f
					: x + 8.0f;
				draw->AddText(ImVec2(tx, textY), theme.TextDim, col.label);
			}
			x += w;
		}

		if (EditorFonts::Small())
			ImGui::PopFont();
	}

	bool OverflowMenuButton(const char* id)
	{
		ImGui::PushID(id);
		const bool clicked = IconButton(ICON_LC_ELLIPSIS, "More");
		ImGui::PopID();
		return clicked;
	}

	int RowActionIcons(std::initializer_list<RowActionIcon> icons, bool rowHovered)
	{
		const int count = (int)icons.size();
		if (count <= 0)
			return -1;

		constexpr float kIcon = 18.0f;
		const ImVec2 rowMin = ImGui::GetItemRectMin();
		const ImVec2 rowMax = ImGui::GetItemRectMax();
		const ImVec2 backup = ImGui::GetCursorScreenPos();
		const EditorTheme& theme = Theme();

		const float y = rowMin.y + (rowMax.y - rowMin.y - kIcon) * 0.5f;
		float x = rowMax.x - (float)count * kIcon - 4.0f;

		int clicked = -1;
		int i = 0;
		for (const RowActionIcon& action : icons)
		{
			ImGui::SetCursorScreenPos(ImVec2(x, y));
			ImGui::PushID(i);
			if (ImGui::InvisibleButton("##ra", ImVec2(kIcon, kIcon)))
				clicked = i;

			const bool hot = ImGui::IsItemHovered();
			ImU32 glyph = theme.TextDim;
			if (action.active || rowHovered || hot)
				glyph = theme.TextPrimary;

			const ImVec2 ts = ImGui::CalcTextSize(action.icon);
			const ImVec2 p0 = ImGui::GetItemRectMin();
			ImGui::GetWindowDrawList()->AddText(
				ImVec2(p0.x + (kIcon - ts.x) * 0.5f, p0.y + (kIcon - ts.y) * 0.5f),
				glyph, action.icon);

			if (action.tooltip)
				ImGui::SetItemTooltip("%s", action.tooltip);

			ImGui::PopID();
			x += kIcon;
			i++;
		}

		ImGui::SetCursorScreenPos(backup);
		return clicked;
	}

	void StatusBarItem(const char* icon, const char* text, ImU32 colour)
	{
		const EditorTheme& theme = Theme();
		if (colour == 0)
			colour = theme.TextPrimary;

		ImGui::PushStyleColor(ImGuiCol_Text, Color(colour));
		if (icon && icon[0])
		{
			ImGui::TextUnformatted(icon);
			if (text && text[0])
				ImGui::SameLine(0.0f, 6.0f);
		}
		if (text && text[0])
			ImGui::TextUnformatted(text);
		ImGui::PopStyleColor();
	}

}
