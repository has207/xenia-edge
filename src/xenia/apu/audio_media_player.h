/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_AUDIO_MEDIA_PLAYER_H_
#define XENIA_APU_AUDIO_MEDIA_PLAYER_H_

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "xenia/apu/audio_driver.h"
#include "xenia/apu/audio_system.h"
#include "xenia/apu/host_media_session.h"
#include "xenia/apu/xmp_state.h"
#include "xenia/kernel/xam/apps/xmp_app.h"

namespace xe {
namespace apu {

using XmpApp = kernel::xam::apps::XmpApp;

enum ProcessAudioResult { Successful = 0, ForcedFinish = 1, Failure = 2 };

// XMP renders as an audio driver client: each pass takes one frame of 48 kHz
// stereo from the decoder, hands it to the title's capture callback and plays
// it unless the title renders it.
class AudioMediaPlayer : public AudioSystem::HostClient {
 public:
  static constexpr uint32_t kSampleRate = 48000;
  static constexpr uint32_t kChannels = 2;
  static constexpr uint32_t kFrameSamples =
      kChannels * AudioDriver::kChannelSamplesDefault;

  // |host_media| outlives the player.
  AudioMediaPlayer(apu::AudioSystem* audio_system,
                   kernel::KernelState* kernel_state,
                   HostMediaSession* host_media);
  ~AudioMediaPlayer() override;

  void Setup();

  X_STATUS Play(uint32_t playlist_handle, uint32_t song_handle, bool force);
  X_STATUS Next();
  X_STATUS Previous();

  void Stop(bool change_state = false, bool force = true);
  void Pause();
  void Continue();

  XmpApp::State GetState() const { return state_; }

  bool IsIdle() const { return state_ == XmpApp::State::kIdle; }
  bool IsPlaying() const { return state_ == XmpApp::State::kPlaying; }
  bool IsPaused() const { return state_ == XmpApp::State::kPaused; }
  // A song the player decodes, which the user's music on the host never is.
  bool IsSongLoaded() const { return active_song_ && !host_playback_; }

  bool IsInRepeatMode() const {
    return repeat_mode_ == XmpApp::RepeatMode::kPlaylist;
  }

  bool IsLastSongInPlaylist() const;

  X_STATUS SetVolume(float volume);

  void SetPlaybackMode(XmpApp::PlaybackMode playback_mode) {
    playback_mode_ = playback_mode;
  }
  XmpApp::PlaybackMode GetPlaybackMode() const { return playback_mode_; }

  void SetRepeatMode(XmpApp::RepeatMode repeat_mode) {
    repeat_mode_ = repeat_mode;
  }
  XmpApp::RepeatMode GetRepeatMode() { return repeat_mode_; }

  void SetPlaybackFlags(XmpApp::PlaybackFlags playback_flags) {
    playback_flags_ = playback_flags;
  }
  XmpApp::PlaybackFlags GetPlaybackFlags() const { return playback_flags_; }

  // Every client's volume before anything sets it, -12 dB (10^(-12/20)).
  static constexpr float kDefaultVolume = 0.25118864f;

  struct ControllerSettings {
    XmpApp::PlaybackMode playback_mode = XmpApp::PlaybackMode::kInOrder;
    XmpApp::RepeatMode repeat_mode = XmpApp::RepeatMode::kPlaylist;
    XmpApp::PlaybackFlags playback_flags = XmpApp::PlaybackFlags::kDefault;
    float volume = kDefaultVolume;
  };
  ControllerSettings& GetControllerSettings(XmpClient xmp_client);

  void SetXMPOverride(bool xmp_override) { xmp_override_ = xmp_override; }
  bool IsXMPOverrideEnabled() const { return xmp_override_; }

  uint32_t GetDashInitState() const { return dash_init_state; }

  void SetXMPClient(XmpClient xmp_client) { xmp_client_ = xmp_client; }

  void SetPlaybackController(PlaybackController playback_controller);

  // A client takes playback, locked against the user or not, or gives it back
  // with kRestore. The user's music on the host pauses while the title holds
  // playback.
  X_HRESULT RequestPlaybackController(XmpClient xmp_client,
                                      PlaybackController request, bool locked);

  PlaybackController GetPlaybackController() const {
    return playback_controller_;
  }

  XmpClient GetXMPClient() const { return xmp_client_; }

