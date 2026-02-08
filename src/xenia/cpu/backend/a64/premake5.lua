project_root = "../../../../.."
include(project_root.."/tools/build")

if TARGET_ARCH ~= "ARM64" then
  return
end

group("src")
project("xenia-cpu-backend-a64")
  uuid("495f3f3e-f5e8-489a-bd0f-289d0495bc08")
  kind("StaticLib")
  language("C++")
  cppdialect("C++20")
  links({
    "fmt",
    "xenia-base",
    "xenia-cpu",
  })
  sysincludedirs({
    project_root.."/third_party/oaknut/include",
  })
  defines({
  })

  -- Add oaknut as external include to suppress warnings
  filter("toolset:clang or toolset:gcc")
    externalincludedirs({
      project_root.."/third_party/oaknut/include",
    })
    -- Also explicitly disable the warning for third-party code
    buildoptions({
      "-Wno-shorten-64-to-32",
    })
  filter("toolset:msc")
    includedirs({
      project_root.."/third_party/oaknut/include",
    })
    -- Disable warnings for oaknut third-party code
    disablewarnings({
      "4146",  -- unary minus operator applied to unsigned type
      "4244",  -- conversion from larger to smaller type
      "4267",  -- conversion from 'size_t' to smaller type
    })
  filter({})

  -- Include only ARM64-specific files
  local_platform_files()
