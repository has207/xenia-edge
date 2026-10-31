/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_EMULATOR_H_
#define XENIA_EMULATOR_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xenia/apu/audio_media_player.h"
#include "xenia/base/delegate.h"
#include "xenia/base/exception_handler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/game_info_database.h"
#include "xenia/kernel/util/xlast.h"
#include "xenia/memory.h"
#include "xenia/patcher/patcher.h"
#include "xenia/patcher/plugin_loader.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/vfs/xbe_metadata.h"
#include "xenia/xbox.h"

namespace xe {
namespace apu {
class AudioSystem;
}  // namespace apu
namespace cpu {
class ExportResolver;
class Processor;
class ThreadState;
}  // namespace cpu
namespace gpu {
class GraphicsSystem;
}  // namespace gpu
namespace hid {
class InputDriver;
class InputSystem;
}  // namespace hid
namespace ui {
class ImGuiDrawer;
class Window;
}  // namespace ui
}  // namespace xe

namespace xe {

constexpr fourcc_t kEmulatorSaveSignature = make_fourcc("XSAV");
static constexpr std::string_view kDefaultGameSymbolicLink = "GAME:";
static constexpr std::string_view kDefaultPartitionSymbolicLink = "D:";
static constexpr std::string_view kDefaultUpdateSymbolicLink = "UPDATE:";

// The original Xbox backwards compatibility emulator.
constexpr uint32_t kXeFuTitleId = 0xFFFE07D2;

// The main type that runs the whole emulator.
// This is responsible for initializing and managing all the various subsystems.
class Emulator {
 public:
  // This is the class for the top-level callbacks. They may be called in an
  // undefined order, so among them there must be no dependencies on each other,
  // especially hierarchical ones. If hierarchical handling is needed, for
  // instance, if a specific implementation of a subsystem needs to handle
  // changes, but the entire implementation must be reloaded, the implementation
  // in this example _must not_ register / unregister its own callback - rather,
  // the proper ordering and hierarchy should be constructed in a single
  // callback (in this example, for the whole subsystem).
  //
  explicit Emulator(const std::filesystem::path& command_line,
                    const std::filesystem::path& storage_root,
                    const std::filesystem::path& content_root,
                    const std::filesystem::path& cache_root);
  ~Emulator();

  // Full command line used when launching the process.
  const std::filesystem::path& command_line() const { return command_line_; }

  // Folder persistent internal emulator data is stored in.
  const std::filesystem::path& storage_root() const { return storage_root_; }

  // Folder XeFu and its xbox.xex are in, shown to them as the system partition
  // Compatibility folder.
  std::filesystem::path xefu_path() const;
  // Folder shown to XeFu as the user partition's Compatibility folder, where
  // it keeps original Xbox saves and downloads under Xbox1.
  std::filesystem::path compatibility_path() const {
    return storage_root_ / "compatibility";
  }
  // The file in xefu_path() an original Xbox game needed and didn't find, for
  // the UI to explain once. Empty when none is missing.
  std::string TakeMissingXeFuFile() {
    return std::exchange(missing_xefu_file_, {});
  }
  void ReportMissingXeFuFile(std::string file) {
    missing_xefu_file_ = std::move(file);
  }
  // The original Xbox game XeFu runs, the file it was launched from and what
  // its executable says about it. Empty when the executable can't be read.
  const std::filesystem::path& xbox_disc_path() const {
    return xbox_disc_path_;
  }
  const std::optional<vfs::XbeMetadata>& xbox_game() const {
    return xbox_game_;
  }
  // The title id and version of the executable XeFu is running. A title
  // update, or a game starting another executable, makes it differ from
  // xbox_game(), which still names the release. Empty until the title starts.
  std::optional<vfs::XbeMetadata> running_xbe() const;
  void SetRunningXbe(vfs::XbeMetadata xbe);
  // Puts an original Xbox game in the drive for XeFu.
  void InsertXboxGame(const std::filesystem::path& path);
  // Whether the original Xbox game stays in the drive for a launch of path, as
  // it does for the XeFu build xbox.xex picks.
  bool KeepsXboxGame(const std::filesystem::path& path) const;
  // The title the running title's per-game config is kept under: the original
  // Xbox game while XeFu runs it, none when XeFu runs without one.
  uint32_t game_config_title_id() const {
    if (title_id() != kXeFuTitleId) {
      return title_id();
    }
    return xbox_game_ ? xbox_game_->title_id : 0;
  }

