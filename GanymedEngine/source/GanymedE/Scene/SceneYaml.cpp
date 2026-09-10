#include "gepch.h"
#include "SceneYaml.h"

#include "Components.h"

// The four managed asset types, complete. Components.h only forward-declares them - deliberately,
// since it is included almost everywhere - but `entt::resolve<AssetRef<T>>()` and the codec
// lambdas below need `T` complete, exactly as ComponentReflection.cpp does.
#include "GanymedE/Renderer/Environment.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/Texture.h"

#include <glm/glm.hpp>

namespace GanymedE {

	namespace Detail {

		void RegisterReflectedCodecs()
		{
			static bool s_Registered = false;
			if (s_Registered)
				return;

			s_Registered = true;

			RegisterReflectedCodec<float>();
			RegisterReflectedCodec<bool>();
			RegisterReflectedCodec<int32_t>();
			RegisterReflectedCodec<uint32_t>();
			RegisterReflectedCodec<uint64_t>();
			RegisterReflectedCodec<std::string>();
			RegisterReflectedCodec<glm::vec3>();
			RegisterReflectedCodec<glm::vec4>();
			RegisterReflectedCodec<FloatCurve>();
			RegisterReflectedCodec<ColorGradient>();

			// entt keys codecs by type id and AssetHandle IS UUID, so this one line covers the
			// bare-handle fields (ScriptComponent::Script, PrefabInstanceComponent::Source) and
			// PrefabMemberComponent::CanonicalID alike.
			RegisterReflectedUUIDCodec();

			// One line per managed asset type, mirroring the list Reflection::Validate checks
			// slots against. A new AssetRef<T> that is missing here asserts at the first save of
			// a component using it rather than writing a scene with the field silently dropped.
			RegisterReflectedAssetRefCodec<Mesh>();
			RegisterReflectedAssetRefCodec<Environment>();
			RegisterReflectedAssetRefCodec<Texture2D>();
			RegisterReflectedAssetRefCodec<Material>();

			// One line per enum, added when the component carrying it is converted. See the
			// comment on RegisterReflectedEnumCodec.
			RegisterReflectedEnumCodec<RigidBodyType>();
			RegisterReflectedEnumCodec<SceneCamera::ProjectionType>();
			RegisterReflectedEnumCodec<ParticleEmitterComponent::Mode>();
			RegisterReflectedEnumCodec<ParticleBlend>();
			RegisterReflectedEnumCodec<AudioGroup>();   // SerializeByName: written as "Music"
		}

		const ReflectedCodec* FindCodec(const entt::meta_type& type)
		{
			auto& codecs = ReflectedCodecs();
			auto it = codecs.find(type.id());
			return it == codecs.end() ? nullptr : &it->second;
		}

		namespace {

			// A field the generic path does not own. `CustomWriter` rather than `Custom`: a
			// bespoke *widget* is the inspector's business and says nothing about how the value
			// is stored, which is why AnimatorComponent::Clip and the two particle curves write
			// generically while still drawing by hand.
			bool Serializable(Reflection::Trait traits)
			{
				return !Reflection::Has(traits, Reflection::Trait::NotSerialized)
					&& !Reflection::Has(traits, Reflection::Trait::CustomWriter);
			}

			// `OmitIfDefault`: is this field already what a freshly constructed component would
			// hold? False whenever the question cannot be answered - no default instance to
			// compare against, or a type whose codec registered no equality - because writing a
			// field that could have been omitted costs a diff line, while omitting one that
			// differs is data loss.
			bool MatchesDefault(const entt::meta_data& field, const entt::meta_any& value,
				const entt::meta_any& defaults)
			{
				if (!defaults)
					return false;

				const ReflectedCodec* codec = FindCodec(field.type());
				if (!codec || !codec->Equal)
					return false;

				const entt::meta_any fallback = field.get(defaults);
				return fallback && codec->Equal(value, fallback);
			}

			// The YAML key a flattened field's child is written under. Bare for a collider's
			// PhysicsMaterial, prefixed for a RangeF - see Attr::KeyPrefix.
			std::string FlattenedKey(const Reflection::Attr* attr, const char* childName)
			{
				if (attr && attr->KeyPrefix)
					return std::string(attr->KeyPrefix) + childName;

				return childName;
			}

			// A struct that is reflected, has members and no codec of its own: written as a
			// nested map under its own key. CameraComponent::Camera is the only one.
			bool IsNestedMap(const entt::meta_type& type)
			{
				if (!Reflection::IsReflected(type) || FindCodec(type))
					return false;

				for (auto&& [id, field] : type.data())
				{
					(void)id;
					(void)field;
					return true;
				}

				return false;   // reflected but memberless: opaque, and nothing to nest
			}

		}

	}

