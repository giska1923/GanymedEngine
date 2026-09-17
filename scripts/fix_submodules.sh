#!/bin/bash
#
# Restore this repository's git submodules to the commits it pins.
#
# ---- Why this does not re-add anything ----
#
# The previous version of this script kept a hand-written list of four submodules
# (GLFW, imgui, glm, spdlog) out of the fifteen in .gitmodules, and "fixed" them with
# `git submodule add --force <url>`. That is worse than doing nothing:
#
#   - `submodule add` checks out the upstream default branch's TIP, not the commit this
#     repository pins. bgfx, bimg, bx, JoltPhysics and RmlUi are all pinned to specific
#     commits that the build is known to work against; silently moving them to master is
#     how you get a broken build that looks repaired.
#   - Its second mode cloned each dependency and deleted the clone's `.git`, turning a
#     submodule into thousands of untracked files under a path `.gitmodules` still calls
#     a submodule.
#   - Its third mode created EMPTY directories, including for `Glad`, `stb_image` and
#     `yaml-cpp`, which are not submodules at all. An empty directory is worse than a
#     missing one: premake generates a project for it that compiles nothing, and the
#     first sign of trouble is a link error naming an archive with no rule to build it.
#
# What actually repairs a submodule is `git submodule update --init --recursive`, which
# checks out exactly the pinned commit. Everything here is that, plus a way to see what
# is wrong first and a bigger hammer for a working tree that will not move.
#
# Usage:
#   ./scripts/fix_submodules.sh            repair: sync URLs, init and update everything
#   ./scripts/fix_submodules.sh --check    report only, change nothing (exit 1 if broken)
#   ./scripts/fix_submodules.sh --force    deinit first, for a wedged working tree
#   ./scripts/fix_submodules.sh --help

set -uo pipefail

MODE="repair"

case "${1:-}" in
    --check) MODE="check" ;;
    --force) MODE="force" ;;
    --help|-h)
        cat <<'USAGE'
Restore this repository's git submodules to the commits it pins.

  ./scripts/fix_submodules.sh            repair: sync URLs, init and update everything
  ./scripts/fix_submodules.sh --check    report only, change nothing (exit 1 if broken)
  ./scripts/fix_submodules.sh --force    deinit first, for a wedged working tree

The list of submodules comes from .gitmodules, so it is never out of date. Nothing here
re-adds or re-clones a submodule: `git submodule add` would check out the upstream tip
rather than the commit this repository pins.
USAGE
        exit 0
        ;;
    "") ;;
    *)
        echo "Unknown option: $1 (try --help)" >&2
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.." || exit 1

echo "=== GanymedEngine - submodules ($MODE) ==="
echo "Project root: $(pwd)"
echo ""

if ! git rev-parse --git-dir >/dev/null 2>&1; then
    echo "❌ Not a git repository. Submodules need one; re-clone rather than downloading a zip."
    exit 1
fi

if [ ! -f .gitmodules ]; then
    echo "❌ No .gitmodules here."
    exit 1
fi

# The list comes from .gitmodules, never from this file. That is the whole point: a
# submodule added to the project is covered the moment it is added, with no second list
# to forget.
mapfile -t PATHS < <(git config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk '{print $2}' | sort)

if [ "${#PATHS[@]}" -eq 0 ]; then
    echo "❌ .gitmodules lists no submodules - it is probably malformed."
    exit 1
fi

echo "📋 ${#PATHS[@]} submodule(s) declared."
echo ""

# ---- Repair ---------------------------------------------------------------------------

if [ "$MODE" = "force" ]; then
    # Only for a working tree that will not move: a half-deleted submodule, or one left
    # behind by an interrupted clone. Deinit drops the checkout and the config entry; the
    # update below puts back the PINNED commit, so this loses local edits inside a
    # submodule and nothing else.
    echo "🧹 Deinitialising (local changes inside submodules will be discarded)..."
    git submodule deinit -f --all || echo "  (deinit reported an error; continuing)"
    echo ""
fi

if [ "$MODE" != "check" ]; then
    # sync first: it rewrites each submodule's configured URL from .gitmodules, which is
    # what repairs a clone made before a URL changed. Without it, update happily fetches
    # from the old remote and reports success.
    echo "🔗 Syncing submodule URLs..."
    git submodule sync --recursive >/dev/null || true

    echo "📥 Initialising and updating (this restores the pinned commits)..."
    if git submodule update --init --recursive; then
        echo "  ✅ update finished"
    else
        echo "  ⚠️  update reported errors - the report below says which ones"
    fi
    echo ""
fi

