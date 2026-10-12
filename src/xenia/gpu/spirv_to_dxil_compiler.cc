/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/spirv_to_dxil_compiler.h"

#include "xenia/base/platform.h"

// Mesa is only built where a backend consumes DXIL: D3D12 on Windows, and the
// Metal Shader Converter path on macOS (see third_party/CMakeLists.txt).
#if XE_PLATFORM_WIN32 || XE_PLATFORM_MAC

#include <cstring>
#include <string>

#include "third_party/dxbc/DXBCChecksum.h"
#include "xenia/base/logging.h"

#if XE_PLATFORM_WIN32
// Windows, wrl/client.h and the DXC validator interfaces (IDxcValidator,
// IDxcVersionInfo) used to sign the converted DXIL.
#include "xenia/base/platform_win.h"

#include <wrl/client.h>
#include <algorithm>
#include <filesystem>
#include <mutex>

#include "third_party/DirectXShaderCompiler/include/dxc/dxcapi.h"
#include "xenia/base/filesystem.h"
#endif

// Mesa C ABI. The header carries its own extern "C" guards.
#include "spirv_to_dxil.h"

namespace xe {
namespace gpu {

namespace {
void SpirvToDxilLog(void* priv, const char* message) {
  XELOGE("spirv_to_dxil: {}", message);
}

dxil_spirv_shader_stage ToDxilStage(SpirvToDxilCompiler::Stage stage_in,
                                    const char** stage_name) {
  using Stage = SpirvToDxilCompiler::Stage;
  switch (stage_in) {
    case Stage::kVertex:
      *stage_name = "vertex";
      return DXIL_SPIRV_SHADER_VERTEX;
    case Stage::kTessellationControl:
      *stage_name = "tessellation control";
      return DXIL_SPIRV_SHADER_TESS_CTRL;
    case Stage::kTessellationEvaluation:
      *stage_name = "tessellation evaluation";
      return DXIL_SPIRV_SHADER_TESS_EVAL;
    case Stage::kGeometry:
      *stage_name = "geometry";
      return DXIL_SPIRV_SHADER_GEOMETRY;
    case Stage::kCompute:
      *stage_name = "compute";
      return DXIL_SPIRV_SHADER_COMPUTE;
    default:
      *stage_name = "pixel";
      return DXIL_SPIRV_SHADER_FRAGMENT;
  }
}

dxil_spirv_runtime_conf MakeRuntimeConf(bool lower_to_bindless,
                                        bool keep_io_vars,
                                        uint32_t input_clip_size) {
  // The guest sets 0..3 map to register spaces 0..3 in the Vulkan environment.
  // Park Dozen's runtime data and push constant CBVs in a high space so they
  // never collide. SpirvShaderTranslator emits no push constants and derives
  // vertex indices from its own constants, so these should stay unused.
  dxil_spirv_runtime_conf conf = {};
  conf.runtime_data_cbv.register_space = 31;
  conf.runtime_data_cbv.base_shader_register = 0;
  conf.push_constant_cbv.register_space =
      SpirvToDxilCompiler::kPushConstantRegisterSpace;
  conf.push_constant_cbv.base_shader_register =
      SpirvToDxilCompiler::kPushConstantShaderRegister;
  conf.first_vertex_and_base_instance_mode = DXIL_SPIRV_SYSVAL_TYPE_ZERO;
  conf.workgroup_id_mode = DXIL_SPIRV_SYSVAL_TYPE_ZERO;
  // No yz_flip. Unlike Dozen (which feeds Vulkan-convention NDC and flips
  // here), the command processor computes the host clip transform with D3D
  // conventions (origin_bottom_left) baked into ndc_scale/ndc_offset, so
  // gl_Position is already in D3D clip space and SV_Position is a direct copy.
  // Flipping would double-invert.
  conf.yz_flip.mode = DXIL_SPIRV_YZ_FLIP_NONE;
  // Strict IEEE float math, matching the DXC -Gis the HLSL path used.
  conf.disable_math_refactoring = true;
  conf.shader_model_max = SHADER_MODEL_6_6;
  conf.lower_to_bindless = lower_to_bindless;
  // SpirvShaderTranslator::kDescriptorSetCount.
  conf.bindless_descriptor_set_count = 4;
  // Independently translated stages (the single-stage Translate path) must keep
  // their declared varyings so the packed VS-output and PS-input signatures
  // line up. Linked stages let them be pruned.
  conf.keep_io_vars = keep_io_vars;
  // Producer clip count, for splitting clip/cull inputs of an isolated stage.
  conf.input_clip_size = input_clip_size;
  return conf;
}

#if XE_PLATFORM_WIN32

// The SPIR-V to DXIL conversion itself is thread safe: Mesa's glsl_type cache
// (its only shared global) locks internally, so conversions run in parallel
// across the main thread and the pipeline creation threads. Only the DXIL.dll
// validator is a shared, non-thread-safe instance, so its creation and the
// signing pass are serialized by this mutex.
std::mutex dxil_validator_mutex;

// DXC validator from DXIL.dll, created lazily and reused. Never destroyed: it
// keeps DXIL.dll loaded for the process lifetime. Guarded by
// dxil_validator_mutex. validator_version is what DXIL.dll stamps. It is passed
// to spirv_to_dxil so the container header matches the signer.
IDxcValidator* dxil_validator_singleton = nullptr;
dxil_validator_version validator_version_value = NO_DXIL_VALIDATION;

// Minimal IDxcBlob over a caller-owned buffer so the validator can sign in
// place without copying. Not reference counted: it lives on the stack for the
// Validate call, which does not retain it.
class InPlaceBlob : public IDxcBlob {
 public:
  InPlaceBlob(void* data, size_t size) : data_(data), size_(size) {}
  LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return data_; }
  SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return size_; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override {
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 0; }

