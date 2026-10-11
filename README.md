# Kazarov

Kazarov is the shared build system for TheMoonPeople's PS2 decompilation projects, Chronicle and ChronicleTwo. It holds the container image, the driver script, the CMake rules, the compiler helpers and the `port.h` header that the projects compile against. Each project consumes it as a git submodule at `tools/kazarov`.

It builds PS2 executables from the original game disc with the Metrowerks MWCC compiler, which runs under wibo inside the image, and builds ports for the host OS with ordinary toolchains.

## Layout

```
bin/            kazarov (driver), kz-* helper tools
lib/            config.sh and host.sh, sourced by bin/kazarov
host/           kazarov.cmd and kazarov.ps1, the Windows entry point; kazarov-quote.ps1 is its argument quoting
cmake/          Kazarov.cmake and the modules it includes
image/          Dockerfile and the MWCC debugging tools
include/kazarov/port.h
python/kazarov_elf/   ELF reader and writer for post-processing scripts
scripts/        m2ctx.py (declaration context), port_override.py
tests/          pytest suites and CMake graph checks
```

A project provides `kazarov.conf` at its root, and its own CMake lists, game sources and disc.

## kazarov.conf

`kazarov.conf` is a bash file that the driver sources from the project root.

| Variable | Meaning |
|---|---|
| `KZ_PROJECT` | Project name, lower case. Mounted at `/work/$KZ_PROJECT` in the container. |
| `KZ_CONFIGS` | Optional array of configuration names, printed by `kazarov configs`. |
| `KZ_DEFAULT_BUILD`, `KZ_DEFAULT_RUN` | Default configuration for `build` and `run`. |
| `KZ_DEFAULT_BUILD_{LINUX,DARWIN,WINDOWS}`, `KZ_DEFAULT_RUN_{LINUX,DARWIN,WINDOWS}` | Per-OS defaults; these win on that OS. |
| `KZ_WINDOWS_HOST_SCRIPT` | Optional PowerShell script for `KZ_WHERE=windows` configurations. |
| `KZ_CONTAINER_CONFIGS` | Optional single-line, space-separated list of container configuration names. `host/kazarov.cmd` takes the first argument as a config only when it is listed here or in `KZ_WINDOWS_CONFIGS`; without this variable, any first argument not starting with `-` is taken as the config. |
| `KZ_ENV_PASS` | Optional array of variable names to pass into the container. |

It also defines `kz_config NAME`, which returns 1 for an unknown name and otherwise sets:

| Variable | Meaning |
|---|---|
| `KZ_CONFIG` | Canonical name. Defaults to `NAME`. |
| `KZ_WHERE` | `container`, `host` or `windows`. |
| `KZ_HOST_OS` | For `host` configurations: `linux`, `darwin` or `windows`. The driver refuses to build it elsewhere. |
| `KZ_BUILD_DIR` | Build directory, relative to the project root. Required for container and host configurations. |
| `KZ_CMAKE_ARGS` (array) or `KZ_PRESET` | How to configure. |
| `KZ_SETUP_TARGETS`, `KZ_SETUP_STAMP` | Targets built before the main build. If the stamp did not exist before setup, CMake reconfigures once after it appears. |
| `KZ_BUILD_TARGETS`, `KZ_RUN_TARGETS` | Default targets for `build` and for `run`. |
| `KZ_LOCKS` (array) | Lock names to hold. Defaults to the configuration name. |
| `KZ_REQUIRE_ANY` (array), `KZ_REQUIRE_MESSAGE` | Files of which at least one must exist, and the error text when none does. |
| `KZ_RUN_ISO` | Disc image to boot in PCSX2 after a run. |
| `KZ_RUN_EXE`, `KZ_RUN_OS` | Program to run, and the OS it needs. |

Optional hooks, run with the project root as the working directory:

- `kz_after_build` runs inside the container, after the default targets only.
- `kz_after_run_build` runs inside, after the run targets. A failure here is reported and does not stop the run.
- `kz_before_launch` runs on the host, in the project root, before an executable starts. It may set `KZ_LAUNCH_PREFIX`, an array of command words (for example a lock wrapper) put in front of the executable: the launch is `exec "${KZ_LAUNCH_PREFIX[@]}" <exe> "$@"`. It is empty unless the hook sets it.

