if(WIN32)
  add_executable(xenia-app WIN32)
elseif(APPLE)
  add_executable(xenia-app MACOSX_BUNDLE)
else()
  add_executable(xenia-app)
endif()

if(APPLE)
  # Drives the .app bundle directory name and the binary in Contents/MacOS/.
  set_target_properties(xenia-app PROPERTIES OUTPUT_NAME "Xenia-edge")
else()
  set_target_properties(xenia-app PROPERTIES OUTPUT_NAME "xenia_edge")
endif()

target_sources(xenia-app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/xenia_main.cc
)

xe_platform_sources(xenia-app ${CMAKE_CURRENT_SOURCE_DIR})

# Per-platform windowed entry point.
if(WIN32)
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_win.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_win.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/main_resources.rc
    ${PROJECT_SOURCE_DIR}/src/xenia/base/app_win32.manifest
  )
  if(XE_TARGET_X86_64)
    # main_init runs the AVX feature check — it must itself not use AVX.
    set_source_files_properties(
      ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_win.cc
      PROPERTIES COMPILE_OPTIONS "/arch:SSE2"
    )
  endif()
  # WIN32 executable defaults to /SUBSYSTEM:WINDOWS; override to CONSOLE
  # in Debug so stdout/stderr reach an attached terminal. Pin the entry
  # explicitly: wxIMPLEMENT_APP_NO_MAIN only emits wWinMain, so without
  # /ENTRY the CONSOLE subsystem looks for a missing main().
  target_link_options(xenia-app PRIVATE
    $<$<CONFIG:Debug>:/SUBSYSTEM:CONSOLE>
    /ENTRY:wWinMainCRTStartup
  )
elseif(APPLE)
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_mac.cc
  )
else()
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_linux.cc
  )
endif()

target_compile_definitions(xenia-app PRIVATE
  XBYAK_NO_OP_NAMES
  XBYAK_ENABLE_OMITTED_OPERAND
)

target_link_libraries(xenia-app PRIVATE
  xenia-apu
  xenia-apu-nop
  xenia-base
  xenia-core
  xenia-cpu
  xenia-gpu
  xenia-gpu-null
  xenia-hid
  xenia-hid-nop
  xenia-kernel
  xenia-patcher
  xenia-ui
  xenia-vfs
  capstone
  fmt
  discord-rpc
  imgui
  libavcodec
  libavutil
  mspack
  snappy
  xxhash
  xenia-app-discord
  xenia-apu-sdl
  xenia-debug-ui
  xenia-helper-sdl
  xenia-hid-sdl
)

# Vulkan/SPIR-V backends.
target_link_libraries(xenia-app PRIVATE
  xenia-gpu-vulkan
  xenia-ui-vulkan
  glslang-spirv
)

# Architecture-specific backend
if(XE_TARGET_X86_64)
  target_link_libraries(xenia-app PRIVATE xenia-cpu-backend-x64)
elseif(XE_TARGET_AARCH64)
  target_link_libraries(xenia-app PRIVATE xenia-cpu-backend-a64)
endif()

