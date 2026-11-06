/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/directory_scanner.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <system_error>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/stb/stb_image.h"
#include "xenia/app/game_library.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/xam/indie_game.h"
#include "xenia/vfs/iso_metadata.h"
#include "xenia/vfs/stfs_metadata.h"
#include "xenia/vfs/xbe_metadata.h"
#include "xenia/vfs/xex_metadata.h"
#include "xenia/vfs/zar_metadata.h"
#include "xenia/xbox.h"

namespace xe {
namespace app {

namespace {

std::string ToLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

bool IsKnownLaunchableExtension(const std::string& ext) {
  return ext == ".xex" || ext == ".xbe" || ext == ".iso" || ext == ".zar";
}

// GOD packages put their data fragments in a sibling "<hash>.data" dir of
// nothing but Data#### blobs — recursing into them is pure waste.
bool IsGodDataDirectory(const std::filesystem::path& path) {
  std::string ext = ToLowerAscii(xe::path_to_utf8(path.extension()));
  return ext == ".data";
}

// STFS content types that boot directly. Everything else (DLC, TUs, themes,
// system updates, etc.) shares the magic but isn't launchable on its own.
bool IsLaunchableContentType(uint32_t content_type) {
  switch (static_cast<XContentType>(content_type)) {
    case XContentType::kInstalledGame:  // 0x00004000 — GOD
    case XContentType::kXbox360Title:   // 0x00007000
    case XContentType::kArcadeTitle:    // 0x000D0000 — XBLA
    case XContentType::kGameDemo:       // 0x00080000
    case XContentType::kCommunityGame:  // 0x02000000 — XNA
      return true;
    default:
      return false;
  }
}

std::string ResolveContentTypeName(uint32_t raw) {
  auto it = XContentTypeMap.find(static_cast<XContentType>(raw));
  if (it != XContentTypeMap.end()) {
    return it->second;
  }
  return fmt::format("Unknown (0x{:08X})", raw);
}

DiscoveredGame MakeFromXex(const std::filesystem::path& path,
                           const char* format, const vfs::XexMetadata& m) {
  DiscoveredGame g;
  g.path = path;
  g.format = format;
  g.title_id = m.title_id;
  g.title_id_hex = fmt::format("{:08X}", m.title_id);
  g.media_id = m.media_id;
  g.module_name = m.module_name;
  g.version = m.version.ToString();
  g.disc_number = m.disc_number;
  g.disc_count = m.disc_count;
  return g;
}

// Keeps |png| for import and decodes it for the dialog, so the dialog never
// has to do PNG work on the UI thread.
void SetIcon(DiscoveredGame& g, std::vector<uint8_t> png) {
  g.icon_png = std::move(png);
  int w = 0, h = 0, channels = 0;
  unsigned char* pixels = stbi_load_from_memory(
      g.icon_png.data(), static_cast<int>(g.icon_png.size()), &w, &h, &channels,
      STBI_rgb_alpha);
  if (pixels) {
    g.icon_width = w;
    g.icon_height = h;
    g.icon_rgba.assign(pixels, pixels + (static_cast<size_t>(w) * h * 4));
    stbi_image_free(pixels);
  }
}

// An original Xbox game is what its executable says it is, whatever holds it.
void ApplyXbe(DiscoveredGame& g, const vfs::XbeMetadata& m) {
  g.title_id = m.title_id;
  g.title_id_hex = fmt::format("{:08X}", m.title_id);
  g.media_id = 0;
  g.title_name = m.title_name;
  g.content_type = uint32_t(XContentType::kXboxTitle);
  g.version = vfs::XexVersion::FromValue(m.version).ToString();
  g.disc_number = static_cast<uint8_t>(m.disc_number);
  g.disc_count = 0;
  if (!m.icon_png.empty()) {
    SetIcon(g, m.icon_png);
  }
}

// An indie game is what its GameInfo.xml says it is, not the XNA title
// launcher's content it installs as.
void ApplyIndieGame(DiscoveredGame& g, kernel::xam::IndieGame game) {
  g.title_id = game.title_id;
  g.title_id_hex = fmt::format("{:08X}", game.title_id);
  g.media_id = 0;
  g.disc_number = 0;
  g.disc_count = 0;
  g.title_name = game.name;
  if (!game.name.empty()) {
    g.display_name = std::move(game.name);
  }
  g.content_type_name = "Indie Game";
  // The header's art is the launcher's.
  g.icon_png.clear();
  g.icon_rgba.clear();
  g.icon_width = 0;
  g.icon_height = 0;
  if (!game.icon.empty()) {
    SetIcon(g, std::move(game.icon));
  }
}

DiscoveredGame MakeFromXbe(const std::filesystem::path& path,
                           const char* format, const vfs::XbeMetadata& m) {
  DiscoveredGame g;
  g.path = path;
  g.format = format;
  ApplyXbe(g, m);
  return g;
}

DiscoveredGame MakeFromStfs(const std::filesystem::path& path,
                            const vfs::StfsMetadata& m) {
  DiscoveredGame g;
  g.path = path;
  g.format = "stfs";
  g.title_id = m.title_id;
  g.title_id_hex = fmt::format("{:08X}", m.title_id);
  g.media_id = m.media_id;
  g.display_name = m.display_name;
  g.title_name = m.title_name;
  g.publisher = m.publisher;
  g.content_type = m.content_type;
  g.content_type_name = ResolveContentTypeName(m.content_type);
  g.disc_number = m.disc_number;
  g.disc_count = m.disc_count;
  if (!m.icon_data.empty()) {
    SetIcon(g, m.icon_data);
  }
  return g;
}

}  // namespace

std::string ResolveImportName(const DiscoveredGame& g) {
  // A disc image or bare executable has no type of its own and keeps the
  // title type GameFileInfo starts with.
  GameFileInfo info;
  if (g.content_type) {
    info.content_type = XContentType(g.content_type);
  }
  info.title_name = g.title_name;
  info.display_name = g.display_name;
  return NameNewRelease(g.title_id, &info, {}, {}, g.path);
}

DirectoryScanner::DirectoryScanner() = default;

DirectoryScanner::~DirectoryScanner() {
  Cancel();
  if (orchestrator_.joinable()) {
    orchestrator_.join();
  }
}

void DirectoryScanner::Start(std::filesystem::path root, XLanguage language) {
  std::lock_guard<std::mutex> lock(mu_);
  if (started_) {
    return;
  }
  started_ = true;
  progress_.started = true;
  language_ = language;
  orchestrator_ = std::thread(&DirectoryScanner::Run, this, std::move(root));
}

void DirectoryScanner::Cancel() {
  cancel_requested_.store(true);
  std::lock_guard<std::mutex> lock(queue_mu_);
  queue_cv_.notify_all();
}

DirectoryScanner::Progress DirectoryScanner::GetProgress() const {
  std::lock_guard<std::mutex> lock(mu_);
  return progress_;
}

std::vector<DiscoveredGame> DirectoryScanner::GetAllResults() const {
  std::lock_guard<std::mutex> lock(mu_);
  return results_;
}

size_t DirectoryScanner::CountResultsWithTitleId(
    const std::unordered_set<uint32_t>& title_ids) const {
  if (title_ids.empty()) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(mu_);
  size_t count = 0;
  for (const auto& g : results_) {
    if (title_ids.count(g.title_id)) {
      ++count;
    }
  }
  return count;
}

void DirectoryScanner::Run(std::filesystem::path root) {
  XELOGI("DirectoryScanner: scan start, root='{}'",
         xe::strip_pii(xe::path_to_utf8(root)));
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec) || ec) {
    XELOGW("DirectoryScanner: not a directory: {} ({})",
           xe::strip_pii(xe::path_to_utf8(root)),
           ec ? ec.message() : "is_directory=false");
    std::lock_guard<std::mutex> lock(mu_);
    progress_.done = true;
    return;
  }

