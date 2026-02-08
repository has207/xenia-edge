if not os.istarget("macosx") then
  return
end

group("third_party")
project("metal-shader-converter")
  uuid("b1b2c3d4-e5f6-7890-abcd-ef1234567890")
  kind("StaticLib")
  language("C++")
  defines({
    "_LIB",
  })
  includedirs({
    "metal-shader-converter/include",
  })
  files({
    "metal-shader-converter/include/metal_irconverter.h",
    "metal-shader-converter/include/metal_irconverter_runtime.h",
  })
  -- Link against the Metal Shader Converter dynamic library
  libdirs({
    "metal-shader-converter/lib",
  })
  links({
    "metalirconverter",
  })
  links({
    "Metal.framework",
    "MetalKit.framework",
  })
