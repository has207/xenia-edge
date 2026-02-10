project_root = "../../../.."
include(project_root.."/tools/build")

group("src")
project("xenia-ui-vulkan")
  uuid("4933d81e-1c2c-4d5d-b104-3c0eb9dc2f00")
  kind("StaticLib")
  language("C++")
  links({
    "xenia-base",
    "xenia-ui",
  })
  includedirs({
    project_root.."/third_party/Vulkan-Headers/include",
    project_root.."/third_party/glslang",  -- For glslang SPIRV headers
  })

  -- Include SPIRV-Tools from Vulkan SDK
  filter("platforms:Windows-*")
    includedirs({
      "$(VULKAN_SDK)/Include",
    })
    libdirs({
      "$(VULKAN_SDK)/Lib",
    })
    links({
      "SPIRV-Tools-opt.lib",
      "SPIRV-Tools.lib",
    })

  filter("platforms:Linux-*")
    links({
      "SPIRV-Tools-opt",
      "SPIRV-Tools",
    })
  filter({})
  local_platform_files()
  local_platform_files("functions")
  files({
    "../shaders/bytecode/vulkan_spirv/*.h",
  })

if enableMiscSubprojects then
  group("demos")
  project("xenia-ui-window-vulkan-demo")
    uuid("97598f13-3177-454c-8e58-c59e2b6ede27")
    single_library_windowed_app_kind()
    language("C++")
    links({
      "fmt",
      "imgui",
      "xenia-apu",
      "xenia-base",
      "xenia-core",
      "xenia-gpu",
      "xenia-hid",
      "xenia-hid-skylander",
      "xenia-kernel",  -- Required by xenia-hid
      "xenia-patcher",
      "xenia-ui",
      "xenia-ui-vulkan",
      "xenia-vfs",
    })
    links({
      "aes_128",
      "capstone",
      "dxbc",
      "glslang-spirv",
      "mspack",
      "snappy",
      "xxhash",
    })
    apu_transitive_deps()
    includedirs({
      project_root.."/third_party/Vulkan-Headers/include",
    })
    files({
      "../window_demo.cc",
      "vulkan_window_demo.cc",
      project_root.."/src/xenia/ui/windowed_app_main_qt.cc",
    })
    resincludedirs({
      project_root,
    })

    filter("architecture:x86_64")
      links({
        "xenia-cpu-backend-x64",
      })

    filter("platforms:Linux-*")
      links({
        "X11",
        "xcb",
        "X11-xcb",
      })
end