  // Folder guest content is stored in.
  const std::filesystem::path& content_root() const { return content_root_; }

  // Folder files safe to remove without significant side effects are stored in.
  const std::filesystem::path& cache_root() const { return cache_root_; }

  // Host path of the most recently launched title.
  const std::filesystem::path& last_launch_path() const {
    return last_launch_path_;
  }
  // The package inside last_launch_path() that title runs from, if any.
  const std::string& last_launch_package() const {
    return last_launch_package_;
  }

  // Name of the title in the default language.
  const std::string& title_name() const { return title_name_; }
  // For a launcher that picks its game after launch.
  void SetTitleName(std::string title_name) {
    title_name_ = std::move(title_name);
    on_title_name_change();
  }

  // Version of the title as a string.
  const std::string& title_version() const { return title_version_; }

  // Currently running title ID
  uint32_t title_id() const {
    return !title_id_.has_value() ? 0 : title_id_.value();
  }

  // Are we currently running a title?
  bool is_title_open() const { return title_id_.has_value(); }
  // Whether a relaunch or reset is between titles.
  bool is_relaunching() const { return relaunching_; }

  uint32_t main_thread_id();

  // Window used for displaying graphical output. Can be null.
  ui::Window* display_window() const { return display_window_; }

  // ImGui drawer for various kinds of dialogs requested by the guest. Can be
  // null.
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_; }

  // Guest memory system modelling the RAM (both virtual and physical) of the
  // system.
  Memory* memory() const { return memory_.get(); }

  // Virtualized processor that can execute PPC code.
  cpu::Processor* processor() const { return processor_.get(); }

  // Audio hardware emulation for decoding and playback.
  apu::AudioSystem* audio_system() const { return audio_system_.get(); }

  // Xbox media player (XMP) emulation for WMA and MP3 playback.
  apu::AudioMediaPlayer* audio_media_player() const {
    return audio_media_player_.get();
  }

  // GPU emulation for command list processing.
  gpu::GraphicsSystem* graphics_system() const {
    return graphics_system_.get();
  }

  // Human-interface Device (HID) adapters for controllers.
  hid::InputSystem* input_system() const { return input_system_.get(); }

  // Kernel function export table used to resolve exports when JITing code.
  cpu::ExportResolver* export_resolver() const {
    return export_resolver_.get();
  }

  // File systems mapped to disc images, folders, etc for games and save data.
  vfs::VirtualFileSystem* file_system() const { return file_system_.get(); }

  // The 'kernel', tracking all kernel objects and other state.
  // This is effectively the guest operating system.
  kernel::KernelState* kernel_state() const { return kernel_state_.get(); }

  patcher::Patcher* patcher() const { return patcher_.get(); }

  patcher::PluginLoader* plugin_loader() const { return plugin_loader_.get(); }

  kernel::util::GameInfoDatabase* game_info_database() const {
    return game_info_database_.get();
  }
  // Bare-essentials init: memory, cpu, vfs, kernel state, input system shell.
  // Stores the subsystem factories but does not create graphics/audio or
  // attach input drivers — call SetupSubsystems for that, after any per-game
  // cvar overrides are in place.
  X_STATUS Setup(
      ui::Window* display_window, ui::ImGuiDrawer* imgui_drawer,
      bool require_cpu_backend,
      std::function<std::unique_ptr<apu::AudioSystem>(cpu::Processor*)>
          audio_system_factory,
      std::function<std::unique_ptr<gpu::GraphicsSystem>()>
          graphics_system_factory,
      std::function<std::vector<std::unique_ptr<hid::InputDriver>>(ui::Window*)>
          input_driver_factory);

