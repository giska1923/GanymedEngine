#include "gepch.h"
#include "GanymedE/Reflection/Reflection.h"

#include "GanymedE/ECS/ComponentTraits.h"
#include "GanymedE/Scene/Components.h"

// AssetRef<T> reaches entt as a reflected member type, and entt's meta machinery needs T complete
// to answer `is_constructible` about it. Components.h only forward-declares the four asset classes,
// which is deliberate - it is included almost everywhere and the renderer headers are not free.
#include "GanymedE/Renderer/Environment.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/Texture.h"

// Every registration block in the engine, in one translation unit.
//
// The alternative - a block next to each struct in Components.h - reads better and is harder to
// forget when adding a field. It also multiplies the linker-stripping risk by 23: in a static
// library, a TU nothing references is dropped, and a dropped registration block is a type that is
// silently unreflected in Release or in the runtime while the Debug editor works fine. One file
// with one explicit Init() that enumerates its own contents cannot half-register.
//
// The cost, stated plainly: field knowledge now lives in two files. Adding a member to a component
// leaves it unreflected until someone edits this file, and no test can catch that - the true member
// set is exactly what is not reflected. The size sentinels at the bottom are the only forcing
// function, and they are honest about their own limits.
//
// **Registered field names are the on-disk contract.** Every YAML key in SceneSerializer.cpp is
// currently character-identical to its C++ member identifier, and R3 will drive save/load from
// these names. A name here that does not match today's key breaks every committed .gscene and
// .gprefab. The human-facing label goes in Attr::Display, which is why SceneCamera can carry the
// key "PerspectiveFOV", the accessor GetPerspectiveVerticalFOV and the label "Vertical FOV" at once.
//
// Numbers in Attr (ranges, drag speeds) and strings in Attr (labels, sections, notes) are copied
// from GanymedEditor/source/Panels/SceneHierarchyPanel.cpp rather than invented, so R2 can collapse
// a hand-written drawer without the widget changing behaviour.
namespace GanymedE::Reflection {

	namespace {

		// ---- Supporting types ------------------------------------------------------------
		//
		// A reflected type with NO data members is the signal "opaque - a bespoke drawer and a
		// bespoke writer own this". FloatCurve and ColorGradient are the clean case: both keep a
		// sorted-by-time invariant and deliberately never expose their key vector mutably, so a
		// generic setter could not preserve the invariant even if one existed.

		void RegisterSupportingTypes()
		{
			// glm's vector types are deliberately NOT registered. Reflecting vec3::x/y/z would
			// invite the generic serializer to write a map where yaml-cpp's converter currently
			// writes a flow sequence, silently changing the file format; R2/R3 identify them by
			// type_info comparison instead, which needs no registration.

			GE_REFLECT_TYPE(PhysicsMaterial)
				.data<&PhysicsMaterial::Friction>("Friction")
					.custom<Attr>(Attr{}.Range(0.0f, 10.0f).Speed(0.01f))
				.data<&PhysicsMaterial::Restitution>("Restitution")
					.custom<Attr>(Attr{}.Range(0.0f, 1.0f).Speed(0.01f));

			// Private state behind accessors, so this needs entt's setter/getter .data overload.
			// The Camera base is not reflected: its only member is the projection matrix, which is
			// derived from these seven values by RecalculateProjection.
			GE_REFLECT_TYPE(SceneCamera)
				.data<&SceneCamera::SetProjectionType, &SceneCamera::GetProjectionType>("ProjectionType")
					.custom<Attr>(Attr{}.Label("Projection"))
				// Radians means the range and the drag speed below are in DISPLAY units (degrees).
				.data<&SceneCamera::SetPerspectiveVerticalFOV, &SceneCamera::GetPerspectiveVerticalFOV>("PerspectiveFOV")
					.traits(Trait::Radians)
					.custom<Attr>(Attr{}.Label("Vertical FOV"))
				.data<&SceneCamera::SetPerspectiveNearClip, &SceneCamera::GetPerspectiveNearClip>("PerspectiveNear")
					.custom<Attr>(Attr{}.Label("Near"))
				.data<&SceneCamera::SetPerspectiveFarClip, &SceneCamera::GetPerspectiveFarClip>("PerspectiveFar")
					.custom<Attr>(Attr{}.Label("Far"))
				.data<&SceneCamera::SetOrthographicSize, &SceneCamera::GetOrthographicSize>("OrthographicSize")
					.custom<Attr>(Attr{}.Label("Size"))
				.data<&SceneCamera::SetOrthographicNearClip, &SceneCamera::GetOrthographicNearClip>("OrthographicNear")
					.custom<Attr>(Attr{}.Label("Near"))
				.data<&SceneCamera::SetOrthographicFarClip, &SceneCamera::GetOrthographicFarClip>("OrthographicFar")
					.custom<Attr>(Attr{}.Label("Far"));

			GE_REFLECT_TYPE(FloatCurve);
			GE_REFLECT_TYPE(ColorGradient);
		}

