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
  -- Try standard location first
  local qconfig_path = path.join(qt_dir, "mkspecs/qconfig.pri")
  local qconfig_file = io.open(qconfig_path, "r")

  -- Try system Qt location (e.g., /usr/lib/x86_64-linux-gnu/qt6)
  if not qconfig_file then
    -- For system Qt, qt_dir might be cmake path - try to find actual Qt dir
    local system_paths = {
      "/usr/lib/x86_64-linux-gnu/qt6/mkspecs/qconfig.pri",
      "/usr/lib/aarch64-linux-gnu/qt6/mkspecs/qconfig.pri",
      "/usr/share/qt6/mkspecs/qconfig.pri",
    }
    for _, sys_path in ipairs(system_paths) do
      qconfig_file = io.open(sys_path, "r")
      if qconfig_file then
        qconfig_path = sys_path
        break
      end
    end
  end

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

  -- Return nil instead of error for system Qt without qconfig.pri
  return nil
end

location(build_root)
targetdir(build_bin)
objdir(build_obj)

-- Define variables for enabling specific submodules
newoption({
  trigger = "tests",
  description = "Enable building test targets",
})
newoption({
  trigger = "mac-x86_64",
  description = "Enable x86_64 platform on macOS ARM64 hosts",
})
newoption({
  trigger = "arch",
  value = "ARCH",
  description = "Target architecture (x86_64 or arm64)",
})

enableTests = _OPTIONS["tests"] ~= nil
enableMiscSubprojects = false

-- Define an ARCH variable
-- Only use this to enable architecture-specific functionality.
if os.istarget("linux") then
  ARCH = os.outputof("uname -p")
else
  ARCH = "unknown"
end

function is_macos_arm64_host()
  local env = os.getenv("XE_MACOS_ARM64_HOST")
  if env == "1" then
    return true
  end
  if env == "0" then
    return false
  end
  if not os.istarget("macosx") then
    return false
  end
  local sysctl = os.outputof("sysctl -n hw.optional.arm64 2>/dev/null")
  if sysctl then
    local sysctl_value = sysctl:match("^(%d+)")
    if sysctl_value == "1" then
      return true
    end
  end
  local machine = os.outputof("uname -m")
  if machine then
    local machine_value = machine:match("^(%S+)")
    if machine_value == "arm64" then
      return true
    end
  end
  return false
end

local function normalize_arch(arch)
  if not arch then
    return nil
  end
  arch = arch:lower()
  if arch == "arm64" or arch == "aarch64" then
    return "ARM64"
  end
  if arch == "x86_64" or arch == "x64" or arch == "x86" or arch == "amd64" then
    return "x86_64"
  end
  return nil
end

local function detect_target_arch()
  local option_arch = normalize_arch(_OPTIONS["arch"])
  if option_arch then
    return option_arch
  end
  if os.istarget("macosx") then
    if _OPTIONS["mac-x86_64"] then
      return "x86_64"
    end
    return is_macos_arm64_host() and "ARM64" or "x86_64"
  end
  if os.istarget("linux") then
    return normalize_arch(os.outputof("uname -m")) or "x86_64"
  end
  if os.istarget("windows") then
    local env_arch = os.getenv("PROCESSOR_ARCHITEW6432") or
                     os.getenv("PROCESSOR_ARCHITECTURE")
    return normalize_arch(env_arch) or "x86_64"
  end
  return "x86_64"
end

TARGET_ARCH = detect_target_arch()

includedirs({
  ".",
  "src",
  "third_party",
})

-- Add SDL2 include path for Linux cmake builds
-- premake-cmake doesn't properly handle includedirs from sdl2_include() function
if os.istarget("linux") then
  local sdl2_config = os.getenv("SDL2_CONFIG") or "sdl2-config"
  local sdl2_cflags = os.outputof(sdl2_config .. " --cflags")
  if sdl2_cflags then
    for inc in string.gmatch(sdl2_cflags, "-I([%S]+)") do
      includedirs({inc})
      print("SDL2: Global include dir: " .. inc)
    end
  end
  -- Fallback to common location if sdl2-config doesn't return -I flags
  if not sdl2_cflags or not string.find(sdl2_cflags, "-I") then
    includedirs({"/usr/include/SDL2"})
    print("SDL2: Using fallback global include /usr/include/SDL2")
  end