  unsigned hwc = std::thread::hardware_concurrency();
  if (hwc == 0) {
    hwc = 4;
  }
  unsigned worker_count = std::max(1u, (hwc * 3) / 4);
  XELOGI("DirectoryScanner: spawning {} worker(s) (hwc={})", worker_count, hwc);

  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    pending_.push_back(std::move(root));
  }

  workers_.reserve(worker_count);
  for (unsigned i = 0; i < worker_count; ++i) {
    workers_.emplace_back(&DirectoryScanner::WorkerLoop, this);
  }
  for (auto& t : workers_) {
    t.join();
  }
  workers_.clear();

  Progress final_snapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    progress_.current_directory.clear();
    progress_.cancelled = cancel_requested_.load();
    progress_.done = true;
    final_snapshot = progress_;
  }
  XELOGI("DirectoryScanner: scan {} — dirs={}, files_probed={}, games_found={}",
         final_snapshot.cancelled ? "cancelled" : "complete",
         final_snapshot.directories_scanned, final_snapshot.files_probed,
         final_snapshot.games_found);
}

void DirectoryScanner::WorkerLoop() {
  while (true) {
    std::filesystem::path dir;
    {
      std::unique_lock<std::mutex> lock(queue_mu_);
      queue_cv_.wait(lock, [this] {
        return cancel_requested_.load() || !pending_.empty() || inflight_ == 0;
      });
      if (cancel_requested_.load()) {
        queue_cv_.notify_all();
        return;
      }
      if (pending_.empty()) {
        // Drain complete and nobody else is processing — every worker
        // observing this condition exits.
        queue_cv_.notify_all();
        return;
      }
      dir = std::move(pending_.front());
      pending_.pop_front();
      inflight_++;
    }

    try {
      ScanDirectory(dir);
    } catch (const std::exception& e) {
      XELOGE("DirectoryScanner: worker exception in '{}': {}",
             xe::strip_pii(xe::path_to_utf8(dir)), xe::strip_pii(e.what()));
    } catch (...) {
      XELOGE("DirectoryScanner: worker unknown exception in '{}'",
             xe::strip_pii(xe::path_to_utf8(dir)));
    }

    {
      std::lock_guard<std::mutex> lock(queue_mu_);
      inflight_--;
      if (pending_.empty() && inflight_ == 0) {
        queue_cv_.notify_all();
      }
    }
  }
}

