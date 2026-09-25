#!/usr/bin/env python3
"""GanymedEngine setup: everything a checkout needs before its projects can be built.

    python scripts/setup.py                  interactive menu (non-interactive shells: `auto`)
    python scripts/setup.py auto             run each step that is out of date, then generate
    python scripts/setup.py status           report only; exit 1 if any step is out of date
    python scripts/setup.py shaders generate force the named steps, in pipeline order

Steps, in the order they depend on each other:

    premake      download the pinned premake5 into vendor/premake/bin
    submodules   `git submodule update --init --recursive` to the pinned commits
    shadertools  build bgfx's shaderc with bgfx's own GENie build -> scripts/tools/<os>/
    shaders      compile assets/shaders/src/*.sc for this OS's backend profiles
    generate     run premake for this OS's generator (vs2022 / gmake / xcode4)

Every step can tell whether it is already done, which is what makes `auto` cheap enough to run
after every pull: only the steps whose inputs changed do any work. Forcing a step by name runs it
regardless -- `shaders` then recompiles everything rather than only what is stale.

Two environment variables point at tools built from source, for hosts the prebuilt binaries do not
run on (older glibc, Intel Macs): PREMAKE=<path to premake5> and GENIE=<path to genie>.

Standard library only, so a fresh machine needs nothing but Python 3.8+ and git.
"""

import argparse
import os
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor

if sys.version_info < (3, 8):
    sys.exit("setup.py needs Python 3.8 or newer (found %d.%d)." % sys.version_info[:2])

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTERN = os.path.join(ROOT, "GanymedEngine", "extern")
BGFX = os.path.join(EXTERN, "bgfx")

# ---- Configuration ----------------------------------------------------------------------------

# One version for every OS. The binary is gitignored and downloaded per machine, so the pin is the
# only thing keeping a Windows and a Linux checkout on the same generator behaviour. beta8 is where
# `gmake2` became `gmake` (the old name still works, as an alias).
PREMAKE_VERSION = "5.0.0-beta8"
PREMAKE_URL = "https://github.com/premake/premake-core/releases/download/v{0}/premake-{0}-{1}"

# Where compiled shaders go, relative to the repository root. Every app runs with the root as its
# working directory and loads "assets/shaders/compiled/...", so one copy serves all of them.
SHADER_TARGETS = ["."]
SHADER_SRC = os.path.join(ROOT, "assets", "shaders", "src")

# (folder, shaderc -p profile) per OS. The folder names must match ProfileDirectory() in
# GanymedEngine/source/GanymedE/Renderer/Shader.cpp: the runtime picks the folder from the live
# bgfx backend, so a profile missing here is not a build error, it is a backend that draws nothing.
# dx11 is Windows-only; "metal" is shaderc's alias for Metal 1.2; macOS caps GL at 4.1.
SHADER_PROFILES = {
    "windows": [("dx11", "s_5_0"), ("spirv", "spirv"), ("glsl", "410")],
    "linux":   [("spirv", "spirv"), ("glsl", "410")],
    "darwin":  [("metal", "metal"), ("glsl", "410")],
}

# The system headers bgfx (xcb, via VK_USE_PLATFORM_XCB_KHR) and GLFW (Xlib) need on Linux.
# Checked, never installed: a setup script that runs sudo is a worse failure than a clear message.
LINUX_HEADERS = ["/usr/include/X11/Xlib.h", "/usr/include/xcb/xcb.h"]
LINUX_PACKAGES = "sudo apt install libglfw3-dev libwayland-dev libxkbcommon-dev xorg-dev"


class StepError(Exception):
    pass


# ---- Host --------------------------------------------------------------------------------------

def host_os():
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform == "darwin":
        return "darwin"
    if sys.platform.startswith("linux"):
        return "linux"
    sys.exit("Unsupported platform: " + sys.platform)


