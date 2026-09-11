# ToDo — Reflection, prefabs and the inspector

R1–R5 of [`REFLECTION_ROADMAP.md`](../history/REFLECTION_ROADMAP.md) are done: every component is
registered and self-validating, 15 of 20 inspector sections and **every** component's serialization
run off that registration, and prefab per-property overrides work. See
[scene.md](../engine/scene.md#member-reflection) and [editor.md](../editor/editor.md).

What is left is a short tail, roughly in descending order of value.

---

## `PrefabSerializer::ReadRootTransform` still reads the transform by hand

The last hand-written component read in the codebase.
[`PrefabSerializer.cpp:189`](../../GanymedEngine/source/GanymedE/Scene/PrefabSerializer.cpp#L189):

```cpp
out.Translation = transform["Translation"].as<glm::vec3>();
out.Rotation    = transform["Rotation"].as<glm::vec3>();
out.Scale       = transform["Scale"].as<glm::vec3>();
```

`ReadReflectedComponent(transform, out)` replaces all three, and gains the generic reader's
tolerance for a missing key — this version throws on one. Everything else in the prefab path
already goes through reflection, because `PrefabSerializer` delegates its component blocks to
`SceneSerializer::SerializeEntity`/`DeserializeEntity`.

## `PrefabSerializer::Save` bakes a stale `PrefabMemberComponent` into the file

`BuildCanonicalCopy` strips `PrefabInstanceComponent` from the subtree before writing, with the
reasoning that an instance root inside a prefab file would be a nested prefab, which v1 does not do.
`PrefabMemberComponent` needs the same treatment and does not get it: creating a prefab from a
subtree that is *itself* part of an instance writes that subtree's old canonical IDs into the new
file.

Found by the R5 round-trip probe, which instantiates and re-saves; pre-existing.

## Per-property apply-to-prefab

Revert-one-field exists. **Push-one-field-to-the-prefab does not** — apply still writes the whole
instance. The roadmap calls this the natural next step now that the diff exists, and the diff
(`EditorPrefabOverrides.h`) is what makes it cheap: the same comparison that decides whether to draw
the revert affordance identifies exactly what a per-property apply would write.

## The prefab template cache cannot see a `.gprefab` edited on disk

It is dropped on scene change, so an edit is picked up eventually, but not by the file watcher.
Hooking `AssetWatcher` would fix it — except prefabs are path-resolved and have no asset manager, so
`AssetManager::OnAssetModified` returns false for them. That is the real blocker and it is an asset
layer question, not an editor one. See [assets.md](../engine/assets.md).

## Multi-entity editing gaps

R4b landed multi-select with one-gesture-one-undo-command. Three things it did not cover:

- **Shift-range selection** in the hierarchy panel. Ctrl-toggle works; shift-range does not.
- **The gizmo moves the primary selection only**, not the whole selection.
- **The no-active-phase edit path is single-entity for undo.** An edit that completes without ImGui
  ever reporting an active ID — a checkbox, a combo — mints a command for the primary entity only.

## Per-field override marking never fully closes

The per-field revert affordance lives in the reflected property drawer, so hand-written sections get
the section-level `*` and nothing finer. **This one does not go away**: the five remaining
hand-written sections (Static Mesh, Animator, Script, plus the Tag field and the prefab action
buttons) are driven by asset and Lua data rather than component members, so they are not converting.
Recorded so the gap is understood as permanent rather than pending.

## Not done deliberately

A scripted drag test proving one-gesture-one-undo-command end to end, by driving ImGui's input state
directly. The property is currently verified by simulating the gesture at the panel's own API
boundary, which does not exercise ImGui's `GetActiveID` transitions. Listed because it was
considered and declined, not because it is queued.
