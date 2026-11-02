/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/redist_installer_wx.h"

#include "xenia/base/cvar.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// wx headers manage their own windows.h inclusion; include them before it.
#include <wx/intl.h>
#include <wx/mstream.h>
#include <wx/string.h>
#include <wx/zipstrm.h>

#include "xenia/base/platform_win.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <softpub.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <wintrust.h>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/config.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wintrust.lib")

#endif  // XE_PLATFORM_WIN32

DEFINE_bool(
    d3d12_install_missing_runtime, true,
    "When a required Direct3D 12 runtime DLL (the DXIL validator, the Agility "
    "SDK, or the debug layer) is missing, offer to download it from "
    "Microsoft. Disable to manage the DLLs manually.",
    "D3D12");

DEFINE_bool(
    update_vc_runtime, true,
    "When the Microsoft Visual C++ runtime is missing or outdated, offer to "
    "download it from Microsoft. Disabled when you decline; set false to "
    "manage "
    "it manually.",
    "Win32");

DEFINE_bool(
    vulkan_install_missing_loader, true,
    "When the Vulkan loader (vulkan-1.dll) is missing, offer to download it "
    "from LunarG. Disable to manage it manually.",
    "Vulkan");

namespace xe {
namespace ui {

#if XE_PLATFORM_WIN32

namespace {

// Pinned DirectX Shader Compiler redistributable. Only dxil.dll is taken from
// it, the signer for the DXIL Mesa emits. Update all of these together when
// bumping: the SHA-256 is the GitHub release asset's "digest" field.
constexpr wchar_t kDxcUrl[] =
    L"https://github.com/microsoft/DirectXShaderCompiler/releases/download/"
    L"v1.9.2602.24/dxc_2026_05_27.zip";
constexpr char kDxcSha256[] =
    "cf658aacf070d3045e31b8f1f8a696c2945f37c1095019481ef7c513368db3b4";

// Pinned DirectX 12 Agility SDK redistributable (a NuGet .nupkg = zip). Version
// must match the D3D12SDKVersion export in windowed_app_main_win (1.619.3 ->
// 619).
constexpr wchar_t kAgilityUrl[] =
    L"https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.619.3";
constexpr char kAgilitySha256[] =
    "43a7d5a3973812eb4b42623fae5275c790a005b8e48b8d7f5bb43cef39e073c5";

// Pinned LunarG Vulkan runtime components (a zip). vulkan-1.dll is the Khronos
// Vulkan-Loader (Apache-2.0, redistributable). Update the SHA-256 and the
// kVulkanLoaderEntries zip path together when bumping the URL version. The zip
// ships only x64 and x86 loaders, no arm64.
#if !XE_ARCH_ARM64
constexpr wchar_t kVulkanLoaderUrl[] =
    L"https://sdk.lunarg.com/sdk/download/1.4.350.0/windows/"
    L"vulkan-runtime-components.zip";
constexpr char kVulkanLoaderSha256[] =
    "23ce69f32cef3e2799617e2b1776cd0c71030d23a91f8375821cc40d76b185b9";
#endif

// Both archives ship per-arch binaries. Pick the one matching the build target.
#if XE_ARCH_ARM64
#define XE_D3D12_REDIST_ARCH "arm64"
#else
#define XE_D3D12_REDIST_ARCH "x64"
#endif

// Entry inside the archive -> destination filename in the D3D12 folder.
struct RedistEntry {
  const char* zip_path;
  const wchar_t* dest_name;
};
constexpr RedistEntry kDxcEntries[] = {
    {"bin/" XE_D3D12_REDIST_ARCH "/dxil.dll", L"dxil.dll"},
};
constexpr RedistEntry kAgilityEntries[] = {
    {"build/native/bin/" XE_D3D12_REDIST_ARCH "/D3D12Core.dll",
     L"D3D12Core.dll"},
};
// The debug layer (D3D12SDKLayers.dll) loads only when the Agility runtime is
// active, so it ships alongside D3D12Core.dll.
constexpr RedistEntry kDebugLayerEntries[] = {
    {"build/native/bin/" XE_D3D12_REDIST_ARCH "/D3D12Core.dll",
     L"D3D12Core.dll"},
    {"build/native/bin/" XE_D3D12_REDIST_ARCH "/d3d12SDKLayers.dll",
     L"D3D12SDKLayers.dll"},
};

#undef XE_D3D12_REDIST_ARCH

#if !XE_ARCH_ARM64
constexpr RedistEntry kVulkanLoaderEntries[] = {
    {"VulkanRT-X64-1.4.350.0-Components/x64/vulkan-1.dll", L"vulkan-1.dll"},
};
#endif

// Microsoft Visual C++ 2015-2022 redistributable. The aka.ms link always
// resolves to the latest build, so there's no stable SHA-256 to pin; the
// download is validated by its Microsoft Authenticode signature instead.
#if XE_ARCH_ARM64
constexpr wchar_t kVCRedistUrl[] =
    L"https://aka.ms/vs/17/release/vc_redist.arm64.exe";
constexpr wchar_t kVCRuntimeRegKey[] =
    L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\arm64";
#else
constexpr wchar_t kVCRedistUrl[] =
    L"https://aka.ms/vs/17/release/vc_redist.x64.exe";
constexpr wchar_t kVCRuntimeRegKey[] =
    L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64";
#endif

// The required runtime is what this binary was built against: the VC runtime
// ships versioned as 14.<_MSC_VER - 1900>, and an older-but-loadable one can
// fault deep in the CRT. But a preview/insider toolset can be newer than any
// redist Microsoft publishes on the release channel we download from, so clamp
// to the newest installable one: otherwise the check demands a runtime that
// doesn't exist yet and prompts on every launch. Bump kVCRuntimeLatestMinor
// when a newer vc_redist ships. Compare Major.Minor only; servicing builds
// within a minor add no exports.
constexpr DWORD kVCRuntimeMinMajor = 14;
constexpr DWORD kVCRuntimeBuiltMinor = DWORD(_MSC_VER - 1900);
constexpr DWORD kVCRuntimeLatestMinor = 44;  // 14.44.x, newest public vc_redist
constexpr DWORD kVCRuntimeMinMinor =
    kVCRuntimeBuiltMinor < kVCRuntimeLatestMinor ? kVCRuntimeBuiltMinor
                                                 : kVCRuntimeLatestMinor;

// True if dxil.dll resolves, either from the D3D12 folder or anywhere on the
// default DLL search path (system-wide or next to the exe).
bool ShaderCompilerPresent(const std::filesystem::path& d3d12_dir) {
  std::error_code ec;
  if (std::filesystem::exists(d3d12_dir / "dxil.dll", ec)) {
    return true;
  }
  HMODULE dxil = LoadLibraryW(L"dxil.dll");
  if (!dxil) {
    return false;
  }
  FreeLibrary(dxil);
  return true;
}

#if !XE_ARCH_ARM64
// True if vulkan-1.dll resolves, either from vulkan_dir or anywhere on the
// default DLL search path (system-wide or next to the exe).
bool VulkanLoaderPresent(const std::filesystem::path& vulkan_dir) {
  std::error_code ec;
  if (std::filesystem::exists(vulkan_dir / "vulkan-1.dll", ec)) {
    return true;
  }
  HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
  if (loader) {
    FreeLibrary(loader);
    return true;
  }
  return false;
}
#endif

// Native MessageBox (not wxMessageBox) since the provider may initialize off
// the GUI thread; only the translated string comes from wx.
bool AskYesNo(const wxString& message) {
  return MessageBoxW(nullptr, message.wc_str(), L"Xenia",
                     MB_YESNO | MB_ICONWARNING | MB_TASKMODAL) == IDYES;
}

void ShowError(const wxString& message) {
  MessageBoxW(nullptr, message.wc_str(), L"Xenia",
              MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

// Persists the user's "no" so the prompt doesn't reappear after a restart.
void RememberDecline() {
  OVERRIDE_PERSIST_bool(d3d12_install_missing_runtime, false);
  config::SaveConfig();
}

// Same, but for the Visual C++ runtime's own cvar, so declining it doesn't
// disable the D3D12 DLL prompts (and vice versa).
void RememberVCDecline() {
  OVERRIDE_PERSIST_bool(update_vc_runtime, false);
  config::SaveConfig();
}

#if !XE_ARCH_ARM64
// Same, but for the Vulkan loader's own cvar.
void RememberVulkanDecline() {
  OVERRIDE_PERSIST_bool(vulkan_install_missing_loader, false);
  config::SaveConfig();
}
#endif

bool DownloadToMemory(const wchar_t* url, std::vector<uint8_t>& out) {
  out.clear();

  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  wchar_t host[256] = {};
  wchar_t path[1024] = {};
  components.lpszHostName = host;
  components.dwHostNameLength = DWORD(xe::countof(host));
  components.lpszUrlPath = path;
  components.dwUrlPathLength = DWORD(xe::countof(path));
  if (!WinHttpCrackUrl(url, 0, 0, &components)) {
    XELOGE("Download: failed to parse URL (error {})", GetLastError());
    return false;
  }

  HINTERNET session =
      WinHttpOpen(L"xenia-edge", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    XELOGE("Download: WinHttpOpen failed (error {})", GetLastError());
    return false;
  }
  WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);

  bool success = false;
  HINTERNET connect = WinHttpConnect(session, host, components.nPort, 0);
  if (connect) {
    DWORD flags =
        components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request =
        WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (request) {
      if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(
            request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
            WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
          success = true;
          for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
              XELOGE("Download: WinHttpQueryDataAvailable failed (error {})",
                     GetLastError());
              success = false;
              break;
            }
            if (!available) {
              break;
            }
            size_t offset = out.size();
            out.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, out.data() + offset, available,
                                 &read)) {
              XELOGE("Download: WinHttpReadData failed (error {})",
                     GetLastError());
              success = false;
              break;
            }
            out.resize(offset + read);
            if (!read) {
              break;
            }
          }
        } else {
          XELOGE("Download: HTTP status {}", status);
        }
      } else {
        XELOGE("Download: request failed (error {})", GetLastError());
      }
      WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connect);
  } else {
    XELOGE("Download: WinHttpConnect failed (error {})", GetLastError());
  }
  WinHttpCloseHandle(session);

  if (!success) {
    out.clear();
  }
  return success;
}