		// ---- Enums -----------------------------------------------------------------------
		//
		// Values are registered on the enum TYPE, so the display names live once beside the enum
		// instead of once per field that uses it. This is why the attribute vocabulary has no
		// EnumNames payload: entt already answers "what are this type's options" via
		// meta_type::is_enum() plus its data() range.

		void RegisterEnums()
		{
			GE_REFLECT_TYPE(SceneCamera::ProjectionType)
				.data<SceneCamera::ProjectionType::Perspective>("Perspective")
				.data<SceneCamera::ProjectionType::Orthographic>("Orthographic");

			GE_REFLECT_TYPE(RigidBodyType)
				.data<RigidBodyType::Static>("Static")
				.data<RigidBodyType::Dynamic>("Dynamic")
				.data<RigidBodyType::Kinematic>("Kinematic");

			// The one enum persisted as a name rather than an ordinal, which is what lets it be
			// reordered freely - see the comment on AudioGroup in Audio/AudioTypes.h. AssetType is
			// the opposite case and is append-only for exactly that reason.
			GE_REFLECT_TYPE(AudioGroup)
				.traits(Trait::SerializeByName)
				.data<AudioGroup::Master>("Master")
				.data<AudioGroup::Music>("Music")
				.data<AudioGroup::SFX>("SFX");

			GE_REFLECT_TYPE(ParticleEmitterComponent::Mode)
				.data<ParticleEmitterComponent::Mode::Billboard>("Billboard")
				.data<ParticleEmitterComponent::Mode::Mesh>("Mesh");

			GE_REFLECT_TYPE(ParticleBlend)
				.data<ParticleBlend::Alpha>("Alpha")
				.data<ParticleBlend::Additive>("Additive");
		}

		// ---- Identity ---------------------------------------------------------------------
		//
		// The two components ComponentList deliberately excludes, because they are entity identity
		// rather than authored state. Registered anyway: the inspector's header row reads the tag,
		// and R4's prefab diffing has to know they exist in order to skip them.

		void RegisterIdentity()
		{
			GE_REFLECT_COMPONENT(IDComponent)
				.traits(Trait::Hidden | Trait::NotSerialized)
				.data<&IDComponent::ID>("ID")
					.traits(Trait::Custom);   // written as the entity's own "Entity:" key

			GE_REFLECT_COMPONENT(TagComponent)
				.custom<Attr>(Attr{}.Label("Tag"))
				.data<&TagComponent::Tag>("Tag");
		}

		// ---- Transform and hierarchy ------------------------------------------------------

		void RegisterTransform()
		{
			GE_REFLECT_COMPONENT(TransformComponent)
				.custom<Attr>(Attr{}.Label("Transform"))
				.data<&TransformComponent::Translation>("Translation")
					.custom<Attr>(Attr{}.Speed(0.1f))
				// Stored in radians, authored in degrees, and the round-trip is NOT exact - the
				// panel writes back only on an actual edit for that reason. A generic drawer has
				// to keep that discipline or it mints a phantom undo command per frame.
				.data<&TransformComponent::Rotation>("Rotation")
					.traits(Trait::Radians)
					.custom<Attr>(Attr{}.Speed(0.1f))
				.data<&TransformComponent::Scale>("Scale")
					.custom<Attr>(Attr{}.Speed(0.1f));

			// Derived data: recomputed by TransformSystem from TransformComponent plus
			// RelationshipComponent, never authored and never saved.
			GE_REFLECT_COMPONENT(WorldTransformComponent)
				.traits(Trait::Runtime)
				.data<&WorldTransformComponent::World>("World");

			// Structural. Both fields are serialized, but by the hand-written block: the two sides
			// of a link must agree, so writing either one generically would corrupt the hierarchy.
			// Reparenting goes through Scene's API.
			GE_REFLECT_COMPONENT(RelationshipComponent)
				.traits(Trait::Hidden)
				.data<&RelationshipComponent::Parent>("Parent")
					.traits(Trait::Custom)
				.data<&RelationshipComponent::Children>("Children")
					.traits(Trait::Custom);
		}

		// ---- Rendering --------------------------------------------------------------------

		void RegisterRendering()
		{
			GE_REFLECT_COMPONENT(SpriteRendererComponent)
				.custom<Attr>(Attr{}.Label("Sprite Renderer"))
				.data<&SpriteRendererComponent::Color>("Color")
					.traits(Trait::Color);

			GE_REFLECT_COMPONENT(StaticMeshComponent)
				.custom<Attr>(Attr{}.Label("Static Mesh"))
				.data<&StaticMeshComponent::Mesh>("Mesh")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Asset(AssetType::StaticMesh).Tip("Drop a mesh here"))
				// One row per renderer slot, and the slot COUNT comes from the mesh asset rather
				// than from this vector - the panel resizes it to match. Nothing generic can draw
				// that, and the YAML is a flow sequence of handles.
				.data<&StaticMeshComponent::MaterialOverrides>("MaterialOverrides")
					.traits(Trait::Custom)
					.custom<Attr>(Attr{}.Asset(AssetType::Material));