  // Creates and starts graphics_system + audio_system from stored factories,
  // and attaches input drivers. Call after Setup and after any per-game cvar
  // overrides have been loaded.
  X_STATUS SetupSubsystems();

  // Tears down graphics_system, audio_system, and input drivers. The bare
  // emulator (kernel, vfs, input_system shell) stays alive.
  void ShutdownSubsystems();

  // gpu/apu cvar values the live subsystems were built with; empty before
  // first SetupSubsystems. Used to detect a backend change driven by per-game
  // overrides so the next launch can route through a fresh process.
  const std::string& active_gpu_backend() const { return active_gpu_backend_; }
  const std::string& active_apu_backend() const { return active_apu_backend_; }

  // Tears down all subsystems. Called by the destructor and by RelaunchTitle.
  void Shutdown();

  // Mounts scratch, cache, and devkit drives based on cvars.
  void MountStandardDrives();

  // Terminates the currently running title.
  X_STATUS TerminateTitle();

  const std::unique_ptr<vfs::Device> CreateVfsDevice(
      const std::filesystem::path& path, const std::string_view mount_path);

  X_STATUS MountPath(const std::filesystem::path& path,
                     const std::string_view mount_path);
  // Mounts |device| as the title's game drive.
  X_STATUS MountDevice(std::unique_ptr<vfs::Device> device,
                       const std::string_view mount_path);

  enum class FileSignatureType {
    XEX0,
    XEXQ,
    XEXH,
    XEX25,
    XEX1,
    XEX2,
    ELF,
    CON,
    LIVE,
    PIRS,
    XISO,
    XBE,
    ZAR,
    EXE,
    Unknown
  };

  // Determine the executable signature
  FileSignatureType GetFileSignature(const std::filesystem::path& path);

  // Launches a game from the given file path.
  // This will attempt to infer the type of the given file (such as an iso, etc)
  // using heuristics. With |package_path| the disc at |path| is mounted as the
  // drive and the game runs from the package at that path on it.
  X_STATUS LaunchPath(const std::filesystem::path& path,
                      std::string_view package_path = {});

  // Launches a game from a .xex file by mounting the containing folder as if it
  // was an extracted STFS container.
  X_STATUS LaunchXexFile(const std::filesystem::path& path);

  // Launches a game from a disc image file (.iso, etc).
  X_STATUS LaunchDiscImage(const std::filesystem::path& path);

  // Launches an original Xbox game through XeFu from its .xbe file, a disc
  // image or an Xbox Original package, with |xbe_name| the game's file in it.
  X_STATUS LaunchXboxOriginal(const std::filesystem::path& path,
                              std::string_view xbe_name);

  // Launches a game from a disc archive file (.zar, etc).
  X_STATUS LaunchDiscArchive(const std::filesystem::path& path);

  // Launches a game from an STFS container file.
  X_STATUS LaunchStfsContainer(const std::filesystem::path& path);

  // Launches a game from the STFS package at |package_path| on the disc at
  // |path|.
  X_STATUS LaunchDiscPackage(const std::filesystem::path& path,
                             std::string_view package_path);

  X_STATUS LaunchDefaultModule(const std::filesystem::path& path);

  enum class InstallState : uint8_t {
    preparing,
    pending,
    installing,
    installed,
    failed
  };

  constexpr static std::string_view installStateStringName[5] = {
      "Preparing", "Pending", "Installing", "Success", "Failed"};

  struct ContentInstallEntry {
    ContentInstallEntry(std::filesystem::path path) : path_(path) {};

