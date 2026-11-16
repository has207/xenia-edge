/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/title_update_dialog_wx.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <string>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/translation.h>

#include "third_party/fmt/include/fmt/format.h"

#include "xenia/app/title_update_db.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/config.h"
#include "xenia/ui/http_download.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

namespace xe {
namespace app {

namespace {

constexpr char kArchive[] = "archive.org";
constexpr char kXboxUnity[] = "xboxunity.net";
// Far more than any title's list of updates takes.
constexpr size_t kMaxUpdateListSize = 1024 * 1024;

wxString AvailableText(const TitleUpdateDownload& update, const wxString& name,
                       const char* source) {
  const wxString size =
      wxFileName::GetHumanReadableSize(wxULongLong(update.size));
  return update.tu
             ? wxString::Format(_("Title update %u for %s is available from "
                                  "%s (%s)."),
                                update.tu, name, source, size)
             : wxString::Format(
                   _("A title update for %s is available from %s (%s)."), name,
                   source, size);
}

wxString NoneText(const wxString& name, const char* source) {
  return wxString::Format(
      _("%s has no newer title update for this release of %s."), source, name);
}

// The file's text, empty when it can't be read or is longer than |limit|.
std::string ReadText(const std::filesystem::path& path, size_t limit) {
  std::string text;
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return text;
  }
  char buffer[4096];
  size_t count;
  while (text.size() <= limit &&
         (count = std::fread(buffer, 1, sizeof(buffer), file)) != 0) {
    text.append(buffer, count);
  }
  std::fclose(file);
  if (text.size() > limit) {
    text.clear();
  }
  return text;
}

// Runs |work| on another thread under a pulsing progress dialog so the window
// keeps responding while it reads a package, which can be gigabytes.
template <typename Result>
Result WithProgress(wxWindow* parent, const wxString& message,
                    std::function<Result()> work) {
  std::future<Result> result = std::async(std::launch::async, std::move(work));
  wxProgressDialog progress(_("Title Update"), message, 100, parent,
                            wxPD_APP_MODAL | wxPD_AUTO_HIDE);
  while (result.wait_for(std::chrono::milliseconds(50)) !=
         std::future_status::ready) {
    progress.Pulse();
  }
  return result.get();
}

wxString TitleUpdateFitMessage(TitleUpdateFit fit, const wxString& name) {
  switch (fit) {
    case TitleUpdateFit::kFits:
      break;
    case TitleUpdateFit::kDamaged:
      return wxString::Format(_("%s is damaged."), name);
    case TitleUpdateFit::kWrongKey:
      return wxString::Format(_("%s doesn't fit this release of the game."),
                              name);
    case TitleUpdateFit::kOtherRelease:
      return wxString::Format(_("%s is for a different release of this game."),
                              name);
  }
  return wxString();
}

class TitleUpdateDialog : public wxDialog {
 public:
  TitleUpdateDialog(wxWindow* parent, const TitleUpdateRelease& release,
                    uint32_t installed, const std::filesystem::path& game,
                    const wxString& name, const std::filesystem::path& folder,
                    bool at_launch);

  bool dont_ask_again() const { return dont_ask_ && dont_ask_->IsChecked(); }

 private:
  void CheckXboxUnity();
  // Installs |update| when it's the one this release needs, else says why.
  void Download(const TitleUpdateDownload& update);
  void InstallFromFile();
  // Moves the checked package at |staged| into the updates' folder, named by
  // its content id. Empty when it's there, else why it isn't.
  wxString Install(const std::filesystem::path& staged,
                   const vfs::XContentContainerHeader& header);
  void Fail(const wxString& message);