			GE_REFLECT_COMPONENT(AnimatorComponent)
				.custom<Attr>(Attr{}.Label("Animator"))
				// A combo over the mesh's own clip names, not a text field: the name IS the
				// reference, so the option list comes from the asset.
				.data<&AnimatorComponent::Clip>("Clip")
					.traits(Trait::OmitIfDefault | Trait::Custom)
				.data<&AnimatorComponent::Speed>("Speed")
					.custom<Attr>(Attr{}.Range(-10.0f, 10.0f).Speed(0.01f))
				.data<&AnimatorComponent::Playing>("Playing")
				.data<&AnimatorComponent::Loop>("Loop")
				// Deliberately not saved: a scene loads at the start of its clip, not wherever it
				// happened to be. Still editable - scrubbing is the only way to move a rig in edit
				// mode - but the scrub range is the clip's duration, which only the asset knows.
				.data<&AnimatorComponent::Time>("Time")
					.traits(Trait::NotSerialized | Trait::Custom)
					.custom<Attr>(Attr{}.Speed(0.01f))
				.data<&AnimatorComponent::Palette>("Palette")
					.traits(Trait::Runtime);

			// The projection type gates which six of the seven camera fields are meaningful, so
			// this component keeps a hand-written drawer. That is the editor's decision and needs
			// no flag here: R2 simply registers a drawer for it.
			GE_REFLECT_COMPONENT(CameraComponent)
				.custom<Attr>(Attr{}.Label("Camera"))
				.data<&CameraComponent::Camera>("Camera")
				.data<&CameraComponent::Primary>("Primary")
				.data<&CameraComponent::FixedAspectRatio>("FixedAspectRatio")
					.custom<Attr>(Attr{}.Label("Fixed Aspect Ratio"));
		}

		// ---- Lighting ---------------------------------------------------------------------
		//
		// Note the intensity ranges differ per light type and are not a typo: a directional light
		// is a multiplier on a full-hemisphere term, a punctual light is not.

