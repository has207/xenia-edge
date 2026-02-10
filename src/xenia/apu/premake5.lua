project_root = "../../.."
include(project_root.."/tools/build")

group("src")
project("xenia-apu")
  uuid("f4df01f0-50e4-4c67-8f54-61660696cc79")
  kind("StaticLib")
  language("C++")
  links({
    "libavcodec",
    "libavutil",
    "libavformat",
    "xenia-base",
  })
  includedirs({
    project_root.."/third_party/FFmpeg",
    project_root.."/third_party/ffmpeg-xenia",
  })
  filter("platforms:Linux-*")
    links({
      "xenia-helper-sdl",
      "SDL2",
    })
    sdl2_include()
  filter({})
  local_platform_files()
