/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_HOST_MEDIA_SESSION_H_
#define XENIA_APU_HOST_MEDIA_SESSION_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xe {
namespace apu {

// What the media an app plays on the host is doing.
struct HostMedia {
  enum class Status { kNone, kPlaying, kPaused };
  Status status = Status::kNone;
  std::u16string title;
  std::u16string artist;
  std::u16string album;
  std::u16string album_artist;
  std::u16string genre;
  uint32_t track_number = 0;
  uint32_t duration_ms = 0;

  bool operator==(const HostMedia&) const = default;
};

// An app with a media session on the host.
struct HostMediaSource {
  // The app's user model ID.
  std::u16string id;
  std::u16string name;
  HostMedia media;
  bool selected = false;
};

// The host's media sessions, one of which the user picks to play their music.
// The pick lasts until the user stops the music or its app closes.
class HostMediaSession {
 public:
  virtual ~HostMediaSession() = default;

  // Null where the platform has no media sessions.
  static std::unique_ptr<HostMediaSession> Create();

  // |on_change| runs on the session's own thread whenever the picked source's
  // media changes. Waits for a call in flight before returning.
  virtual void SetListener(std::function<void()> on_change) = 0;

  // Every app with a session, kept current for a while after each call.
  virtual std::vector<HostMediaSource> GetSources() = 0;
  // The picked source's media, none until it plays.
  virtual HostMedia GetMedia() = 0;

  // These ask the apps without waiting for them.
  // Picks |id| and plays it, pausing the previous pick.
  virtual void Select(const std::u16string& id) = 0;
  // Pauses the picked source and drops the pick.
  virtual void Stop() = 0;
  // The pick's music stops holding playback until it plays again.
  virtual void ReleaseHold() = 0;
  virtual void Pause() = 0;
  virtual void Resume() = 0;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_HOST_MEDIA_SESSION_H_