OS = host_os()
EXE = ".exe" if OS == "windows" else ""
TOOLS_DIR = os.path.join(ROOT, "scripts", "tools", OS)
SHADERC = os.path.join(TOOLS_DIR, "shaderc" + EXE)
# Records which bgfx commit shaderc was built from; a submodule bump makes it stale.
SHADERC_STAMP = os.path.join(TOOLS_DIR, "shaderc.bgfx-commit")
# PREMAKE=<path> uses a premake built elsewhere (see premake_run) and is never downloaded over.
PREMAKE_OVERRIDE = os.environ.get("PREMAKE")
PREMAKE = os.path.abspath(PREMAKE_OVERRIDE) if PREMAKE_OVERRIDE else \
    os.path.join(ROOT, "vendor", "premake", "bin", "premake5" + EXE)
DEFAULT_ACTION = {"windows": "vs2022", "linux": "gmake", "darwin": "xcode4"}[OS]


def run(cmd, cwd=ROOT, capture=False, check=True):
    """Runs a command, echoing it. With capture, returns stdout; otherwise output streams live."""
    if not capture:
        print("  $ " + " ".join(cmd), flush=True)
    try:
        result = subprocess.run(cmd, cwd=cwd, universal_newlines=True,
                                stdout=subprocess.PIPE if capture else None,
                                stderr=subprocess.PIPE if capture else None)
    except OSError as e:
        raise StepError("could not run %s: %s" % (cmd[0], e))
    if check and result.returncode != 0:
        raise StepError("%s exited with code %d" % (os.path.basename(cmd[0]), result.returncode))
    return result.stdout if capture else result.returncode


def git(*args):
    return run(["git"] + list(args), capture=True, check=False) or ""


def rel(path):
    try:
        return os.path.relpath(path, ROOT).replace("\\", "/")
    except ValueError:  # another drive on Windows
        return path


# ---- Step: premake -----------------------------------------------------------------------------