bool VerifySha256(const std::vector<uint8_t>& data, const char* expected_hex) {
  uint8_t digest[32];
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) !=
      0) {
    return false;
  }
  bool hashed = false;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
    hashed = BCryptHashData(hash, const_cast<PUCHAR>(data.data()),
                            ULONG(data.size()), 0) == 0 &&
             BCryptFinishHash(hash, digest, ULONG(sizeof(digest)), 0) == 0;
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  if (!hashed) {
    return false;
  }

  // Both this and the pinned constant are lowercase hex, so a plain compare is
  // enough.
  char hex[sizeof(digest) * 2 + 1];
  static const char kHexDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(digest); ++i) {
    hex[i * 2] = kHexDigits[digest[i] >> 4];
    hex[i * 2 + 1] = kHexDigits[digest[i] & 0xF];
  }
  hex[sizeof(digest) * 2] = '\0';
  return std::strcmp(hex, expected_hex) == 0;
}

// The installed Visual C++ runtime as recorded by the redist bootstrapper, plus
// whether it meets the version Xenia needs. The caller surfaces the numbers in
// the prompt so the user sees exactly what's outdated.
struct VCRuntimeStatus {
  bool installed = false;
  DWORD major = 0;
  DWORD minor = 0;
  DWORD bld = 0;
  bool up_to_date = false;
};

