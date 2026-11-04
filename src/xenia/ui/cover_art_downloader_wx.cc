/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/cover_art_downloader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/ui/http_download.h"

namespace xe {
namespace ui {

// Requests in flight at once.
constexpr size_t kWorkerCount = 4;

static bool IsImage(const std::vector<uint8_t>& data) {
  static constexpr uint8_t kJpeg[] = {0xFF, 0xD8, 0xFF};
  static constexpr uint8_t kPng[] = {0x89, 'P', 'N', 'G'};
  auto starts_with = [&data](const auto& magic) {
    return data.size() >= sizeof(magic) &&
           std::equal(std::begin(magic), std::end(magic), data.begin());
  };
  return starts_with(kJpeg) || starts_with(kPng);
}

// The image at the first of |urls| that gives one, empty when none does or
// |cancelled| is set.
static std::vector<uint8_t> DownloadImage(HttpDownloader& downloader,
                                          const std::vector<std::string>& urls,
                                          const std::atomic<bool>& cancelled) {
  std::vector<uint8_t> data;
  for (const auto& url : urls) {
    if (cancelled) {
      break;
    }
    if (downloader.DownloadToMemory(url, data) && IsImage(data)) {
      return data;
    }
  }
  data.clear();
  return data;
}

// Writes beside |path| first so an interrupted write never leaves a partial
// cover where it would be shown.
static bool WriteCover(const std::filesystem::path& path,
                       const std::vector<uint8_t>& data) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  auto partial = path;
  partial += ".partial";
  if (xe::filesystem::WriteAllBytes(partial, data)) {
    std::filesystem::rename(partial, path, ec);
    if (!ec) {
      return true;
    }
  }
  std::filesystem::remove(partial, ec);
  XELOGE("Cover art: failed to save {}", xe::strip_pii(xe::path_to_utf8(path)));
  return false;
}

CoverArtResult DownloadCoverArt(
    const std::vector<CoverArtDownload>& downloads,
    const std::function<bool(size_t done, size_t total)>& progress) {
  std::atomic<size_t> next = 0;
  std::atomic<size_t> done = 0;
  std::atomic<size_t> downloaded = 0;
  std::atomic<size_t> not_downloaded = 0;
  std::atomic<size_t> not_saved = 0;
  std::atomic<bool> cancelled = false;
  const size_t worker_count = std::min(kWorkerCount, downloads.size());
  std::atomic<size_t> running = worker_count;
  // One per worker, made and destroyed on this thread.
  std::vector<HttpDownloader> downloaders(worker_count);
  auto work = [&](HttpDownloader& downloader) {
    for (size_t i = next++; i < downloads.size() && !cancelled; i = next++) {
      const auto image =
          DownloadImage(downloader, downloads[i].urls, cancelled);
      if (image.empty()) {
        if (!cancelled) {
          ++not_downloaded;
        }
      } else {
        bool saved = true;
        for (const auto& file : downloads[i].files) {
          saved = WriteCover(file, image) && saved;
        }
        ++(saved ? downloaded : not_saved);
      }
      ++done;
    }
    --running;
  };
  std::vector<std::thread> workers;
  for (size_t i = 0; i < worker_count; ++i) {
    workers.emplace_back(work, std::ref(downloaders[i]));
  }
  // Polled until the workers finish, even after a cancel while they end the
  // requests in flight, which keeps the caller's window responding.
  while (running) {
    if (!progress(done, downloads.size())) {
      cancelled = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  for (auto& worker : workers) {
    worker.join();
  }
  return {downloaded, not_downloaded, not_saved, cancelled};
}

}  // namespace ui
}  // namespace xe
