include("tools/build")
if _ACTION == "export-compile-commands" then
  require("third_party/premake-export-compile-commands/export-compile-commands")
end
if os.istarget("android") then
  require("third_party/premake-androidndk/androidndk")
end
if _ACTION == "cmake" then
  require("third_party/premake-cmake/cmake")
end

-- Helper function to extract Qt version from qconfig.pri
function get_qt_version(qt_dir)
  local qconfig_path = path.join(qt_dir, "mkspecs/qconfig.pri")
  local qconfig_file = io.open(qconfig_path, "r")
  if qconfig_file then
    for line in qconfig_file:lines() do
      local version = line:match("^QT_VERSION%s*=%s*(.+)$")
      if version then
        qconfig_file:close()
        return version:gsub("^%s*", ""):gsub("%s*$", "")  -- trim whitespace
      end
    end
    qconfig_file:close()
  end
  error("Could not read Qt version from " .. qconfig_path)
end

location(build_root)
targetdir(build_bin)
objdir(build_obj)

-- Define variables for enabling specific submodules
-- Todo: Add changing from xb command
enableTests = false
enableMiscSubprojects = false

-- Define an ARCH variable
-- Only use this to enable architecture-specific functionality.
if os.istarget("linux") then
  ARCH = os.outputof("uname -p")
else
  ARCH = "unknown"
end

includedirs({
  ".",
  "src",
  "third_party",
})

defines({
  "VULKAN_HPP_NO_TO_STRING",
  "IMGUI_DISABLE_OBSOLETE_FUNCTIONS",
  "IMGUI_DISABLE_DEFAULT_FONT",
  --"IMGUI_USE_WCHAR32",
  "IMGUI_USE_STB_SPRINTF",
  --"IMGUI_ENABLE_FREETYPE",
  "USE_CPP17", -- Tabulate
})

cdialect("C17")
cppdialect("C++20")
symbols("On")
fatalwarnings("All")

-- TODO(DrChat): Find a way to disable this on other architectures.
if ARCH ~= "ppc64" then
  filter({"architecture:x86_64"})
    -- AVX2 requires Haswell (2013+) or newer
    vectorextensions("AVX")
  filter({})
end

filter("kind:StaticLib")
  defines({
    "_LIB",
  })

filter("configurations:Checked")
  runtime("Debug")
  inlining("Auto")  -- /Ob2 for Checked builds
  flags("NoIncrementalLink")
  editandcontinue("Off")
  staticruntime("Off")
  optimize("Off")
  removedefines({
    "IMGUI_USE_STB_SPRINTF",
  })
  defines({
    "DEBUG",
    "NDEBUG",
  })

filter({"configurations:Checked", "platforms:Linux"})
  buildoptions({
    "-fsanitize=undefined",
  })
  linkoptions({
    "-fsanitize=undefined",
  })

