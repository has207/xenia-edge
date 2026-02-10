project_root = "../../../../.."
include(project_root.."/tools/build")

group("tests")
project("xenia-cpu-ppc-tests")
  uuid("2a57d5ac-4024-4c49-9cd3-aa3a603c2ef8")
  kind("ConsoleApp")
  language("C++")
  links({
    "xenia-apu",
    "xenia-apu-nop",
    "xenia-base",
    "xenia-core",
    "xenia-cpu",
    "xenia-gpu",
    "xenia-gpu-null",
    "xenia-hid",
    "xenia-hid-nop",
    "xenia-kernel",
    "xenia-patcher",
    "xenia-ui",
    "xenia-vfs",
  })
  links({
    "capstone",
    "fmt",
    "imgui",
    "mspack",
  })
  apu_transitive_deps()
  -- Qt is required because xenia-kernel now uses Qt for achievements dialog
  filter("platforms:Linux-*")
    local qt_dir = os.getenv("QT_DIR")
    if qt_dir then
      links({
        "Qt6Core",
        "Qt6Gui",
        "Qt6Widgets",
      })
    end
  filter({})
  files({
    "ppc_testing_main.cc",
    "../../../base/console_app_main_"..platform_suffix..".cc",
  })
  files({
    "*.s",
  })
  filter("files:*.s")
    flags({
      "ExcludeFromBuild",
    })
  filter("architecture:x86_64")
    links({
      "xenia-cpu-backend-x64",
    })
  filter("platforms:Windows-*")
    debugdir(project_root)
    debugargs({
      "2>&1",
      "1>scratch/stdout-testing.txt",
    })

  filter({})

if ARCH == "ppc64" or ARCH == "powerpc64" then

project("xenia-cpu-ppc-nativetests")
  uuid("E381E8EE-65CD-4D5E-9223-D9C03B2CE78C")
  kind("ConsoleApp")
  language("C++")
  links({
    "fmt",
    "xenia-base",
  })
  files({
    "ppc_testing_native_main.cc",
    "../../../base/console_app_main_"..platform_suffix..".cc",
  })
  files({
    "instr_*.s",
    "seq_*.s",
  })
  filter("files:instr_*.s", "files:seq_*.s")
    flags({
      "ExcludeFromBuild",
    })
  filter({})
  buildoptions({
    "-Wa,-mregnames",  -- Tell GAS to accept register names.
  })

  files({
    "ppc_testing_native_thunks.s",
  })

end