VCRuntimeStatus QueryVCRuntime() {
  VCRuntimeStatus status;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVCRuntimeRegKey, 0,
                    KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return status;
  }
  auto read_dword = [&](const wchar_t* name) -> DWORD {
    DWORD value = 0;
    DWORD type = 0;
    DWORD size = sizeof(value);
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value),
                         &size) != ERROR_SUCCESS ||
        type != REG_DWORD) {
      return 0;
    }
    return value;
  };
  status.installed = read_dword(L"Installed") == 1;
  status.major = read_dword(L"Major");
  status.minor = read_dword(L"Minor");
  status.bld = read_dword(L"Bld");
  RegCloseKey(key);

  status.up_to_date =
      status.installed && (status.major > kVCRuntimeMinMajor ||
                           (status.major == kVCRuntimeMinMajor &&
                            status.minor >= kVCRuntimeMinMinor));
  return status;
}

// Verifies the file carries a valid Authenticode signature chaining to a
// trusted root AND that the signer is Microsoft. With no SHA-256 to pin for the
// rolling aka.ms URL, this is what guards against running a tampered binary.
bool VerifyMicrosoftSignature(const std::wstring& path) {
  WINTRUST_FILE_INFO file_info = {};
  file_info.cbStruct = sizeof(file_info);
  file_info.pcwszFilePath = path.c_str();

  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  WINTRUST_DATA trust_data = {};
  trust_data.cbStruct = sizeof(trust_data);
  trust_data.dwUIChoice = WTD_UI_NONE;
  trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
  trust_data.dwUnionChoice = WTD_CHOICE_FILE;
  trust_data.pFile = &file_info;
  trust_data.dwStateAction = WTD_STATEACTION_VERIFY;
  LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action,
                               &trust_data);
  trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust_data);
  if (status != ERROR_SUCCESS) {
    XELOGE("VC++ runtime: Authenticode verification failed (status 0x{:08X})",
           static_cast<uint32_t>(status));
    return false;
  }

  // A valid chain isn't enough; confirm the signer is Microsoft.
  HCERTSTORE store = nullptr;
  HCRYPTMSG message = nullptr;
  if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
                        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                        CERT_QUERY_FORMAT_FLAG_BINARY, 0, nullptr, nullptr,
                        nullptr, &store, &message, nullptr)) {
    return false;
  }
  bool is_microsoft = false;
  DWORD signer_size = 0;
  if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr,
                       &signer_size)) {
    std::vector<uint8_t> signer_buffer(signer_size);
    if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0,
                         signer_buffer.data(), &signer_size)) {
      auto* signer = reinterpret_cast<CMSG_SIGNER_INFO*>(signer_buffer.data());
      CERT_INFO cert_info = {};
      cert_info.Issuer = signer->Issuer;
      cert_info.SerialNumber = signer->SerialNumber;
      PCCERT_CONTEXT cert = CertFindCertificateInStore(
          store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
          CERT_FIND_SUBJECT_CERT, &cert_info, nullptr);
      if (cert) {
        wchar_t name[256];
        DWORD name_length =
            CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                               name, DWORD(xe::countof(name)));
        if (name_length > 1) {
          is_microsoft = wcsstr(name, L"Microsoft Corporation") != nullptr;
        }
        CertFreeCertificateContext(cert);
      }
    }
  }
  if (message) {
    CryptMsgClose(message);
  }
  if (store) {
    CertCloseStore(store, 0);
  }
  if (!is_microsoft) {
    XELOGE("VC++ runtime: downloaded installer is not signed by Microsoft");
  }
  return is_microsoft;
}