A minimal example:

```bash
KZ_PROJECT=example
KZ_CONFIGS=(ps2 linux)
KZ_DEFAULT_BUILD=ps2
KZ_DEFAULT_RUN=ps2
KZ_DEFAULT_BUILD_LINUX=linux
KZ_DEFAULT_RUN_LINUX=linux

kz_config() {
    case $1 in
        ps2)
            KZ_CONFIG=ps2
            KZ_WHERE=container
            KZ_BUILD_DIR=build/ps2
            KZ_CMAKE_ARGS=(-DGAME_TARGET=ps2)
            KZ_BUILD_TARGETS=(game.iso)
            KZ_RUN_ISO=build/ps2/game.iso ;;
        linux)
            KZ_CONFIG=linux
            KZ_WHERE=host
            KZ_HOST_OS=linux
            KZ_BUILD_DIR=build/linux
            KZ_CMAKE_ARGS=(-DGAME_TARGET=linux)
            KZ_RUN_EXE=build/linux/game
            KZ_RUN_OS=linux ;;
        *) return 1 ;;
    esac
}
```

## Driver: bin/kazarov

```
kazarov [--inside|--locked] build [config] [targets...]
kazarov [--inside|--locked] run [config] [args...]
kazarov shell [cmd...]
kazarov image
kazarov configs
```

- The first argument of `build` or `run` is a configuration name if `kz_config` knows it. Otherwise the default configuration is used and the argument is a target (or a program argument for `run`).
- Run from any directory. The driver resolves its own location and the project root, and maps the caller's directory into the container.
- `build` configures when the cache is missing or the configure arguments or image tag changed. It deletes the cache and reconfigures with `--fresh` when `CMAKE_HOME_DIRECTORY` or `CMAKE_CACHEFILE_DIR` do not match the directories on disk, so a moved project works.
- `run` builds the run targets, then boots the disc with `bin/kz-pcsx2` or starts the executable on the host. A container run writes a descriptor (`iso:<path>` or `exe:<path>`) to `.kazarov/`, and the host reads it after the container exits.
- Locks are per configuration, in `<root>/.kazarov/locks/<name>.lock`. Two runs of one configuration queue; different configurations run together.

Environment variables:

| Variable | Effect |
|---|---|
| `CLEAN=1` | Deletes the build directory under the lock first. The lock file itself is kept. |
| `JOBS=N` | Parallel jobs. Defaults to the CPU count. |
| `BUILDER=docker\|podman` | Container runtime. Podman is preferred when both are present, except on Apple silicon. |
| `REBUILD_IMAGE=1` | Rebuilds the image even when its tag exists. |
| `PCSX2=...` | Emulator command for `kz-pcsx2`. |

Inside the container the driver also reads `KZ_PROJECT_ROOT`, `KZ_HOST_OS` and `KZ_RUN_OUT` (the descriptor path).

## Helper tools

All are Python 3 with the standard library, except `kazarov` and `kz-pcsx2`, which are bash.