  bool IsTitleInPlaybackControl() const {
    if (IsXMPOverrideEnabled()) {
      return false;
    }

    return xmp_client_ == XmpClient::kGame &&
           playback_controller_ == PlaybackController::kGame;
  }

  // Waits for a call of the previous callback in flight before returning.
  void SetCaptureCallback(uint32_t callback, uint32_t context,
                          bool title_render);

  void AddPlaylist(uint32_t handle, std::unique_ptr<XmpApp::Playlist> playlist);
  // Whether it was the current playlist.
  bool RemovePlaylist(uint32_t handle);

  XmpApp::Song* GetCurrentSong() const { return active_song_; }

  // Queues decoded interleaved 48 kHz stereo, waiting while the queue is full.
  // False once the song is unloaded.
  bool PushSamples(const float* samples, size_t frames);

  void Pump(cpu::ThreadState* thread_state) override;

 private:
  // About 200 ms decoded ahead.
  static constexpr size_t kQueuedSamplesMax = kSampleRate / 5 * kChannels;

  void OnStateChanged();

  // The app the user picked on the host plays their music. OnHostMediaChanged
  // takes host_mutex_, the rest run under it.
  void OnHostMediaChanged();
  void ApplyHostMedia(const HostMedia& media);
  void BeginHostPlayback(const HostMedia& media);
  void UpdateHostPlayback(const HostMedia& media);
  void EndHostPlayback();
  void SetHostSong(const HostMedia& media);
  // A title's own playback drops the user's song, which doesn't come back
  // until the user plays it again. Takes host_mutex_.
  void ReplaceHostPlayback();

  void Play();
  void WorkerThreadMain();
  std::span<uint8_t> LoadSongToMemory();

  // Waits until the pump has taken the whole song.
  void WaitForDrain();
  void FlushSamples();

  XmpApp::State state_ = XmpApp::State::kIdle;
  bool xmp_override_ = false;
  XmpApp::PlaybackMode playback_mode_ = XmpApp::PlaybackMode::kInOrder;
  XmpApp::RepeatMode repeat_mode_ = XmpApp::RepeatMode::kPlaylist;
  XmpApp::PlaybackFlags playback_flags_ = XmpApp::PlaybackFlags::kDefault;
  PlaybackController playback_controller_ = PlaybackController::kGame;
  XmpClient xmp_client_ = XmpClient::kGame;
  ControllerSettings controller_settings_[4];
  std::atomic<float> volume_ = kDefaultVolume;
  uint32_t dash_init_state = 0;

  std::unordered_map<uint32_t, std::unique_ptr<XmpApp::Playlist>> playlists_;
  XmpApp::Playlist* active_playlist_;
  XmpApp::Song* active_song_;

  size_t current_song_handle_ = 0;

  // Guards the capture fields, never held while guest code runs.
  std::mutex capture_mutex_;
  uint32_t capture_callback_ = 0;
  uint32_t capture_context_ = 0;
  bool capture_title_render_ = false;
  bool capture_in_flight_ = false;
  bool capture_waiter_ = false;
  xe::threading::Fence capture_done_fence_;
  // One buffer for every call.
  uint32_t capture_buffer_ = 0;

  // Decoded samples the pump hasn't taken yet.
  std::mutex samples_mutex_;
  std::condition_variable samples_cv_;
  std::deque<float> samples_;
  bool song_decoded_ = false;

  size_t client_index_ = 0;
  bool client_registered_ = false;

  AudioSystem* audio_system_ = nullptr;
  kernel::KernelState* kernel_state_ = nullptr;

  xe::global_critical_region global_critical_region_;
  std::atomic<bool> worker_running_ = {false};
  std::unique_ptr<threading::Thread> worker_thread_;
  xe::threading::Fence resume_fence_;  // Signaled when resume requested.
  xe::threading::Fence processing_end_fence_;

  HostMediaSession* host_media_ = nullptr;
  // Serializes the host's changes with clients taking and giving back
  // playback.
  std::mutex host_mutex_;
  // The user's music on the host holds playback.
  std::atomic<bool> host_playback_ = {false};
  bool host_paused_for_title_ = false;
  HostMedia host_media_state_;
  // Every song the host has played, as GetCurrentSong hands out pointers.
  std::vector<std::unique_ptr<XmpApp::Song>> host_songs_;
};

}  // namespace apu
}  // namespace xe

#endif