bool ExtractEntries(const std::vector<uint8_t>& zip_data,
                    const RedistEntry* entries, size_t entry_count,
                    const std::filesystem::path& out_dir) {
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);

  // Skip entries already on disk. They're immutable, and a present one may be
  // loaded and locked (e.g. D3D12Core.dll once Agility is active).
  std::vector<bool> wanted(entry_count);
  size_t remaining = 0;
  for (size_t i = 0; i < entry_count; ++i) {
    wanted[i] = !std::filesystem::exists(out_dir / entries[i].dest_name, ec);
    remaining += wanted[i] ? 1 : 0;
  }
  if (remaining == 0) {
    return true;
  }

  wxMemoryInputStream mem(zip_data.data(), zip_data.size());
  wxZipInputStream zip(mem);
  if (!zip.IsOk()) {
    XELOGE("DLL install: failed to open the downloaded archive");
    return false;
  }
  for (std::unique_ptr<wxZipEntry> entry(zip.GetNextEntry()); entry;
       entry.reset(zip.GetNextEntry())) {
    std::string name(entry->GetInternalName().utf8_str().data());
    for (size_t i = 0; i < entry_count; ++i) {
      if (!wanted[i] || name != entries[i].zip_path) {
        continue;
      }
      std::vector<uint8_t> buffer(static_cast<size_t>(entry->GetSize()));
      zip.Read(buffer.data(), buffer.size());
      auto dest_path = out_dir / entries[i].dest_name;
      if (static_cast<size_t>(zip.LastRead()) != buffer.size() ||
          !xe::filesystem::WriteAllBytes(dest_path, buffer)) {
        XELOGE("DLL install: failed to extract {}", entries[i].zip_path);
        return false;
      }
      wanted[i] = false;
      if (--remaining == 0) {
        return true;
      }
      break;
    }
  }

  XELOGE("DLL install: archive is missing an expected entry");
  return false;
}