filter({"configurations:Checked", "platforms:Windows"}) -- "toolset:msc"
  buildoptions({
    "/RTCsu",           -- Full Run-Time Checks.
  })
  -- AddressSanitizer on Windows (doesn't conflict with memory layout like on Linux)
  sanitize("Address")

filter({"configurations:Checked or Debug", "platforms:Linux"})
  defines({
    -- "_GLIBCXX_DEBUG",   -- libstdc++ debug mode (disabled - causes ABI issues with system libraries)
  })

filter({"configurations:Checked or Debug", "platforms:Windows"}) -- "toolset:msc"
  symbols("Full")

filter("configurations:Debug")
  runtime("Release")
  optimize("Off")
  inlining("Disabled")  -- No inlining for debug
  defines({
    "DEBUG",
    "_NO_DEBUG_HEAP=1",
  })

filter("configurations:Release")
  runtime("Release")
  defines({
    "NDEBUG",
    "_NO_DEBUG_HEAP=1",
  })
  optimize("Speed")
  flags({
    "NoBufferSecurityCheck"
  })
  editandcontinue("Off")
  -- Not using floatingpoint("Fast") - NaN checks are used in some places
  -- (though rarely), overall preferable to avoid any functional differences
  -- between debug and release builds, and to have calculations involved in GPU
  -- (especially anything that may affect vertex position invariance) and CPU
  -- (such as constant propagation) emulation as predictable as possible,
  -- including handling of specials since games make assumptions about them.

filter({"configurations:Release", "platforms:not Windows"})
  symbols("On")  -- Enable debug symbols for crash debugging
  linktimeoptimization("On")
  buildoptions({
    "-O3",  -- Maximum optimization (premake's optimize("Speed") might only be -O2)
    "-finline-functions",  -- Aggressive function inlining
    "-funroll-loops",  -- Unroll loops where beneficial
    "-fomit-frame-pointer",  -- Don't keep frame pointer for better performance
  })

filter({"configurations:Release", "platforms:Windows"}) -- "toolset:msc"
  symbols("Off")  -- Disable PDB generation for release builds
  linktimeoptimization("On")
  buildoptions({
    "/Gw",
    "/Ob3",  -- Aggressive inlining for maximum performance
--    "/Qpar",   -- TODO: Test this.
  })

filter("configurations:Valgrind")
  runtime("Release")
  optimize("Debug")  -- -Og for reasonable performance with debuggability
  symbols("Full")  -- Full debug symbols for better Valgrind output
  defines({
    "DEBUG",
    "NDEBUG",
    "_NO_DEBUG_HEAP=1",
  })
  -- NO sanitize("Address") - incompatible with Valgrind
  -- Disable optimizations that make debugging harder
  buildoptions({
    "-fno-omit-frame-pointer",  -- Keep frame pointers for better stack traces
    "-fno-inline-functions",    -- Disable function inlining for clearer traces
  })

filter({"configurations:Valgrind", "platforms:Linux"})
  -- Additional Valgrind-friendly settings for Linux
  buildoptions({
    "-g3",  -- Maximum debug info
  })

filter("platforms:Linux")
  system("linux")
  toolset("clang")
  local qt_dir = os.getenv("QT_DIR")
  if qt_dir then
    local qt_version = get_qt_version(qt_dir)
    includedirs({
      path.join(qt_dir, "include"),
      path.join(qt_dir, "include/QtCore"),
      path.join(qt_dir, "include/QtCore", qt_version),
      path.join(qt_dir, "include/QtCore", qt_version, "QtCore"),
      path.join(qt_dir, "include/QtGui"),
      path.join(qt_dir, "include/QtGui", qt_version),
      path.join(qt_dir, "include/QtGui", qt_version, "QtGui"),
      path.join(qt_dir, "include/QtWidgets"),
    })
    libdirs({
      path.join(qt_dir, "lib"),
    })
    runpathdirs({
      path.join(qt_dir, "lib"),
    })
    -- For CMake: set RPATH to find Qt libraries
    linkoptions({
      "-Wl,-rpath," .. path.join(qt_dir, "lib"),
    })
    links({
      "Qt6Core",
      "Qt6Gui",
      "Qt6Widgets",
    })
  end

  links({
    "stdc++fs",
    "dl",
    "lz4",
    "m",
    "pthread",
    "rt",
  })

filter({"platforms:Linux", "kind:*App"})
  linkgroups("On")

filter({"language:C++", "toolset:clang or gcc"}) -- "platforms:Linux"
  disablewarnings({
    "switch",
    "attributes",
  })

filter({"language:C++", "toolset:gcc"}) -- "platforms:Linux"
  disablewarnings({
    "unused-result",
    "volatile",
    "template-id-cdtor",
    "return-type",
    "deprecated",
  })

filter("toolset:gcc") -- "platforms:Linux"
  removefatalwarnings("All") -- HACK
  if ARCH == "ppc64" then
    buildoptions({
      "-m32",
      "-mpowerpc64"
    })
    linkoptions({
      "-m32",
      "-mpowerpc64"
    })
  else
    buildoptions({
      "-fpermissive", -- HACK
    })
    linkoptions({
      "-fpermissive", -- HACK
    })
  end

filter({"language:C++", "toolset:clang"}) -- "platforms:Linux"
  disablewarnings({
    "deprecated-register",
    "deprecated-volatile",
    "deprecated-enum-enum-conversion",
  })
CLANG_BIN = os.getenv("CC") or _OPTIONS["cc"] or "clang"
if os.istarget("linux") and string.contains(CLANG_BIN, "clang") then
  CLANG_VER = tonumber(string.match(os.outputof(CLANG_BIN.." --version"), "version (%d%d)"))
  if CLANG_VER >= 20 then
    filter({"language:C++", "toolset:clang"}) -- "platforms:Linux"
      disablewarnings({
        "deprecated-literal-operator",   -- Needed only for tabulate
        "nontrivial-memcall",
      })
  end
  if CLANG_VER >= 21 then
    filter({"language:C++", "toolset:clang"}) -- "platforms:Linux"
      disablewarnings({
        "character-conversion",          -- Needed for utfcpp third-party library
      })
  end
end

filter({"language:C", "toolset:clang or gcc"}) -- "platforms:Linux"
  disablewarnings({
    "implicit-function-declaration",
  })

if os.istarget("android") then
  filter("platforms:Android-*")
    system("android")
    systemversion("24")
    cppstl("c++")
    staticruntime("On")
    -- Hidden visibility is needed to prevent dynamic relocations in FFmpeg
    -- AArch64 Neon libavcodec assembly with PIC (accesses extern lookup tables
    -- using `adrp` and `add`, without the Global Object Table, expecting that all
    -- FFmpeg symbols that aren't a part of the FFmpeg API are hidden by FFmpeg's
    -- original build system) by resolving those relocations at link time instead.
    visibility("Hidden")
    links({
      "android",
      "dl",
      "log",
    })
end

filter("platforms:Windows")
  system("windows")
  toolset("msc")
  buildoptions({
    "/utf-8",   -- 'build correctly on systems with non-Latin codepages'.
    "/Zc:__cplusplus",   -- Enable correct __cplusplus macro value (required for Qt6).
    "/Zc:preprocessor",   -- Enable conformant preprocessor (supports #if in macro args).
    -- Disable warnings
    "/wd4201",   -- Nameless struct/unions are ok.
  })
  flags({
    "MultiProcessorCompile",   -- Multiprocessor compilation.
    "NoMinimalRebuild",        -- Required for /MP above.
  })

  defines({
    "_CRT_NONSTDC_NO_DEPRECATE",
    "_CRT_SECURE_NO_WARNINGS",
    "WIN32",
    "_WIN64=1",
    "_AMD64=1",
  })
  linkoptions({
    "/ignore:4006",  -- Ignores complaints about empty obj files.
    "/ignore:4221",
  })
  links({
    "ntdll",
    "wsock32",
    "ws2_32",
    "xinput",
    "comctl32",
    "shcore",
    "shlwapi",
    "dxguid",
    "bcrypt",
  })
  -- Add Qt for Windows
  local qt_dir = os.getenv("QT_DIR")
  if qt_dir then
    includedirs({
      path.join(qt_dir, "include"),
      path.join(qt_dir, "include/QtCore"),
      path.join(qt_dir, "include/QtGui"),
      path.join(qt_dir, "include/QtWidgets"),
    })
    libdirs({
      path.join(qt_dir, "lib"),
    })
  end

filter({"platforms:Windows", "configurations:Release"})
  if qt_dir then
    links({
      "Qt6Core",
      "Qt6Gui",
      "Qt6Widgets",
    })
  end

filter({"platforms:Windows", "configurations:Debug or Checked"})
  if qt_dir then
    links({
      "Qt6Cored",
      "Qt6Guid",
      "Qt6Widgetsd",
    })
  end

filter("platforms:Windows")

-- Embed the manifest for things like dependencies and DPI awareness.
filter({"platforms:Windows", "kind:ConsoleApp or WindowedApp"})
  files({
    "src/xenia/base/app_win32.manifest"
  })

-- Create scratch/ path
if not os.isdir("scratch") then
  os.mkdir("scratch")
end

workspace("xenia")
  uuid("931ef4b0-6170-4f7a-aaf2-0fece7632747")
  startproject("xenia-app")
  if os.istarget("android") then
    platforms({"Android-ARM64", "Android-x86_64"})
    filter("platforms:Android-ARM64")
      architecture("ARM64")
    filter("platforms:Android-x86_64")
      architecture("x86_64")
    filter({})
  else
    architecture("x86_64")
    if os.istarget("linux") then
      platforms({"Linux"})
    elseif os.istarget("macosx") then
      platforms({"Mac"})
      xcodebuildsettings({
        ["ARCHS"] = "x86_64"
      })
    elseif os.istarget("windows") then
      platforms({"Windows"})
      -- 10.0.15063.0: ID3D12GraphicsCommandList1::SetSamplePositions.
      -- 10.0.19041.0: D3D12_HEAP_FLAG_CREATE_NOT_ZEROED.
      -- 10.0.22000.0: DWMWA_WINDOW_CORNER_PREFERENCE.
      systemversion("latest")
      filter({})
    end
  end
  configurations({"Checked", "Debug", "Release", "Valgrind"})

  include("third_party/aes_128.lua")
  include("third_party/asio.lua")
  include("third_party/capstone.lua")
  include("third_party/dxbc.lua")
  include("third_party/discord-rpc.lua")
  include("third_party/cxxopts.lua")
  include("third_party/tomlplusplus.lua")
  include("third_party/FFmpeg/premake5.lua")
  include("third_party/fmt.lua")
  include("third_party/glslang-spirv.lua")
  include("third_party/imgui.lua")
  include("third_party/metal-shader-converter.lua")
  include("third_party/metal-cpp.lua")
  include("third_party/miniaudio.lua")
  include("third_party/mspack.lua")
  include("third_party/snappy.lua")
  include("third_party/xxhash.lua")
  include("third_party/zarchive.lua")
  include("third_party/zstd.lua")
  include("third_party/zlib-ng.lua")
  include("third_party/pugixml.lua")

  if os.istarget("windows") then
    include("third_party/libusb.lua")
  end

  if not os.istarget("android") then
    -- SDL2 requires sdl2-config, and as of November 2020 isn't high-quality on
    -- Android yet, most importantly in game controllers - the keycode and axis
    -- enums are being ruined during conversion to SDL2 enums resulting in only
    -- one controller (Nvidia Shield) being supported, digital triggers are also
    -- not supported; lifecycle management (especially surface loss) is also
    -- complicated.
    include("third_party/SDL2.lua")
  end

  -- Disable treating warnings as fatal errors for all third party projects, as
  -- well as other things relevant only to Xenia itself.
  for _, prj in ipairs(premake.api.scope.current.solution.projects) do
    project(prj.name)
    removefiles({
      "src/xenia/base/app_win32.manifest"
    })
    removefatalwarnings("All")

    filter({"platforms:Linux", "configurations:Checked"})
      buildoptions({
        "-fsanitize=undefined",
      })
      linkoptions({
        "-fsanitize=undefined",
      })
    filter({})

    -- Add POSIX feature test macros for FFmpeg on Linux
    if prj.name == "libavutil" or prj.name == "libavcodec" or prj.name == "libavformat" then
      filter({"platforms:Linux"})
        defines({
          "_GNU_SOURCE",
          "_POSIX_C_SOURCE=200809L",
          "_XOPEN_SOURCE=700",
        })
      filter({})
    end

    -- Suppress warnings for third_party modules on Windows
    filter({"platforms:Windows"})
      if string.startswith(prj.name, "third_party") or
         prj.name == "aes_128" or
         prj.name == "capstone" or
         prj.name == "dxbc" or
         prj.name == "discord-rpc" or
         prj.name == "cxxopts" or
         prj.name == "tomlplusplus" or
         prj.name == "FFmpeg" or
         prj.name == "fmt" or
         prj.name == "glslang-spirv" or
         prj.name == "imgui" or
         prj.name == "mspack" or
         prj.name == "snappy" or
         prj.name == "xxhash" or
         prj.name == "zarchive" or
         prj.name == "zstd" or
         prj.name == "zlib-ng" or
         prj.name == "pugixml" or
         prj.name == "libusb" or
         prj.name == "SDL2" then
        buildoptions({
          "/W0",  -- Disable all warnings for third_party code
        })
      end
    filter({})
  end

  include("src/xenia")
  include("src/xenia/app")
  include("src/xenia/app/discord")
  include("src/xenia/apu")
  include("src/xenia/apu/nop")
  include("src/xenia/base")
  include("src/xenia/cpu")
  include("src/xenia/cpu/backend/x64")
  include("src/xenia/debug/ui")
  include("src/xenia/gpu")
  include("src/xenia/gpu/null")
  include("src/xenia/gpu/vulkan")
  include("src/xenia/hid")
  include("src/xenia/hid/nop")
  include("src/xenia/hid/skylander")
  include("src/xenia/kernel")
  include("src/xenia/patcher")
  include("src/xenia/ui")
  include("src/xenia/ui/vulkan")
  include("src/xenia/vfs")

  if not os.istarget("android") then
    include("src/xenia/apu/sdl")
    include("src/xenia/helper/sdl")
    include("src/xenia/hid/sdl")
  end

  if os.istarget("linux") then
    include("src/xenia/apu/alsa")
  end

  if os.istarget("windows") then
    include("src/xenia/apu/xaudio2")
    include("src/xenia/debug/gdb") -- Needs socket support for other systems.
    include("src/xenia/gpu/d3d12")
    include("src/xenia/hid/winkey")
    include("src/xenia/hid/xinput")
    include("src/xenia/ui/d3d12")
  end
