#include "gepch.h"
#include "SceneYaml.h"

#include "Components.h"

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

			// One line per enum, added when the component carrying it is converted. See the
			// comment on RegisterReflectedEnumCodec.
			RegisterReflectedEnumCodec<RigidBodyType>();
		}

		namespace {

			const ReflectedCodec* FindCodec(const entt::meta_type& type)
			{
				auto& codecs = ReflectedCodecs();
				auto it = codecs.find(type.id());
				return it == codecs.end() ? nullptr : &it->second;
			}

			// A field the generic path cannot honour must stop the conversion loudly rather than
			// write a file that is quietly missing something. Every case here means "this
			// component is not ready to be converted", not "this scene is broken".
			bool Serializable(const entt::meta_data& field, Reflection::Trait traits)
			{
				if (Reflection::Has(traits, Reflection::Trait::NotSerialized)
					|| Reflection::Has(traits, Reflection::Trait::Custom))
				{
					return false;
				}

				// OmitIfDefault needs a comparison against a default-constructed value, which
				// needs equality on the field type; entt cannot supply that without a
				// registration nothing has made. No converted component uses the flag today, so
				// rather than implement it half way this refuses to write the field at all - the
				// component stays hand-written until the flag is genuinely supported.
				GE_CORE_ASSERT(!Reflection::Has(traits, Reflection::Trait::OmitIfDefault),
					"WriteReflected met OmitIfDefault, which it does not implement. Keep this "
					"component's serialization hand-written, or implement the flag first.");

				return true;
			}

		}

	}

	void WriteReflected(YAML::Emitter& out, const entt::meta_any& instance)
	{
		Detail::RegisterReflectedCodecs();

		const entt::meta_type type = instance.type();
		if (!Reflection::IsReflected(type))
			return;

		for (auto&& [id, field] : type.data())
		{
			const Reflection::Trait traits = field.traits<Reflection::Trait>();
			if (!Detail::Serializable(field, traits))
				continue;

			entt::meta_any value = field.get(instance);
			if (!value)
				continue;

			// Flatten emits the nested struct's fields as siblings, never as a sub-map. This is
			// not cosmetic: it is the shape every collider in every saved scene already has, so
			// nesting them here would invalidate all of them.
			if (Reflection::Has(traits, Reflection::Trait::Flatten))
			{
				WriteReflected(out, value.as_ref());
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

	void ReadReflected(const YAML::Node& node, entt::meta_any instance)
	{
		Detail::RegisterReflectedCodecs();

		const entt::meta_type type = instance.type();
		if (!node || !Reflection::IsReflected(type))
			return;

		for (auto&& [id, field] : type.data())
		{
			const Reflection::Trait traits = field.traits<Reflection::Trait>();
			if (Reflection::Has(traits, Reflection::Trait::NotSerialized)
				|| Reflection::Has(traits, Reflection::Trait::Custom))
			{
				continue;
			}

			if (Reflection::Has(traits, Reflection::Trait::Flatten))
			{
				// Read into a copy and write it back: `get` may hand back a value rather than a
				// reference, and which one it is is entt's policy rather than anything here.
				entt::meta_any nested = field.get(instance);
				if (!nested)
					continue;

				ReadReflected(node, nested.as_ref());
				field.set(instance, nested);
				continue;
			}

			const YAML::Node value = node[field.name()];
			if (!value)
				continue;   // absent: leave the constructed value alone. See the header.

			const Detail::ReflectedCodec* codec = Detail::FindCodec(field.type());
			if (!codec)
				continue;

			codec->Read(value, field, instance);
		}
	}

}