    // Move constructor
    ContentInstallEntry(ContentInstallEntry&& other) noexcept
        : name_(std::move(other.name_)),
          path_(std::move(other.path_)),
          data_installation_path_(std::move(other.data_installation_path_)),
          header_installation_path_(std::move(other.header_installation_path_)),
          content_size_(other.content_size_.load()),
          currently_installed_size_(other.currently_installed_size_.load()),
          content_type_(other.content_type_),
          installation_state_(other.installation_state_),
          installation_result_(other.installation_result_),
          installation_error_message_(
              std::move(other.installation_error_message_)),
          icon_data_(std::move(other.icon_data_)),
          cancelled_(other.cancelled_.load()),
          mutex_(std::move(other.mutex_)) {}

    // Move assignment
    ContentInstallEntry& operator=(ContentInstallEntry&& other) noexcept {
      if (this != &other) {
        name_ = std::move(other.name_);
        path_ = std::move(other.path_);
        data_installation_path_ = std::move(other.data_installation_path_);
        header_installation_path_ = std::move(other.header_installation_path_);
        content_size_.store(other.content_size_.load());
        currently_installed_size_.store(other.currently_installed_size_.load());
        content_type_ = other.content_type_;
        installation_state_ = other.installation_state_;
        installation_result_ = other.installation_result_;
        installation_error_message_ =
            std::move(other.installation_error_message_);
        icon_data_ = std::move(other.icon_data_);
        cancelled_.store(other.cancelled_.load());
        mutex_ = std::move(other.mutex_);
      }
      return *this;
    }

    // Delete copy constructor and copy assignment
    ContentInstallEntry(const ContentInstallEntry&) = delete;
    ContentInstallEntry& operator=(const ContentInstallEntry&) = delete;

    std::string name_{};
    std::filesystem::path path_;
    std::filesystem::path data_installation_path_;
    std::filesystem::path header_installation_path_;

    std::atomic<uint64_t> content_size_{0};
    std::atomic<uint64_t> currently_installed_size_{0};
    XContentType content_type_{};

    InstallState installation_state_{};
    X_STATUS installation_result_{};
    std::string installation_error_message_{};

    std::vector<uint8_t> icon_data_;
    std::atomic<bool> cancelled_{false};

    // Guards every non-atomic field the install thread writes and Tick reads.
    std::unique_ptr<std::mutex> mutex_ = std::make_unique<std::mutex>();
  };

  // Whether content holds title data from before profiles for DataMigration.
  bool HasDataToMigrate() const;
  // Migrates data from content to content/xuid with respect to common data.
  // Returns how many moves and copies failed.
  uint32_t DataMigration(const uint64_t xuid);

  X_STATUS ProcessContentPackageHeader(const std::filesystem::path& path,
                                       ContentInstallEntry& installation_info);

  // Extract content of package to content specific directory.
  X_STATUS InstallContentPackage(const std::filesystem::path& path,
                                 ContentInstallEntry& installation_info);

  void Pause();
  void Resume();
  bool is_paused() const { return paused_; }
  bool SaveToFile(const std::filesystem::path& path);
  bool RestoreFromFile(const std::filesystem::path& path);

  // Full in-process relaunch: terminates threads, Shutdown(), Setup(),
  // then launches with new params. Must be called from a non-guest thread.
  // keep_xbox_game leaves an original Xbox game in the drive for the XeFu
  // build a guest launches. A launch from the UI is a fresh one.
  void RelaunchTitle(const std::string& host_path,
                     const std::string& launch_module, uint32_t launch_flags,
                     std::vector<uint8_t> launch_data,
                     bool keep_xbox_game = true,
                     const std::string& package_path = {});

  // Stops the current title and returns the kernel to a fresh, idle state
  // (no title loaded). Must be called from a non-guest thread.
  void ResetTitle();

  struct TitleDisc {
    std::string label;
    std::filesystem::path path;
  };
  // The app sets these so the core can reach the game library it can't include.
  using DiscProvider = std::function<std::vector<TitleDisc>(uint32_t title_id)>;
  void set_disc_provider(DiscProvider provider) {
    disc_provider_ = std::move(provider);
  }

