# ToDo — Reflection, prefabs and the inspector

R1–R5 of [`REFLECTION_ROADMAP.md`](../history/REFLECTION_ROADMAP.md) are done: every component is
registered and self-validating, 15 of 20 inspector sections and **every** component's serialization
run off that registration, and prefab per-property overrides work. See
[scene.md](../engine/scene.md#member-reflection) and [editor.md](../editor/editor.md).

What is left is a short tail, roughly in descending order of value.

**Per-property apply-to-prefab is done** — right-clicking an overridden field offers *Apply to
Prefab* beside *Revert to Prefab*, writing that one field into the asset and nothing else. See
[editor.md](../editor/editor.md#per-property-overrides).

**The prefab template cache is done** — it is dropped on scene change, after a whole-instance
apply, and now on a `.gprefab` edited on disk. That last one was recorded as blocked on prefabs
having no asset manager; it was not. The watcher had always detected the edit, settled it and
resolved the handle, and `OnAssetModified` dropped it because no manager owned the type. A change
listener forwards it in about fifteen lines, and no `Prefab` asset class had to be invented. See
[assets.md](../engine/assets.md#change-listeners-for-state-the-asset-layer-does-not-own).

**The three multi-entity editing gaps are done too**: shift-range selection, the gizmo driving the
whole selection, and the no-active-phase undo path — which was not a gap but a correctness bug, and
lost data (toggle a checkbox across four entities, Ctrl+Z, and one came back). See
[editor.md](../editor/editor.md#multi-entity-editing). Verified by driving the editor with
synthesised input rather than by inspection.

Two further entries are gone: `PrefabSerializer::ReadRootTransform` now uses `ReadReflectedComponent` (the
last hand-written component read in the engine), and `PrefabSerializer::Save` no longer bakes a
stale `PrefabMemberComponent` into the file it writes — that one turned out to assert on the next
instantiate, so it was a crash rather than diff noise. Both are described in
[scene.md](../engine/scene.md).

---

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