- `kz-lock [--shared] [--lock FILE]... [--message TEXT] -- CMD...` takes the `flock` locks in sorted order, exclusive unless `--shared` is given (shared holders run together and wait only for an exclusive one), announces a wait once, runs CMD in its own process group, forwards SIGINT and SIGTERM to the group, and exits with CMD's status.
- `kz-extract --iso ISO --checksum FILE --out DIR --id-file FILE [--force]` extracts the disc's files into `DIR/iso`. It checks the image against the sha256 checksum file, and records a state file (the id file name without `.id`, plus `.json`) with the image size, mtime, sha256, extractor version and each file's size. When nothing changed, it touches nothing. The id file changes only when the content id (sha256 and version) changes. Writes are atomic.
- `kz-split-stamp snapshot DIR SNAP` and `kz-split-stamp restore DIR SNAP STAMP` keep a split's output mtimes steady when a re-split changes nothing. The stamp is rewritten only when the content hash changes.
- `kz-cached --stamp FILE --input PATH... -- CMD...` runs CMD only when a command line or an input's size or mtime has changed. The stamp is written after success and removed after failure.
- `kz-mwcc --obj OBJ --depfile DEP -- FLAGS... SOURCE` compiles one unit through `kz-mwcc-exec` and writes a Ninja depfile. The project root is the working directory. It reads `KZ_MWCC` (compiler), `KZ_MWCC_RUNNER` (default `wibo`) and `MWCIncludes`.
- `kz-mwcc-exec EXE ARGS...` is a drop-in for wibo. It runs the compiler in a temporary directory that mirrors the top level of the project root, then rewrites the compiler's dependency map into absolute paths for `KZ_DEPFILE`. The temporary directory is removed afterwards.
- `kz-pcsx2 ISO` boots an image in PCSX2, using `PCSX2`, `pcsx2-qt`, the PCSX2 install directory on Windows, `/Applications/PCSX2.app` on macOS, or a Flatpak or AppImage install.
- `scripts/m2ctx.py -o OUT.c [unit.cpp] [-I DIR] [-D NAME]` writes the declaration context that m2c decompiles against: `OUT.cpp` for the compiler and `OUT.c` for m2c. The default include directories are `ps2/include`, `ps2/include/std` and `ps2/include/sce`. It needs the libclang Python bindings, which the image provides.
- `scripts/port_override.py list|strip|check` finds the `PORT_OVERRIDE` definitions in a port's source and removes the matching definitions from the PS2 sources.
- `image/mwcc-debug/` captures and replays MWCC 2.3.3's internal state for one function (`capture.py`, `gdb_capture.py`, `replay.py`). It is installed at `/opt/kazarov/mwcc-debug` in the image.

## CMake API

Include the modules from the project's `CMakeLists.txt`:

```cmake
set(KZ_MWCC "tools/compilers/mwcc.exe")          # relative to CMAKE_SOURCE_DIR
set(KZ_MWLD "tools/compilers/mwld.exe")
set(KZ_LIB_INCLUDE_DIRS ps2/include ps2/include/std)
include(${CMAKE_SOURCE_DIR}/tools/kazarov/cmake/Kazarov.cmake)
```

Inputs, set before the include:

- `KZ_MWCC`, `KZ_MWLD`: compiler and linker.
- `KZ_LIB_INCLUDE_DIRS`: directories passed to MWCC as `MWCIncludes`. Angle includes need these. Quoted includes search the `-i` directories only.
- `KZ_MWCC_RUNNER` (default `wibo`), `KZ_AS_FLAGS`.
- Optional: `KZ_AS` (a command list, such as `python3 tools/fake_as.py`; the script words that name files are rebuild inputs), `KZ_OBJCOPY`, `KZ_OBJDUMP`, `KZ_READELF`, `KZ_WIBO`, `MIPS_TOOL_PREFIX`, and the overlay variables below.

Variables set by the include:

- `KZ_ROOT` (the Kazarov checkout), `KZ_ROOT_REL`, `KZ_BUILD_REL`, `KZ_BIN`, `KZ_INCLUDE_REL`, `KZ_PYTHONPATH`.
- `KZ_PYTHON`: the Python interpreter. `KZ_PYTHONPATH` is set in every POST command.
- `KZ_AS`, `KZ_OBJCOPY`, `KZ_OBJDUMP`, `KZ_READELF`: binutils, from `MIPS_TOOL_PREFIX` (default `mips-ps2-decompals-`).
- `KZ_WIBO`, `KZ_LD` (`wibo` plus `KZ_MWLD`).
- `KZ_TOOL_DEPENDS`: `kz-mwcc`, `kz-mwcc-exec` and `/opt/kazarov/image-id`, if present. Every object depends on them, so a changed toolchain rebuilds the objects.

Functions:

- `kz_object_dirs(<objs>...)` creates the directories of the objects. Neither the assembler nor `kz-mwcc` creates them.
- `kz_asm_object(<obj> <src> [FLAGS ...] [POST COMMAND ... [COMMAND ...]] [DEPENDS ...] [COMMENT text])` assembles with `KZ_AS`. Each `COMMAND` group in `POST` runs after it, with `PYTHONPATH` set.
- `kz_mwcc_object(<obj> <src> FLAGS ... [ENV N=V ...] [POST ...] [DEPENDS ...] [COMMENT text] [RAW <raw obj>])` compiles through `kz-mwcc`. It appends `-MD` unless the flags already have it, and writes `<obj>.d` as the depfile. With `RAW`, MWCC writes `<raw obj>` and only the POST commands produce `<obj>`, so two outputs can come from one compile. The depfile is retargeted to `<obj>`.
- `kz_disc_setup(ISO <p> ISO_CHECKSUM <f> EXTRACT_DIR <d> INPUTS <f>... INPUTS_CHECKSUM <f> INPUTS_CHECKSUM_DIR <d> SPLIT_DIR <d> SPLIT_STAMP <f> SPLIT_COMMAND <cmd>... SPLIT_DEPENDS <f>... [SPLIT_BYPRODUCTS <f>...])` creates the targets `extract` (only when the disc exists at configure time), `setup` (the split, through a stamp) and `disassemble` (the split, forced). The split runs `sha256sum -c`, a snapshot, `SPLIT_COMMAND`, then a restore. It sets `KZ_EXTRACT_ID` and `KZ_SPLIT_STAMP` in the caller's scope.
- `kz_ctx(OUTPUT ctx.c ctx.cpp HEADERS <f>... [ARGS ...])` creates the target `ctx`, which runs `scripts/m2ctx.py`. The second output must have the first one's name with a `.cpp` extension.
- Overlays, for links with overlay sections: set `KZ_OVERLAYS`, `KZ_OVERLAY_ORIGIN`, and optionally `KZ_OVERLAY_HEADER_SIZE` (default `0x40`) and `KZ_OVERLAY_FILE_ALIGN` (default 128). The functions are `kz_expand_overlay_markers(template out)`, which replaces `// @OVERLAY <name>` markers with the overlay header, and `kz_overlay_link_args(out_var)`, which builds the `-og` and `-ol` arguments for `mwldps2`. Others are `kz_overlay_rename_flags`, `kz_overlay_for_object`, `kz_check_overlay_prefixes`, `kz_overlay_section_prefix`, `kz_overlay_header` and `kz_overlay_name_bytes`. Overlay objects go in `<build>/<name>/`.

Other included modules: `KazarovToolchain.cmake` (paths, tools and `kz_absolute_path`), `KazarovRetargetDepfile.cmake` (used by `RAW`).

## Image

Built locally from `image/` only, with `docker build --platform linux/amd64 --build-arg KZ_IMAGE_ID=<tag> -t kazarov-dev:<tag> image`. There is no registry. The driver builds the image when it is missing or when its tag starts with `dirty-`, and runs containers by the image ID.

- Tag: the first 12 hex digits of `git rev-parse HEAD:image`, or `dirty-<hash>` when `image/` has uncommitted changes or there is no commit, where `<hash>` covers the path and content of each file under `image/` (`kazarov.cmd` uses plain `dirty`, which is always rebuilt). The image carries the label `org.themoonpeople.kazarov.image=<tag>`.
- `/opt/kazarov/image-id` holds the tag. The last layer writes it, so a changed image id rebuilds the objects and an unchanged one rebuilds nothing.
- Base: `debian:trixie-slim`, `linux/amd64` only, because wibo needs an x86_64 kernel.
- Contents:
  - wibo, pinned by digest. Runs the Metrowerks compiler and linker.
  - SatansFiddle, built from source at a pinned revision with four patches from `image/patches/`. Its tests are in `/usr/local/libexec/satansfiddle-tests/`.
  - binutils-mips-ps2-decompals v0.10 in `/usr/local/binutils-mips-ps2-decompals`.
  - clang, lld and llvm 23 from apt.llvm.org; clangd 22.1.6; objdiff-cli v3.7.3.
  - `/opt/venv` with pycdlib, splat64[mips], libclang, graphviz, toml and levenshtein.
  - m2c and mwccps2-debugger at pinned commits in `/opt/kazarov/`, and `mwcc-debug` tools.
  - gdb, lldb-19, cmake, ninja, and the libraries the PC port links against.
- Environment: `PATH` includes `/opt/venv/bin`, LLVM 23 and binutils. `M2C`, `MWCCPS2_DEBUGGER_DIR`, `KZ_MWCC_DEBUG_DIR` and `LLDB_DEBUGSERVER_PATH` are set.


## port.h