void DirectoryScanner::EnqueueSubdirs(
    std::vector<std::filesystem::path> subdirs) {
  if (subdirs.empty()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    for (auto& sub : subdirs) {
      pending_.push_back(std::move(sub));
    }
  }
  queue_cv_.notify_all();
}

void DirectoryScanner::SetCurrentDirectory(const std::filesystem::path& dir) {
  std::lock_guard<std::mutex> lock(mu_);
  progress_.current_directory = dir;
  progress_.directories_scanned++;
}

void DirectoryScanner::RecordGame(DiscoveredGame game) {
  std::lock_guard<std::mutex> lock(mu_);
  results_.push_back(std::move(game));
  progress_.games_found = results_.size();
}

void DirectoryScanner::ScanDirectory(const std::filesystem::path& dir) {
  if (cancel_requested_.load()) {
    return;
  }
  SetCurrentDirectory(dir);
  XELOGD("DirectoryScanner: enter '{}'", xe::strip_pii(xe::path_to_utf8(dir)));

  std::vector<std::filesystem::path> files;
  std::vector<std::filesystem::path> subdirs;
  size_t entries_skipped = 0;
  {
    std::error_code ec;
    auto opts = std::filesystem::directory_options::skip_permission_denied;
    auto it = std::filesystem::directory_iterator(dir, opts, ec);
    if (ec) {
      XELOGW("DirectoryScanner: cannot open '{}': {}",
             xe::strip_pii(xe::path_to_utf8(dir)), ec.message());
      return;
    }
    auto end = std::filesystem::directory_iterator();
    while (it != end) {
      if (cancel_requested_.load()) {
        return;
      }
      const auto& entry = *it;
      std::error_code ftec;
      bool is_file = entry.is_regular_file(ftec);
      if (ftec) {
        XELOGW("DirectoryScanner: is_regular_file failed on '{}': {}",
               xe::strip_pii(xe::path_to_utf8(entry.path())), ftec.message());
        ftec.clear();
        entries_skipped++;
      } else if (is_file) {
        files.push_back(entry.path());
      } else {
        bool is_dir = entry.is_directory(ftec);
        if (ftec) {
          XELOGW("DirectoryScanner: is_directory failed on '{}': {}",
                 xe::strip_pii(xe::path_to_utf8(entry.path())), ftec.message());
          ftec.clear();
          entries_skipped++;
        } else if (is_dir) {
          if (IsGodDataDirectory(entry.path())) {
            XELOGD("DirectoryScanner:   skip god data dir '{}'",
                   xe::strip_pii(xe::path_to_utf8(entry.path())));
          } else {
            subdirs.push_back(entry.path());
          }
        } else {
          // Symlink, block device, FIFO, etc.
          entries_skipped++;
        }
      }
      it.increment(ec);
      if (ec) {
        XELOGW("DirectoryScanner: increment failed in '{}': {}",
               xe::strip_pii(xe::path_to_utf8(dir)), ec.message());
        ec.clear();
      }
    }
  }

  XELOGD("DirectoryScanner: '{}' contains {} files, {} subdirs ({} skipped)",
         xe::strip_pii(xe::path_to_utf8(dir)), files.size(), subdirs.size(),
         entries_skipped);

  std::sort(files.begin(), files.end());
  std::sort(subdirs.begin(), subdirs.end());

  bool found_xex = false;
  size_t phase1_hits = 0;

  // Phase 1: only default.xex is a launchable XEX; other .xex files are
  // modules/companions and skipped. The same goes for an original Xbox
  // default.xbe. ISO/ZAR collected as multi-game.
  for (const auto& path : files) {
    if (cancel_requested_.load()) {
      return;
    }
    std::string ext = ToLowerAscii(xe::path_to_utf8(path.extension()));
    if (!IsKnownLaunchableExtension(ext)) {
      continue;
    }
    if ((ext == ".xex" || ext == ".xbe") &&
        ToLowerAscii(xe::path_to_utf8(path.stem())) != "default") {
      XELOGD("DirectoryScanner:   skip non-default {} '{}'", ext,
             xe::strip_pii(xe::path_to_utf8(path)));
      continue;
    }
    // A default executable makes the directory a game, but a disc for both
    // consoles holds a default.xbe and a default.xex, one game each.
    if (found_xex && ext != ".xex" && ext != ".xbe") {
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      progress_.files_probed++;
    }
    XELOGD("DirectoryScanner:   probe {} '{}'", ext,
           xe::strip_pii(xe::path_to_utf8(path)));
    try {
      if (ext == ".xex") {
        if (auto m = vfs::ExtractXexMetadata(path)) {
          XELOGD("DirectoryScanner:     -> xex hit, title_id={:08X}",
                 m->title_id);
          RecordGame(MakeFromXex(path, "xex", *m));
          found_xex = true;
          phase1_hits++;
          continue;
        }
        XELOGD("DirectoryScanner:     -> xex miss");
      } else if (ext == ".xbe") {
        if (auto m = vfs::ExtractXbeMetadata(path)) {
          XELOGD("DirectoryScanner:     -> xbe hit, title_id={:08X}",
                 m->title_id);
          RecordGame(MakeFromXbe(path, "xbe", *m));
          found_xex = true;
          phase1_hits++;
          continue;
        }
        XELOGD("DirectoryScanner:     -> xbe miss");
      } else if (ext == ".iso") {
        if (auto m = vfs::ExtractIsoMetadata(path)) {
          XELOGD("DirectoryScanner:     -> iso hit, title_id={:08X}",
                 m->title_id);
          RecordGame(MakeFromXex(path, "iso", *m));
          phase1_hits++;
        } else if (auto xbe = vfs::ExtractXbeMetadata(path)) {
          XELOGD(
              "DirectoryScanner:     -> original Xbox iso hit, "
              "title_id={:08X}",
              xbe->title_id);
          RecordGame(MakeFromXbe(path, "iso", *xbe));
          phase1_hits++;
        } else {
          XELOGD("DirectoryScanner:     -> iso miss");
        }
      } else if (ext == ".zar") {
        if (auto m = vfs::ExtractZarMetadata(path)) {
          XELOGD("DirectoryScanner:     -> zar hit, title_id={:08X}",
                 m->title_id);
          RecordGame(MakeFromXex(path, "zar", *m));
          phase1_hits++;
        } else {
          XELOGD("DirectoryScanner:     -> zar miss");
        }
      }
    } catch (const std::exception& e) {
      XELOGW("DirectoryScanner: extractor threw on '{}': {}",
             xe::strip_pii(xe::path_to_utf8(path)), xe::strip_pii(e.what()));
    } catch (...) {
      XELOGW("DirectoryScanner: extractor threw on '{}' (unknown)",
             xe::strip_pii(xe::path_to_utf8(path)));
    }
  }

  // Phase 2: STFS probe on non-{xex,iso,zar} files (GOD has no extension).
  // Multi-game like ISO/ZAR — collect every hit, no early-out.
  size_t phase2_probed = 0;
  size_t phase2_hits = 0;
  if (!found_xex) {
    for (const auto& path : files) {
      if (cancel_requested_.load()) {
        return;
      }
      std::string ext = ToLowerAscii(xe::path_to_utf8(path.extension()));
      if (IsKnownLaunchableExtension(ext)) {
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        progress_.files_probed++;
      }
      phase2_probed++;
      try {
        if (auto m = vfs::ExtractStfsMetadata(path, language_)) {
          if (m->content_type == uint32_t(XContentType::kXboxTitle)) {
            if (auto xbe = vfs::ExtractXbeMetadata(path)) {
              XELOGD(
                  "DirectoryScanner:   original Xbox stfs hit '{}' — "
                  "title_id={:08X}, name='{}'",
                  xe::strip_pii(xe::path_to_utf8(path)), xbe->title_id,
                  xbe->title_name);
              DiscoveredGame g = MakeFromStfs(path, *m);
              ApplyXbe(g, *xbe);
              RecordGame(std::move(g));
              phase2_hits++;
              continue;
            }
          }
          if (m->content_type == uint32_t(XContentType::kMarketplaceContent) &&
              m->title_id == kernel::kXN_2002) {
            if (auto game = kernel::xam::ReadIndieGame(path)) {
              XELOGD(
                  "DirectoryScanner:   indie stfs hit '{}' — title_id={:08X}, "
                  "name='{}'",
                  xe::strip_pii(xe::path_to_utf8(path)), game->title_id,
                  game->name);
              DiscoveredGame g = MakeFromStfs(path, *m);
              ApplyIndieGame(g, std::move(*game));
              RecordGame(std::move(g));
              phase2_hits++;
              continue;
            }
          }
          // The XNA title launcher's content isn't a game of its own.
          if (m->title_id == kernel::kXN_2002) {
            XELOGD(
                "DirectoryScanner:   stfs filtered '{}' — XNA title launcher",
                xe::strip_pii(xe::path_to_utf8(path)));
            continue;
          }
          if (!IsLaunchableContentType(m->content_type)) {
            XELOGD(
                "DirectoryScanner:   stfs filtered '{}' — content_type="
                "0x{:08X} ({}), name='{}'",
                xe::strip_pii(xe::path_to_utf8(path)), m->content_type,
                ResolveContentTypeName(m->content_type), m->display_name);
            continue;
          }
          XELOGD(
              "DirectoryScanner:   stfs hit '{}' — title_id={:08X}, "
              "content_type=0x{:08X}, name='{}'",
              xe::strip_pii(xe::path_to_utf8(path)), m->title_id,
              m->content_type, m->display_name);
          RecordGame(MakeFromStfs(path, *m));
          phase2_hits++;
        } else {
          XELOGD("DirectoryScanner:   stfs miss '{}'",
                 xe::strip_pii(xe::path_to_utf8(path)));
        }
      } catch (const std::exception& e) {
        XELOGW("DirectoryScanner: stfs probe threw on '{}': {}",
               xe::strip_pii(xe::path_to_utf8(path)), xe::strip_pii(e.what()));
      } catch (...) {
        XELOGW("DirectoryScanner: stfs probe threw on '{}' (unknown)",
               xe::strip_pii(xe::path_to_utf8(path)));
      }
    }
  }

  XELOGD(
      "DirectoryScanner: leave '{}' — found_xex={}, phase1_hits={}, "
      "phase2_probed={}, phase2_hits={}, recurse={}",
      xe::strip_pii(xe::path_to_utf8(dir)), found_xex, phase1_hits,
      phase2_probed, phase2_hits, !found_xex);

  // Phase 3: recurse only when no XEX found in this directory.
  if (!found_xex && !cancel_requested_.load()) {
    EnqueueSubdirs(std::move(subdirs));
  }
}

}  // namespace app
}  // namespace xe