end

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

filter({"configurations:Checked", "platforms:Linux-*"})
  buildoptions({
    "-fsanitize=undefined",
  })
  linkoptions({
    "-fsanitize=undefined",
  })

filter({"configurations:Checked", "platforms:Windows-*"}) -- "toolset:msc"
  buildoptions({
    "/RTCsu",           -- Full Run-Time Checks.
  })
  -- AddressSanitizer on Windows (doesn't conflict with memory layout like on Linux)
  sanitize("Address")

filter({"configurations:Checked or Debug", "platforms:Linux-*"})
  defines({
    -- "_GLIBCXX_DEBUG",   -- libstdc++ debug mode (disabled - causes ABI issues with system libraries)
  })

filter({"configurations:Checked or Debug", "platforms:Windows-*"}) -- "toolset:msc"
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

filter({"configurations:Release", "platforms:Linux-* or Mac-* or Android-*"})
  symbols("On")  -- Enable debug symbols for crash debugging
  linktimeoptimization("On")
  buildoptions({
    "-O3",  -- Maximum optimization (premake's optimize("Speed") might only be -O2)
    "-finline-functions",  -- Aggressive function inlining
    "-funroll-loops",  -- Unroll loops where beneficial
    "-fomit-frame-pointer",  -- Don't keep frame pointer for better performance
  })

filter({"configurations:Release", "platforms:Windows-*"}) -- "toolset:msc"
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

filter({"configurations:Valgrind", "platforms:Linux-*"})
  -- Additional Valgrind-friendly settings for Linux
  buildoptions({
    "-g3",  -- Maximum debug info
  })

if os.istarget("linux") then
  filter("platforms:Linux-*")
    system("linux")
    toolset("clang")
    local qt_dir = os.getenv("QT_DIR")
    if qt_dir then
      local qt_version = get_qt_version(qt_dir)
      if qt_version then
        -- Qt installed via aqtinstall or similar (has version-specific dirs)
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
        linkoptions({
          "-Wl,-rpath," .. path.join(qt_dir, "lib"),
        })
      else
        -- QT_DIR set but no version found - assume it's a system path or cmake dir
        -- Use standard system paths instead
        includedirs({
          "/usr/include/x86_64-linux-gnu/qt6",
          "/usr/include/x86_64-linux-gnu/qt6/QtCore",
          "/usr/include/x86_64-linux-gnu/qt6/QtGui",
          "/usr/include/x86_64-linux-gnu/qt6/QtWidgets",
          "/usr/include/aarch64-linux-gnu/qt6",
          "/usr/include/aarch64-linux-gnu/qt6/QtCore",
          "/usr/include/aarch64-linux-gnu/qt6/QtGui",
          "/usr/include/aarch64-linux-gnu/qt6/QtWidgets",
          "/usr/include/qt6",
          "/usr/include/qt6/QtCore",
          "/usr/include/qt6/QtGui",
          "/usr/include/qt6/QtWidgets",
        })
      end
    else
      -- No QT_DIR set - use system Qt paths (installed via apt)
      includedirs({
        "/usr/include/x86_64-linux-gnu/qt6",
        "/usr/include/x86_64-linux-gnu/qt6/QtCore",
        "/usr/include/x86_64-linux-gnu/qt6/QtGui",
        "/usr/include/x86_64-linux-gnu/qt6/QtWidgets",
        "/usr/include/aarch64-linux-gnu/qt6",
        "/usr/include/aarch64-linux-gnu/qt6/QtCore",
        "/usr/include/aarch64-linux-gnu/qt6/QtGui",
        "/usr/include/aarch64-linux-gnu/qt6/QtWidgets",
        "/usr/include/qt6",
        "/usr/include/qt6/QtCore",
        "/usr/include/qt6/QtGui",
        "/usr/include/qt6/QtWidgets",
      })
    end
    -- Always link Qt on Linux
    links({
      "Qt6Core",
      "Qt6Gui",
      "Qt6Widgets",
    })

    links({
      "stdc++fs",
      "dl",
      "lz4",
      "m",
      "pthread",
      "rt",
    })
end

filter({"platforms:Linux-*", "kind:*App"})
  linkgroups("On")

if os.istarget("macosx") then
  filter("platforms:Mac-*")
    buildoptions({
      "-mmacosx-version-min=15.0",
    })
    linkoptions({
      "-mmacosx-version-min=15.0",
    })
    xcodebuildsettings({
      ["MACOSX_DEPLOYMENT_TARGET"] = "15.0",
    })
    local qt_dir = os.getenv("QT_DIR")
    if not qt_dir then
      local brew_prefix = os.outputof("brew --prefix qt@6 2>/dev/null")
      if not brew_prefix or brew_prefix == "" then
        brew_prefix = os.outputof("brew --prefix qt 2>/dev/null")
      end
      if brew_prefix then
        brew_prefix = brew_prefix:gsub("%s+$", "")
        if #brew_prefix > 0 and os.isdir(brew_prefix) then
          qt_dir = brew_prefix
        end
      end
    end
    if not qt_dir then
      local candidates = {
        "/opt/homebrew/opt/qt",
        "/opt/homebrew/opt/qt@6",
        "/usr/local/opt/qt",
        "/usr/local/opt/qt@6",
      }
      for _, candidate in ipairs(candidates) do
        if os.isdir(candidate) then
          qt_dir = candidate
          break
        end
      end
    end
    if qt_dir then
      frameworkdirs({
        path.join(qt_dir, "lib"),
      })
      externalincludedirs({
        path.join(qt_dir, "lib/QtCore.framework/Headers"),
        path.join(qt_dir, "lib/QtGui.framework/Headers"),
        path.join(qt_dir, "lib/QtWidgets.framework/Headers"),
      })
      buildoptions({
        "-I" .. path.join(qt_dir, "lib/QtCore.framework/Headers"),
        "-I" .. path.join(qt_dir, "lib/QtGui.framework/Headers"),
        "-I" .. path.join(qt_dir, "lib/QtWidgets.framework/Headers"),
      })
      links({
        "QtCore.framework",
        "QtGui.framework",
        "QtWidgets.framework",
      })
      linkoptions({
        "-Wl,-rpath," .. path.join(qt_dir, "lib"),
      })
    end
end

filter({"platforms:Mac-*", "toolset:clang"})
  buildoptions({
    "-w",
  })
  removefatalwarnings("All")
filter({"platforms:Mac-x86_64", "toolset:clang"})
  buildoptions({
    "-mavx",
  })
filter({})

filter({"language:C++", "toolset:clang or gcc"}) -- "platforms:Linux-*"
  disablewarnings({
    "switch",
    "attributes",
  })

filter({"language:C++", "toolset:gcc"}) -- "platforms:Linux-*"
  disablewarnings({
    "unused-result",
    "volatile",
    "template-id-cdtor",
    "return-type",
    "deprecated",
  })

filter("toolset:gcc") -- "platforms:Linux-*"
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

filter({"language:C++", "toolset:clang"}) -- "platforms:Linux-*"
  disablewarnings({
    "deprecated-register",
    "deprecated-volatile",
    "deprecated-enum-enum-conversion",
  })
CLANG_BIN = os.getenv("CC") or _OPTIONS["cc"] or "clang"
if os.istarget("linux") and string.contains(CLANG_BIN, "clang") then
  CLANG_VER = tonumber(string.match(os.outputof(CLANG_BIN.." --version"), "version (%d%d)"))
  if CLANG_VER >= 20 then
    filter({"language:C++", "toolset:clang"}) -- "platforms:Linux-*"
      disablewarnings({
        "deprecated-literal-operator",   -- Needed only for tabulate
        "nontrivial-memcall",
      })
  end
  if CLANG_VER >= 21 then
    filter({"language:C++", "toolset:clang"}) -- "platforms:Linux-*"
      disablewarnings({
        "character-conversion",          -- Needed for utfcpp third-party library
        "absolute-value",                -- Needed for tomlplusplus third-party library
        "enum-compare-switch",           -- Needed for a64_backend with capstone
        "deprecated-enum-compare",       -- Needed for a64_backend with capstone
      })
  end
end
if os.istarget("linux") then
  if ARCH == "aarch64" or ARCH == "arm64" then
    filter({"platforms:Linux-*", "toolset:clang"})
      buildoptions({
        "-include arm_acle.h",
      })
  end
end
filter({})

filter({"language:C", "toolset:clang or gcc"}) -- "platforms:Linux-*"
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

filter("platforms:Windows-*")
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

filter("platforms:Windows-x86_64")
  defines({
    "_AMD64=1",
  })

filter("platforms:Windows-ARM64")
  defines({
    "_ARM64_=1",
  })

filter({"platforms:Windows-*", "configurations:Release"})
  if qt_dir then
    links({
      "Qt6Core",
      "Qt6Gui",
      "Qt6Widgets",
    })
  end

filter({"platforms:Windows-*", "configurations:Debug or Checked"})
  if qt_dir then
    links({
      "Qt6Cored",
      "Qt6Guid",
      "Qt6Widgetsd",
    })
  end

filter("platforms:Windows-*")

-- Embed the manifest for things like dependencies and DPI awareness.
filter({"platforms:Windows-*", "kind:ConsoleApp or WindowedApp"})
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
    if os.istarget("linux") then
      if TARGET_ARCH == "ARM64" then
        platforms({"Linux-ARM64"})
        architecture("ARM64")
      else
        platforms({"Linux-x86_64"})
        architecture("x86_64")
      end
    elseif os.istarget("macosx") then
      local mac_platforms = nil
      if is_macos_arm64_host() then
        if _OPTIONS["mac-x86_64"] then
          mac_platforms = {"Mac-x86_64"}
        else
          mac_platforms = {"Mac-ARM64"}
        end
      else
        mac_platforms = {"Mac-x86_64"}
      end
      platforms(mac_platforms)
      filter("platforms:Mac-ARM64")
        architecture("ARM64")
        xcodebuildsettings({
          ["ARCHS"] = "arm64",
        })
      filter("platforms:Mac-x86_64")
        architecture("x86_64")
        xcodebuildsettings({
          ["ARCHS"] = "x86_64",
          ["CLANG_X86_VECTOR_INSTRUCTION_SET"] = "avx",
        })
      filter({})
    elseif os.istarget("windows") then
      if TARGET_ARCH == "ARM64" then
        platforms({"Windows-ARM64"})
        architecture("ARM64")
      else
        platforms({"Windows-x86_64"})
        architecture("x86_64")
      end
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

    filter({"platforms:Linux-*", "configurations:Checked"})
      buildoptions({
        "-fsanitize=undefined",
      })
      linkoptions({
        "-fsanitize=undefined",
      })
    filter({})

    -- Add POSIX feature test macros for FFmpeg on Linux
    if prj.name == "libavutil" or prj.name == "libavcodec" or prj.name == "libavformat" then
      filter({"platforms:Linux-*"})
        defines({
          "_GNU_SOURCE",
          "_POSIX_C_SOURCE=200809L",
          "_XOPEN_SOURCE=700",
        })
      filter({})
    end

    -- Suppress warnings for third_party modules on Windows
    filter({"platforms:Windows-*"})
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
  if TARGET_ARCH == "ARM64" then
    include("src/xenia/cpu/backend/a64")
  end
  if TARGET_ARCH == "x86_64" then
    include("src/xenia/cpu/backend/x64")
  end
  include("src/xenia/debug/ui")
  include("src/xenia/gpu")
  if os.istarget("macosx") then
    include("src/xenia/gpu/metal")
  end
  include("src/xenia/gpu/null")
  include("src/xenia/gpu/vulkan")
  include("src/xenia/hid")
  include("src/xenia/hid/nop")
  include("src/xenia/hid/skylander")
  include("src/xenia/kernel")
  include("src/xenia/patcher")
  include("src/xenia/ui")
  if os.istarget("macosx") then
    include("src/xenia/ui/metal")
  end
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