  using DiscRecorder =
      std::function<void(uint32_t title_id, const std::filesystem::path& path)>;
  void set_disc_recorder(DiscRecorder recorder) {
    disc_recorder_ = std::move(recorder);
  }
  void RecordDisc(uint32_t title_id, const std::filesystem::path& path) {
    if (disc_recorder_) {
      disc_recorder_(title_id, path);
    }
  }

  // Disc in the drive, 1-based; follows XamSwapDisc. 0 when unknown.
  uint8_t current_disc_number() const { return current_disc_number_; }
  void set_current_disc_number(uint8_t disc_number) {
    current_disc_number_ = disc_number;
  }

  // The game can request another title to be loaded.
  const std::filesystem::path GetNewDiscPath(std::string window_message = "");

  void WaitUntilExit();

 public:
  xe::Delegate<uint32_t, const std::string_view> on_launch;
  // A relaunch's title failed to start.
  xe::Delegate<> on_relaunch_failed;
  xe::Delegate<bool> on_shader_storage_initialization;
  xe::Delegate<bool> on_shader_compilation;
  xe::Delegate<> on_patch_apply;
  xe::Delegate<> on_title_name_change;
  xe::Delegate<> on_terminate;
  xe::Delegate<> on_exit;

  // Fired as a relaunch or reset starts, while the title's threads still run.
  xe::Delegate<> on_title_closing;
  // Fired before Shutdown() during relaunch, while subsystems are still alive.
  xe::Delegate<> on_before_shutdown;

  // Called when XamLoaderLaunchTitle requests launching a new title.
  // The callback should spawn a new process with the given parameters.
  // Parameters: host_path, launch_module, launch_flags, launch_data (hex),
  // the original Xbox game left in the drive for it, and the package inside
  // host_path it runs from
  using LaunchNewTitleCallback = std::function<void(
      const std::string&, const std::string&, uint32_t, const std::string&,
      const std::string&, const std::string&)>;
  LaunchNewTitleCallback on_launch_new_title() const {
    return on_launch_new_title_;
  }
  void set_on_launch_new_title(LaunchNewTitleCallback callback) {
    on_launch_new_title_ = std::move(callback);
  }

  // Called when the game requests an exit to the dashboard. Returns true if
  // the title is being reset in-process (the calling guest thread is about to
  // be terminated), false if process exit was scheduled instead.
  using ExitToDashboardCallback = std::function<bool()>;
  ExitToDashboardCallback on_exit_to_dashboard() const {
    return on_exit_to_dashboard_;
  }
  void set_on_exit_to_dashboard(ExitToDashboardCallback callback) {
    on_exit_to_dashboard_ = std::move(callback);
  }

  // Called when XamSwapDisc successfully swaps to a new disc.
  // Parameters: new_disc_number
  using DiscSwapCallback = std::function<void(uint8_t)>;
  DiscSwapCallback on_disc_swap() const { return on_disc_swap_; }
  void set_on_disc_swap(DiscSwapCallback callback) {
    on_disc_swap_ = std::move(callback);
  }

 private:
  enum : uint64_t { EmulatorFlagDisclaimerAcknowledged = 1ULL << 0 };
  static uint64_t GetPersistentEmulatorFlags();
  static void SetPersistentEmulatorFlags(uint64_t new_flags);
  static bool ExceptionCallbackThunk(Exception* ex, void* data);
  bool ExceptionCallback(Exception* ex);

  std::string RemountAndResolveLaunchPath(const std::string& launch_path);
  std::string FindLaunchModule();

  // Applies the media_type cvar override, if any.
  void SetDeploymentType(XDeploymentType detected_type);