// Launches a fresh copy of this executable with the same command line and
// exits. Returns only if the relaunch couldn't be started.
void RelaunchAndExit() {
  wchar_t exe_path[MAX_PATH];
  DWORD length =
      GetModuleFileNameW(nullptr, exe_path, DWORD(xe::countof(exe_path)));
  if (length == 0 || length >= xe::countof(exe_path)) {
    return;
  }
  // Continues this session's log rather than truncating it.
  std::wstring command_line = GetCommandLineW();
  command_line += L" --log_append=true";
  xe::FlushLog();
  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info = {};
  if (CreateProcessW(exe_path, command_line.data(), nullptr, nullptr, FALSE, 0,
                     nullptr, nullptr, &startup_info, &process_info)) {
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    ExitProcess(0);
  }
}

// Downloads `url`, verifies it against the expected SHA-256, and extracts the
// given entries into out_dir. Shows an error dialog and returns false on any
// failure.
bool DownloadAndExtract(const wchar_t* url, const char* sha256_hex,
                        const RedistEntry* entries, size_t entry_count,
                        const std::filesystem::path& out_dir) {
  std::vector<uint8_t> package;
  if (!DownloadToMemory(url, package)) {
    ShowError(
        _("Download failed. Check your internet connection and try again."));
    return false;
  }
  if (!VerifySha256(package, sha256_hex)) {
    XELOGE("Downloaded package failed SHA-256 verification, discarding");
    ShowError(
        _("The downloaded file failed integrity verification and was "
          "discarded."));
    return false;
  }
  if (!ExtractEntries(package, entries, entry_count, out_dir)) {
    ShowError(_("Failed to install the downloaded files."));
    return false;
  }
  return true;
}

}  // namespace

bool EnsureShaderCompilerRuntime(const std::filesystem::path& d3d12_dir) {
  if (ShaderCompilerPresent(d3d12_dir)) {
    return true;
  }
  if (!cvars::d3d12_install_missing_runtime) {
    XELOGW(
        "The DXIL validator (dxil.dll) is missing and auto-install is disabled "
        "(d3d12_install_missing_runtime=false)");
    return false;
  }

  // Prompt once per process. Don't re-ask on every provider re-init.
  static bool prompted = false;
  if (prompted) {
    return false;
  }
  prompted = true;

  if (!AskYesNo(_("dxil.dll not found, required for Direct3D 12 shaders. "
                  "Download now (~26 MB)?"))) {
    XELOGW("User declined the DXIL validator download");
    RememberDecline();
    return false;
  }

  XELOGI("Downloading the DXIL validator from Microsoft...");
  if (!DownloadAndExtract(kDxcUrl, kDxcSha256, kDxcEntries,
                          xe::countof(kDxcEntries), d3d12_dir)) {
    return false;
  }

  XELOGI("DXIL validator installed to {}",
         xe::strip_pii(xe::path_to_utf8(d3d12_dir)));
  return true;
}

bool EnsureAgilityRuntime(const std::filesystem::path& d3d12_dir) {
  std::error_code ec;
  if (std::filesystem::exists(d3d12_dir / "D3D12Core.dll", ec)) {
    // Already downloaded, yet the runtime still didn't report Shader Model 6.6.
    // Don't loop on download + restart.
    XELOGE(
        "D3D12Core.dll is present but the Agility SDK runtime could not be "
        "enabled");
    return false;
  }
  if (!cvars::d3d12_install_missing_runtime) {
    XELOGW(
        "In-box Direct3D 12 runtime lacks Shader Model 6.6 and auto-install is "
        "disabled (d3d12_install_missing_runtime=false)");
    return false;
  }

  static bool prompted = false;
  if (prompted) {
    return false;
  }
  prompted = true;

  if (!AskYesNo(
          _("D3D12Core.dll not found, required for Shader Model 6.6. Download "
            "now (~35 MB) and restart?"))) {
    XELOGW("User declined the DirectX 12 Agility SDK runtime download");
    RememberDecline();
    return false;
  }

  XELOGI("Downloading the DirectX 12 Agility SDK runtime from Microsoft...");
  if (!DownloadAndExtract(kAgilityUrl, kAgilitySha256, kAgilityEntries,
                          xe::countof(kAgilityEntries), d3d12_dir)) {
    return false;
  }

  XELOGI("DirectX 12 Agility SDK runtime installed to {}; restarting",
         xe::strip_pii(xe::path_to_utf8(d3d12_dir)));
  RelaunchAndExit();
  // Only reached if the relaunch couldn't be started.
  ShowError(
      _("Installed the DirectX 12 Agility SDK runtime, but couldn't "
        "restart automatically. Please restart Xenia."));
  return false;
}