	void WriteReflected(YAML::Emitter& out, const entt::meta_any& instance,
		const entt::meta_any& defaults)
	{
		Detail::RegisterReflectedCodecs();

		const entt::meta_type type = instance.type();
		if (!Reflection::IsReflected(type))
			return;

		for (auto&& [id, field] : type.data())
		{
			const Reflection::Trait traits = field.traits<Reflection::Trait>();
			if (!Detail::Serializable(traits))
				continue;

			entt::meta_any value = field.get(instance);
			if (!value)
				continue;

			// Flatten emits the nested struct's fields as siblings, never as a sub-map. This is
			// not cosmetic: it is the shape every collider in every saved scene already has, so
			// nesting them here would invalidate all of them.
			//
			// OmitIfDefault is evaluated per CHILD here rather than on the struct as a whole,
			// which is what keeps a RangeF writing LifetimeMin alone when only the minimum was
			// authored - the shape the hand-written emitter produced, field by field.
			if (Reflection::Has(traits, Reflection::Trait::Flatten))
			{
				const Reflection::Attr* attr = Reflection::Attributes(field);
				entt::meta_any nestedDefaults = defaults ? field.get(defaults) : entt::meta_any{};

				for (auto&& [nestedId, nested] : field.type().data())
				{
					const Reflection::Trait nestedTraits = nested.traits<Reflection::Trait>();
					if (!Detail::Serializable(nestedTraits))
						continue;

					entt::meta_any nestedValue = nested.get(value);
					if (!nestedValue)
						continue;

					if (Reflection::Has(traits, Reflection::Trait::OmitIfDefault)
						&& Detail::MatchesDefault(nested, nestedValue, nestedDefaults))
					{
						continue;
					}

					const Detail::ReflectedCodec* nestedCodec = Detail::FindCodec(nested.type());
					GE_CORE_ASSERT(nestedCodec, "No YAML codec for a flattened field");
					if (!nestedCodec)
						continue;

					out << YAML::Key << Detail::FlattenedKey(attr, nested.name()) << YAML::Value;
					nestedCodec->Write(out, nestedValue);
				}

				continue;
			}

			if (Reflection::Has(traits, Reflection::Trait::OmitIfDefault)
				&& Detail::MatchesDefault(field, value, defaults))
			{
				continue;
			}

			// A reflected struct with no codec becomes a nested map. Its own OmitIfDefault-ness
			// is the parent field's business, already decided above.
			if (Detail::IsNestedMap(field.type()))
			{
				out << YAML::Key << field.name() << YAML::Value << YAML::BeginMap;
				WriteReflected(out, value.as_ref(),
					defaults ? field.get(defaults) : entt::meta_any{});
				out << YAML::EndMap;
				continue;
			}

			const Detail::ReflectedCodec* codec = Detail::FindCodec(field.type());
			GE_CORE_ASSERT(codec, "No YAML codec for a reflected field - the component cannot be "
				"serialized generically until one is registered in RegisterReflectedCodecs");

			if (!codec)
				continue;

			out << YAML::Key << field.name() << YAML::Value;
			codec->Write(out, value);
		}
	}

	std::string EmitReflectedValue(const entt::meta_any& instance, const entt::meta_data& field)
	{
		Detail::RegisterReflectedCodecs();

		entt::meta_any value = field.get(instance);
		if (!value)
			return {};

		const Reflection::Trait traits = field.traits<Reflection::Trait>();
		const Detail::ReflectedCodec* codec = Detail::FindCodec(field.type());

		// A struct-valued field - a collider's PhysicsMaterial, a particle RangeF, a camera - has
		// no codec of its own, and returning empty for it made the prefab-override diff blind to
		// every one of them: an empty string compares equal to an empty string, so "overridden"
		// was answered false whatever the author had changed. Emitting the sub-map instead keeps
		// this function's contract - "what would this field serialize to" - true for every shape
		// the writer above can produce.
		if (!codec && (Reflection::Has(traits, Reflection::Trait::Flatten)
			|| Detail::IsNestedMap(field.type())))
		{
			YAML::Emitter out;
			out << YAML::BeginMap << YAML::Key << "v" << YAML::Value << YAML::BeginMap;
			WriteReflected(out, value.as_ref());
			out << YAML::EndMap << YAML::EndMap;

			return out.c_str() ? out.c_str() : std::string{};
		}

		if (!codec)
			return {};

		YAML::Emitter out;
		out << YAML::BeginMap << YAML::Key << "v" << YAML::Value;
		codec->Write(out, value);
		out << YAML::EndMap;

		return out.c_str() ? out.c_str() : std::string{};
	}

	void ReadReflected(const YAML::Node& node, entt::meta_any instance)
	{
		Detail::RegisterReflectedCodecs();

		const entt::meta_type type = instance.type();
		if (!node || !Reflection::IsReflected(type))
			return;

		for (auto&& [id, field] : type.data())
		{
			const Reflection::Trait traits = field.traits<Reflection::Trait>();
			if (!Detail::Serializable(traits))
				continue;

			if (Reflection::Has(traits, Reflection::Trait::Flatten))
			{
				// Read into a copy and write it back: `get` may hand back a value rather than a
				// reference, and which one it is is entt's policy rather than anything here.
				entt::meta_any nested = field.get(instance);
				if (!nested)
					continue;

				// Keyed the same way the writer keys them, prefix included, rather than by
				// recursing on the whole node - which is what makes five RangeF fields readable
				// from one flat map without colliding on "Min".
				const Reflection::Attr* attr = Reflection::Attributes(field);
				entt::meta_any nestedRef = nested.as_ref();

				for (auto&& [nestedId, child] : field.type().data())
				{
					const Reflection::Trait childTraits = child.traits<Reflection::Trait>();
					if (!Detail::Serializable(childTraits))
						continue;

					const YAML::Node childNode = node[Detail::FlattenedKey(attr, child.name())];
					if (!childNode)
						continue;

					if (const Detail::ReflectedCodec* childCodec = Detail::FindCodec(child.type()))
						childCodec->Read(childNode, child, nestedRef);
				}

				field.set(instance, nested);
				continue;
			}

			const YAML::Node value = node[field.name()];
			if (!value)
				continue;   // absent: leave the constructed value alone. See the header.

			if (Detail::IsNestedMap(field.type()))
			{
				entt::meta_any nested = field.get(instance);
				if (!nested)
					continue;

				ReadReflected(value, nested.as_ref());
				field.set(instance, nested);
				continue;
			}

			const Detail::ReflectedCodec* codec = Detail::FindCodec(field.type());
			if (!codec)
				continue;

			codec->Read(value, field, instance);
		}
	}

}
