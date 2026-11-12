/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_media_player.h"

#include <algorithm>
#include <array>
#include <random>

#include "xenia/apu/audio_driver.h"
#include "xenia/apu/audio_system.h"
#include "xenia/apu/xma_context.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/guest_scheduler.h"

extern "C" {
#if XE_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "third_party/FFmpeg/libavcodec/avcodec.h"
#include "third_party/FFmpeg/libavformat/avformat.h"
#include "third_party/FFmpeg/libavformat/avio.h"
#include "third_party/FFmpeg/libswresample/swresample.h"
#if XE_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

namespace xe {
namespace apu {

// Set while the audio worker runs the title's capture callback.
static thread_local bool in_capture_callback = false;

// The voice category titles put their music in.
constexpr uint32_t kMusicVoiceCategory = 1;

int32_t InitializeAndOpenAvCodec(std::span<uint8_t> song_data,
                                 AVFormatContext*& format_context,
                                 AVCodecContext*& av_context) {
  AVIOContext* io_ctx =
      avio_alloc_context(song_data.data(), (int)song_data.size(), 0, nullptr,
                         nullptr, nullptr, nullptr);

  format_context = avformat_alloc_context();
  format_context->pb = io_ctx;

  int ret = avformat_open_input(&format_context, nullptr, nullptr, nullptr);
  if (ret < 0) {
    return ret;
  }
  // Processing data
  ret = avformat_find_stream_info(format_context, nullptr);
  if (ret < 0) {
    return ret;
  }
  AVStream* stream = format_context->streams[0];

  // find & open codec
  AVCodecParameters* codec = stream->codecpar;
  auto decoder = avcodec_find_decoder(codec->codec_id);
  av_context = avcodec_alloc_context3(decoder);

  // Fill codec context with codec parameters
  ret = avcodec_parameters_to_context(av_context, codec);
  if (ret < 0) {
    return ret;
  }

  ret = avcodec_open2(av_context, decoder, NULL);
  return ret;
}

// XMP plays every song as 48 kHz stereo and a mono song on both channels at
// full level.
SwrContext* CreateResampler(const AVCodecContext* avctx) {
  AVChannelLayout in_layout = {};
  if (avctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
    av_channel_layout_default(&in_layout, avctx->ch_layout.nb_channels);
  } else if (av_channel_layout_copy(&in_layout, &avctx->ch_layout) < 0) {
    return nullptr;
  }
  AVChannelLayout out_layout = {};
  av_channel_layout_default(&out_layout, AudioMediaPlayer::kChannels);

  SwrContext* resampler = nullptr;
  int ret = swr_alloc_set_opts2(
      &resampler, &out_layout, AV_SAMPLE_FMT_FLT, AudioMediaPlayer::kSampleRate,
      &in_layout, avctx->sample_fmt, avctx->sample_rate, 0, nullptr);
  if (ret >= 0 && in_layout.nb_channels == 1) {
    const double matrix[] = {1.0, 1.0};
    ret = swr_set_matrix(resampler, matrix, 1);
  }
  if (ret >= 0) {
    ret = swr_init(resampler);
  }
  av_channel_layout_uninit(&in_layout);
  if (ret < 0) {
    swr_free(&resampler);
  }
  return resampler;
}

// Converts `frame`, or what the resampler holds back when it's null, and
// queues it. False once the song is unloaded.
bool ResampleFrame(AudioMediaPlayer* player, SwrContext* resampler,
                   const AVFrame* frame, std::vector<float>* samples) {
  const int in_count = frame ? frame->nb_samples : 0;
  const int out_count = swr_get_out_samples(resampler, in_count);
  if (out_count <= 0) {
    return true;
  }
  samples->resize(size_t(out_count) * AudioMediaPlayer::kChannels);
  uint8_t* out[] = {reinterpret_cast<uint8_t*>(samples->data())};
  const int converted =
      swr_convert(resampler, out, out_count,
                  frame ? frame->extended_data : nullptr, in_count);
  if (converted < 0) {
    XELOGW("XMP: resampling failed: {:08X}", uint32_t(converted));
    return true;
  }
  return !converted || player->PushSamples(samples->data(), converted);
}

ProcessAudioResult ProcessAudioLoop(AudioMediaPlayer* player,
                                    AVFormatContext* s, AVCodecContext* avctx,
                                    int streamIndex) {
  SwrContext* resampler = avctx ? CreateResampler(avctx) : nullptr;
  if (!resampler) {
    XELOGE("XMP: can't convert the song to {} Hz stereo",
           AudioMediaPlayer::kSampleRate);
    return ProcessAudioResult::Failure;
  }
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  std::vector<float> samples;
  ProcessAudioResult result = ProcessAudioResult::Successful;

  while (result == ProcessAudioResult::Successful &&
         av_read_frame(s, packet) >= 0) {
    if (!player->IsSongLoaded()) {
      result = ProcessAudioResult::ForcedFinish;
    } else if (packet->stream_index == streamIndex) {
      int ret = avcodec_send_packet(avctx, packet);
      if (ret < 0) {
        XELOGE("Error sending packet for decoding: {:X}", ret);
        av_packet_unref(packet);
        break;
      }

      while (ret >= 0) {
        ret = avcodec_receive_frame(avctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          break;
        }
        if (ret < 0) {
          XELOGW("Error during decoding: {:X}", ret);
          break;
        }

        if (!ResampleFrame(player, resampler, frame, &samples)) {
          result = ProcessAudioResult::ForcedFinish;
          break;
        }
      }
    }
    av_packet_unref(packet);
  }

  if (result == ProcessAudioResult::Successful &&
      (!ResampleFrame(player, resampler, nullptr, &samples) ||
       !player->IsSongLoaded())) {
    result = ProcessAudioResult::ForcedFinish;
  }

  av_frame_free(&frame);
  av_packet_free(&packet);
  swr_free(&resampler);
  return result;
}

AudioMediaPlayer::AudioMediaPlayer(apu::AudioSystem* audio_system,
                                   kernel::KernelState* kernel_state,
                                   HostMediaSession* host_media)
    : audio_system_(audio_system),
      kernel_state_(kernel_state),
      active_playlist_(nullptr),
      active_song_(nullptr),
      host_media_(host_media) {};

AudioMediaPlayer::~AudioMediaPlayer() {
  if (host_media_) {
    host_media_->SetListener(nullptr);
  }
  Stop();
  // Thread::reset() only closes the handle; must wait for the worker to exit.
  worker_running_ = false;
  resume_fence_.Signal();
  if (worker_thread_) {
    xe::threading::Wait(worker_thread_.get(), false);
    worker_thread_.reset();
  }
  if (client_registered_) {
    audio_system_->UnregisterClient(client_index_);
  }
};

void AudioMediaPlayer::WorkerThreadMain() {
  while (worker_running_) {
    if (!IsPlaying()) {
      resume_fence_.Wait();
    }

    if (!active_playlist_) {
      xe::threading::Sleep(std::chrono::milliseconds(500));
      continue;
    }

    if (active_song_) {
      Play();
    }
  }
}

void AudioMediaPlayer::Setup() {
  worker_running_ = true;
  worker_thread_ = threading::Thread::Create({}, [&] { WorkerThreadMain(); });
  worker_thread_->set_name("Audio Media Player");
  if (host_media_) {
    host_media_->SetListener([this]() { OnHostMediaChanged(); });
    // Music the user left playing holds playback in the next title too.
    OnHostMediaChanged();
  }
};

X_STATUS AudioMediaPlayer::Play(uint32_t playlist_handle, uint32_t song_handle,
                                bool force) {
  auto playlist_itr = playlists_.find(playlist_handle);
  if (playlist_itr == playlists_.cend()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  ReplaceHostPlayback();
  active_playlist_ = playlist_itr->second.get();

  // I've set it to false (force) for PGR3
  if (!IsIdle()) {
    Stop(false, force);
  }

  if (!song_handle) {
    active_song_ = active_playlist_->songs.cbegin()->get();
    resume_fence_.Signal();
    return X_STATUS_SUCCESS;
  }

  auto song_itr = std::ranges::find_if(
      std::as_const(active_playlist_->songs),
      [song_handle](const std::unique_ptr<XmpApp::Song>& song) {
        return song->handle == song_handle;
      });

  if (song_itr == active_playlist_->songs.cend()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  active_song_ = song_itr->get();
  resume_fence_.Signal();
  return X_STATUS_SUCCESS;
}

void AudioMediaPlayer::Play() {
  std::span<uint8_t> song_buffer = LoadSongToMemory();
  if (song_buffer.empty()) {
    return;
  }

  // The driver client stays registered once playback has started.
  if (!client_registered_) {
    if (XFAILED(audio_system_->RegisterHostClient(this, &client_index_))) {
      XELOGE("XMP: no free audio driver client");
      av_free(song_buffer.data());
      return;
    }
    client_registered_ = true;
  }

  AVFormatContext* formatContext = nullptr;
  AVCodecContext* codecContext = nullptr;
  InitializeAndOpenAvCodec(song_buffer, formatContext, codecContext);

  state_ = XmpApp::State::kPlaying;
  current_song_handle_ = active_song_->handle;
  OnStateChanged();

  auto result = ProcessAudioLoop(this, formatContext, codecContext, 0);

  // We need to stop playback only if it wasn't, once the pump has played out
  // what's queued.
  if (result != ProcessAudioResult::ForcedFinish) {
    WaitForDrain();
    if (IsSongLoaded()) {
      Stop(true, true);
    } else {
      result = ProcessAudioResult::ForcedFinish;
    }
  }

  // Cleanup after work
  avcodec_free_context(&codecContext);
  av_freep(&formatContext->pb->buffer);
  avio_context_free(&formatContext->pb);
  avformat_close_input(&formatContext);

  processing_end_fence_.Signal();

  if (result == ProcessAudioResult::ForcedFinish) {
    return;
  }

  if (!IsLastSongInPlaylist()) {
    Next();
    return;
  }

  // We're after last song in playlist
  if (IsInRepeatMode()) {
    Play(active_playlist_->handle, 0, true);
  };
}

void AudioMediaPlayer::Pause() {
  if (!IsPlaying()) {
    return;
  }

  state_ = XmpApp::State::kPaused;
  OnStateChanged();
};

void AudioMediaPlayer::Stop(bool change_state, bool force) {
  if (IsIdle()) {
    return;
  }

  // Only a title's stop waits. The user's song has no decoder to wait for.
  if (!force && host_playback_) {
    ReplaceHostPlayback();
    return;
  }

  state_ = XmpApp::State::kIdle;
  active_song_ = nullptr;
  FlushSamples();

  if (!force) {
    // Reachable from the guest XMP shim, so on a fiber this must park rather
    // than block: a plain fence wait would stall the whole dispatch CPU,
    // including the fibers that could satisfy it.
    kernel::GuestScheduler::WaitOnFence(processing_end_fence_);
  }

  if (change_state) {
    OnStateChanged();
  }
};

void AudioMediaPlayer::Continue() {
  if (!IsPaused()) {
    return;
  }

  state_ = XmpApp::State::kPlaying;
  resume_fence_.Signal();
  OnStateChanged();
}

X_STATUS AudioMediaPlayer::Next() {
  if (!active_playlist_) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (active_song_) {
    Stop(false, false);
  }

  auto itr =
      std::ranges::find_if(std::as_const(active_playlist_->songs),
                           [this](const std::unique_ptr<XmpApp::Song>& song) {
                             return song->handle == current_song_handle_;
                           });

  // There is no song with such ID?
  if (itr == active_playlist_->songs.cend()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  itr = std::next(itr);

  if (itr != active_playlist_->songs.cend()) {
    active_song_ = itr->get();
    resume_fence_.Signal();
    return X_STATUS_SUCCESS;
  }

  active_song_ = active_playlist_->songs.cbegin()->get();
  resume_fence_.Signal();
  return X_STATUS_SUCCESS;
}

X_STATUS AudioMediaPlayer::Previous() {
  if (!active_playlist_) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (active_song_) {
    Stop(false, false);
  }

  auto itr =
      std::ranges::find_if(std::as_const(active_playlist_->songs),
                           [this](const std::unique_ptr<XmpApp::Song>& song) {
                             return song->handle == current_song_handle_;
                           });

  // We're at the first entry, we need to go to the end.
  if (itr == active_playlist_->songs.cbegin()) {
    active_song_ = active_playlist_->songs.crbegin()->get();
    resume_fence_.Signal();
    return X_STATUS_SUCCESS;
  }

  itr = std::prev(itr);
  active_song_ = itr->get();
  resume_fence_.Signal();
  return X_STATUS_SUCCESS;
}

std::span<uint8_t> AudioMediaPlayer::LoadSongToMemory() {
  if (!active_song_) {
    return {};
  }

  // Find file based on provided path?
  vfs::File* vfs_file;
  vfs::FileAction file_action;
  X_STATUS result = kernel_state_->file_system()->OpenFile(
      nullptr, xe::to_utf8(active_song_->file_path),
      vfs::FileDisposition::kOpen, vfs::FileAccess::kGenericRead, false, true,
      &vfs_file, &file_action);

  if (result) {
    return {};
  }

  const size_t file_size = vfs_file->entry()->size();
  uint8_t* data =
      file_size ? static_cast<uint8_t*>(av_malloc(file_size)) : nullptr;
  if (!data) {
    vfs_file->Destroy();
    return {};
  }
  std::span<uint8_t> buffer = {data, file_size};

  size_t bytes_read = 0;
  result = vfs_file->ReadSync(buffer, 0, &bytes_read);
  vfs_file->Destroy();
  if (result != X_ERROR_SUCCESS) {
    // Read failed. We need to manually release resources from av_malloc.
    av_free(data);
    return {};
  }

  return buffer;
}

void AudioMediaPlayer::AddPlaylist(uint32_t handle,
                                   std::unique_ptr<XmpApp::Playlist> playlist) {
  if (playlists_.count(handle) != 0) {
    return;
  }

  if (playback_mode_ == XmpApp::PlaybackMode::kShuffle) {
    auto rng = std::default_random_engine{};
    std::ranges::shuffle(playlist->songs, rng);
  }

  playlists_.insert({handle, std::move(playlist)});
}

bool AudioMediaPlayer::RemovePlaylist(uint32_t handle) {
  if (playlists_.count(handle) == 0) {
    return false;
  }

  // TODO: Check if currently played song is from that playlist and stop
  // playback.
  if (active_playlist_ && active_song_) {
    Stop();
  }

  const bool current = active_playlist_ && active_playlist_->handle == handle;
  if (current) {
    active_playlist_ = nullptr;
  }
  playlists_.erase(handle);
  return current;
}

X_STATUS AudioMediaPlayer::SetVolume(float volume) {
  volume_.store(std::min(volume, 1.0f));
  return X_STATUS_SUCCESS;
}

AudioMediaPlayer::ControllerSettings& AudioMediaPlayer::GetControllerSettings(
    XmpClient xmp_client) {
  PlaybackController controller = PlaybackController::kUser;
  if (xmp_client == XmpClient::kGame) {
    controller = PlaybackController::kGame;
  } else if (xmp_client == XmpClient::kMce) {
    controller = PlaybackController::kMce;
  }

  return controller_settings_[uint32_t(controller)];
}

void AudioMediaPlayer::SetPlaybackController(
    PlaybackController playback_controller) {
  const bool changed = playback_controller != playback_controller_;
  playback_controller_ = playback_controller;
  if (!changed ||
      uint32_t(playback_controller) >= xe::countof(controller_settings_)) {
    return;
  }
  // The user's music silences the title's.
  audio_system_->SetVoiceCategoryVolume(
      kMusicVoiceCategory,
      playback_controller == PlaybackController::kUser ? 0.0f : 1.0f);

  const ControllerSettings& settings =
      controller_settings_[uint32_t(playback_controller)];
  SetPlaybackMode(settings.playback_mode);
  SetRepeatMode(settings.repeat_mode);
  SetPlaybackFlags(settings.playback_flags);
  SetVolume(settings.volume);
}

X_HRESULT AudioMediaPlayer::RequestPlaybackController(
    XmpClient xmp_client, PlaybackController request, bool locked) {
  std::lock_guard<std::mutex> lock(host_mutex_);
  // xam 81704300, then 81702DE0.
  const bool title_locked =
      playback_controller_ == PlaybackController::kGame && xmp_override_;
  if (xmp_client == XmpClient::kGame) {
    if (request == PlaybackController::kRestore) {
      // Only a locked hold has anything to give back.
      if (!title_locked) {
        return X_E_FUNCTION_FAILED;
      }
    } else if (request != PlaybackController::kGame) {
      return X_E_INVALIDARG;
    } else if (locked && title_locked) {
      return X_E_FUNCTION_FAILED;
    }
  }
  if (xmp_override_ && xmp_client != xmp_client_) {
    return X_E_ACCESS_DENIED;
  }
  if (request > PlaybackController::kRestore) {
    return X_E_FUNCTION_FAILED;
  }

  const bool restore = request == PlaybackController::kRestore;
  bool resumed = false;
  if (restore) {
    SetXMPClient(XmpClient::kGame);
    SetXMPOverride(false);
    if (host_playback_) {
      // Resumed ahead of the controller change.
      if (host_paused_for_title_) {
        resumed = true;
        host_media_->Resume();
        state_ = XmpApp::State::kPlaying;
        OnStateChanged();
      }
      SetPlaybackController(PlaybackController::kUser);
    } else {
      SetPlaybackController(PlaybackController::kGame);
    }
  } else {
    if (host_playback_ && request == PlaybackController::kGame && IsPlaying()) {
      host_paused_for_title_ = true;
      host_media_->Pause();
      state_ = XmpApp::State::kPaused;
      OnStateChanged();
    }
    SetXMPClient(xmp_client);
    SetPlaybackController(request);
    SetXMPOverride(locked);
  }

  // 58411446
  kernel_state_->BroadcastNotification(
      kXNotificationXmpPlaybackControllerChanged, IsTitleInPlaybackControl());

  // What the user's music did while the title held playback shows now.
  if (restore && host_media_) {
    HostMedia media = host_media_->GetMedia();
    // The app reads paused until the resume reaches it.
    if (resumed && media.status == HostMedia::Status::kPaused) {
      media.status = HostMedia::Status::kPlaying;
    }
    ApplyHostMedia(media);
  }
  return X_E_SUCCESS;
}

void AudioMediaPlayer::ReplaceHostPlayback() {
  if (!host_playback_) {
    return;
  }
  std::lock_guard<std::mutex> lock(host_mutex_);
  if (!host_playback_) {
    return;
  }
  XELOGI("XMP: the title's playback replaces the user's music on the host");
  EndHostPlayback();
  host_media_->ReleaseHold();
}

void AudioMediaPlayer::OnHostMediaChanged() {
  std::lock_guard<std::mutex> lock(host_mutex_);
  ApplyHostMedia(host_media_->GetMedia());
}

void AudioMediaPlayer::ApplyHostMedia(const HostMedia& media) {
  if (media.status == HostMedia::Status::kPlaying) {
    host_paused_for_title_ = false;
  }
  if (media.status == HostMedia::Status::kNone) {
    if (host_playback_) {
      EndHostPlayback();
    }
    return;
  }
  // A title holding playback locked keeps it until it gives it back.
  if (xmp_override_) {
    return;
  }
  if (!host_playback_) {
    BeginHostPlayback(media);
    return;
  }
  // Playing takes playback back from a title holding it unlocked.
  if (media.status == HostMedia::Status::kPlaying &&
      playback_controller_ != PlaybackController::kUser) {
    SetPlaybackController(PlaybackController::kUser);
    kernel_state_->BroadcastNotification(
        kXNotificationXmpPlaybackControllerChanged, IsTitleInPlaybackControl());
  }
  UpdateHostPlayback(media);
}

void AudioMediaPlayer::BeginHostPlayback(const HostMedia& media) {
  XELOGI("XMP: the user's music on the host takes over playback");
  // It replaces the title's own. Set first so the decoder stops at once.
  host_playback_ = true;
  if (!IsIdle()) {
    Stop(false, true);
  }
  active_playlist_ = nullptr;
  active_song_ = nullptr;
  SetPlaybackController(PlaybackController::kUser);
  kernel_state_->BroadcastNotification(
      kXNotificationXmpPlaybackControllerChanged, IsTitleInPlaybackControl());
  UpdateHostPlayback(media);
}

void AudioMediaPlayer::UpdateHostPlayback(const HostMedia& media) {
  const bool new_song = !active_song_ ||
                        media.title != host_media_state_.title ||
                        media.artist != host_media_state_.artist ||
                        media.album != host_media_state_.album;
  host_media_state_ = media;
  if (new_song) {
    SetHostSong(media);
  }
  const auto state = media.status == HostMedia::Status::kPlaying
                         ? XmpApp::State::kPlaying
                         : XmpApp::State::kPaused;
  if (new_song || state != state_) {
    state_ = state;
    OnStateChanged();
  }
}

void AudioMediaPlayer::EndHostPlayback() {
  XELOGI("XMP: the user's music on the host stopped");
  host_playback_ = false;
  host_paused_for_title_ = false;
  host_media_state_ = {};
  state_ = XmpApp::State::kIdle;
  active_song_ = nullptr;
  OnStateChanged();
  // A title holding playback keeps it.
  if (playback_controller_ == PlaybackController::kUser) {
    SetPlaybackController(PlaybackController::kGame);
    kernel_state_->BroadcastNotification(
        kXNotificationXmpPlaybackControllerChanged, IsTitleInPlaybackControl());
  }
}

void AudioMediaPlayer::SetHostSong(const HostMedia& media) {
  auto song = std::make_unique<XmpApp::Song>();
  song->handle = static_cast<uint32_t>(host_songs_.size());
  song->name = media.title;
  song->artist = media.artist;
  song->album = media.album;
  song->album_artist = media.album_artist;
  song->genre = media.genre;
  song->track_number = media.track_number;
  song->duration_ms = media.duration_ms;
  song->format = XmpApp::SongFormat::kWma;
  active_song_ = song.get();
  current_song_handle_ = song->handle;
  host_songs_.push_back(std::move(song));
}

bool AudioMediaPlayer::IsLastSongInPlaylist() const {
  if (!active_playlist_) {
    return false;
  }

  auto itr =
      std::ranges::find_if(std::as_const(active_playlist_->songs),
                           [this](const std::unique_ptr<XmpApp::Song>& song) {
                             return song->handle == current_song_handle_;
                           });
  itr = std::next(itr);
  return itr == active_playlist_->songs.cend();
}

void AudioMediaPlayer::SetCaptureCallback(uint32_t callback, uint32_t context,
                                          bool title_render) {
  if (callback && !capture_buffer_) {
    capture_buffer_ =
        kernel_state_->memory()->SystemHeapAlloc(kFrameSamples * sizeof(float));
  }
  bool wait;
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    capture_callback_ = callback;
    capture_context_ = context;
    capture_title_render_ = title_render;
    // The title may free the context once this returns. A callback that
    // replaces itself can't wait for its own return.
    wait = capture_in_flight_ && !in_capture_callback;
    capture_waiter_ |= wait;
  }
  if (wait) {
    kernel::GuestScheduler::WaitOnFence(capture_done_fence_);
  }
}

void AudioMediaPlayer::OnStateChanged() {
  // The parameter says whether a song is playing or paused.
  kernel_state_->BroadcastNotification(kXNotificationXmpStateChanged,
                                       state_ != XmpApp::State::kIdle);
}

bool AudioMediaPlayer::PushSamples(const float* samples, size_t frames) {
  std::unique_lock<std::mutex> lock(samples_mutex_);
  samples_cv_.wait(lock, [this] {
    return !IsSongLoaded() || samples_.size() < kQueuedSamplesMax;
  });
  if (!IsSongLoaded()) {
    return false;
  }
  samples_.insert(samples_.end(), samples, samples + frames * kChannels);
  return true;
}

void AudioMediaPlayer::WaitForDrain() {
  std::unique_lock<std::mutex> lock(samples_mutex_);
  // Lets the pump take a last partial frame.
  song_decoded_ = true;
  samples_cv_.wait(lock,
                   [this] { return samples_.empty() || !IsSongLoaded(); });
  song_decoded_ = false;
}

void AudioMediaPlayer::FlushSamples() {
  {
    std::lock_guard<std::mutex> lock(samples_mutex_);
    samples_.clear();
  }
  samples_cv_.notify_all();
}

void AudioMediaPlayer::Pump(cpu::ThreadState* thread_state) {
  std::array<float, kFrameSamples> pcm;
  bool have_pcm = false;
  {
    std::lock_guard<std::mutex> lock(samples_mutex_);
    if (IsPlaying() && (samples_.size() >= kFrameSamples ||
                        (song_decoded_ && !samples_.empty()))) {
      const size_t count = std::min<size_t>(samples_.size(), kFrameSamples);
      std::copy_n(samples_.begin(), count, pcm.begin());
      std::fill(pcm.begin() + count, pcm.end(), 0.0f);
      samples_.erase(samples_.begin(), samples_.begin() + count);
      have_pcm = true;
    }
  }
  if (have_pcm) {
    samples_cv_.notify_all();
  }

  uint32_t callback = 0;
  uint32_t context = 0;
  bool title_render;
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (have_pcm) {
      callback = capture_callback_;
      context = capture_context_;
      capture_in_flight_ = callback != 0;
    }
    // xam takes the rendering back while anyone else controls playback.
    title_render = capture_title_render_ &&
                   playback_controller_ == PlaybackController::kGame &&
                   !xmp_override_;
  }

  // It runs on the audio driver thread with the same buffer every time and
  // only while there's music. The samples don't have XMP's volume applied.
  // The third argument says whether the title renders.
  if (callback) {
    auto samples =
        kernel_state_->memory()->TranslateVirtual<float*>(capture_buffer_);
    for (uint32_t i = 0; i < kFrameSamples; ++i) {
      xe::store_and_swap<float>(samples + i, pcm[i]);
    }
    uint64_t args[] = {capture_buffer_, context, title_render};
    in_capture_callback = true;
    audio_system_->processor()->Execute(thread_state, callback, args,
                                        xe::countof(args));
    in_capture_callback = false;
    bool signal;
    {
      std::lock_guard<std::mutex> lock(capture_mutex_);
      capture_in_flight_ = false;
      signal = capture_waiter_;
      capture_waiter_ = false;
    }
    if (signal) {
      capture_done_fence_.Signal();
    }
  }

  // Driver frames are planar big-endian, front left and right first.
  constexpr uint32_t kChannelSamples = AudioDriver::kChannelSamplesDefault;
  float frame[AudioDriver::kFrameSamplesMax] = {};
  if (have_pcm && !title_render) {
    const float volume = volume_;
    for (uint32_t i = 0; i < kChannelSamples; ++i) {
      xe::store_and_swap<float>(&frame[i], pcm[i * kChannels] * volume);
      xe::store_and_swap<float>(&frame[kChannelSamples + i],
                                pcm[i * kChannels + 1] * volume);
    }
  }
  audio_system_->SubmitFrame(client_index_, frame);
}

}  // namespace apu
}  // namespace xe
