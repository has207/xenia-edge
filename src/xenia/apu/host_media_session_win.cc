/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/host_media_session.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/threading.h"

#include <shlobj.h>

// windows.h defines this, which the C++/WinRT headers use as a name.
#undef GetCurrentTime
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

namespace xe {
namespace apu {

namespace {

using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::
    GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::
    GlobalSystemMediaTransportControlsSessionPlaybackStatus;

std::u16string ToU16String(const winrt::hstring& text) {
  return std::u16string(reinterpret_cast<const char16_t*>(text.c_str()),
                        text.size());
}

HostMedia::Status GetStatus(
    const GlobalSystemMediaTransportControlsSession& session) {
  switch (session.GetPlaybackInfo().PlaybackStatus()) {
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
      return HostMedia::Status::kPlaying;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
      return HostMedia::Status::kPaused;
    default:
      return HostMedia::Status::kNone;
  }
}

void ReadMetadata(const GlobalSystemMediaTransportControlsSession& session,
                  HostMedia& media) {
  const auto properties = session.TryGetMediaPropertiesAsync().get();
  media.title = ToU16String(properties.Title());
  media.artist = ToU16String(properties.Artist());
  media.album = ToU16String(properties.AlbumTitle());
  media.album_artist = ToU16String(properties.AlbumArtist());
  const auto genres = properties.Genres();
  if (genres.Size()) {
    media.genre = ToU16String(genres.GetAt(0));
  }
  media.track_number = static_cast<uint32_t>(properties.TrackNumber());
  const auto timeline = session.GetTimelineProperties();
  media.duration_ms = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          timeline.EndTime() - timeline.StartTime())
          .count());
}

// The Global System Media Transport Controls sessions, Windows 10 1809 and
// later.
class HostMediaSessionWin final : public HostMediaSession {
 public:
  HostMediaSessionWin() : thread_([this]() { Run(); }) {}

  ~HostMediaSessionWin() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
  }

  void SetListener(std::function<void()> on_change) override {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    on_change_ = std::move(on_change);
  }

