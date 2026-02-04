project_root = "../../.."
include(project_root.."/tools/build")

group("src")
project("xenia-gpu")
  uuid("0e8d3370-e4b1-4b05-a2e8-39ebbcdf9b17")
  kind("StaticLib")
  language("C++")
  links({
    "dxbc",
    "fmt",
    "glslang-spirv",
    "snappy",
    "xenia-base",
    "xenia-ui",
    "xxhash",
  })
  includedirs({
    project_root.."/third_party/glslang",  -- For glslang SPIRV headers
  })
  filter("platforms:Linux-* or Windows-* or Android-*")
    includedirs({
      project_root.."/third_party/Vulkan-Headers/include",
    })
  filter({})

  -- Include SPIRV-Tools headers from Vulkan SDK for Windows
  filter("platforms:Windows-*")
    includedirs({
      "$(VULKAN_SDK)/Include",
    })
  filter({})

  local_platform_files()

  if os.istarget("macosx") then
    removefiles({
      "spirv_shader*.cc",
      "spirv_shader*.h",
    })
    filter("files:**/spirv_shader*.cc")
      flags({ "ExcludeFromBuild" })
    filter({})
  end

if enableMiscSubprojects and not os.istarget("macosx") then
  group("src")
  project("xenia-gpu-shader-compiler")
    uuid("ad76d3e4-4c62-439b-a0f6-f83fcf0e83c5")
    kind("ConsoleApp")
    language("C++")
    links({
      "dxbc",
      "fmt",
      "glslang-spirv",
      "snappy",
      "xenia-base",
      "xenia-gpu",
      "xenia-ui",
      "xenia-ui-vulkan",
    })
    includedirs({
      project_root.."/third_party/Vulkan-Headers/include",
    })
    files({
      "shader_compiler_main.cc",
      "../base/console_app_main_"..platform_suffix..".cc",
    })

    -- Include SPIRV-Tools headers from Vulkan SDK
    filter("platforms:Windows-*")
      includedirs({
        "$(VULKAN_SDK)/Include",
      })
    filter({})

    filter("platforms:Windows-*")
      -- Only create the .user file if it doesn't already exist.
      local user_file = project_root.."/build/xenia-gpu-shader-compiler.vcxproj.user"
      if not os.isfile(user_file) then
        debugdir(project_root)
        debugargs({
          "2>&1",
          "1>scratch/stdout-shader-compiler.txt",
        })
      end
    filter({})
end