def premake_probe():
    """Returns (version, None), or (None, why) when the binary is missing or will not run."""
    if not os.path.isfile(PREMAKE):
        return None, "missing: " + rel(PREMAKE)
    # Run from a scratch directory: in the repo root premake parses premake5.lua first and buries
    # the version line under the script's deprecation warnings.
    with tempfile.TemporaryDirectory() as scratch:
        try:
            result = subprocess.run([PREMAKE, "--version"], cwd=scratch, universal_newlines=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        except OSError as e:
            return None, "will not run on this machine: %s" % e
    words = result.stdout.split()
    if result.returncode != 0 or not words:
        first = (result.stderr or result.stdout).strip().splitlines()
        return None, "will not run on this machine: " + (first[0] if first else "no output")
    return words[-1], None


def premake_status():
    version, why = premake_probe()
    if version is None:
        return True, why
    if PREMAKE_OVERRIDE:
        return False, "premake %s (PREMAKE override)" % version
    if version != PREMAKE_VERSION:
        return True, "have %s, project pins %s" % (version, PREMAKE_VERSION)
    return False, "premake " + version


def premake_asset():
    if OS == "windows":
        return "windows.zip"
    if OS == "linux":
        return "linux.tar.gz"
    # beta8 publishes a separate x64 build next to the default macosx one.
    return "macosx-x64.tar.gz" if platform.machine() == "x86_64" else "macosx.tar.gz"


def download(url, dest):
    print("  downloading " + url, flush=True)
    try:
        with urllib.request.urlopen(url) as response, open(dest, "wb") as out:
            shutil.copyfileobj(response, out)
        return
    except Exception as e:  # noqa: BLE001 -- any failure falls through to curl
        first_error = e
    # python.org's macOS installer ships without a CA bundle until "Install Certificates.command"
    # is run, so urllib fails TLS there while the system curl works.
    if shutil.which("curl"):
        print("  urllib failed (%s); retrying with curl" % first_error, flush=True)
        run(["curl", "-fL", "-o", dest, url])
        return
    raise StepError("download failed: %s" % first_error)


def premake_run(force):
    if PREMAKE_OVERRIDE:
        raise StepError("PREMAKE is set, so nothing is downloaded over %s" % PREMAKE)
    name = os.path.basename(PREMAKE)
    with tempfile.TemporaryDirectory() as scratch:
        archive = os.path.join(scratch, "premake")
        download(PREMAKE_URL.format(PREMAKE_VERSION, premake_asset()), archive)
        if zipfile.is_zipfile(archive):
            with zipfile.ZipFile(archive) as z:
                members = [m for m in z.namelist() if os.path.basename(m) == name]
                if not members:
                    raise StepError("no %s in the downloaded archive" % name)
                z.extract(members[0], scratch)
                extracted = os.path.join(scratch, members[0])
        else:
            with tarfile.open(archive) as t:
                members = [m for m in t.getmembers() if os.path.basename(m.name) == name]
                if not members:
                    raise StepError("no %s in the downloaded archive" % name)
                t.extract(members[0], scratch)
                extracted = os.path.join(scratch, members[0].name)
        os.makedirs(os.path.dirname(PREMAKE), exist_ok=True)
        shutil.copy2(extracted, PREMAKE)
    os.chmod(PREMAKE, os.stat(PREMAKE).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    version, why = premake_probe()
    if version is None:
        # premake's Linux release binaries link against a recent glibc (beta8 wants 2.38, i.e.
        # Ubuntu 24.04+), so older distros get a loader error, not a premake one.
        raise StepError("the downloaded premake5 %s\n  Build it from source and point PREMAKE at it:\n"
                        "    git clone --depth 1 --branch v%s https://github.com/premake/premake-core\n"
                        "    make -C premake-core -f Bootstrap.mak linux\n"
                        "    PREMAKE=$PWD/premake-core/bin/release/premake5 python3 scripts/setup.py"
                        % (why, PREMAKE_VERSION))
    print("  installed %s (%s)" % (rel(PREMAKE), version))


# ---- Step: submodules --------------------------------------------------------------------------
#
# Nothing here ever re-adds or re-clones a submodule. `git submodule add` checks out the upstream
# tip, not the commit this repository pins; bgfx, bimg, bx, JoltPhysics and RmlUi are pinned to
# commits the build is known to work against, and moving them to master is how a build breaks
# while looking repaired. `update --init --recursive` restores exactly the pinned commits.

def declared_submodules():
    out = git("config", "-f", ".gitmodules", "--get-regexp", r"^submodule\..*\.path$")
    return sorted(line.split()[1] for line in out.splitlines() if line.strip())


def submodule_problems():
    """Returns (problems, notes). Problems break the build; notes are reported but not fatal."""
    if not os.path.isdir(os.path.join(ROOT, ".git")) and not git("rev-parse", "--git-dir"):
        raise StepError("not a git repository -- re-clone rather than downloading a zip")
    paths = declared_submodules()
    if not paths:
        raise StepError(".gitmodules lists no submodules -- it is probably malformed")

    problems, notes = [], []
    # `git submodule status` prefixes each line: '-' not initialised, '+' checked out at a commit
    # other than the pinned one, 'U' merge conflict, ' ' correct.
    for line in git("submodule", "status", "--recursive").splitlines():
        if not line:
            continue
        flag, fields = line[0], line[1:].split()
        path = fields[1] if len(fields) > 1 else "?"
        if flag == "-":
            problems.append("not initialised   " + path)
        elif flag == "+":
            problems.append("wrong commit      %s (at %s, not the pinned one)" % (path, fields[0][:10]))
        elif flag == "U":
            problems.append("merge conflict    " + path)

    # Two states `git submodule status` cannot report, both of which look like success:
    # an empty directory (premake still generates a project for it, and the first symptom is a
    # link error against an archive nothing builds), and a declared path the index tracks as
    # ordinary files instead of a gitlink (status omits it entirely; it builds, so it is a note).
    for path in paths:
        full = os.path.join(ROOT, path)
        if not os.path.isdir(full):
            problems.append("missing directory " + path)
        elif not os.listdir(full):
            problems.append("empty directory   " + path)
        else:
            entry = git("ls-files", "-s", "--", path).splitlines()
            if not any(e.startswith("160000 ") and e.endswith("\t" + path) for e in entry):
                notes.append("not a submodule   %s (declared in .gitmodules, tracked as files)" % path)
    return problems, notes


def submodules_status():
    try:
        problems, notes = submodule_problems()
    except StepError as e:
        return True, str(e)
    if problems:
        return True, "%d problem(s), first: %s" % (len(problems), " ".join(problems[0].split()))
    return False, "%d declared, all at their pinned commits" % len(declared_submodules())


def submodules_run(force, deinit=False):
    if deinit:
        # For a working tree that will not move. Deinit drops each checkout; the update below puts
        # back the pinned commit, so this loses local edits inside submodules and nothing else.
        run(["git", "submodule", "deinit", "-f", "--all"], check=False)
    # sync first: it rewrites each submodule's URL from .gitmodules, which is what repairs a clone
    # made before a URL changed. Without it, update fetches from the old remote and "succeeds".
    run(["git", "submodule", "sync", "--recursive"], check=False)
    run(["git", "submodule", "update", "--init", "--recursive"], check=False)

    problems, notes = submodule_problems()
    for note in notes:
        print("  note: " + note)
    if notes:
        print("  (a path tracked as files still builds; re-add it as a submodule or drop it from"
              " .gitmodules so the two agree)")
    if problems:
        for p in problems:
            print("  FAIL " + p)
        raise StepError("%d submodule problem(s) remain. Usual causes: no network, an SSH key the"
                        " URLs need, or a shallow clone. Try the deinit option." % len(problems))
    if OS == "linux":
        warn_missing_linux_headers()


def warn_missing_linux_headers():
    missing = [h for h in LINUX_HEADERS if not os.path.isfile(h)]
    if missing:
        print("  warning: missing system headers %s -- install them with:\n    %s"
              % (", ".join(missing), LINUX_PACKAGES))


# ---- Step: shadertools -------------------------------------------------------------------------
#
# shaderc is built with bgfx's own GENie project rather than premake: it pulls in glslang,
# spirv-tools, spirv-cross, glsl-optimizer, fcpp and tint, and re-describing that graph in premake
# would be a large, fragile duplication for a tool built once per machine.

def bgfx_commit():
    return git("-C", BGFX, "rev-parse", "HEAD").strip()


def shadertools_status():
    if not os.path.isfile(SHADERC):
        return True, "missing: " + rel(SHADERC)
    if not os.path.isfile(SHADERC_STAMP):
        # Built before the stamp existed. Rebuilding costs minutes, so do not assume it is stale.
        return False, "present (bgfx commit unknown; force this step after a bgfx update)"
    with open(SHADERC_STAMP) as f:
        built_from = f.read().strip()
    current = bgfx_commit()
    if current and built_from != current:
        return True, "built from bgfx %s, submodule is at %s" % (built_from[:10], current[:10])
    return False, "built from bgfx " + built_from[:10]


def find_msbuild():
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.isfile(vswhere):
        raise StepError("vswhere.exe not found -- is Visual Studio installed?")
    out = run([vswhere, "-latest", "-requires", "Microsoft.Component.MSBuild",
               "-find", r"MSBuild\**\Bin\MSBuild.exe"], capture=True)
    lines = out.strip().splitlines()
    if not lines:
        raise StepError("MSBuild.exe not found")
    return lines[0]


def build_shaderc_windows():
    msbuild = find_msbuild()
    genie = os.path.join(EXTERN, "bx", "tools", "bin", "windows", "genie.exe")
    print("  [1/3] generating bgfx tool projects with GENie")
    run([genie, "--with-tools", "vs2022"], cwd=BGFX)
    print("  [2/3] building shaderc (Release x64) -- this takes a few minutes")
    run([msbuild, os.path.join(".build", "projects", "vs2022", "shaderc.vcxproj"),
         "/p:Configuration=Release", "/p:Platform=x64", "/m", "/v:minimal", "/nologo"], cwd=BGFX)
    print("  [3/3] staging into " + rel(TOOLS_DIR))
    os.makedirs(TOOLS_DIR, exist_ok=True)
    shutil.copy2(os.path.join(BGFX, ".build", "win64_vs2022", "bin", "shadercRelease.exe"), SHADERC)
    # shaderc loads these at runtime for the D3D/DXIL profiles.
    for dll in ("d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll"):
        shutil.copy2(os.path.join(BGFX, "tools", "bin", "windows", dll), TOOLS_DIR)


def build_shaderc_unix():
    extra = []
    if OS == "linux":
        toolchain = "linux-gcc"
    else:
        toolchain = "osx-arm64" if platform.machine() == "arm64" else "osx-x64"
        # bx defaults the macOS target to 10.13.6 for the gmake action (its newer defaults only
        # apply to xcode*), and glslang uses std::filesystem, which libc++ marks unavailable
        # before 10.15 -- the tool build fails on "'absolute' is unavailable" without this.
        extra = ["--with-macos=13.0"]

    # bx bundles prebuilt GENie binaries that do not run everywhere: the darwin one is arm64-only
    # ("Bad CPU type" on Intel), the linux one needs glibc 2.38 (older than Ubuntu 24.04 fails in
    # the loader). GENie builds from source in seconds, so GENIE=<path> overrides the bundled one:
    #   git clone https://github.com/bkaradzic/GENie && make -C GENie
    bundled = os.path.join(EXTERN, "bx", "tools", "bin", OS, "genie")
    genie = os.environ.get("GENIE", bundled)
    try:
        os.chmod(genie, os.stat(genie).st_mode | stat.S_IXUSR)
    except OSError:
        pass
    # `genie --version` exits 1 even on success, so check it printed its banner instead: that
    # separates "ran fine" from "could not exec" and "loader rejected it" alike.
    try:
        banner = subprocess.run([genie, "--version"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                universal_newlines=True).stdout
    except OSError:
        banner = ""
    if "GENie" not in banner:
        raise StepError("%s will not run on this machine. Build GENie from source and re-run with"
                        " GENIE=<path> (git clone https://github.com/bkaradzic/GENie && make -C GENie)"
                        % genie)

    print("  [1/3] generating bgfx tool projects with GENie")
    run([genie, "--with-tools", "--gcc=" + toolchain] + extra + ["gmake"], cwd=BGFX)
    print("  [2/3] building shaderc (release64) -- this takes a few minutes")
    # bx writes the makefiles to .build/projects/<action>-<--gcc value>; derive it, do not spell it.
    run(["make", "-C", os.path.join(".build", "projects", "gmake-" + toolchain), "config=release64",
         "-j%d" % (os.cpu_count() or 4), "shaderc"], cwd=BGFX)
    print("  [3/3] staging into " + rel(TOOLS_DIR))
    built = None
    for dirpath, _, files in os.walk(os.path.join(BGFX, ".build")):
        if "shadercRelease" in files:
            built = os.path.join(dirpath, "shadercRelease")
            break
    if built is None:
        raise StepError("could not locate the built shaderc binary under bgfx/.build")
    os.makedirs(TOOLS_DIR, exist_ok=True)
    shutil.copy2(built, SHADERC)
    os.chmod(SHADERC, os.stat(SHADERC).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def shadertools_run(force):
    if not os.path.isfile(os.path.join(BGFX, "src", "amalgamated.cpp")):
        raise StepError("bgfx submodule is not checked out -- run the submodules step first")
    if OS == "windows":
        build_shaderc_windows()
    else:
        build_shaderc_unix()
    with open(SHADERC_STAMP, "w") as f:
        f.write(bgfx_commit() + "\n")
    print("  " + run([SHADERC, "--version"], capture=True, check=False).strip())


# ---- Step: shaders -----------------------------------------------------------------------------

def varying_def(stem):
    # Shaders whose vertex layout is fixed by a third party (ImGui, RmlUi) ship
    # varying.<Name>.def.sc, which is preferred over the shared one. "vs_ImGui" -> "ImGui".
    specific = os.path.join(SHADER_SRC, "varying.%s.def.sc" % stem[3:])
    return specific if os.path.isfile(specific) else os.path.join(SHADER_SRC, "varying.def.sc")


def shader_jobs():
    """Every (source, output, type, profile, varying) this OS compiles."""
    sources = sorted(f for f in os.listdir(SHADER_SRC)
                     if f.endswith(".sc") and (f.startswith("vs_") or f.startswith("fs_")))
    jobs = []
    for target in SHADER_TARGETS:
        compiled = os.path.normpath(os.path.join(ROOT, target, "assets", "shaders", "compiled"))
        for folder, profile in SHADER_PROFILES[OS]:
            for source in sources:
                stem = source[:-3]
                jobs.append((os.path.join(SHADER_SRC, source),
                             os.path.join(compiled, folder, stem + ".bin"),
                             "vertex" if stem.startswith("vs_") else "fragment",
                             profile, varying_def(stem)))
    return jobs


def mtime(path):
    try:
        return os.path.getmtime(path)
    except OSError:
        return 0.0


def stale_jobs(jobs):
    # An output is stale when anything that shapes it is newer: its source, its varying file, the
    # bgfx shader header every .sc includes, or shaderc itself (a rebuilt compiler recompiles all).
    shared = max(mtime(os.path.join(BGFX, "src", "bgfx_shader.sh")), mtime(SHADERC))
    return [j for j in jobs
            if not os.path.isfile(j[1]) or mtime(j[1]) < max(mtime(j[0]), mtime(j[4]), shared)]


def shaders_status():
    if not os.path.isfile(SHADERC):
        return True, "needs shaderc first"
    jobs = shader_jobs()
    stale = stale_jobs(jobs)
    if stale:
        return True, "%d of %d outputs missing or stale" % (len(stale), len(jobs))
    return False, "%d outputs up to date (%s)" % (len(jobs), ", ".join(f for f, _ in SHADER_PROFILES[OS]))


def shaders_run(force):
    if not os.path.isfile(SHADERC):
        raise StepError("shaderc not found at %s -- run the shadertools step first" % rel(SHADERC))
    jobs = shader_jobs()
    todo = jobs if force else stale_jobs(jobs)
    if not todo:
        print("  all %d outputs up to date" % len(jobs))
        return
    include = os.path.join(BGFX, "src")
    shader_platform = {"windows": "windows", "linux": "linux", "darwin": "osx"}[OS]

    def compile_one(job):
        source, output, shader_type, profile, varying = job
        os.makedirs(os.path.dirname(output), exist_ok=True)
        result = subprocess.run([SHADERC, "-f", source, "-o", output, "--type", shader_type,
                                 "--platform", shader_platform, "-p", profile, "-i", include,
                                 "--varyingdef", varying],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        return job, result.returncode, result.stdout

    # shaderc is single-threaded and each call is independent, so one process per core.
    failed = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for job, code, output in pool.map(compile_one, todo):
            if code != 0:
                failed += 1
                print("  FAILED %s\n%s" % (rel(job[1]), output.rstrip()))
    if failed:
        raise StepError("%d of %d shader(s) failed to compile" % (failed, len(todo)))
    print("  compiled %d of %d shader binaries" % (len(todo), len(jobs)))


# ---- Step: generate ----------------------------------------------------------------------------

def generate_status():
    # Always runs under `auto`: it takes seconds, and generated project files are gitignored, so a
    # pull that touches premake5.lua or extern/*.lua never updates them otherwise.
    return True, "premake5 " + ACTION


def generate_run(force):
    if not os.path.isfile(PREMAKE):
        raise StepError("premake5 not found -- run the premake step first")
    run([PREMAKE, ACTION])
    hints = {
        "vs2022": "open GanymedEngine.sln, or MSBuild it (x64, Debug/Release/Dist)",
        "gmake": "make -j%d config=debug   (at the repository root)" % (os.cpu_count() or 4),
        "xcode4": "open GanymedEngine.xcworkspace, or xcodebuild -workspace GanymedEngine.xcworkspace"
                  " -scheme GanymedEditor -configuration Debug build",
    }
    if ACTION in hints:
        print("  next: " + hints[ACTION])


# ---- Driver ------------------------------------------------------------------------------------

ACTION = DEFAULT_ACTION

STEPS = [
    ("premake",     "premake",      premake_status,     premake_run),
    ("submodules",  "submodules",   submodules_status,  submodules_run),
    ("shadertools", "shader tools", shadertools_status, shadertools_run),
    ("shaders",     "shaders",      shaders_status,     shaders_run),
    ("generate",    "generate",     generate_status,    generate_run),
]
STEP_NAMES = [s[0] for s in STEPS]

# Shader outputs are runtime assets, not build inputs, so `generate` does not wait on them.
STEP_DEPENDS = {
    "premake":     [],
    "submodules":  [],
    "shadertools": ["submodules"],
    "shaders":     ["submodules", "shadertools"],
    "generate":    ["premake", "submodules"],
}


def run_step(step, force, **kwargs):
    name, label, _, action = step
    print("\n== %s %s" % (label, "(forced)" if force else ""), flush=True)
    action(force, **kwargs)


def run_auto():
    """Runs each out-of-date step in order. Status is re-read per step, because an earlier step
    (submodules, shaderc) changes what the later ones see. A failure skips only the steps that
    depend on it: a machine that cannot build shaderc still gets its project files."""
    failed = {}
    for step in STEPS:
        blocked = [d for d in STEP_DEPENDS[step[0]] if d in failed]
        if blocked:
            failed[step[0]] = "skipped, needs " + ", ".join(blocked)
            print("\n== %s: %s" % (step[1], failed[step[0]]))
            continue
        needed, detail = step[2]()
        if not needed:
            print("\n== %s: up to date -- %s" % (step[1], detail))
            continue
        try:
            run_step(step, force=False)
        except StepError as e:
            failed[step[0]] = str(e)
            print("\n== %s FAILED: %s" % (step[1], e))
    if failed:
        raise StepError("%d step(s) did not complete: %s" % (len(failed), ", ".join(failed)))


def run_named(names, deinit=False):
    for step in STEPS:
        if step[0] in names:
            kwargs = {"deinit": deinit} if step[0] == "submodules" else {}
            run_step(step, force=True, **kwargs)


def print_status():
    print("\nGanymedEngine setup -- %s %s, Python %d.%d, root %s"
          % (OS, platform.machine(), sys.version_info[0], sys.version_info[1], ROOT))
    any_needed = False
    for i, (name, label, status, _) in enumerate(STEPS, 1):
        needed, detail = status()
        if name == "generate":
            state = "run"
        else:
            state = "NEEDED" if needed else "ok"
            any_needed = any_needed or needed
        print("  %d  %-13s %-7s %s" % (i, label, state, detail))
    return any_needed


def menu():
    while True:
        print_status()
        print("\n  a    run what is needed, then generate   [Enter]"
              "\n  1-5  force steps, e.g. \"4 5\" (forced shaders recompile everything)"
              "\n  d    deinit + re-checkout all submodules (discards edits inside them)"
              "\n  q    quit")
        try:
            choice = input("> ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        try:
            if choice in ("", "a"):
                run_auto()
            elif choice == "q":
                return 0
            elif choice == "d":
                if input("  discard local changes inside every submodule? [y/N] ").strip().lower() != "y":
                    continue
                run_named({"submodules"}, deinit=True)
            else:
                picks = choice.replace(",", " ").split()
                if not picks or not all(p.isdigit() and 1 <= int(p) <= len(STEPS) for p in picks):
                    print("  ? unknown choice: " + choice)
                    continue
                run_named({STEPS[int(p) - 1][0] for p in picks})
            print("\n== done")
        except StepError as e:
            print("\n== FAILED: %s" % e)
        except KeyboardInterrupt:
            print("\n== interrupted")


def main():
    global ACTION
    parser = argparse.ArgumentParser(
        description="Set up a GanymedEngine checkout and generate its project files.",
        epilog="steps: " + ", ".join(STEP_NAMES) + ". With no command: the interactive menu"
               " (or `auto` when stdin is not a terminal).")
    parser.add_argument("command", nargs="*", metavar="command",
                        help="auto | status | one or more step names to force")
    parser.add_argument("--action", default=DEFAULT_ACTION,
                        help="premake action for the generate step (default: %(default)s)")
    parser.add_argument("--deinit", action="store_true",
                        help="submodules step: deinit first, for a working tree that will not move")
    args = parser.parse_args()
    ACTION = args.action

    commands = args.command
    unknown = [c for c in commands if c not in STEP_NAMES + ["auto", "status"]]
    if unknown:
        parser.error("unknown command(s): %s (steps: %s)" % (", ".join(unknown), ", ".join(STEP_NAMES)))

    try:
        if not commands:
            if sys.stdin.isatty():
                return menu()
            commands = ["auto"]
        if commands == ["status"]:
            return 1 if print_status() else 0
        if commands == ["auto"]:
            run_auto()
        elif "auto" in commands or "status" in commands:
            parser.error("auto and status cannot be combined with other commands")
        else:
            run_named(set(commands), deinit=args.deinit)
    except StepError as e:
        print("\n== FAILED: %s" % e)
        return 1
    except KeyboardInterrupt:
        print("\n== interrupted")
        return 130
    print("\n== done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
