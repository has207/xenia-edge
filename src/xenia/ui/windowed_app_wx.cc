/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/windowed_app_wx.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <wx/dialog.h>
#include <wx/image.h>
#include <wx/modalhook.h>
#include <wx/msgdlg.h>

#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/console.h"
#include "xenia/base/main_win.h"
#include "xenia/base/platform_win.h"
#endif
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_wx.h"

#if XE_PLATFORM_WIN32
DECLARE_bool(enable_console);
#endif
namespace xe {
namespace ui {

namespace {
// Logs every modal wx dialog as it opens. Unattended, nobody can answer
// it, so it isn't shown, and the caller gets the answer that turns it down.
class ModalDialogLogHook final : public wxModalDialogHook {
 protected:
  int Enter(wxDialog* dialog) override {
    std::string title = dialog->GetTitle().utf8_string();
    if (auto* message_dialog = dynamic_cast<wxMessageDialogBase*>(dialog)) {
      std::string text = message_dialog->GetMessage().utf8_string();
      std::string extended =
          message_dialog->GetExtendedMessage().utf8_string();
      if (!extended.empty()) {
        text += "\n" + extended;
      }
      LogUIElement("message box", title, text);
      if (!xe::IsUnattended()) {
        return wxID_NONE;
      }
      long style = message_dialog->GetMessageDialogStyle();
      int answer = style & wxCANCEL   ? wxID_CANCEL
                   : style & wxYES_NO ? wxID_NO
                                      : wxID_OK;
      XELOGI("Unattended: dismissed message box with {}",
             answer == wxID_CANCEL ? "Cancel"
             : answer == wxID_NO   ? "No"
                                   : "OK");
      return answer;
    }
    LogUIElement("dialog", title, "");
    if (!xe::IsUnattended()) {
      return wxID_NONE;
    }
    XELOGI("Unattended: dismissed dialog with Cancel");
    return wxID_CANCEL;
  }
};
ModalDialogLogHook modal_dialog_log_hook;
}  // namespace

#if !XE_PLATFORM_WIN32
int g_argc = 0;
char** g_argv = nullptr;
#endif

#if XE_PLATFORM_MAC
void SuppressMacAutoMenuItems();
void ActivateMacApp();
#endif

bool XeniaWxApp::OnInit() {
  // Skip wxApp::OnInit() — its default implementation parses argv with a
  // parser that doesn't know our cvars and would reject them. We do our own
  // parsing below, after the WindowedApp is constructed and can supply its
  // positional options.

  app_context_ = std::make_unique<WxWindowedAppContext>();
  app_ = GetWindowedAppCreator()(*app_context_);

#if XE_PLATFORM_MAC
  SuppressMacAutoMenuItems();
#endif

#if XE_PLATFORM_WIN32
  // Use GetCommandLineW so Unicode paths in positional args survive.
  if (!xe::ParseWin32LaunchArguments(false, app_->GetPositionalOptionsUsage(),
                                     app_->GetPositionalOptions(), nullptr)) {
    return false;
  }
  // Before logging starts, so its stdout sink has somewhere to go.
  if (cvars::enable_console || xe::IsUnattended()) {
    xe::AttachConsoleForStdio(cvars::enable_console);
  }
  xe::InitializeWin32App(app_->GetName());
#else
  cvar::ParseLaunchArguments(g_argc, g_argv, app_->GetPositionalOptionsUsage(),
                             app_->GetPositionalOptions());
  xe::InitializeLogging(app_->GetName());
#endif

  wxInitAllImageHandlers();

  modal_dialog_log_hook.Register();

#if XE_PLATFORM_WIN32
  // Opt into UXTheme dark mode when the system is in dark theme.
  MSWEnableDarkMode();
#endif

  // Don't popup dialog boxes for wx warnings (e.g. libpng iCCP profile
  // complaints from our app icon).
  wxLog::SetLogLevel(wxLOG_Error);

  // Reduce wx event loop overhead — only process idle events when explicitly
  // requested, not after every message. Critical for real-time rendering perf.
  wxIdleEvent::SetMode(wxIDLE_PROCESS_SPECIFIED);

  std::string id(app_->GetTitle());
  std::transform(id.begin(), id.end(), id.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  SetAppName(wxString::FromUTF8(id));
  SetVendorName(wxString::FromUTF8(app_->GetName()));

  if (!app_->OnInitialize()) {
    return false;
  }

#if XE_PLATFORM_MAC
  ActivateMacApp();
#endif

  return true;
}

int XeniaWxApp::OnExit() {
  if (app_) {
    app_->InvokeOnDestroy();
    app_.reset();
  }
  app_context_.reset();

#if XE_PLATFORM_WIN32
  xe::ShutdownWin32App();
#else
  xe::ShutdownLogging();
#endif

  return wxApp::OnExit();
}

}  // namespace ui
}  // namespace xe