  TitleUpdateRelease release_;
  uint32_t installed_;
  std::filesystem::path game_;
  wxString name_;
  std::filesystem::path folder_;
  const TitleUpdateDownload* archive_update_;
  TitleUpdateDownloads unity_downloads_;
  const TitleUpdateDownload* unity_update_ = nullptr;
  wxStaticText* unity_text_;
  wxButton* unity_button_;
  wxCheckBox* dont_ask_ = nullptr;
};

TitleUpdateDialog::TitleUpdateDialog(
    wxWindow* parent, const TitleUpdateRelease& release, uint32_t installed,
    const std::filesystem::path& game, const wxString& name,
    const std::filesystem::path& folder, bool at_launch)
    : wxDialog(parent, wxID_ANY, _("Title Update")),
      release_(release),
      installed_(installed),
      game_(game),
      name_(name),
      folder_(folder),
      archive_update_(FindNewerTitleUpdate(
          GetTitleUpdateDownloads(release.title_id), release, installed)) {
  const int gap = FromDIP(8);
  const int wrap = FromDIP(440);
  auto* sizer = new wxBoxSizer(wxVERTICAL);

  auto* archive_text = new wxStaticText(
      this, wxID_ANY,
      archive_update_ ? AvailableText(*archive_update_, name_, kArchive)
                      : NoneText(name_, kArchive));
  archive_text->Wrap(wrap);
  sizer->Add(archive_text, 0, wxLEFT | wxRIGHT | wxTOP, gap * 2);
  unity_text_ = new wxStaticText(this, wxID_ANY, wxString());
  sizer->Add(unity_text_, 0, wxLEFT | wxRIGHT | wxTOP, gap * 2);
  unity_text_->Hide();

  auto* sources = new wxBoxSizer(wxHORIZONTAL);
  auto* archive = new wxButton(this, wxID_ANY, _("Download from archive.org"));
  archive->Enable(archive_update_ != nullptr);
  archive->Bind(wxEVT_BUTTON,
                [this](wxCommandEvent&) { Download(*archive_update_); });
  sources->Add(archive);
  unity_button_ = new wxButton(this, wxID_ANY, _("Check xboxunity.net"));
  unity_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (unity_update_) {
      Download(*unity_update_);
    } else {
      CheckXboxUnity();
    }
  });
  sources->Add(unity_button_, 0, wxLEFT, gap);
  auto* file = new wxButton(this, wxID_ANY, _("Install from File..."));
  file->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { InstallFromFile(); });
  sources->Add(file, 0, wxLEFT, gap);
  sizer->Add(sources, 0, wxLEFT | wxRIGHT | wxTOP, gap * 2);

  auto* bottom = new wxBoxSizer(wxHORIZONTAL);
  if (at_launch) {
    dont_ask_ =
        new wxCheckBox(this, wxID_ANY, _("Don't ask again for this game"));
    bottom->Add(dont_ask_, 0, wxALIGN_CENTER_VERTICAL);
  }
  bottom->AddStretchSpacer();
  bottom->Add(
      new wxButton(this, wxID_CANCEL, at_launch ? _("&Not Now") : _("&Close")));
  sizer->Add(bottom, 0, wxEXPAND | wxALL, gap * 2);

  (archive_update_ ? archive : unity_button_)->SetDefault();
  SetSizerAndFit(sizer);
  CentreOnParent();
}