		void RegisterLighting()
		{
			GE_REFLECT_COMPONENT(DirectionalLightComponent)
				.custom<Attr>(Attr{}.Label("Directional Light")
					.Tip("Direction is the entity's forward vector (-Z of its world transform)"))
				.data<&DirectionalLightComponent::Color>("Color")
					.traits(Trait::Color)
				.data<&DirectionalLightComponent::Intensity>("Intensity")
					.custom<Attr>(Attr{}.Range(0.0f, 100.0f).Speed(0.05f))
				.data<&DirectionalLightComponent::CastShadows>("CastShadows")
					.custom<Attr>(Attr{}.Label("Cast Shadows"));

			GE_REFLECT_COMPONENT(PointLightComponent)
				.custom<Attr>(Attr{}.Label("Point Light"))
				.data<&PointLightComponent::Color>("Color")
					.traits(Trait::Color)
				.data<&PointLightComponent::Intensity>("Intensity")
					.custom<Attr>(Attr{}.Range(0.0f, 1000.0f).Speed(0.05f))
				.data<&PointLightComponent::Radius>("Radius")
					.custom<Attr>(Attr{}.Range(0.0f, 1000.0f).Speed(0.1f))
				.data<&PointLightComponent::Falloff>("Falloff")
					.custom<Attr>(Attr{}.Range(0.01f, 16.0f).Speed(0.05f));

			GE_REFLECT_COMPONENT(SpotLightComponent)
				.custom<Attr>(Attr{}.Label("Spot Light"))
				.data<&SpotLightComponent::Color>("Color")
					.traits(Trait::Color)
				.data<&SpotLightComponent::Intensity>("Intensity")
					.custom<Attr>(Attr{}.Range(0.0f, 1000.0f).Speed(0.05f))
				.data<&SpotLightComponent::Range>("Range")
					.custom<Attr>(Attr{}.Range(0.0f, 1000.0f).Speed(0.1f))
				// Stored as half-angles in radians; the ranges are in degrees because Radians is
				// set. The panel also clamps outer >= inner, which is cross-field logic a generic
				// drawer cannot express - it stays hand-written.
				.data<&SpotLightComponent::InnerConeAngle>("InnerConeAngle")
					.traits(Trait::Radians)
					.custom<Attr>(Attr{}.Label("Inner Cone").Range(0.0f, 89.0f).Speed(0.5f))
				.data<&SpotLightComponent::OuterConeAngle>("OuterConeAngle")
					.traits(Trait::Radians)
					.custom<Attr>(Attr{}.Label("Outer Cone").Range(0.0f, 89.0f).Speed(0.5f))
				.data<&SpotLightComponent::Falloff>("Falloff")
					.custom<Attr>(Attr{}.Range(0.01f, 16.0f).Speed(0.05f));

			// Environment gates the two procedural colors: with a baked HDR cubemap assigned they
			// are unreachable fallbacks. Another hand-written drawer.
			GE_REFLECT_COMPONENT(SkyLightComponent)
				.custom<Attr>(Attr{}.Label("Sky Light"))
				.data<&SkyLightComponent::Environment>("Environment")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Asset(AssetType::Environment)
						.Tip("Using HDR IBL (procedural colors are fallback)"))
				.data<&SkyLightComponent::SkyColor>("SkyColor")
					.traits(Trait::Color)
					.custom<Attr>(Attr{}.Label("Sky Color"))
				.data<&SkyLightComponent::GroundColor>("GroundColor")
					.traits(Trait::Color)
					.custom<Attr>(Attr{}.Label("Ground Color"))
				.data<&SkyLightComponent::Intensity>("Intensity")
					.custom<Attr>(Attr{}.Range(0.0f, 20.0f).Speed(0.02f))
				.data<&SkyLightComponent::DrawSkybox>("DrawSkybox")
					.custom<Attr>(Attr{}.Label("Draw Skybox"));
		}

		// ---- Scripting --------------------------------------------------------------------

		void RegisterScripting()
		{
			// Three function/instance pointers and nothing authorable: a C++ binding, not data.
			// Registered with no fields so a consumer that walks all components does not have to
			// special-case its absence.
			GE_REFLECT_COMPONENT(NativeScriptComponent)
				.traits(Trait::Runtime);

			GE_REFLECT_COMPONENT(ScriptComponent)
				.custom<Attr>(Attr{}.Label("Script"))
				.data<&ScriptComponent::Script>("Script")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Asset(AssetType::Script))
				// Only the values this entity OVERRIDES, keyed by name, typed by a closed variant.
				// The widget for each one comes from the script's declared field list in
				// ScriptEngine, and the YAML is a sequence of {Name, Type, Value} maps - neither
				// is derivable from the map alone.
				.data<&ScriptComponent::Fields>("Fields")
					.traits(Trait::Custom);
		}

		// ---- Audio ------------------------------------------------------------------------

		void RegisterAudio()
		{
			GE_REFLECT_COMPONENT(AudioSourceComponent)
				.custom<Attr>(Attr{}.Label("Audio Source")
					.Tip("Clip, Spatialize and Stream apply when play starts"))
				.data<&AudioSourceComponent::Clip>("Clip")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Asset(AssetType::Audio)
						.Tip("Drop a .wav, .mp3 or .flac file here"))
				.data<&AudioSourceComponent::Volume>("Volume")
					.custom<Attr>(Attr{}.Range(0.0f, 1.0f).Speed(0.01f))
				.data<&AudioSourceComponent::Pitch>("Pitch")
					.custom<Attr>(Attr{}.Range(0.25f, 4.0f).Speed(0.01f))
				.data<&AudioSourceComponent::Loop>("Loop")
				.data<&AudioSourceComponent::PlayOnStart>("PlayOnStart")
					.custom<Attr>(Attr{}.Label("Play On Start"))
				.data<&AudioSourceComponent::Spatialize>("Spatialize")
				.data<&AudioSourceComponent::Stream>("Stream")
				.data<&AudioSourceComponent::Group>("Group");

			GE_REFLECT_COMPONENT(AudioListenerComponent)
				.custom<Attr>(Attr{}.Label("Audio Listener")
					.Tip("Falls back to the primary camera when absent"))
				.data<&AudioListenerComponent::Primary>("Primary");
		}

		// ---- Physics ----------------------------------------------------------------------

		void RegisterPhysics()
		{
			GE_REFLECT_COMPONENT(RigidBodyComponent)
				.custom<Attr>(Attr{}.Label("Rigid Body"))
				.data<&RigidBodyComponent::Type>("Type")
				.data<&RigidBodyComponent::Mass>("Mass")
					.custom<Attr>(Attr{}.Range(0.001f, 100000.0f).Speed(0.05f))
				.data<&RigidBodyComponent::LinearDamping>("LinearDamping")
					.custom<Attr>(Attr{}.Label("Linear Damping").Range(0.0f, 10.0f).Speed(0.01f))
				.data<&RigidBodyComponent::AngularDamping>("AngularDamping")
					.custom<Attr>(Attr{}.Label("Angular Damping").Range(0.0f, 10.0f).Speed(0.01f))
				.data<&RigidBodyComponent::UseGravity>("UseGravity")
					.custom<Attr>(Attr{}.Label("Use Gravity"));

			// Flatten on Material is not cosmetic: SceneSerializer emits Friction and Restitution
			// as SIBLINGS of HalfExtents, never under a "Material" sub-map. Without this flag a
			// generic writer would nest them and invalidate every collider in every saved scene.
			GE_REFLECT_COMPONENT(BoxColliderComponent)
				.custom<Attr>(Attr{}.Label("Box Collider"))
				.data<&BoxColliderComponent::HalfExtents>("HalfExtents")
					.custom<Attr>(Attr{}.Label("Half Extents").Speed(0.05f))
				.data<&BoxColliderComponent::Offset>("Offset")
					.custom<Attr>(Attr{}.Speed(0.05f))
				.data<&BoxColliderComponent::Material>("Material")
					.traits(Trait::Flatten);

			GE_REFLECT_COMPONENT(SphereColliderComponent)
				.custom<Attr>(Attr{}.Label("Sphere Collider"))
				.data<&SphereColliderComponent::Radius>("Radius")
					.custom<Attr>(Attr{}.Range(0.001f, 1000.0f).Speed(0.05f))
				.data<&SphereColliderComponent::Offset>("Offset")
					.custom<Attr>(Attr{}.Speed(0.05f))
				.data<&SphereColliderComponent::Material>("Material")
					.traits(Trait::Flatten);

			GE_REFLECT_COMPONENT(CapsuleColliderComponent)
				.custom<Attr>(Attr{}.Label("Capsule Collider"))
				.data<&CapsuleColliderComponent::Radius>("Radius")
					.custom<Attr>(Attr{}.Range(0.001f, 1000.0f).Speed(0.05f))
				.data<&CapsuleColliderComponent::HalfHeight>("HalfHeight")
					.custom<Attr>(Attr{}.Label("Half Height").Range(0.001f, 1000.0f).Speed(0.05f))
				.data<&CapsuleColliderComponent::Offset>("Offset")
					.custom<Attr>(Attr{}.Speed(0.05f))
				.data<&CapsuleColliderComponent::Material>("Material")
					.traits(Trait::Flatten);
		}

		// ---- Prefabs ----------------------------------------------------------------------

		void RegisterPrefabs()
		{
			// Read-only on purpose: the link is created by "Create Prefab" and followed by
			// Apply/Revert. There is no field to type a handle into.
			GE_REFLECT_COMPONENT(PrefabInstanceComponent)
				.custom<Attr>(Attr{}.Label("Prefab Instance")
					.Tip("Removing this unlinks the entity from its prefab"))
				.data<&PrefabInstanceComponent::Source>("Source")
					.traits(Trait::ReadOnly | Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Asset(AssetType::Prefab));
		}

		// ---- Particles --------------------------------------------------------------------
		//
		// The largest block, and the one that pays least: 142 of the inspector's 660 reflectable
		// lines are here, and most of that is the curve and gradient editors plus the paired
		// min/max widgets - exactly what a generic inspector handles worst.
		//
		// Every authored field carries OmitIfDefault because this component's serializer compares
		// against a default-constructed instance for all twenty of them, unlike every other
		// component, which writes unconditionally. Encoding that per field rather than as a
		// type-level "all my fields omit" rule is deliberate: an inherited flag is invisible at
		// the point a reader is looking.

		void RegisterParticles()
		{
			GE_REFLECT_COMPONENT(ParticleEmitterComponent)
				.custom<Attr>(Attr{}.Label("Particle Emitter"))

				// Emission
				.data<&ParticleEmitterComponent::RateOverTime>("RateOverTime")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Rate Over Time").In("Emission").Range(0.0f, 100000.0f).Speed(0.1f))
				.data<&ParticleEmitterComponent::MaxParticles>("MaxParticles")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Max Particles").In("Emission").Range(0.0f, 100000.0f).Speed(1.0f))
				.data<&ParticleEmitterComponent::Looping>("Looping")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.In("Emission"))
				.data<&ParticleEmitterComponent::Duration>("Duration")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.In("Emission").Range(0.0f, 1000.0f).Speed(0.05f))
				.data<&ParticleEmitterComponent::PlayOnStart>("PlayOnStart")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Play On Start").In("Emission"))

				// Initial state. The Min/Max pairs are drawn as one row that clamps the other half,
				// so they keep a hand-written widget; the attributes below still drive R3.
				.data<&ParticleEmitterComponent::LifetimeMin>("LifetimeMin")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Lifetime Min").In("Initial").Speed(0.02f))
				.data<&ParticleEmitterComponent::LifetimeMax>("LifetimeMax")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Lifetime Max").In("Initial").Speed(0.02f))
				.data<&ParticleEmitterComponent::SpeedMin>("SpeedMin")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Speed Min").In("Initial").Speed(0.05f))
				.data<&ParticleEmitterComponent::SpeedMax>("SpeedMax")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Speed Max").In("Initial").Speed(0.05f))
				// Degrees on disk and in the widget, unlike the spot-light cone: no Radians here.
				.data<&ParticleEmitterComponent::ConeAngle>("ConeAngle")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Cone Angle").In("Initial").Range(0.0f, 180.0f).Speed(0.5f))
				.data<&ParticleEmitterComponent::StartSizeMin>("StartSizeMin")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Start Size Min").In("Initial").Speed(0.01f))
				.data<&ParticleEmitterComponent::StartSizeMax>("StartSizeMax")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Start Size Max").In("Initial").Speed(0.01f))
				.data<&ParticleEmitterComponent::StartRotationMin>("StartRotationMin")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Start Rotation Min").In("Initial").Speed(1.0f))
				.data<&ParticleEmitterComponent::StartRotationMax>("StartRotationMax")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Start Rotation Max").In("Initial").Speed(1.0f))
				.data<&ParticleEmitterComponent::RotationSpeedMin>("RotationSpeedMin")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Rotation Speed Min").In("Initial").Speed(1.0f))
				.data<&ParticleEmitterComponent::RotationSpeedMax>("RotationSpeedMax")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Rotation Speed Max").In("Initial").Speed(1.0f))
				.data<&ParticleEmitterComponent::GravityModifier>("GravityModifier")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Gravity Modifier").In("Initial").Speed(0.05f))
				.data<&ParticleEmitterComponent::WorldSpace>("WorldSpace")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("World Space").In("Initial"))
				// The upper bound is INT_MAX as a float, which is not representable exactly; it is
				// a widget clamp, not a validity rule (SeedRng accepts any uint32).
				.data<&ParticleEmitterComponent::Seed>("Seed")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.In("Initial").Range(0.0f, 2147483647.0f).Speed(1.0f)
						.Tip("0 derives from the entity UUID at play"))

				// Over lifetime. Min/Max is the Y range the curve editor draws.
				.data<&ParticleEmitterComponent::SizeCurve>("SizeCurve")
					.traits(Trait::OmitIfDefault | Trait::Custom)
					.custom<Attr>(Attr{}.Label("Size Curve").In("Over Lifetime").Range(0.0f, 2.0f))
				.data<&ParticleEmitterComponent::ColorOverLifetime>("ColorOverLifetime")
					.traits(Trait::OmitIfDefault | Trait::Custom)
					.custom<Attr>(Attr{}.Label("Color Over Lifetime").In("Over Lifetime"))

				// Rendering. RenderMode gates which asset slots are meaningful.
				.data<&ParticleEmitterComponent::RenderMode>("RenderMode")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.Label("Render Mode").In("Rendering"))
				.data<&ParticleEmitterComponent::Texture>("Texture")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.In("Rendering").Asset(AssetType::Texture)
						.Tip("Drop a texture here; unset is white"))
				.data<&ParticleEmitterComponent::Blend>("Blend")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.In("Rendering"))
				.data<&ParticleEmitterComponent::Mesh>("Mesh")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.In("Rendering").Asset(AssetType::StaticMesh)
						.Tip("Drop a mesh here"))
				.data<&ParticleEmitterComponent::Material>("Material")
					.traits(Trait::OmitIfDefault)
					.custom<Attr>(Attr{}.In("Rendering").Asset(AssetType::Material)
						.Tip("Drop a .gmat here; unset is the mesh default"))

				// Runtime. Playing, Time and the live count are shown in the emitter's status line
				// but never edited or saved; the rest is not shown at all. Scene::Copy resets all
				// of it through ResetRuntime().
				.data<&ParticleEmitterComponent::Playing>("Playing")
					.traits(Trait::ReadOnly | Trait::NotSerialized)
				.data<&ParticleEmitterComponent::Time>("Time")
					.traits(Trait::ReadOnly | Trait::NotSerialized)
				.data<&ParticleEmitterComponent::EmitAccumulator>("EmitAccumulator")
					.traits(Trait::Runtime)
				.data<&ParticleEmitterComponent::BurstPending>("BurstPending")
					.traits(Trait::Runtime)
				.data<&ParticleEmitterComponent::Rng>("Rng")
					.traits(Trait::Runtime)
				.data<&ParticleEmitterComponent::Pool>("Pool")
					.traits(Trait::Runtime)
				.data<&ParticleEmitterComponent::WorldBounds>("WorldBounds")
					.traits(Trait::Runtime);
		}

		// ---- Size sentinels ---------------------------------------------------------------
		//
		// The only forcing function for "added a member, forgot to register it". Bump one
		// deliberately, AFTER reflecting the new field.
		//
		// Two honest limits. Padding: a bool dropped into existing padding does not move sizeof -
		// AudioSourceComponent has three spare bytes right now, so a fifth flag there would slip
		// through. And these cover 16 of the 21 ComponentList entries - every one with NO
		// standard-library container member. sizeof(std::string) is 40 with MSVC's STL and 32 with
		// libstdc++, and sizeof(std::vector) and sizeof(std::unordered_map) differ likewise, so a
		// sentinel on TagComponent, RelationshipComponent, StaticMeshComponent, AnimatorComponent,
		// ScriptComponent or ParticleEmitterComponent would have to be a table of per-platform
		// numbers - which costs more than it catches, on a codebase that builds for Windows, Linux
		// and macOS. The rule is mechanical rather than a judgement call per component: library
		// container member => no sentinel.
		//
		// AssetRef<T> is not a container and keeps its sentinel: it is a UUID, a shared_ptr and a
		// uint32_t, and sizeof(shared_ptr) is two pointers on every mainstream implementation.
		// SkyLightComponent's number moved from 40 to 64 when its Environment stopped being a
		// bare handle, which is the sentinel doing exactly the job it exists for.
		static_assert(sizeof(TransformComponent) == 36, "TransformComponent changed - reflect the new field");
		static_assert(sizeof(WorldTransformComponent) == 64, "WorldTransformComponent changed - reflect the new field");
		static_assert(sizeof(SpriteRendererComponent) == 16, "SpriteRendererComponent changed - reflect the new field");
		static_assert(sizeof(CameraComponent) == 112, "CameraComponent changed - reflect the new field");
		static_assert(sizeof(DirectionalLightComponent) == 20, "DirectionalLightComponent changed - reflect the new field");
		static_assert(sizeof(PointLightComponent) == 24, "PointLightComponent changed - reflect the new field");
		static_assert(sizeof(SpotLightComponent) == 32, "SpotLightComponent changed - reflect the new field");
		static_assert(sizeof(SkyLightComponent) == 64, "SkyLightComponent changed - reflect the new field");
		static_assert(sizeof(NativeScriptComponent) == 24, "NativeScriptComponent changed - reflect the new field");
		static_assert(sizeof(AudioSourceComponent) == 24, "AudioSourceComponent changed - reflect the new field");
		static_assert(sizeof(AudioListenerComponent) == 1, "AudioListenerComponent changed - reflect the new field");
		static_assert(sizeof(RigidBodyComponent) == 20, "RigidBodyComponent changed - reflect the new field");
		static_assert(sizeof(BoxColliderComponent) == 32, "BoxColliderComponent changed - reflect the new field");
		static_assert(sizeof(SphereColliderComponent) == 24, "SphereColliderComponent changed - reflect the new field");
		static_assert(sizeof(CapsuleColliderComponent) == 28, "CapsuleColliderComponent changed - reflect the new field");
		static_assert(sizeof(PrefabInstanceComponent) == 8, "PrefabInstanceComponent changed - reflect the new field");

		// Not in ComponentList, but reflected here and equally worth guarding.
		static_assert(sizeof(IDComponent) == 8, "IDComponent changed - reflect the new field");
		static_assert(sizeof(PhysicsMaterial) == 8, "PhysicsMaterial changed - reflect the new field");

		// ---- Validation helpers -----------------------------------------------------------

		bool IsSameType(const entt::meta_type& type, const entt::type_info& info)
		{
			return type && type.info() == info;
		}

		// What may carry an `Asset(...)` attribute. A bare AssetHandle accepts any slot - it is a
		// UUID and nothing about it says which kind - while an AssetRef<T> accepts exactly one,
		// so the two cases are checked differently on purpose.
		bool IsValidAssetSlot(const entt::meta_type& type, AssetType slot)
		{
			if (IsSameType(type, entt::type_id<AssetHandle>())
				|| IsSameType(type, entt::type_id<std::vector<AssetHandle>>()))
			{
				return true;
			}

			struct TypedSlot
			{
				const entt::type_info& Info;
				AssetType Slot;
			};

			// Spelled out rather than derived, for the same reason AssetTypeFromString is: a new
			// managed type that is missing here fails validation loudly at boot instead of
			// silently opting out of the check.
			const TypedSlot typed[] = {
				{ entt::type_id<AssetRef<Mesh>>(),                      AssetType::StaticMesh  },
				{ entt::type_id<AssetRef<Environment>>(),               AssetType::Environment },
				{ entt::type_id<AssetRef<Texture2D>>(),                 AssetType::Texture     },
				{ entt::type_id<AssetRef<Material>>(),                  AssetType::Material    },
				{ entt::type_id<std::vector<AssetRef<Material>>>(),     AssetType::Material    },
			};

			for (const TypedSlot& candidate : typed)
			{
				if (IsSameType(type, candidate.Info))
					return candidate.Slot == slot;
			}

			return false;
		}

		// Reports every problem rather than stopping at the first: a registration mistake is
		// usually a copy-paste that was repeated.
		bool ValidateField(const char* owner, const entt::meta_data& field)
		{
			bool ok = true;

			if (field.name() == nullptr)
			{
				GE_CORE_ERROR("Reflection: {0} has a field with no name", owner);
				return false;   // nothing below can be reported usefully without one
			}

			const entt::meta_type type = field.type();
			const Attr* attr = Attributes(field);

			if (Has(field, Trait::Color)
				&& !IsSameType(type, entt::type_id<glm::vec3>())
				&& !IsSameType(type, entt::type_id<glm::vec4>()))
			{
				GE_CORE_ERROR("Reflection: {0}::{1} is marked Color but is not a vec3/vec4", owner, field.name());
				ok = false;
			}

			if (Has(field, Trait::Radians)
				&& !IsSameType(type, entt::type_id<float>())
				&& !IsSameType(type, entt::type_id<glm::vec3>()))
			{
				GE_CORE_ERROR("Reflection: {0}::{1} is marked Radians but is not a float/vec3", owner, field.name());
				ok = false;
			}

			// AssetHandle is a UUID alias, so this is the check that catches an Asset() attribute
			// put on the wrong field - the type system cannot.
			//
			// An AssetRef<T> field carries its asset type in the C++ type, so the check gets
			// stronger there: the declared slot has to *agree* with AssetTypeOf<T>. That catches
			// a copy-pasted `.Asset(AssetType::Texture)` on an AssetRef<Environment>, which the
			// bare-handle form could never catch.
			if (attr && attr->Slot != AssetType::None && !IsValidAssetSlot(type, attr->Slot))
			{
				GE_CORE_ERROR("Reflection: {0}::{1} has an asset slot ({2}) that does not match "
					"its type", owner, field.name(), AssetTypeToString(attr->Slot));
				ok = false;
			}

			if (Has(field, Trait::Flatten) && !IsReflected(type))
			{
				GE_CORE_ERROR("Reflection: {0}::{1} is marked Flatten but its type is not reflected", owner, field.name());
				ok = false;
			}

			return ok;
		}
	}

	void Init()
	{
		GE_PROFILE_FUNCTION();

		// One meta_ctx for the process, from entt's locator. Fine here because the engine is a
		// single static library linked into one binary; a plugin DLL would need its context
		// passed in explicitly, which is a problem Ganymed does not have.
		RegisterSupportingTypes();
		RegisterEnums();
		RegisterIdentity();
		RegisterTransform();
		RegisterRendering();
		RegisterLighting();
		RegisterScripting();
		RegisterAudio();
		RegisterPhysics();
		RegisterPrefabs();
		RegisterParticles();

		// entt::resolve() returns an iterable adaptor, not a container - it has no size(), and the
		// count is worth logging on its own: a boot line reading a number far below 40 is the
		// cheapest possible signal that a registration block was dropped by the linker.
		std::size_t types = 0, fields = 0;
		for (const auto [id, type] : entt::resolve())
		{
			if (!IsReflected(type))
				continue;

			++types;
			for (const auto [fieldId, field] : type.data())
				++fields;
		}

		GE_CORE_INFO("Reflection initialised: {0} types, {1} members", types, fields);

#ifdef GE_DEBUG
		if (Validate())
			GE_CORE_TRACE("Reflection validation passed");
		else
			GE_CORE_ASSERT(false, "Reflection registration is inconsistent - see the log above");
#endif
	}

	bool Validate()
	{
		bool ok = true;

		// Note what this deliberately does NOT do: `if (entt::resolve<T>())`. entt synthesizes a
		// node for any type it is asked about, so that test passes for a type nobody registered
		// and would report an empty registration file as healthy. IsReflected checks the name,
		// which only .type(id, name) ever sets.
		ForEachType(ComponentList{}, [&ok](auto tag)
		{
			using T = typename decltype(tag)::Type;

			const entt::meta_type type = entt::resolve<T>();
			if (!IsReflected(type))
			{
				GE_CORE_ERROR("Reflection: {0} is in ComponentList but was never registered",
					entt::type_id<T>().name());
				ok = false;
				return;
			}

			if (!Has(type, Trait::Component))
			{
				GE_CORE_ERROR("Reflection: {0} was registered with GE_REFLECT_TYPE, not GE_REFLECT_COMPONENT",
					type.name());
				ok = false;
			}
		});

		for (const auto [id, type] : entt::resolve())
		{
			if (!IsReflected(type))
				continue;

			if (Has(type, Trait::SerializeByName) && !type.is_enum())
			{
				GE_CORE_ERROR("Reflection: {0} is marked SerializeByName but is not an enum", type.name());
				ok = false;
			}

			// Both meta ranges iterate as [id, value] pairs, not as bare values.
			for (const auto [fieldId, field] : type.data())
				ok &= ValidateField(type.name(), field);
		}

		return ok;
	}
}