bool EnsureDebugLayer(const std::filesystem::path& d3d12_dir) {
  std::error_code ec;
  if (std::filesystem::exists(d3d12_dir / "D3D12Core.dll", ec) &&
      std::filesystem::exists(d3d12_dir / "D3D12SDKLayers.dll", ec)) {
    // Both present yet the debug layer still couldn't be enabled. Don't loop.
    XELOGE(
        "The Agility debug layer DLLs are present but the Direct3D 12 debug "
        "layer could not be enabled");
    return false;
  }
  if (!cvars::d3d12_install_missing_runtime) {
    XELOGW(
        "Direct3D 12 debug layer is unavailable and auto-install is disabled "
        "(d3d12_install_missing_runtime=false)");
    return false;
  }

  static bool prompted = false;
  if (prompted) {
    return false;
  }
  prompted = true;

  if (!AskYesNo(
          _("D3D12SDKLayers.dll not found, required for the Direct3D 12 debug "
            "layer. Download now (~35 MB) and restart?"))) {
    XELOGW("User declined the Direct3D 12 debug layer download");
    RememberDecline();
    return false;
  }

  XELOGI("Downloading the DirectX 12 debug layer from Microsoft...");
  if (!DownloadAndExtract(kAgilityUrl, kAgilitySha256, kDebugLayerEntries,
                          xe::countof(kDebugLayerEntries), d3d12_dir)) {
    return false;
  }

  XELOGI("DirectX 12 debug layer installed to {}; restarting",
         xe::strip_pii(xe::path_to_utf8(d3d12_dir)));
  RelaunchAndExit();
  // Only reached if the relaunch couldn't be started.
  ShowError(
      _("Installed the Direct3D 12 debug layer, but couldn't restart "
        "automatically. Please restart Xenia."));
  return false;
}

#if XE_ARCH_ARM64
// LunarG's runtime components ship only x64/x86 loaders, not arm64.
bool EnsureVulkanLoader(const std::filesystem::path&) { return false; }
#else
bool EnsureVulkanLoader(const std::filesystem::path& vulkan_dir) {
  if (VulkanLoaderPresent(vulkan_dir)) {
    return true;
  }
  if (!cvars::vulkan_install_missing_loader) {
    XELOGW(
        "Vulkan loader (vulkan-1.dll) is missing and auto-install is disabled "
        "(vulkan_install_missing_loader=false)");
    return false;
  }

  static bool prompted = false;
  if (prompted) {
    return false;
  }
  prompted = true;

  if (!AskYesNo(
          _("vulkan-1.dll not found, required for the Vulkan graphics backend. "
            "Download now (~20 MB)?"))) {
    XELOGW("User declined the Vulkan loader download");
    RememberVulkanDecline();
    return false;
  }

  XELOGI("Downloading the Vulkan loader from LunarG...");
  if (!DownloadAndExtract(kVulkanLoaderUrl, kVulkanLoaderSha256,
                          kVulkanLoaderEntries,
                          xe::countof(kVulkanLoaderEntries), vulkan_dir)) {
    return false;
  }

  XELOGI("Vulkan loader installed to {}",
         xe::strip_pii(xe::path_to_utf8(vulkan_dir)));
  return true;
}
#endif

