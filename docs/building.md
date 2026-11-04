# Building

You must have a 64-bit machine for building and running the project. Always
run your system updater before building and make sure you have the latest
drivers.

## Setup

### Windows

* Windows 10 or later
* [Visual Studio 2022 or 2026](https://www.visualstudio.com/downloads/)
* Windows 11 SDK version 10.0.22000.0 (for Visual Studio 2022, this or any newer version)
* [Python 3.6+ 64-bit](https://www.python.org/downloads/)
  * Ensure Python is in PATH.
* CMake 3.10+ (or C++ CMake tools for Windows)
  * Can install using python:
    ```
    python -m pip install cmake
    ```
* wxWidgets is built from a vendored submodule (`third_party/wxWidgets`).
  No system install needed.
* The Slang shader compiler (`slangc`) is a build-time dependency, used to
  compile the emulator's built-in (system) shaders into backend bytecode.
  `xb slang` downloads the pinned release into `.slang/`. Run it before
  `xb setup`, as the cmake configure step fails if `slangc` is missing. To
  point at an existing install instead, set the `SLANGC_PATH` environment
  variable. See `.github/workflows/build.yml` for the version CI pins and how
  it caches the download.
* The D3D12 backend builds Mesa's `spirv_to_dxil` (the SPIR-V to DXIL guest
  shader compiler) from the `third_party/mesa` submodule using Meson. You need
  Meson and the Python `mako` and `pyyaml` modules on PATH, Ninja is already
  provided by the "C++ CMake tools for Windows" component. Install with:
    ```
    python -m pip install meson mako pyyaml
    ```
  Initialize the submodule along with the others (`git submodule update --init
  third_party/mesa`). This is Windows/macOS only.

```
git clone https://github.com/has207/xenia-edge.git
cd xenia-edge

# Download the Slang shader compiler (see the note above):
xb slang

xb setup

# Build on command line (add --config=release for release):
xb build

# Run premake and open Visual Studio (add --config=release for release):
xb devenv

# Format code to the style guide:
xb format

# Build and run the unit tests:
xb test
```

> **Note:** The shaders Xenia generates need a Direct3D 12 runtime with shader
> model 6.6, which comes from the runtime, not the GPU driver. The executable
> loads the [DirectX 12 Agility SDK](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12)
> runtime (`D3D12Core.dll`) from a `D3D12\` subfolder next to it to guarantee
> this. Release builds bundle that DLL automatically (see
> `.github/workflows/build.yml`), a local build does not, so it falls back to
> your system's in-box runtime. If that runtime predates shader model 6.6, copy
> `D3D12Core.dll` from the Agility SDK into a `D3D12\` subfolder next to the
> built `xenia_edge.exe` (its version must match the `D3D12SDKVersion` exported
> in `src/xenia/ui/windowed_app_main_win.cc`).

#### Cross-compiling (Windows ARM64 ↔ x64)

The build supports cross-compiling between Windows x64 and ARM64 on the same
machine, `xb` configures into `build-<target>/` so it never clobbers the
native-build tree.

* Install Visual Studio components for the target architecture:
  * x64 host targeting ARM64: **MSVC v143 - VS 2022 C++ ARM64/ARM64EC build tools**
  * ARM64 host targeting x64: **MSVC v143 - VS 2022 C++ x64/x86 build tools**
* From an **x64 host** targeting ARM64:
  ```
  xb build --target-arch arm64 --config=release
  ```
  Output lands in `build-arm64\bin\Windows\Release\`.
* From an **ARM64 host** targeting x64:
  ```
  xb build --target-arch=x64 --config=release
  ```
  Output lands in `build-x64\bin\Windows\Release\`.

#### Testing

```
# Generate tests:
xb gentests

# Run tests:
xb test
```

#### Debugging

VS behaves oddly with the debug paths. Open the 'xenia-app' project properties
and set the 'Command' to `$(SolutionDir)$(TargetPath)` and the
'Working Directory' to `$(SolutionDir)..\..`. You can specify flags and
the file to run in the 'Command Arguments' field (or use `--flagfile=flags.txt`).

By default logs are written to xenia.log. You can
override this with `--log_file=log.txt`.

If running under Visual Studio and you want to look at the JIT'ed code
(available around 0xA0000000) you should pass `--emit_source_annotations` to
get helpful spacers/movs in the disassembly.

### Linux

The build script uses Clang 21.

* Normal building via `xb build` uses CMake+Ninja.
* The Slang shader compiler (`slangc`) is a build-time dependency, used to
  compile the emulator's built-in (system) shaders into backend bytecode.
  Run `./xb slang` to download the pinned release into `.slang/` before
  building, as the cmake configure step fails if `slangc` is missing. To point
  at an existing install instead, set the `SLANGC_PATH` environment variable.
  See `.github/workflows/build.yml` for the version CI pins and how it caches
  the download.
* Environment variables:
  Name  | Default Value
  ----- | -------------
  `CC`  | `clang`
  `CXX` | `clang++`

You will also need some development libraries. To get them on an Ubuntu system:

```sh
sudo apt-get install build-essential mesa-vulkan-drivers libc++-dev libc++abi-dev liblz4-dev libvulkan-dev libx11-xcb-dev clang-21 llvm-21 ninja-build libfontconfig1-dev \
  libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libcurl4-openssl-dev \
  libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxss-dev libxkbcommon-dev libxfixes-dev
```

In addition, you will need up to date Vulkan libraries and drivers for your hardware, which most distributions have in their standard repositories nowadays.

### macOS

* macOS 15 or later (the build sets `CMAKE_OSX_DEPLOYMENT_TARGET` to 15.0)
* Xcode Command Line Tools (provides clang, the macOS SDK, and `python3`)
  * Install with:
    ```sh
    xcode-select --install
    ```
* CMake, Ninja, clang-format
  * Can install using python:
    ```sh
    python3 -m pip install cmake ninja clang-format
    ```
* wxWidgets is built from a vendored submodule (`third_party/wxWidgets`).
  No system install needed.
* The Slang shader compiler (`slangc`) is a build-time dependency, used to
  compile the emulator's built-in (system) shaders into backend bytecode.
  `./xb slang` downloads the pinned release into `.slang/`. Run it before
  `./xb setup`, as the cmake configure step fails if `slangc` is missing. To
  point at an existing install instead, set the `SLANGC_PATH` environment
  variable. See `.github/workflows/build.yml` for the version CI pins and how
  it caches the download.
* Mesa's `spirv_to_dxil` is built from the `third_party/mesa` submodule using
  Meson. Its codegen requires Python 3.10+ and Apple ships 3.9, so install a
  newer one and put Meson and the modules Mesa imports in it:
    ```sh
    curl -O https://www.python.org/ftp/python/3.14.6/python-3.14.6-macos11.pkg
    sudo installer -pkg python-3.14.6-macos11.pkg -target /
    "/Applications/Python 3.14/Install Certificates.command"
    python3.14 -m pip install meson mako pyyaml packaging
    ```
  The installer puts `python3.14` in `/usr/local/bin`, where Meson finds it. It
  also becomes the default `python3`, and it ships no CA bundle until that
  certificates step runs, which `xb`'s downloads need.
  Initialize the submodule along with the others (`git submodule update --init
  third_party/mesa`).
* Apple's Metal Shader Converter turns that DXIL into the AIR the Metal backend
  runs. Its headers are vendored in `third_party/metal-shader-converter`, the
  dylib is downloaded, as the cmake configure step fails without it:
    ```sh
    ./xenia-build.py msc
    ```
  Apple's own download needs a developer login, so the dylib is mirrored as a
  release asset and verified against Apple's code signature after download. To
  move to a newer release, install the [Metal Developer
  Tools](https://developer.apple.com/metal/), compare its version against
  `MSC_VERSION` in `xenia-build.py`, then re-mirror
  `/usr/local/lib/libmetalirconverter.dylib` and refresh the headers with
  `rsync -a /usr/local/include/metal_irconverter* third_party/metal-shader-converter/include/`.

```sh
git clone https://github.com/has207/xenia-edge.git
cd xenia-edge

# Download the Slang shader compiler (see the note above):
./xb slang

./xb setup

# Build on command line (add --config=release for release):
./xb build
```

#### Cross-compiling (macOS arm64 ↔ x86_64)

Pass `--target-arch` for the non-host architecture:

```sh
./xb build --target-arch=arm64 --config=release      # arm64
./xb build --target-arch=x64 --config=release      # x86_64
```

Output lands in `build-arm64/` or `build-x64/` respectively while host native ends up in `build/`.

## Running

To make life easier you can set the program startup arguments in your IDE to something like `--log_file=stdout /path/to/Default.xex` to log to console rather than a file and start up the emulator right away.
