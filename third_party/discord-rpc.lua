group("third_party")
project("discord-rpc")
  uuid("012f6131-efc0-4abd-852d-a33640732d4c")
  kind("StaticLib")
  language("C++")
  includedirs({
    "discord-rpc/include",
    "rapidjson/include"
  })

  -- Common files for all platforms
  files({
    "discord-rpc/src/connection.h",
    "discord-rpc/src/discord_rpc.cpp",
    "discord-rpc/src/msg_queue.h",
    "discord-rpc/src/rpc_connection.cpp",
    "discord-rpc/src/rpc_connection.h",
    "discord-rpc/src/serialization.cpp",
    "discord-rpc/src/serialization.h"
  })

  -- x86_64 uses SSE4.2, ARM64 uses NEON for rapidjson SIMD
  filter("architecture:x86_64")
    defines({ "RAPIDJSON_SSE42" })
  filter("architecture:ARM64")
    defines({ "RAPIDJSON_NEON" })
  filter({})

  -- Platform-specific connection implementations
  -- For premake-cmake compatibility, we use os.istarget() which is evaluated at
  -- premake generation time. This ensures the correct files are included in cmake.
  if os.istarget("linux") then
    files({
      "discord-rpc/src/connection_unix.cpp",
      "discord-rpc/src/discord_register_linux.cpp"
    })
  elseif os.istarget("macosx") then
    files({
      "discord-rpc/src/connection_unix.cpp",
      "discord-rpc/src/discord_register_osx.m"
    })
  elseif os.istarget("windows") then
    files({
      "discord-rpc/src/connection_win.cpp",
      "discord-rpc/src/discord_register_win.cpp"
    })
  end

  -- Also add system filter for generators that handle platforms differently
  filter("system:linux")
    files({
      "discord-rpc/src/connection_unix.cpp",
      "discord-rpc/src/discord_register_linux.cpp"
    })
  filter("system:macosx")
    files({
      "discord-rpc/src/connection_unix.cpp",
      "discord-rpc/src/discord_register_osx.m"
    })
  filter("system:windows")
    files({
      "discord-rpc/src/connection_win.cpp",
      "discord-rpc/src/discord_register_win.cpp"
    })
  filter({})