bool EnsureVCRuntime() {
  VCRuntimeStatus status = QueryVCRuntime();
  if (status.up_to_date) {
    return true;
  }
  if (!cvars::update_vc_runtime) {
    return false;
  }

  static bool prompted = false;
  if (prompted) {
    return false;
  }
  prompted = true;

  // Put the detected version in the prompt itself, so the user sees exactly
  // what's outdated rather than just "out of date".
  wxString message =
      status.installed
          ? wxString::Format(
                _("The installed Microsoft Visual C++ runtime %u.%u.%u is "
                  "older "
                  "than the %u.%u this build of Xenia requires. Download the "
                  "latest from Microsoft now (~25 MB) and install?"),
                unsigned(status.major), unsigned(status.minor),
                unsigned(status.bld), unsigned(kVCRuntimeMinMajor),
                unsigned(kVCRuntimeMinMinor))
          : wxString::Format(
                _("The Microsoft Visual C++ runtime (%u.%u or newer) isn't "
                  "installed. Download it from Microsoft now (~25 MB) and "
                  "install?"),
                unsigned(kVCRuntimeMinMajor), unsigned(kVCRuntimeMinMinor));
  if (!AskYesNo(message)) {
    RememberVCDecline();
    return false;
  }

  XELOGI("Downloading the Microsoft Visual C++ runtime from Microsoft...");
  std::vector<uint8_t> installer;
  if (!DownloadToMemory(kVCRedistUrl, installer)) {
    ShowError(
        _("Download failed. Check your internet connection and try again."));
    return false;
  }

  wchar_t temp_dir[MAX_PATH];
  DWORD temp_length = GetTempPathW(DWORD(xe::countof(temp_dir)), temp_dir);
  if (temp_length == 0 || temp_length >= xe::countof(temp_dir)) {
    ShowError(_("Failed to install the downloaded files."));
    return false;
  }
  std::filesystem::path installer_path =
      std::filesystem::path(temp_dir) / L"xenia_vc_redist.exe";
  if (!xe::filesystem::WriteAllBytes(installer_path, installer)) {
    ShowError(_("Failed to install the downloaded files."));
    return false;
  }

  std::wstring installer_path_w = installer_path.wstring();
  if (!VerifyMicrosoftSignature(installer_path_w)) {
    DeleteFileW(installer_path_w.c_str());
    ShowError(
        _("The downloaded file failed integrity verification and was "
          "discarded."));
    return false;
  }

  // The redist bootstrapper self-elevates; "runas" surfaces the UAC prompt up
  // front. /passive shows progress without interaction, /norestart leaves the
  // reboot decision to us.
  SHELLEXECUTEINFOW execute_info = {};
  execute_info.cbSize = sizeof(execute_info);
  execute_info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  execute_info.lpVerb = L"runas";
  execute_info.lpFile = installer_path_w.c_str();
  execute_info.lpParameters = L"/install /passive /norestart";
  execute_info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&execute_info) || !execute_info.hProcess) {
    DWORD error = GetLastError();
    DeleteFileW(installer_path_w.c_str());
    if (error == ERROR_CANCELLED) {
      XELOGW("User cancelled the Microsoft Visual C++ runtime installer");
      return false;
    }
    ShowError(_("Failed to install the downloaded files."));
    return false;
  }
  WaitForSingleObject(execute_info.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(execute_info.hProcess, &exit_code);
  CloseHandle(execute_info.hProcess);
  DeleteFileW(installer_path_w.c_str());

  // 0 installed, 3010 installed (reboot pending), 1638 same or newer already
  // present. Anything else is a failure.
  if (exit_code != 0 && exit_code != 3010 && exit_code != 1638) {
    XELOGE("Microsoft Visual C++ runtime installer failed (exit code {})",
           exit_code);
    ShowError(_("Failed to install the downloaded files."));
    return false;
  }

  XELOGI("Microsoft Visual C++ runtime installed; restarting");
  RelaunchAndExit();
  // Only reached if the relaunch couldn't be started.
  ShowError(
      _("Installed the Microsoft Visual C++ runtime, but couldn't restart "
        "automatically. Please restart Xenia."));
  return false;
}

#else

bool EnsureShaderCompilerRuntime(const std::filesystem::path&) { return false; }
bool EnsureAgilityRuntime(const std::filesystem::path&) { return false; }
bool EnsureDebugLayer(const std::filesystem::path&) { return false; }
bool EnsureVulkanLoader(const std::filesystem::path&) { return false; }
bool EnsureVCRuntime() { return false; }

#endif  // XE_PLATFORM_WIN32

}  // namespace ui
}  // namespace xe
