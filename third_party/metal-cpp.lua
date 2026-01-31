if not os.istarget("macosx") then
  return
end

group("third_party")
project("metal-cpp")
  uuid("11c84c8d-bfa2-4b14-b832-4e85e9ca8e3b")
  kind("None")  -- Header-only library
  language("C++")
  defines({
    "METAL_CPP_AVAILABLE=1",
  })
  includedirs({
    "metal-cpp",
  })
  files({
    "metal-cpp/Foundation/Foundation.hpp",
    "metal-cpp/Metal/Metal.hpp",
    "metal-cpp/MetalFX/MetalFX.hpp",
    "metal-cpp/QuartzCore/QuartzCore.hpp",
  })