`include/kazarov/port.h` lets one source tree serve the PS2 build and the port. The port defines `PORT`.

```c
#define PORT_INIT(...)    // PORT: = __VA_ARGS__      otherwise: nothing
#define PORT_RETURN(...)  // PORT: return __VA_ARGS__ otherwise: nothing
#define PORT_FIX(...)     // PORT: __VA_ARGS__        otherwise: nothing
#define PORT_OVERRIDE     // empty marker, read by scripts/port_override.py
```

Usage:

```c
#include "kazarov/port.h"

int count PORT_INIT(0);                       /* initialiser only in the port */
Vec origin PORT_INIT({0, 0, 0});              /* commas are fine inside the macro */
int Lookup(int i) { PORT_RETURN(table[i]); }  /* final return the PS2 code does not have */
PORT_FIX(if (index >= count) return 0;)       /* port-only code */
PORT_OVERRIDE void MGClearScreen(u_char r) { /* replaces the ps2/src definition */ }
```

Do not wrap `PORT_RETURN` in a PS2-visible `if`. The PS2 build then compiles that code. Under MWCC the expansions are empty apart from the stray `;` that `PORT_RETURN(...);` and `PORT_FIX(...);` leave. `tests/port_h/run.sh MWCC_EXE` checks that the macro uses compile to the same code as their hand-written PS2 text, and that a PORT build takes the values the macros give.

`#include "kazarov/port.h"` resolves through an `-i` directory: `-i <root>/tools/kazarov/include`. Angle-bracket includes do not search `-i` directories, so they need `MWCIncludes`.

## Windows

- `host/kazarov.cmd ROOT build|run [config] [args...]` is the Windows entry point. It is a thin shim: it runs `host/kazarov.ps1` with `pwsh`, or `powershell` when `pwsh` is not on PATH, and forwards the arguments as typed, so cmd never expands them a second time. `ROOT` is the project directory, and the script reads `KZ_PROJECT`, the default configurations and `KZ_WINDOWS_CONFIGS` from its `kazarov.conf`. A configuration listed in `KZ_WINDOWS_CONFIGS` runs through `KZ_WINDOWS_HOST_SCRIPT` with PowerShell. A configuration listed in `KZ_CONTAINER_CONFIGS` runs in the container. A first argument in neither list is a program argument, and the default configuration is used. Any other configuration runs in the container, through `docker` or `podman` (`BUILDER` picks one). `run` starts PCSX2 or the executable after the container exits.
- `.gitattributes` keeps scripts LF and `.cmd`/`.bat` files CRLF. `host/kazarov.cmd` must stay CRLF.
- Git Bash (MSYS) runs `bin/kazarov` directly. The driver sets `MSYS_NO_PATHCONV=1` and converts mounts with `cygpath -m`.
- Clone the project inside the WSL filesystem (`~/`), not under `/mnt/c`. Bind mounts of Windows drives are much slower for the many small reads of a build.

## Testing

```bash
python -m pytest -q tests/test_kz_mwcc.py tests/test_kz_lock.py tests/test_kz_extract.py \
    tests/test_kz_split_stamp.py tests/test_kz_cached.py tests/test_kazarov_elf.py tests/cmake \
    tests/test_host_ps1.py
shellcheck bin/kazarov bin/kz-pcsx2 lib/*.sh tests/port_h/run.sh
```

`tests/test_kazarov_elf.py` compares `kazarov_elf` with the `elf.py` it was ported from and with baseline objects. It reads them from the paths in `KZ_ELF_FIXTURES` (colon-separated) and `KZ_ELF_REFERENCE`, and skips when they are absent. `tests/test_kz_extract.py` needs `xorriso`, `genisoimage` or `mkisofs` and skips without one. `tests/cmake` needs `cmake`, `ninja` and the helper tools in `bin/`. `tests/test_host_ps1.py` runs `host/kazarov.ps1` under `pwsh` (from `KZ_PWSH`, or PATH) and skips without one.

## License

Kazarov is released under the MIT License (see `LICENSE`), copyright TheMoonPeople.

`python/kazarov_elf/` is derived from mwccgap's `elf.py`, which is MIT licensed, copyright (c) 2024 Mark Street. Its license text is in `python/kazarov_elf/LICENSE.mwccgap`.