if(WIN32)
  target_link_libraries(xenia-app PRIVATE
    xenia-apu-xaudio2
    xenia-debug-gdb
    xenia-gpu-d3d12
    xenia-hid-keyboard
    xenia-ui-d3d12
    uxtheme
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(xenia-app PRIVATE
    xenia-hid-keyboard
    X11
    xcb
    X11-xcb
    SDL3
  )
elseif(APPLE)
  target_link_libraries(xenia-app PRIVATE
    xenia-gpu-metal
    xenia-hid-keyboard
    xenia-ui-metal
  )

  # macOS .app bundle: identity, Info.plist substitutions, AppIcon.icns,
  # rpath for frameworks alongside the executable, ad-hoc codesign with the
  # JIT entitlements so the binary can map writable+executable pages.
  #
  # iconutil only accepts a strictly-named .iconset directory, so stage one
  # in the build tree by copying assets/icon/<size>.png to the canonical
  # icon_<size>x<size>[@2x].png names.
  set(_xe_app_icon_src "${PROJECT_SOURCE_DIR}/assets/icon")
  set(_xe_app_iconset_dir "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.iconset")
  set(_xe_app_icns "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns")
  set(_xe_app_iconset_pairs
    16   icon_16x16.png
    32   icon_16x16@2x.png
    32   icon_32x32.png
    64   icon_32x32@2x.png
    128  icon_128x128.png
    256  icon_128x128@2x.png
    256  icon_256x256.png
    512  icon_256x256@2x.png
    512  icon_512x512.png
    1024 icon_512x512@2x.png)
  set(_xe_app_iconset_copy_cmds
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_xe_app_iconset_dir}")
  set(_xe_app_iconset_deps "")
  while(_xe_app_iconset_pairs)
    list(POP_FRONT _xe_app_iconset_pairs _xe_size _xe_dst)
    list(APPEND _xe_app_iconset_copy_cmds
      COMMAND "${CMAKE_COMMAND}" -E copy
              "${_xe_app_icon_src}/${_xe_size}.png"
              "${_xe_app_iconset_dir}/${_xe_dst}")
    list(APPEND _xe_app_iconset_deps "${_xe_app_icon_src}/${_xe_size}.png")
  endwhile()
  add_custom_command(
    OUTPUT "${_xe_app_icns}"
    ${_xe_app_iconset_copy_cmds}
    COMMAND iconutil --convert icns
            --output "${_xe_app_icns}"
            "${_xe_app_iconset_dir}"
    DEPENDS ${_xe_app_iconset_deps}
    VERBATIM)
  target_sources(xenia-app PRIVATE "${_xe_app_icns}")
  set_source_files_properties("${_xe_app_icns}" PROPERTIES
    GENERATED TRUE
    MACOSX_PACKAGE_LOCATION Resources)

  set_target_properties(xenia-app PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    MACOSX_BUNDLE_BUNDLE_NAME "Xenia-edge"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.xenia.edge"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_ICON_FILE "AppIcon.icns")

  target_link_options(xenia-app PRIVATE
    "-Wl,-rpath,@executable_path/../Frameworks"
    "-Wl,-rpath,@loader_path/../Frameworks")
  # Linking the imported Metal Shader Converter otherwise bakes its source-tree
  # directory in as a third rpath, shipping a machine-specific path. The two
  # rpaths above are explicit link options, so dropping the generated ones
  # leaves only them.
  set_target_properties(xenia-app PROPERTIES SKIP_BUILD_RPATH TRUE)

  # The Metal Shader Converter dylib has an @rpath install name, so it has to
  # sit in Frameworks for the rpaths above to find it. Copied before the
  # codesign step below, which seals whatever is in the bundle.
  add_custom_command(TARGET xenia-app POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:xenia-app>/Frameworks"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:xenia-third-party-metal-shader-converter>"
            "$<TARGET_BUNDLE_CONTENT_DIR:xenia-app>/Frameworks/"
    VERBATIM)
endif()

xe_target_defaults(xenia-app)

# Sign the bundle last, after every POST_BUILD step that drops files into
# it — otherwise Gatekeeper rejects the downloaded artifact with "a sealed
# resource is missing or invalid".
if(APPLE)
  add_custom_command(TARGET xenia-app POST_BUILD
    COMMAND codesign --force --deep --sign -
            --entitlements "${PROJECT_SOURCE_DIR}/xenia.entitlements"
            "$<TARGET_BUNDLE_DIR:xenia-app>"
    # Strip macos-26 provenance/macl xattrs so AMFI on macOS 15 accepts the seal.
    COMMAND xattr -cr "$<TARGET_BUNDLE_DIR:xenia-app>"
    VERBATIM)
endif()

# Bake icons into the app binary (font lives with xenia-ui).
xe_embed_binary_assets(xenia-app
    "${PROJECT_SOURCE_DIR}/assets/icons" icons)

# Bake game-compat data (compatibility_data.json from xenia-canary/game-compatibility).
xe_embed_compressed_bundle(xenia-app
    "${PROJECT_SOURCE_DIR}/build/data_repos/game-compatibility"
    game_compat)

# Bake the x360db title database (games.json from xenia-manager/x360db).
xe_embed_compressed_bundle(xenia-app
    "${PROJECT_SOURCE_DIR}/build/data_repos/x360db/games.json"
    game_db)

# Bake the original Xbox title database (games.json made from xemu-project/xdb
# by tools/xdb-index.py). Regenerating it re-runs configure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/assets/xdb/games.json")
xe_embed_compressed_bundle(xenia-app
    "${PROJECT_SOURCE_DIR}/assets/xdb/games.json"
    xbox_db)

target_include_directories(xenia-app PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/rapidjson/include")