  X_STATUS CompleteLaunch(const std::filesystem::path& path,
                          const std::string_view module_path);
  // UI-thread half of CompleteLaunch, ends with the main thread suspended.
  X_STATUS PrepareLaunch(const std::filesystem::path& path,
                         const std::string_view module_path);
  // Forgets the title being launched, leaving none open.
  void ResetTitleState();
  // Sets the emulator up again after Shutdown(). Nothing runs without it, so a
  // failure is fatal.
  void SetupAgain();
  // Takes the original Xbox game XeFu runs out of the drive.
  void EjectXboxGame() {
    xbox_disc_path_.clear();
    xbox_game_.reset();
  }

  std::filesystem::path command_line_;
  std::filesystem::path last_launch_path_;  // persists across relaunch
  std::string last_launch_package_;
  // The original Xbox game XeFu runs, its .xbe file, disc image or package,
  // shown to XeFu as the disc. Persists across launches from xefu_path.
  std::filesystem::path xbox_disc_path_;
  std::optional<vfs::XbeMetadata> xbox_game_;
  // Set by the UI, cleared by whichever thread launches.
  mutable std::mutex running_xbe_mutex_;
  std::optional<vfs::XbeMetadata> running_xbe_;
  std::string missing_xefu_file_;
  DiscProvider disc_provider_;
  DiscRecorder disc_recorder_;
  std::filesystem::path storage_root_;
  std::filesystem::path content_root_;
  std::filesystem::path cache_root_;

  std::string title_name_;
  std::string title_version_;

  ui::Window* display_window_ = nullptr;
  ui::ImGuiDrawer* imgui_drawer_ = nullptr;

  std::unique_ptr<Memory> memory_;

  std::unique_ptr<cpu::Processor> processor_;
  std::unique_ptr<apu::AudioSystem> audio_system_;
  std::unique_ptr<apu::AudioMediaPlayer> audio_media_player_;
  std::unique_ptr<gpu::GraphicsSystem> graphics_system_;
  std::unique_ptr<hid::InputSystem> input_system_;

  std::unique_ptr<cpu::ExportResolver> export_resolver_;
  std::unique_ptr<vfs::VirtualFileSystem> file_system_;
  std::unique_ptr<patcher::Patcher> patcher_;
  std::unique_ptr<patcher::PluginLoader> plugin_loader_;

  std::unique_ptr<kernel::KernelState> kernel_state_;

  kernel::object_ref<kernel::XThread> main_thread_;
  kernel::object_ref<kernel::XHostThread> plugin_loader_thread_;
  std::optional<uint32_t> title_id_;  // Currently running title ID
  uint8_t current_disc_number_ = 0;
  std::unique_ptr<kernel::util::GameInfoDatabase> game_info_database_;

  bool paused_;
  bool restoring_;
  std::atomic<bool> relaunching_{false};
  // Held across a launch and a whole relaunch so title teardown waits for a
  // launch to finish. Recursive, as a relaunch launches under it.
  std::recursive_mutex launch_mutex_;
  // Relaunch requests so far, to drop one a newer request replaced while it
  // waited for the lock.
  std::atomic<uint64_t> relaunch_requests_{0};
  threading::Fence restore_fence_;  // Fired on restore finish.

  // Persisted across Shutdown/Setup for relaunch.
  bool require_cpu_backend_ = false;
  std::function<std::unique_ptr<apu::AudioSystem>(cpu::Processor*)>
      audio_system_factory_;
  std::function<std::unique_ptr<gpu::GraphicsSystem>()>
      graphics_system_factory_;
  std::function<std::vector<std::unique_ptr<hid::InputDriver>>(ui::Window*)>
      input_driver_factory_;

  std::string active_gpu_backend_;
  std::string active_apu_backend_;

  LaunchNewTitleCallback on_launch_new_title_;
  ExitToDashboardCallback on_exit_to_dashboard_;
  DiscSwapCallback on_disc_swap_;
};

}  // namespace xe

#endif  // XENIA_EMULATOR_H_
