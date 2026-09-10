#include "EditorInspector.h"

#include "AssetDragDrop.h"
#include "GanymedE/Math/Curve.h"
#include "EditorWidgets.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetRef.h"
#include "GanymedE/Core/Log.h"
#include "GanymedE/Renderer/Environment.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/Texture.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace GanymedE::EditorUI {

	using Reflection::Attr;
	using Reflection::Trait;

	float PropertyContext::Speed(float fallback) const
	{
		return (Attributes && Attributes->DragSpeed != 0.0f) ? Attributes->DragSpeed : fallback;
	}

	bool PropertyContext::Range(float& min, float& max) const
	{
		if (!Attributes || !Attributes->HasRange)
			return false;

		min = Attributes->Min;
		max = Attributes->Max;
		return true;
	}

	namespace {

		std::unordered_map<entt::id_type, PropertyDrawer>& Drawers()
		{
			static std::unordered_map<entt::id_type, PropertyDrawer> s_Drawers;
			return s_Drawers;
		}

		// "CastShadows" -> "Cast Shadows". The registered name is a serialized key (decision 3),
		// so it is written for the file rather than for the panel; a display label is derived
		// from it only when no Label attribute overrides it, which is what the `Display = nullptr`
		// comment in Reflection.h reserves this job for.
		const char* DisplayLabel(const entt::meta_data& field, const Attr* attr)
		{
			if (attr && attr->Display)
				return attr->Display;

			static std::unordered_map<entt::id_type, std::string> s_Cache;

			const entt::id_type key = entt::meta_data{ field }.type().info().hash()
				^ (entt::id_type)(uintptr_t)field.name();

			auto it = s_Cache.find(key);
			if (it != s_Cache.end())
				return it->second.c_str();

			const char* name = field.name() ? field.name() : "<unnamed>";

			std::string spaced;
			for (const char* c = name; *c; ++c)
			{
				// A space before a capital that follows a lower-case letter or a digit. "HalfExtents"
				// splits; "SFX" and "UseGravity"'s leading U do not.
				if (c != name && *c >= 'A' && *c <= 'Z'
					&& ((c[-1] >= 'a' && c[-1] <= 'z') || (c[-1] >= '0' && c[-1] <= '9')))
				{
					spaced += ' ';
				}
				spaced += *c;
			}

			return s_Cache.emplace(key, std::move(spaced)).first->second.c_str();
		}

		// Read a field into a concrete T. Works whether entt hands back a reference or a copy,
		// which is why every drawer edits a local and writes it back rather than poking through
		// the any.
		template<typename T>
		bool ReadField(const PropertyContext& ctx, T& out)
		{
			entt::meta_any value = ctx.Field.get(*ctx.Instance);
			if (const T* typed = value.try_cast<T>())
			{
				out = *typed;
				return true;
			}

			return false;
		}

		template<typename T>
		bool WriteField(const PropertyContext& ctx, const T& value)
		{
			return ctx.Field.set(*ctx.Instance, value);
		}

		// ---- Scalars -------------------------------------------------------------------

		bool DrawFloat(const PropertyContext& ctx)
		{
			float value = 0.0f;
			if (!ReadField(ctx, value))
				return false;

			float min = 0.0f, max = 0.0f;
			const bool hasRange = ctx.Range(min, max);

			// Radians is a storage fact, not a display one: the field holds radians and the
			// registered range is already in degrees (see SpotLightComponent's registration).
			const bool radians = Reflection::Has(ctx.Traits, Trait::Radians);
			float shown = radians ? glm::degrees(value) : value;

			if (!ImGui::DragFloat(ctx.Label, &shown, ctx.Speed(0.1f), min, hasRange ? max : 0.0f))
				return false;

			return WriteField(ctx, radians ? glm::radians(shown) : shown);
		}

		bool DrawBool(const PropertyContext& ctx)
		{
			bool value = false;
			if (!ReadField(ctx, value))
				return false;

			if (!ImGui::Checkbox(ctx.Label, &value))
				return false;

			return WriteField(ctx, value);
		}

		template<typename T>
		bool DrawIntegral(const PropertyContext& ctx)
		{
			T value{};
			if (!ReadField(ctx, value))
				return false;

			float min = 0.0f, max = 0.0f;
			const bool hasRange = ctx.Range(min, max);

			int shown = (int)value;
			if (!ImGui::DragInt(ctx.Label, &shown, ctx.Speed(1.0f), (int)min, hasRange ? (int)max : 0))
				return false;

			return WriteField(ctx, (T)shown);
		}

		bool DrawString(const PropertyContext& ctx)
		{
			std::string value;
			if (!ReadField(ctx, value))
				return false;

			char buffer[256];
			std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());

			if (!ImGui::InputText(ctx.Label, buffer, sizeof(buffer)))
				return false;

			return WriteField(ctx, std::string(buffer));
		}

		// ---- glm vectors ---------------------------------------------------------------

		bool DrawVec2(const PropertyContext& ctx)
		{
			glm::vec2 value(0.0f);
			if (!ReadField(ctx, value))
				return false;

			float min = 0.0f, max = 0.0f;
			const bool hasRange = ctx.Range(min, max);

			if (!ImGui::DragFloat2(ctx.Label, glm::value_ptr(value), ctx.Speed(0.1f), min,
				hasRange ? max : 0.0f))
			{
				return false;
			}

			return WriteField(ctx, value);
		}

		bool DrawVec3(const PropertyContext& ctx)
		{
			glm::vec3 value(0.0f);
			if (!ReadField(ctx, value))
				return false;

			// Stored in radians, authored in degrees. The round-trip is NOT exact, which is why
			// the write below is gated on `edited`: writing back unconditionally changed the
			// stored value on frames with no user input, and under any value-diff scheme that
			// mints a phantom undo command and marks the scene dirty on mere selection. Returning
			// `edited && Write` keeps that discipline for free - it is the house rule the whole
			// commit boundary is built on.
			const bool radians = Reflection::Has(ctx.Traits, Trait::Radians);
			glm::vec3 shown = radians ? glm::degrees(value) : value;

			// Color is what separates the two vec3 widgets, and it has to: a colour wants a swatch
			// and a picker, a position wants the X/Y/Z row with reset buttons. The type cannot
			// tell them apart, which is the two-tier vocabulary earning its keep.
			const bool edited = Reflection::Has(ctx.Traits, Trait::Color)
				? ImGui::ColorEdit3(ctx.Label, glm::value_ptr(shown))
				: DrawVec3Control(ctx.Label, shown,
					ctx.Attributes ? ctx.Attributes->ResetValue : 0.0f, 100.0f, ctx.Speed(0.1f));

			if (!edited)
				return false;

			return WriteField(ctx, radians ? glm::radians(shown) : shown);
		}

		bool DrawVec4(const PropertyContext& ctx)
		{
			glm::vec4 value(0.0f);
			if (!ReadField(ctx, value))
				return false;

			float min = 0.0f, max = 0.0f;
			const bool hasRange = ctx.Range(min, max);

			const bool edited = Reflection::Has(ctx.Traits, Trait::Color)
				? ImGui::ColorEdit4(ctx.Label, glm::value_ptr(value))
				: ImGui::DragFloat4(ctx.Label, glm::value_ptr(value), ctx.Speed(0.1f), min,
					hasRange ? max : 0.0f);

			return edited && WriteField(ctx, value);
		}

		// ---- Enums ---------------------------------------------------------------------
		//
		// The names come from the enum's own registration (`.data<Value>("Name")` on the enum
		// type), so they live once beside the enum rather than in an EnumNames attribute - which
		// is exactly why R1 dropped that attribute from the vocabulary.
		bool DrawEnum(const PropertyContext& ctx)
		{
			const entt::meta_type type = ctx.Field.type();

			entt::meta_any current = ctx.Field.get(*ctx.Instance);
			if (!current)
				return false;

			const char* currentName = "<unregistered>";
			for (auto&& [id, value] : type.data())
			{
				entt::meta_any option = value.get({});
				if (option && option == current)
				{
					currentName = value.name() ? value.name() : currentName;
					break;
				}
			}

			bool edited = false;
			if (ImGui::BeginCombo(ctx.Label, currentName))
			{
				for (auto&& [id, value] : type.data())
				{
					entt::meta_any option = value.get({});
					if (!option)
						continue;

					const bool selected = (option == current);
					const char* name = value.name() ? value.name() : "<unnamed>";

					if (ImGui::Selectable(name, selected) && !selected)
						edited = ctx.Field.set(*ctx.Instance, option);

					if (selected)
						ImGui::SetItemDefaultFocus();
				}

				ImGui::EndCombo();
			}

			return edited;
		}

		// ---- Ranges --------------------------------------------------------------------
		//
		// The reason `RangeF` is a type. A generic drawer that saw `LifetimeMin` and `LifetimeMax`
		// as two unrelated floats could draw them, but it could not clamp them: the hand-written
		// panel pushes Max up when Min passes it and pulls Min down when Max drops below, and
		// which of those to do depends on **which half the author just moved**. One drawer owning
		// both halves knows; two independent drawers never can.
		bool DrawRange(const PropertyContext& ctx)
		{
			RangeF value;
			if (!ReadField(ctx, value))
				return false;

			float lo = 0.0f, hi = 0.0f;
			const bool hasRange = ctx.Range(lo, hi);
			const float speed = ctx.Speed(0.05f);

			ImGui::PushID(ctx.Label);
			ImGui::PushMultiItemsWidths(2, ImGui::CalcItemWidth());

			const bool editedMin = ImGui::DragFloat("##Min", &value.Min, speed, lo,
				hasRange ? hi : 0.0f);
			ImGui::PopItemWidth();
			ImGui::SameLine();

			const bool editedMax = ImGui::DragFloat("##Max", &value.Max, speed, lo,
				hasRange ? hi : 0.0f);
			ImGui::PopItemWidth();
			ImGui::SameLine();

			ImGui::TextUnformatted(ctx.Label);
			ImGui::PopID();

			if (!editedMin && !editedMax)
				return false;

			// Push the other half out of the way, in the direction the edit implies.
			if (editedMin && value.Max < value.Min)
				value.Max = value.Min;
			if (editedMax && value.Min > value.Max)
				value.Min = value.Max;

			return WriteField(ctx, value);
		}

		// ---- Curves and gradients ------------------------------------------------------
		//
		// The house widgets from EditorWidgets, which own ActiveId for a whole drag - the
		// property the undo commit boundary cannot work without. Attr::Range carries the Y range
		// the curve editor draws, which is what that attribute means on a FloatCurve.
		bool DrawCurve(const PropertyContext& ctx)
		{
			FloatCurve value;
			if (!ReadField(ctx, value))
				return false;

			float lo = 0.0f, hi = 1.0f;
			ctx.Range(lo, hi);

			if (!CurveEditor(ctx.Label, value, lo, hi))
				return false;

			return WriteField(ctx, value);
		}

		bool DrawGradient(const PropertyContext& ctx)
		{
			ColorGradient value;
			if (!ReadField(ctx, value))
				return false;

			if (!GradientEditor(ctx.Label, value))
				return false;

			return WriteField(ctx, value);
		}

		// ---- Asset slots ---------------------------------------------------------------

		template<typename T>
		bool DrawAssetRef(const PropertyContext& ctx)
		{
			AssetRef<T> value;
			if (!ReadField(ctx, value))
				return false;

			// "Is a reference authored here" without triggering a load - HasHandle, never Ready.
			const char* text = "None";
			std::string path;
			if (value.HasHandle())
			{
				if (const AssetMetadata* metadata = AssetManager::GetMetadata(value.Handle()))
					path = metadata->FilePath;

				text = path.empty() ? "<missing>" : path.c_str();
			}

			ImGui::Text("%s: %s", ctx.Label, text);

			AssetRef<T> dropped = AcceptAssetDropRef<T>();
			if (!dropped.HasHandle())
				return false;

			return WriteField(ctx, dropped);
		}

		// ---- Bare asset handles --------------------------------------------------------
		//
		// `AssetHandle` is an alias for `UUID`, the same type as `RelationshipComponent::Parent`,
		// so the type alone cannot say this is an asset slot - `Attr::Slot` does, which is the
		// exact case the two-tier vocabulary was introduced for. A UUID field with no Slot is not
		// an asset reference and is shown read-only rather than given a drop target.
		bool DrawAssetHandle(const PropertyContext& ctx)
		{
			AssetHandle value = InvalidAssetHandle;
			if (!ReadField(ctx, value))
				return false;

			const AssetType slot = ctx.Attributes ? ctx.Attributes->Slot : AssetType::None;
			if (slot == AssetType::None)
			{
				ImGui::Text("%s: %llu", ctx.Label, (unsigned long long)(uint64_t)value);
				return false;
			}

			std::string path;
			if (IsAssetHandleValid(value))
			{
				if (const AssetMetadata* metadata = AssetManager::GetMetadata(value))
					path = metadata->FilePath;
			}

			ImGui::Text("%s: %s", ctx.Label,
				IsAssetHandleValid(value) ? (path.empty() ? "<missing>" : path.c_str()) : "None");

			// ReadOnly has to gate the *interactive* parts explicitly. `BeginDisabled` around the
			// call blocks clicks, but a drag-drop target is not an item click - it would still
			// accept a payload and silently write a field the registration says is not editable.
			if (Reflection::Has(ctx.Traits, Trait::ReadOnly))
				return false;

			bool edited = false;

			if (IsAssetHandleValid(value))
			{
				ImGui::SameLine();
				ImGui::PushID(ctx.Label);
				if (ImGui::SmallButton("Clear"))
				{
					edited = WriteField(ctx, InvalidAssetHandle);
					value = InvalidAssetHandle;
				}
				ImGui::PopID();
			}

			const AssetHandle dropped = AcceptAssetDropHandle(slot);
			if (IsAssetHandleValid(dropped))
				edited = WriteField(ctx, dropped) || edited;

			return edited;
		}

		// ---- The dispatch --------------------------------------------------------------

		PropertyDrawer FindDrawer(const entt::meta_type& type)
		{
			auto& drawers = Drawers();

			auto it = drawers.find(type.id());
			if (it != drawers.end())
				return it->second;

			// One entry covers every registered enum rather than one per enum type.
			if (type.is_enum())
				return &DrawEnum;

			return nullptr;
		}

		void WarnOnce(const entt::meta_data& field, const entt::meta_type& owner)
		{
			static std::unordered_set<entt::id_type> s_Warned;

			const entt::id_type key = field.type().id();
			if (!s_Warned.insert(key).second)
				return;

			GE_WARN("Inspector: no property drawer for the type of '{0}::{1}' - the field is not "
				"drawn. Register one with EditorUI::RegisterPropertyDrawer, or mark the field "
				"Hidden or Custom if that is deliberate.",
				owner.name() ? owner.name() : "<type>", field.name() ? field.name() : "<field>");
		}

	}

	void RegisterPropertyDrawer(const entt::meta_type& type, PropertyDrawer drawer)
	{
		if (!type)
			return;

		Drawers()[type.id()] = drawer;
	}

	void InitPropertyDrawers()
	{
		RegisterPropertyDrawer(entt::resolve<float>(), &DrawFloat);
		RegisterPropertyDrawer(entt::resolve<bool>(), &DrawBool);
		RegisterPropertyDrawer(entt::resolve<int32_t>(), &DrawIntegral<int32_t>);
		RegisterPropertyDrawer(entt::resolve<uint32_t>(), &DrawIntegral<uint32_t>);
		RegisterPropertyDrawer(entt::resolve<std::string>(), &DrawString);

		RegisterPropertyDrawer(entt::resolve<glm::vec2>(), &DrawVec2);
		RegisterPropertyDrawer(entt::resolve<glm::vec3>(), &DrawVec3);
		RegisterPropertyDrawer(entt::resolve<glm::vec4>(), &DrawVec4);

		// Every AssetHandle-typed field routes here; the drawer itself decides whether the field
		// is an asset slot by looking for Attr::Slot.
		RegisterPropertyDrawer(entt::resolve<AssetHandle>(), &DrawAssetHandle);

		RegisterPropertyDrawer(entt::resolve<RangeF>(), &DrawRange);
		RegisterPropertyDrawer(entt::resolve<FloatCurve>(), &DrawCurve);
		RegisterPropertyDrawer(entt::resolve<ColorGradient>(), &DrawGradient);

		RegisterPropertyDrawer(entt::resolve<AssetRef<Mesh>>(), &DrawAssetRef<Mesh>);
		RegisterPropertyDrawer(entt::resolve<AssetRef<Material>>(), &DrawAssetRef<Material>);
		RegisterPropertyDrawer(entt::resolve<AssetRef<Texture2D>>(), &DrawAssetRef<Texture2D>);
		RegisterPropertyDrawer(entt::resolve<AssetRef<Environment>>(), &DrawAssetRef<Environment>);
	}

	bool DrawProperty(entt::meta_any& instance, const entt::meta_data& field,
		const OverrideHook& overrides, const MultiEditHook& multi, const FieldFilter& filter)
	{
		const Trait traits = field.traits<Trait>();
		if (Reflection::Has(traits, Trait::Hidden) || Reflection::Has(traits, Trait::Custom))
			return false;

		if (filter && !filter(field))
			return false;

		const Attr* attr = Reflection::Attributes(field);

		// A Flatten'ed member is a struct whose fields belong to the parent - the shape
		// SceneSerializer already writes for a collider's PhysicsMaterial, and the shape the
		// hand-written collider sections already draw.
		if (Reflection::Has(traits, Trait::Flatten))
		{
			entt::meta_any nested = field.get(instance);
			if (!nested)
				return false;

			// The nested fields keep the parent's override hook: an overridden Friction belongs
			// to the collider, which is what the hook is keyed on.
			entt::meta_any ref = nested.as_ref();
			if (!DrawReflectedProperties(ref.as_ref(), overrides, multi, filter))
				return false;

			// Written back rather than edited in place: `get` may have handed back a copy, and
			// which of the two it is depends on entt's policy rather than on anything here.
			return field.set(instance, nested);
		}

		PropertyDrawer drawer = FindDrawer(field.type());
		if (!drawer)
		{
			// A reflected struct with no drawer of its own is drawn inline: its fields become rows
			// of the parent, which is what the hand-written Camera section did with SceneCamera.
			//
			// This is an *inspector* decision only. It says nothing about serialization, where
			// SceneCamera really is a nested map on disk - `Trait::Flatten` is the statement that
			// a nested struct's fields are siblings in the file, and putting it here to get this
			// layout would have been a lie the first generic writer believed.
			if (Reflection::IsReflected(field.type()) && field.type().is_class())
			{
				entt::meta_any nested = field.get(instance);
				if (!nested)
					return false;

				entt::meta_any ref = nested.as_ref();
				if (!DrawReflectedProperties(ref.as_ref(), overrides, multi, filter))
					return false;

				return field.set(instance, nested);
			}

			WarnOnce(field, instance.type());
			return false;
		}

		PropertyContext ctx;
		ctx.Label = DisplayLabel(field, attr);
		ctx.Instance = &instance;
		ctx.Field = field;
		ctx.Attributes = attr;
		ctx.Traits = traits;

		const bool readOnly = Reflection::Has(traits, Trait::ReadOnly);
		if (readOnly)
			ImGui::BeginDisabled();

		// A field that differs from the prefab is tinted for its whole row. Unity bolds just the
		// label; doing that here would need a second font the editor does not load, and a colour
		// is unambiguous without disturbing layout - which matters because the row is drawn by
		// the widget itself, not by this function.
		// Mixed wins over overridden when both apply: "these entities disagree" is the more urgent
		// fact, because the widget is showing one of several values rather than the value.
		const bool mixed = multi && multi.IsMixed(multi.Owner, field);
		const bool overridden = !mixed && overrides && overrides.IsOverridden(overrides.Owner, field);

		if (mixed)
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0f, 0.78f, 0.35f, 1.0f });
		else if (overridden)
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 0.45f, 0.72f, 1.0f, 1.0f });

		const bool edited = drawer(ctx);

		if (mixed || overridden)
			ImGui::PopStyleColor();

		// The primary now holds the new value; give it to the rest of the selection. Only on a
		// frame a widget actually reported an edit, so merely selecting several entities never
		// flattens their differing values.
		if (edited && multi && multi.Propagate)
			multi.Propagate(multi.Owner, field);

		if (readOnly)
			ImGui::EndDisabled();

		// Right-click the field itself, which is where an author looks for it.
		if (overridden && ImGui::BeginPopupContextItem(field.name()))
		{
			if (ImGui::MenuItem("Revert to Prefab"))
			{
				// Reported as an edit so the section's commit boundary turns it into one undo
				// command, exactly as a drag would - a revert is a scene edit like any other.
				if (overrides.Revert(overrides.Owner, field))
				{
					ImGui::EndPopup();
					return true;
				}
			}

			ImGui::EndPopup();
		}

		// Rendered under the widget as TextDisabled, which is what every hand-written section
		// that has a note already does. A hover tooltip was the alternative; matching the
		// existing panel matters more than the preference.
		if (attr && attr->Note)
			ImGui::TextDisabled("%s", attr->Note);

		return edited && !readOnly;
	}

	bool DrawReflectedProperties(entt::meta_any instance, const OverrideHook& overrides,
		const MultiEditHook& multi, const FieldFilter& filter)
	{
		const entt::meta_type type = instance.type();
		if (!Reflection::IsReflected(type))
			return false;

		// Fields carrying Attr::Section are grouped under a CollapsingHeader, in registration
		// order, exactly as the hand-written particle-emitter section did by hand. A section ends
		// when the next field names a different one - the registration lists them contiguously,
		// which is a property worth keeping if you reorder it.
		bool edited = false;
		const char* openSection = nullptr;
		bool sectionVisible = true;

		for (auto&& [id, field] : type.data())
		{
			const Reflection::Attr* attr = Reflection::Attributes(field);
			const char* section = attr ? attr->Section : nullptr;

			if (section != openSection)
			{
				openSection = section;
				sectionVisible = section == nullptr
					|| ImGui::CollapsingHeader(section, ImGuiTreeNodeFlags_DefaultOpen);
			}

			if (!sectionVisible)
				continue;

			edited |= DrawProperty(instance, field, overrides, multi, filter);
		}

		return edited;
	}

}