 private:
  void* data_;
  size_t size_;
};

// Returns DXIL.dll's DxcCreateInstance, or null. The provider preloads DXIL.dll
// by full path during setup, so the plain-name load here resolves to that
// module. The D3D12-directory path is a fallback if it has not been loaded yet.
DxcCreateInstanceProc LoadDxilCreateInstance() {
  HMODULE dxil = LoadLibraryW(L"DXIL.dll");
  if (!dxil) {
    std::filesystem::path dxil_path =
        xe::filesystem::GetExecutablePath().parent_path() / "D3D12" /
        "dxil.dll";
    dxil = LoadLibraryW(dxil_path.wstring().c_str());
  }
  if (!dxil) {
    return nullptr;
  }
  return reinterpret_cast<DxcCreateInstanceProc>(
      GetProcAddress(dxil, "DxcCreateInstance"));
}

// Maps the DXIL.dll validator version to the enum spirv_to_dxil stamps into the
// container, mirroring Mesa's dxil_validator. The validator rejects a container
// tagged with a version it does not implement.
dxil_validator_version QueryValidatorVersion(IDxcValidator* validator) {
  Microsoft::WRL::ComPtr<IDxcVersionInfo> version_info;
  if (FAILED(
          validator->QueryInterface(version_info.ReleaseAndGetAddressOf()))) {
    return NO_DXIL_VALIDATION;
  }
  UINT32 major = 0, minor = 0;
  if (FAILED(version_info->GetVersion(&major, &minor))) {
    return NO_DXIL_VALIDATION;
  }
  if (major == 1) {
    return dxil_validator_version(DXIL_VALIDATOR_1_0 + std::min(minor, 8u));
  }
  if (major > 1) {
    return DXIL_VALIDATOR_1_8;
  }
  return NO_DXIL_VALIDATION;
}

// Lazily creates the DXIL.dll validator. dxil_validator_mutex must be held.
bool EnsureValidatorLocked() {
  if (!dxil_validator_singleton) {
    DxcCreateInstanceProc create = LoadDxilCreateInstance();
    if (!create) {
      XELOGE(
          "spirv_to_dxil: could not load DxcCreateInstance from DXIL.dll; "
          "cannot sign DXIL");
      return false;
    }
    IDxcValidator* validator = nullptr;
    if (FAILED(create(CLSID_DxcValidator, IID_PPV_ARGS(&validator))) ||
        !validator) {
      XELOGE(
          "spirv_to_dxil: could not create the DXIL validator (is DXIL.dll "
          "present in D3D12/?); cannot sign DXIL");
      return false;
    }
    dxil_validator_singleton = validator;
    validator_version_value = QueryValidatorVersion(validator);
  }
  return true;
}

// Briefly locks to ensure the validator exists and read its version (the only
// validator access the conversion needs). Returns false if it cannot be
// created. The conversion that follows runs unlocked.
bool AcquireValidatorVersion(enum dxil_validator_version* out_version) {
  std::lock_guard<std::mutex> lock(dxil_validator_mutex);
  if (!EnsureValidatorLocked()) {
    return false;
  }
  *out_version = validator_version_value;
  return true;
}

// Signs a DXIL container in place. spirv_to_dxil emits UNSIGNED DXIL (it only
// records the validator version in the header). D3D12 rejects unsigned DXIL
// with E_INVALIDARG, so the DXIL.dll validator must rewrite the container hash,
// the same way DXC signs its own output.
bool SignDxil(std::vector<uint8_t>& dxil, const char* stage) {
  // The shared validator is not thread safe.
  std::lock_guard<std::mutex> lock(dxil_validator_mutex);
  InPlaceBlob blob(dxil.data(), dxil.size());
  Microsoft::WRL::ComPtr<IDxcOperationResult> result;
  HRESULT hr = dxil_validator_singleton->Validate(
      &blob, DxcValidatorFlags_InPlaceEdit, &result);
  if (SUCCEEDED(hr) && result) {
    result->GetStatus(&hr);
  }
  if (FAILED(hr)) {
    std::string message;
    Microsoft::WRL::ComPtr<IDxcBlobEncoding> error_blob;
    if (result && SUCCEEDED(result->GetErrorBuffer(&error_blob)) &&
        error_blob && error_blob->GetBufferSize()) {
      message.assign(
          reinterpret_cast<const char*>(error_blob->GetBufferPointer()),
          error_blob->GetBufferSize());
    }
    XELOGE("spirv_to_dxil: DXIL validation/signing failed for {} shader: {}",
           stage, message.empty() ? "(no message)" : message.c_str());
    return false;
  }
  return true;
}

#else

// Offset of the 16-byte container digest, right after the DXBC magic.
constexpr size_t kDxilContainerHashOffset = 4;

// DXIL.dll is Windows only, so there is no validator to stamp a version from.
// Mesa falls back to the newest version it can emit.
bool AcquireValidatorVersion(enum dxil_validator_version* out_version) {
  *out_version = NO_DXIL_VALIDATION;
  return true;
}

// Stamps the container digest that spirv_to_dxil leaves zeroed. This is the
// same MD5 variant over everything past the digest that DXC's validator writes
// as its retail hash, and that Xenia's DXBC translator already uses for its own
// containers.
bool SignDxil(std::vector<uint8_t>& dxil, const char* stage) {
  if (dxil.size() <= kDxilContainerHashOffset + sizeof(uint32_t) * 4) {
    XELOGE("spirv_to_dxil: {} shader container is too small to sign", stage);
    return false;
  }
  unsigned int hash[4];
  CalculateDXBCChecksum(dxil.data(), static_cast<unsigned int>(dxil.size()),
                        hash);
  std::memcpy(dxil.data() + kDxilContainerHashOffset, hash, sizeof(hash));
  return true;
}

#endif  // XE_PLATFORM_WIN32
}  // namespace

uint64_t SpirvToDxilCompiler::version() { return spirv_to_dxil_get_version(); }

bool SpirvToDxilCompiler::IsSignerAvailable() {
  enum dxil_validator_version validator_version;
  return AcquireValidatorVersion(&validator_version);
}

std::vector<uint8_t> SpirvToDxilCompiler::Translate(const uint32_t* spirv_words,
                                                    size_t spirv_word_count,
                                                    Stage stage_in,
                                                    bool lower_to_bindless,
                                                    uint32_t input_clip_size) {
  const char* stage_name;
  dxil_spirv_shader_stage stage = ToDxilStage(stage_in, &stage_name);
  // A single stage compiled on its own: keep unused varyings so its signature
  // matches the separately compiled neighbor stage.
  dxil_spirv_runtime_conf conf = MakeRuntimeConf(
      lower_to_bindless, /*keep_io_vars=*/true, input_clip_size);

  dxil_spirv_debug_options debug_options = {};
  dxil_spirv_logger logger = {};
  logger.log = SpirvToDxilLog;

  enum dxil_validator_version validator_version;
  if (!AcquireValidatorVersion(&validator_version)) {
    return {};
  }

  dxil_spirv_object object = {};
  if (!spirv_to_dxil(spirv_words, spirv_word_count, nullptr, 0, stage, "main",
                     validator_version, &debug_options, &conf, &logger,
                     &object)) {
    XELOGE("spirv_to_dxil: translation failed for {} shader", stage_name);
    return {};
  }

  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(object.binary.buffer);
  std::vector<uint8_t> dxil(bytes, bytes + object.binary.size);
  spirv_to_dxil_free(&object);

  if (!SignDxil(dxil, stage_name)) {
    return {};
  }
  return dxil;
}

std::vector<std::vector<uint8_t>> SpirvToDxilCompiler::TranslateLinked(
    const std::vector<LinkedStage>& stages, bool lower_to_bindless,
    uint32_t tess_input_control_point_count) {
  if (stages.empty()) {
    return {};
  }

  std::vector<dxil_spirv_link_stage> link_stages(stages.size());
  std::vector<const char*> stage_names(stages.size());
  for (size_t i = 0; i < stages.size(); ++i) {
    link_stages[i].words = stages[i].spirv_words;
    link_stages[i].word_count = stages[i].spirv_word_count;
    link_stages[i].stage = ToDxilStage(stages[i].stage, &stage_names[i]);
    link_stages[i].entry_point_name = "main";
  }

  // Linking reconciles the inter-stage signatures, so dead varyings may be
  // pruned, and the clip/cull split comes from the linked producer.
  dxil_spirv_runtime_conf conf =
      MakeRuntimeConf(lower_to_bindless, /*keep_io_vars=*/false,
                      /*input_clip_size=*/0);
  conf.tess_input_control_point_count = tess_input_control_point_count;
  dxil_spirv_debug_options debug_options = {};
  dxil_spirv_logger logger = {};
  logger.log = SpirvToDxilLog;

  enum dxil_validator_version validator_version;
  if (!AcquireValidatorVersion(&validator_version)) {
    return {};
  }

  std::vector<dxil_spirv_object> objects(stages.size());
  if (!spirv_to_dxil_link(link_stages.data(), link_stages.size(),
                          validator_version, &debug_options, &conf, &logger,
                          objects.data())) {
    XELOGE("spirv_to_dxil: linked translation failed ({} stages)",
           stages.size());
    return {};
  }

  // Copy out and free every Mesa object before signing.
  std::vector<std::vector<uint8_t>> result(stages.size());
  for (size_t i = 0; i < stages.size(); ++i) {
    const uint8_t* bytes =
        reinterpret_cast<const uint8_t*>(objects[i].binary.buffer);
    result[i].assign(bytes, bytes + objects[i].binary.size);
    spirv_to_dxil_free(&objects[i]);
  }
  for (size_t i = 0; i < stages.size(); ++i) {
    if (!SignDxil(result[i], stage_names[i])) {
      return {};
    }
  }
  return result;
}

}  // namespace gpu
}  // namespace xe

#endif  // XE_PLATFORM_WIN32 || XE_PLATFORM_MAC
