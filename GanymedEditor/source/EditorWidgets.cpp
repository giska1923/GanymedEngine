#include "EditorWidgets.h"

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
		draw->PushClipRect(canvas, canvasMax, true);
		draw->AddRectFilled(canvas, canvasMax, IM_COL32(24, 24, 28, 255), 3.0f);
		draw->AddRect(canvas, canvasMax, IM_COL32(70, 70, 80, 255), 3.0f);

		for (int i = 0; i <= 4; i++)
		{
			const float t = kTimeMin + (kTimeMax - kTimeMin) * (float)i / 4.0f;
			const float x = CanvasToScreen(canvas, size, t, yMin, yMin, yMax).x;
			draw->AddLine(ImVec2(x, canvas.y), ImVec2(x, canvasMax.y), IM_COL32(50, 50, 58, 255));
		}
		if (0.0f >= yMin && 0.0f <= yMax)
		{
			const float y = CanvasToScreen(canvas, size, 0.0f, 0.0f, yMin, yMax).y;
			draw->AddLine(ImVec2(canvas.x, y), ImVec2(canvasMax.x, y), IM_COL32(70, 70, 40, 255));
		}

		const auto& drawn = curve.Keys();
		for (size_t i = 1; i < drawn.size(); i++)
		{
			const ImVec2 a = CanvasToScreen(canvas, size, drawn[i - 1].Time, drawn[i - 1].Value, yMin, yMax);
			const ImVec2 b = CanvasToScreen(canvas, size, drawn[i].Time, drawn[i].Value, yMin, yMax);
			draw->AddLine(a, b, IM_COL32(220, 180, 70, 255), 1.5f);
		}

		const int hoverIndex = hovered ? HitKey(canvas, size, mouse, yMin, yMax, drawn) : -1;
		for (int i = 0; i < (int)drawn.size(); i++)
		{
			const ImVec2 p = CanvasToScreen(canvas, size, drawn[i].Time, drawn[i].Value, yMin, yMax);
			const bool hot = i == dragIndex || i == hoverIndex;
			draw->AddCircleFilled(p, hot ? kKeyRadius + 1.5f : kKeyRadius,
				hot ? IM_COL32(255, 220, 90, 255) : IM_COL32(240, 200, 80, 255));
			draw->AddCircle(p, hot ? kKeyRadius + 1.5f : kKeyRadius, IM_COL32(20, 20, 24, 255));
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
		draw->AddRect(canvas, canvasMax, IM_COL32(70, 70, 80, 255));

		const auto& drawn = gradient.Keys();
		const int hoverIndex = hovered ? HitGradientKey(canvas, width, height, mouse, drawn) : -1;
		for (int i = 0; i < (int)drawn.size(); i++)
		{
			const float x = canvas.x + std::clamp(drawn[i].Time, kTimeMin, kTimeMax) * width;
			const float y = canvas.y + height * 0.5f;
			const bool hot = i == dragIndex || i == hoverIndex || i == selected;
			draw->AddCircleFilled(ImVec2(x, y), hot ? kKeyRadius + 1.5f : kKeyRadius, ColorU32(drawn[i].Value));
			draw->AddCircle(ImVec2(x, y), hot ? kKeyRadius + 1.5f : kKeyRadius,
				hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 20, 24, 255));
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

}
