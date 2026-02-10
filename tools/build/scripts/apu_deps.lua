-- Helper function to add APU transitive dependencies
-- Call this after linking to xenia-apu to ensure all transitive dependencies are included
-- On Linux, xenia-apu's AudioMediaPlayer directly instantiates SDLAudioDriver,
-- so consumers must link xenia-apu-sdl (which in turn brings in xenia-helper-sdl and SDL2)
function apu_transitive_deps()
  filter("platforms:Linux-*")
    links({
      "xenia-apu-sdl",  -- Contains SDLAudioDriver used by AudioMediaPlayer
    })
  filter({})
end
