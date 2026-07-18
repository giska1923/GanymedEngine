#pragma once

#include <entt/entt.hpp>

#include "AccessWrappers.h"
#include "SingletonTraits.h"
#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Log.h"
#include "GanymedE/Scene/Scene.h"

// Singletons over registry.ctx().
//
// entt already stores per-registry data; what it does not give you is presence-checked access with
// declared read/write intent, or a way to ask "has this changed since I last looked". These views
// add exactly that, deliberately as a separate small family rather than folding singletons into
// the component views — a singleton has no entity, so most of the component grammar is meaningless
// for it.
//
// Access is declared with the same wrappers as component views: SingletonAccessView<T> is
// read-only, SingletonAccessView<RW<T>> is writeable and requires Modify() to write.

namespace GanymedE {

	namespace ECS {

		template<typename T, bool Writeable>
		class SingletonAccessor
		{
		public:
			using SingletonType = T;

			SingletonAccessor() = default;
			SingletonAccessor(T* singleton, size_t* epoch)
				: m_Singleton(singleton), m_Epoch(epoch) {}

			explicit operator bool() const { return m_Singleton != nullptr; }
			const T& operator*()  const { return *m_Singleton; }
			const T* operator->() const { return m_Singleton; }
			const T* Get()        const { return m_Singleton; }

			// The only write path, mirroring ComponentAccessor: bumps the epoch once per accessor
			// so a SingletonChangeView can tell that this singleton moved on.
			T& Modify()
			{
				static_assert(Writeable, "Modify() on a read-only singleton view");
				GE_CORE_ASSERT(m_Singleton, "Modify() on a singleton that is not present");
				if (m_Epoch)
				{
					++*m_Epoch;
					m_Epoch = nullptr;
				}
				return *m_Singleton;
			}

		private:
			T* m_Singleton = nullptr;
			size_t* m_Epoch = nullptr;
		};

		namespace Detail {

			// Unwraps `T` or `RW<T>` into the singleton type plus its declared writeability.
			template<typename E>
			struct SingletonElement
			{
				using Traits = ElementTraits<E>;
				using Type = TypeListElementT<typename Traits::AccessedTypes, 0>;
				static constexpr bool Writeable = Traits::Writeable;
			};
		}

		template<typename E>
		class SingletonAccessView
		{
			using Element = Detail::SingletonElement<E>;
			using T = typename Element::Type;
			static constexpr bool Writeable = Element::Writeable;

		public:
			struct State {};

			SingletonAccessView(Scene& scene, State&) : m_Scene(&scene) {}
			explicit SingletonAccessView(Scene& scene) : m_Scene(&scene) {}

			bool Has() const { return m_Scene->HasSingleton<T>(); }

			// Null accessor when absent — checkable rather than fatal.
			SingletonAccessor<T, Writeable> Find() const
			{
				T* singleton = m_Scene->FindSingleton<T>();
				if (!singleton)
					return {};
				return { singleton, &m_Scene->GetSingletonEpoch(entt::type_hash<T>::value()) };
			}

			SingletonAccessor<T, Writeable> Get() const
			{
				GE_CORE_ASSERT(Has(), "SingletonAccessView::Get on a singleton that is not present");
				return Find();
			}

		private:
			Scene* m_Scene = nullptr;
		};

		// "Has the singleton changed since *this system* last looked" — the singleton counterpart
		// of ChangeView, and read-time based in the same way.
		//
		// The epoch starts at 1 when the singleton is created, and view state starts at 0, so the
		// first read reports a present singleton as changed. That matches the component rule:
		// a first-ever read returns the world rather than nothing.
		template<typename E>
		class SingletonChangeView
		{
			using Element = Detail::SingletonElement<E>;
			using T = typename Element::Type;
			static constexpr bool Writeable = Element::Writeable;

			static_assert(SingletonTraits<T>::TrackChanges,
				"SingletonChangeView needs SingletonTraits<T>::TrackChanges");

		public:
			struct State { size_t LastEpoch = 0; };

			SingletonChangeView(Scene& scene, State& state) : m_Scene(&scene)
			{
				const size_t current = scene.GetSingletonEpoch(entt::type_hash<T>::value());
				m_Changed = (current != state.LastEpoch);
				state.LastEpoch = current;
			}

			// Present *and* changed since the last read.
			bool Has() const { return m_Changed && m_Scene->HasSingleton<T>(); }

			SingletonAccessor<T, Writeable> Find() const
			{
				if (!Has())
					return {};
				return { m_Scene->FindSingleton<T>(),
					&m_Scene->GetSingletonEpoch(entt::type_hash<T>::value()) };
			}

			SingletonAccessor<T, Writeable> Get() const
			{
				GE_CORE_ASSERT(Has(), "SingletonChangeView::Get when the singleton did not change");
				return Find();
			}

		private:
			Scene* m_Scene = nullptr;
			bool m_Changed = false;
		};
	}
}