void TitleUpdateDialog::CheckXboxUnity() {
  const auto list_path = folder_.parent_path() / "title_update_list.partial";
  ui::DownloadResult result;
  {
    wxProgressDialog progress(_("Title Update"), _("Checking xboxunity.net..."),
                              100, this,
                              wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE);
    result = ui::DownloadFile(
        XboxUnityTitleUpdatesUrl(release_.title_id), list_path,
        [&progress](uint64_t, uint64_t) { return progress.Pulse(); });
  }
  std::optional<TitleUpdateDownloads> listed;
  if (result == ui::DownloadResult::kSuccess) {
    listed =
        ParseXboxUnityTitleUpdates(ReadText(list_path, kMaxUpdateListSize));
  }
  std::error_code ec;
  std::filesystem::remove(list_path, ec);
  if (result == ui::DownloadResult::kCancelled) {
    return;
  }

  wxString text;
  if (!listed) {
    switch (result) {
      case ui::DownloadResult::kSuccess:
        text = _("xboxunity.net's answer couldn't be read.");
        break;
      case ui::DownloadResult::kWriteFailed:
        text = _("xboxunity.net's answer couldn't be saved.");
        break;
      default:
        text = _("xboxunity.net couldn't be reached.");
        break;
    }
  } else {
    unity_downloads_ = std::move(*listed);
    unity_update_ =
        FindNewerTitleUpdate(unity_downloads_, release_, installed_);
    if (unity_update_) {
      text = AvailableText(*unity_update_, name_, kXboxUnity);
      unity_button_->SetLabel(_("Download from xboxunity.net"));
    } else {
      text = NoneText(name_, kXboxUnity);
      unity_button_->Disable();
    }
  }
  unity_text_->SetLabel(text);
  unity_text_->Wrap(FromDIP(440));
  unity_text_->Show();
  GetSizer()->SetSizeHints(this);
  Layout();
}

void TitleUpdateDialog::Download(const TitleUpdateDownload& update) {
  std::error_code ec;
  std::filesystem::create_directories(folder_, ec);
  // Downloaded beside the updates' folder, where nothing takes it for one.
  const auto partial_path = folder_.parent_path() / "title_update.partial";
  ui::DownloadResult result;
  {
    wxProgressDialog progress(_("Title Update"),
                              _("Downloading the title update..."), 100, this,
                              wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE);
    result = ui::DownloadFile(
        update.url, partial_path,
        [&progress, &update](uint64_t received, uint64_t size) {
          size = size ? size : std::max<uint64_t>(update.size, 1);
          return progress.Update(
              int(std::min(received * 100 / size, uint64_t(99))));
        });
  }
  wxString failure;
  switch (result) {
    case ui::DownloadResult::kSuccess: {
      const auto header =
          vfs::XContentContainerDevice::ReadContainerHeader(partial_path);
      // An early update's header has no version so its patch says which
      // release it's for.
      const uint32_t number =
          PackageTitleUpdateNumber(partial_path, release_.version);
      const bool expected =
          header && header->content_header.is_magic_valid() &&
          header->content_metadata.content_type == XContentType::kInstaller &&
          header->content_metadata.execution_info.title_id ==
              release_.title_id &&
          std::filesystem::file_size(partial_path, ec) == update.size &&
          number && (!update.tu || number == update.tu);
      if (!expected) {
        failure =
            _("The downloaded title update isn't the one this game "
              "needs.");
        break;
      }
      if (!WithProgress<bool>(this, _("Checking the title update..."), [&]() {
            return IsPackageComplete(partial_path);
          })) {
        failure = _("The downloaded title update is damaged.");
        break;
      }
      failure = Install(partial_path, *header);
      break;
    }
    case ui::DownloadResult::kCancelled:
      break;
    case ui::DownloadResult::kFailed:
      failure = _("The title update couldn't be downloaded.");
      break;
    case ui::DownloadResult::kWriteFailed:
      failure = _("The title update couldn't be saved.");
      break;
  }
  std::filesystem::remove(partial_path, ec);
  if (result == ui::DownloadResult::kCancelled) {
    return;
  }
  if (!failure.empty()) {
    Fail(failure);
    return;
  }
  EndModal(wxID_OK);
}

