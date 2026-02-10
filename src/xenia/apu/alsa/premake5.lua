group("src")
project("xenia-apu-alsa")
  uuid("8c2e1340-f847-4f9a-8b2e-5d8c1b7a8f9e")
  kind("StaticLib")
  language("C++")
  links({
    "xenia-apu",
    "xenia-base",
  })
  defines({
  })
  local_platform_files()

  filter("platforms:Linux-*")
    links({
      "asound",
      "pthread",
    })