  std::vector<HostMediaSource> GetSources() override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    if (now >= list_until_) {
      refresh_ = true;
      wake_.notify_all();
    }
    list_until_ = now + kListTime;
    return sources_;
  }

  HostMedia GetMedia() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return media_;
  }

  void Select(const std::u16string& id) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (id != selected_) {
        if (!selected_.empty()) {
          requests_.push_back({Command::kPause, selected_});
        }
        // Playback the previous pick held passes to this one.
        played_ =
            !selected_.empty() && media_.status != HostMedia::Status::kNone;
        selected_ = id;
      }
      released_ = false;
      requests_.push_back({Command::kPlay, id});
    }
    wake_.notify_all();
  }

  void Stop() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (selected_.empty()) {
        return;
      }
      requests_.push_back({Command::kPause, selected_});
      selected_.clear();
      played_ = false;
      released_ = false;
    }
    wake_.notify_all();
  }

  void ReleaseHold() override {
    std::lock_guard<std::mutex> lock(mutex_);
    played_ = false;
    released_ = true;
    media_ = {};
  }

  void Pause() override { RequestSelected(Command::kPause); }
  void Resume() override { RequestSelected(Command::kPlay); }

 private:
  using Clock = std::chrono::steady_clock;

  enum class Command { kPause, kPlay };
  struct Request {
    Command command;
    std::u16string id;
  };

  static constexpr auto kPollInterval = std::chrono::milliseconds(500);
  // How long the sources stay current after GetSources.
  static constexpr auto kListTime = std::chrono::seconds(2);

  bool IsActive(Clock::time_point now) const {
    return !selected_.empty() || !requests_.empty() || now < list_until_;
  }

  void RequestSelected(Command command) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (selected_.empty()) {
        return;
      }
      requests_.push_back({command, selected_});
    }
    wake_.notify_all();
  }

  void Run() {
    xe::threading::set_name("Host Media Session");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    try {
      manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync()
                    .get();
    } catch (const winrt::hresult_error& error) {
      XELOGW("Host media session unavailable: {:08X}",
             static_cast<uint32_t>(error.code()));
    }
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_ && manager) {
      if (!IsActive(Clock::now())) {
        sources_.clear();
        wake_.wait(lock,
                   [this]() { return stopping_ || IsActive(Clock::now()); });
        continue;
      }
      refresh_ = false;
      const auto requests = std::exchange(requests_, {});
      const std::u16string selected = selected_;
      const bool listing = Clock::now() < list_until_;
      lock.unlock();
      std::vector<HostMediaSource> sources;
      bool polled = true;
      try {
        sources = Poll(manager, requests, selected, listing);
      } catch (const winrt::hresult_error&) {
        polled = false;
      }
      lock.lock();
      // A pick made during the poll waits for the next one. No pick needs no
      // poll to release playback.
      if (selected == selected_ && (polled || selected_.empty())) {
        if (Update(std::move(sources), listing && polled)) {
          lock.unlock();
          {
            std::lock_guard<std::mutex> listener_lock(listener_mutex_);
            if (on_change_) {
              on_change_();
            }
          }
          lock.lock();
        }
      }
      wake_.wait_for(lock, kPollInterval, [this]() {
        return stopping_ || refresh_ || !requests_.empty();
      });
    }
    lock.unlock();
    manager = nullptr;
    winrt::uninit_apartment();
  }

  // Runs on the session's thread, unlocked.
  std::vector<HostMediaSource> Poll(
      const GlobalSystemMediaTransportControlsSessionManager& manager,
      const std::vector<Request>& requests, const std::u16string& selected,
      bool listing) {
    struct Entry {
      GlobalSystemMediaTransportControlsSession session;
      HostMediaSource source;
    };
    std::vector<Entry> entries;
    for (const auto& session : manager.GetSessions()) {
      try {
        Entry entry{session, {}};
        entry.source.id = ToU16String(session.SourceAppUserModelId());
        entry.source.media.status = GetStatus(session);
        // An app with several sessions is the one playing.
        const auto it = std::find_if(
            entries.begin(), entries.end(),
            [&](const Entry& e) { return e.source.id == entry.source.id; });
        if (it == entries.end()) {
          entries.push_back(std::move(entry));
        } else if (entry.source.media.status == HostMedia::Status::kPlaying) {
          *it = std::move(entry);
        }
      } catch (const winrt::hresult_error&) {
        // The session's app went away mid-call.
      }
    }
    for (const Request& request : requests) {
      const auto it = std::find_if(
          entries.begin(), entries.end(),
          [&](const Entry& e) { return e.source.id == request.id; });
      if (it == entries.end()) {
        continue;
      }
      try {
        if (request.command == Command::kPause) {
          it->session.TryPauseAsync().get();
        } else {
          it->session.TryPlayAsync().get();
        }
      } catch (const winrt::hresult_error&) {
      }
    }
    std::vector<HostMediaSource> sources;
    for (Entry& entry : entries) {
      HostMediaSource& source = entry.source;
      if (!listing && source.id != selected) {
        continue;
      }
      if (source.media.status != HostMedia::Status::kNone) {
        try {
          ReadMetadata(entry.session, source.media);
        } catch (const winrt::hresult_error&) {
        }
      }
      source.name = GetName(source.id);
      sources.push_back(std::move(source));
    }
    return sources;
  }

  // Under mutex_. True if the picked source's media changed.
  bool Update(std::vector<HostMediaSource> sources, bool listing) {
    HostMedia media;
    if (!selected_.empty()) {
      const auto it = std::find_if(
          sources.begin(), sources.end(),
          [this](const HostMediaSource& s) { return s.id == selected_; });
      if (it == sources.end()) {
        // Its app closed.
        selected_.clear();
        played_ = false;
      } else {
        it->selected = true;
        // A pick that hasn't played yet can be any tab left paused. It counts
        // as none until it plays, then its pauses hold playback.
        switch (it->media.status) {
          case HostMedia::Status::kPlaying:
            // A poll can still read playing from before the pause took.
            if (released_) {
              break;
            }
            played_ = true;
            media = it->media;
            break;
          case HostMedia::Status::kPaused:
            released_ = false;
            if (played_) {
              media = it->media;
            }
            break;
          case HostMedia::Status::kNone:
            released_ = false;
            played_ = false;
            break;
        }
      }
    }
    if (listing) {
      sources_ = std::move(sources);
    }
    if (media == media_) {
      return false;
    }
    media_ = std::move(media);
    return true;
  }

  // The name the Start menu shows for the app, which a desktop app without an
  // ID of its own lacks.
  std::u16string GetName(const std::u16string& id) {
    auto it = names_.find(id);
    if (it != names_.end()) {
      return it->second;
    }
    std::u16string name = id;
    winrt::com_ptr<IShellItem> item;
    PWSTR display_name = nullptr;
    if (SUCCEEDED(SHCreateItemInKnownFolder(
            FOLDERID_AppsFolder, KF_FLAG_DEFAULT,
            reinterpret_cast<const wchar_t*>(id.c_str()),
            IID_PPV_ARGS(item.put()))) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display_name))) {
      name = reinterpret_cast<const char16_t*>(display_name);
      CoTaskMemFree(display_name);
    } else if (name.size() > 4 &&
               !_wcsicmp(reinterpret_cast<const wchar_t*>(name.c_str()) +
                             name.size() - 4,
                         L".exe")) {
      name.resize(name.size() - 4);
    }
    return names_.emplace(id, std::move(name)).first->second;
  }

  std::mutex listener_mutex_;
  std::function<void()> on_change_;

  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  // GetSources found them out of date.
  bool refresh_ = false;
  std::vector<Request> requests_;
  std::u16string selected_;
  // The pick has played since it was made.
  bool played_ = false;
  // ReleaseHold ran and the pick hasn't read paused or stopped since.
  bool released_ = false;
  HostMedia media_;
  std::vector<HostMediaSource> sources_;
  Clock::time_point list_until_;

  // Only the session's thread touches these.
  std::unordered_map<std::u16string, std::u16string> names_;

  // Last so it starts once the rest is constructed.
  std::thread thread_;
};

}  // namespace

std::unique_ptr<HostMediaSession> HostMediaSession::Create() {
  return std::make_unique<HostMediaSessionWin>();
}

}  // namespace apu
}  // namespace xe