void TitleUpdateDialog::InstallFromFile() {
  wxFileDialog picker(this, _("Select a title update package"), wxString(),
                      wxString(), _("All Files (*.*)|*.*"),
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (picker.ShowModal() != wxID_OK) {
    return;
  }
  const auto source = xe::to_path(picker.GetPath().utf8_string());
  const wxString file_name =
      wxString::FromUTF8(xe::path_to_utf8(source.filename()));
  const auto header = vfs::XContentContainerDevice::ReadContainerHeader(source);
  if (!header || !header->content_header.is_magic_valid() ||
      header->content_metadata.content_type != XContentType::kInstaller) {
    Fail(wxString::Format(_("%s is not a title update package."), file_name));
    return;
  }
  const uint32_t title_id = header->content_metadata.execution_info.title_id;
  if (title_id != release_.title_id) {
    Fail(wxString::Format(_("%s belongs to title %08X."), file_name, title_id));
    return;
  }
  // Copied beside the updates' folder first so a copy cut short is never
  // taken for an update.
  const auto staged = folder_.parent_path() / "title_update.partial";
  std::error_code ec;
  std::filesystem::create_directories(staged.parent_path(), ec);
  ec = WithProgress<std::error_code>(
      this, _("Copying the title update..."), [&]() {
        std::error_code copy_ec;
        std::filesystem::copy_file(
            source, staged, std::filesystem::copy_options::overwrite_existing,
            copy_ec);
        return copy_ec;
      });
  wxString failure;
  if (ec) {
    failure = wxString::Format(_("%s could not be copied: %s"), file_name,
                               wxString::FromUTF8(ec.message()));
  } else {
    failure = CheckTitleUpdatePackage(
        this, staged, file_name,
        header->content_metadata.execution_info.version_value, game_,
        release_.version);
    if (failure.empty()) {
      failure = Install(staged, *header);
    }
  }
  std::filesystem::remove(staged, ec);
  if (!failure.empty()) {
    Fail(failure);
    return;
  }
  EndModal(wxID_OK);
}

wxString TitleUpdateDialog::Install(
    const std::filesystem::path& staged,
    const vfs::XContentContainerHeader& header) {
  std::string file_name;
  for (const uint8_t byte : header.content_header.content_id) {
    file_name += fmt::format("{:02X}", byte);
  }
  std::error_code ec;
  std::filesystem::create_directories(folder_, ec);
  std::filesystem::rename(staged, folder_ / file_name, ec);
  if (ec) {
    return _("The title update couldn't be saved.");
  }
  XELOGI("Installed title update {} for {:08X}", file_name, release_.title_id);
  return wxString();
}

void TitleUpdateDialog::Fail(const wxString& message) {
  wxMessageBox(message, _("Title Update"), wxOK | wxICON_WARNING, this);
}

}  // namespace

std::optional<TitleUpdateRelease> LibraryTitleUpdateRelease(
    const LibraryEntry& entry, const LibraryPath& disc) {
  if (entry.content_type == XContentType::kMarketplaceContent) {
    return std::nullopt;
  }
  // The library keys a release by its package's header, which an early Arcade
  // package leaves zero.
  if (!entry.version) {
    return ReadTitleUpdateRelease(disc.path);
  }
  return TitleUpdateRelease{entry.title_id, disc.media_id, entry.version};
}

wxString CheckTitleUpdatePackage(wxWindow* parent,
                                 const std::filesystem::path& package,
                                 const wxString& name, uint32_t package_version,
                                 const std::filesystem::path& game,
                                 uint32_t version) {
  return TitleUpdateFitMessage(
      WithProgress<TitleUpdateFit>(
          parent, _("Checking the title update..."),
          [&]() {
            return CheckTitleUpdateFit(package, package_version, game, version);
          }),
      name);
}

bool ShowTitleUpdateDialog(wxWindow* parent, const TitleUpdateRelease& release,
                           uint32_t installed,
                           const std::filesystem::path& game,
                           const wxString& name,
                           const std::filesystem::path& folder,
                           bool at_launch) {
  TitleUpdateDialog dialog(parent, release, installed, game, name, folder,
                           at_launch);
  const bool installed_one = dialog.ShowModal() == wxID_OK;
  if (dialog.dont_ask_again()) {
    // The game's own override, which its Config tab shows and undoes.
    toml::table game_config = config::LoadGameConfig(release.title_id);
    if (auto* section = config::ResolveSectionTable(game_config, "General")) {
      section->insert_or_assign("offer_title_updates", false);
      config::SaveGameConfig(release.title_id, game_config);
    }
  }
  return installed_one;
}

}  // namespace app
}  // namespace xe
