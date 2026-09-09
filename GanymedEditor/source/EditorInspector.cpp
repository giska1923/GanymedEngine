#include "EditorInspector.h"

#include "AssetDragDrop.h"
#include "EditorWidgets.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetRef.h"
#include "GanymedE/Core/Log.h"
#include "GanymedE/Renderer/Environment.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/Texture.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <imgui/imgui.h>

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

			// Color is what separates the two vec3 widgets, and it has to: a colour wants a swatch
			// and a picker, a position wants the X/Y/Z row with reset buttons. The type cannot
			// tell them apart, which is the two-tier vocabulary earning its keep.
			const bool edited = Reflection::Has(ctx.Traits, Trait::Color)
				? ImGui::ColorEdit3(ctx.Label, glm::value_ptr(value))
				: DrawVec3Control(ctx.Label, value,
					ctx.Attributes ? ctx.Attributes->ResetValue : 0.0f, 100.0f, ctx.Speed(0.1f));

			return edited && WriteField(ctx, value);
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

		RegisterPropertyDrawer(entt::resolve<AssetRef<Mesh>>(), &DrawAssetRef<Mesh>);
		RegisterPropertyDrawer(entt::resolve<AssetRef<Material>>(), &DrawAssetRef<Material>);
		RegisterPropertyDrawer(entt::resolve<AssetRef<Texture2D>>(), &DrawAssetRef<Texture2D>);
		RegisterPropertyDrawer(entt::resolve<AssetRef<Environment>>(), &DrawAssetRef<Environment>);
	}

	bool DrawProperty(entt::meta_any& instance, const entt::meta_data& field)
	{
		const Trait traits = field.traits<Trait>();
		if (Reflection::Has(traits, Trait::Hidden) || Reflection::Has(traits, Trait::Custom))
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

			entt::meta_any ref = nested.as_ref();
			if (!DrawReflectedProperties(ref.as_ref()))
				return false;

			// Written back rather than edited in place: `get` may have handed back a copy, and
			// which of the two it is depends on entt's policy rather than on anything here.
			return field.set(instance, nested);
		}

		PropertyDrawer drawer = FindDrawer(field.type());
		if (!drawer)
		{
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

		const bool edited = drawer(ctx);

		if (readOnly)
			ImGui::EndDisabled();

		// Rendered under the widget as TextDisabled, which is what every hand-written section
		// that has a note already does. A hover tooltip was the alternative; matching the
		// existing panel matters more than the preference.
		if (attr && attr->Note)
			ImGui::TextDisabled("%s", attr->Note);

		return edited && !readOnly;
	}

	bool DrawReflectedProperties(entt::meta_any instance)
	{
		const entt::meta_type type = instance.type();
		if (!Reflection::IsReflected(type))
			return false;

		bool edited = false;
		for (auto&& [id, field] : type.data())
			edited |= DrawProperty(instance, field);

		return edited;
	}

}