# ---- Report ---------------------------------------------------------------------------
#
# `git submodule status` prefixes each line: '-' not initialised, '+' checked out at a
# commit other than the one pinned, 'U' merge conflicts, ' ' correct. Reading that is
# what turns "the build says there is no rule to make libenkiTS.a" into "enkiTS is not
# checked out", which is a one-line answer rather than an afternoon.

echo "🔍 Status:"
BROKEN=0
EMPTY=0
MISFILED=0
CHECKED=0

while IFS= read -r line; do
    flag="${line:0:1}"
    rest="${line:1}"
    sha="${rest%% *}"
    path="$(echo "${rest#* }" | awk '{print $1}')"

    CHECKED=$((CHECKED + 1))
    case "$flag" in
        "-") echo "  ❌ not initialised   $path"; BROKEN=$((BROKEN + 1)) ;;
        "+") echo "  ⚠️  wrong commit      $path (checked out $sha, not the pinned one)"; BROKEN=$((BROKEN + 1)) ;;
        "U") echo "  ❌ merge conflict    $path"; BROKEN=$((BROKEN + 1)) ;;
        *)   echo "  ✅ ok                $path" ;;
    esac
done < <(git submodule status --recursive 2>/dev/null | grep -v '^$')

# Two states `git submodule status` cannot report, both of which look like success.
#
# 1. An empty directory. A path can be "initialised" as far as git is concerned and still
#    hold nothing if something removed its contents without telling git. premake does not
#    check, so this is the state that produces a project which builds nothing - and the
#    only symptom is a link error naming an archive with no rule to build it.
#
# 2. A path .gitmodules declares that the index tracks as ORDINARY FILES rather than as a
#    gitlink (mode 160000). `git submodule status` omits such a path entirely, so nothing
#    above would mention it. That is what the old version of this script produced when it
#    cloned a dependency and deleted the clone's `.git`, and `GanymedEngine/extern/entt`
#    is in that state today: 307 tracked files under a path .gitmodules still calls a
#    submodule. It builds, which is why it went unnoticed.
for p in "${PATHS[@]}"; do
    if [ ! -d "$p" ]; then
        echo "  ❌ missing directory $p"
        EMPTY=$((EMPTY + 1))
        continue
    fi

    if [ -z "$(ls -A "$p" 2>/dev/null)" ]; then
        echo "  ❌ empty directory   $p"
        EMPTY=$((EMPTY + 1))
        continue
    fi

    if ! git ls-files -s -- "$p" | awk -v path="$p" '$4 == path && $1 == "160000" { found = 1 } END { exit !found }'; then
        echo "  ⚠️  not a submodule   $p (declared in .gitmodules, tracked as ordinary files)"
        MISFILED=$((MISFILED + 1))
    fi
done

echo ""

# Reported apart from the count below, and deliberately not fatal: a path tracked as
# ordinary files still builds. It is a repository-structure decision - re-add it as a
# submodule, or drop it from .gitmodules and call it vendored - not something a repair
# script should make on someone's behalf.
if [ "$MISFILED" -gt 0 ]; then
    echo "ℹ️  $MISFILED path(s) above are declared in .gitmodules but tracked as ordinary files."
    echo "   Nothing here can repair that, and it does not break the build. Either re-add"
    echo "   them as submodules or remove them from .gitmodules so the two agree."
    echo ""
fi

if [ "$((BROKEN + EMPTY))" -eq 0 ]; then
    # CHECKED, not ${#PATHS[@]}: --recursive also walks nested submodules (freetype
    # carries one), so the number checked is larger than the number .gitmodules declares.
    echo "🎉 All $CHECKED submodules are present and at their pinned commits."
    if [ "$MODE" != "check" ]; then
        echo ""
        echo "Next steps:"
        echo "  1. Premake:  ./scripts/setup_premake.sh"
        echo "  2. Projects: ./scripts/Linux_GenerateProjects.sh   (or the Win_/macOS_ variant)"
        echo "  3. Build:    make -j\$(nproc) config=debug"
        echo ""
        echo "Generated makefiles are gitignored, so regenerate after any pull that touches"
        echo "premake5.lua or GanymedEngine/extern/*.lua - a stale one is missing whole projects."
    fi
    exit 0
fi

echo "⚠️  $((BROKEN + EMPTY)) submodule problem(s) remain."
if [ "$MODE" = "check" ]; then
    echo "   Run without --check to repair."
else
    echo "   Try ./scripts/fix_submodules.sh --force, which deinitialises first."
    echo "   If that fails too, the usual causes are no network, an SSH key the submodule"
    echo "   URLs need, or a partial clone (git clone --depth) that has no submodule data."
fi
exit 1
