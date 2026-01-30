/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/simple_config_dialog_qt.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "xenia/app/emulator_window.h"
#include "xenia/base/cvar.h"
#include "xenia/config.h"
#include "xenia/ui/config_dialog_qt.h"
#include "xenia/ui/config_helpers.h"
#include "xenia/ui/qt_util.h"

namespace xe {
namespace app {

using xe::ui::SafeQString;
using xe::ui::SafeStdString;

SimpleConfigDialogQt::SimpleConfigDialogQt(QWidget* parent,
                                           EmulatorWindow* emulator_window)
    : QDialog(parent), emulator_window_(emulator_window) {
  setWindowTitle("Emulator Settings");
  setMinimumWidth(500);
  SetupUI();
  LoadConfigValues();
}

SimpleConfigDialogQt::~SimpleConfigDialogQt() = default;

void SimpleConfigDialogQt::ReloadConfigValues() { LoadConfigValues(); }

void SimpleConfigDialogQt::SetupUI() {
  auto* main_layout = new QVBoxLayout(this);

  // Graphics section
  auto* graphics_group = new QGroupBox("Graphics", this);
  auto* graphics_layout = new QFormLayout(graphics_group);

  const auto& enum_options = ui::GetKnownEnumOptions();

  auto* gpu_combo = new QComboBox(this);
  auto gpu_it = enum_options.find("gpu");
  if (gpu_it != enum_options.end()) {
    for (const auto& opt : gpu_it->second) {
      gpu_combo->addItem(SafeQString(opt));
    }
  }
  options_["gpu"].cvar_name = "gpu";
  options_["gpu"].editor_widget = gpu_combo;
  options_["gpu"].label_widget = new QLabel("Graphics Backend:", this);
  connect(gpu_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &SimpleConfigDialogQt::OnValueChanged);
  graphics_layout->addRow(options_["gpu"].label_widget, gpu_combo);

  auto* render_path_combo = new QComboBox(this);
  auto render_path_it = enum_options.find("render_target_path");
  if (render_path_it != enum_options.end()) {
    for (const auto& opt : render_path_it->second) {
      render_path_combo->addItem(SafeQString(opt));
    }
  }
  options_["render_target_path"].cvar_name = "render_target_path";
  options_["render_target_path"].editor_widget = render_path_combo;
  options_["render_target_path"].label_widget = new QLabel("Rendering:", this);
  connect(render_path_combo,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &SimpleConfigDialogQt::OnValueChanged);
  graphics_layout->addRow(options_["render_target_path"].label_widget,
                          render_path_combo);

  auto* scale_combo = new QComboBox(this);
  scale_combo->addItem("Native", 1);
  scale_combo->addItem("2x", 2);
  scale_combo->addItem("3x", 3);
  scale_combo->addItem("4x", 4);
  scale_combo->addItem("5x", 5);
  scale_combo->addItem("6x", 6);
  scale_combo->addItem("7x", 7);
  scale_combo->addItem("8x", 8);
  options_["draw_resolution_scale"].cvar_name = "draw_resolution_scale";
  options_["draw_resolution_scale"].editor_widget = scale_combo;
  options_["draw_resolution_scale"].label_widget =
      new QLabel("Resolution Scale:", this);
  connect(scale_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &SimpleConfigDialogQt::OnValueChanged);
  graphics_layout->addRow(options_["draw_resolution_scale"].label_widget,
                          scale_combo);

  auto* fps_spin = new QSpinBox(this);
  fps_spin->setMinimum(0);
  fps_spin->setMaximum(1000);
  fps_spin->setSpecialValueText("Unlimited");
  options_["framerate_limit"].cvar_name = "framerate_limit";
  options_["framerate_limit"].editor_widget = fps_spin;
  options_["framerate_limit"].label_widget =
      new QLabel("Framerate Limit:", this);
  connect(fps_spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
          &SimpleConfigDialogQt::OnValueChanged);
  graphics_layout->addRow(options_["framerate_limit"].label_widget, fps_spin);

  // Guest Refresh Rate radio group (controls guest_display_refresh_cap and
  // use_50Hz_mode)
  auto* guest_refresh_widget = new QWidget(this);
  auto* guest_refresh_layout = new QHBoxLayout(guest_refresh_widget);
  guest_refresh_layout->setContentsMargins(0, 0, 0, 0);
  auto* guest_refresh_group = new QButtonGroup(this);
  auto* uncapped_radio = new QRadioButton("Uncapped", guest_refresh_widget);
  auto* hz50_radio = new QRadioButton("50Hz", guest_refresh_widget);
  auto* hz60_radio = new QRadioButton("60Hz", guest_refresh_widget);
  guest_refresh_group->addButton(uncapped_radio, 0);
  guest_refresh_group->addButton(hz50_radio, 1);
  guest_refresh_group->addButton(hz60_radio, 2);
  guest_refresh_layout->addWidget(uncapped_radio);
  guest_refresh_layout->addWidget(hz50_radio);
  guest_refresh_layout->addWidget(hz60_radio);
  guest_refresh_layout->addStretch();
  options_["guest_refresh_rate"].cvar_name = "guest_refresh_rate";
  options_["guest_refresh_rate"].editor_widget = guest_refresh_widget;
  options_["guest_refresh_rate"].label_widget =
      new QLabel("Emulated Display Refresh Rate:", this);
  connect(guest_refresh_group, &QButtonGroup::idClicked, this,
          &SimpleConfigDialogQt::OnValueChanged);
  graphics_layout->addRow(options_["guest_refresh_rate"].label_widget,
                          guest_refresh_widget);

  // VSync checkbox (inverted from variable_refresh_rate - VSync enabled means
  // VRR disabled)
  auto* vsync_check = new QCheckBox(this);
  options_["vsync"].cvar_name = "vsync";
  options_["vsync"].editor_widget = vsync_check;
  options_["vsync"].label_widget = new QLabel("VSync:", this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(vsync_check, &QCheckBox::checkStateChanged, this,
          &SimpleConfigDialogQt::OnValueChanged);
#else
  connect(vsync_check, &QCheckBox::stateChanged, this,
          &SimpleConfigDialogQt::OnValueChanged);
#endif
  graphics_layout->addRow(options_["vsync"].label_widget, vsync_check);

  auto* fullscreen_check = new QCheckBox(this);
  options_["fullscreen"].cvar_name = "fullscreen";
  options_["fullscreen"].editor_widget = fullscreen_check;
  options_["fullscreen"].label_widget = new QLabel("Full Screen:", this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(fullscreen_check, &QCheckBox::checkStateChanged, this,
          &SimpleConfigDialogQt::OnValueChanged);
#else
  connect(fullscreen_check, &QCheckBox::stateChanged, this,
          &SimpleConfigDialogQt::OnValueChanged);
#endif
  graphics_layout->addRow(options_["fullscreen"].label_widget,
                          fullscreen_check);

  auto* letterbox_check = new QCheckBox(this);
  options_["present_letterbox"].cvar_name = "present_letterbox";
  options_["present_letterbox"].editor_widget = letterbox_check;
  options_["present_letterbox"].label_widget = new QLabel("Letterbox:", this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(letterbox_check, &QCheckBox::checkStateChanged, this,
          &SimpleConfigDialogQt::OnValueChanged);
#else
  connect(letterbox_check, &QCheckBox::stateChanged, this,
          &SimpleConfigDialogQt::OnValueChanged);
#endif
  graphics_layout->addRow(options_["present_letterbox"].label_widget,
                          letterbox_check);

  main_layout->addWidget(graphics_group);

  // Audio section
  auto* audio_group = new QGroupBox("Audio", this);
  auto* audio_layout = new QFormLayout(audio_group);

  auto* apu_combo = new QComboBox(this);
  auto apu_it = enum_options.find("apu");
  if (apu_it != enum_options.end()) {
    for (const auto& opt : apu_it->second) {
      apu_combo->addItem(SafeQString(opt));
    }
  }
  options_["apu"].cvar_name = "apu";
  options_["apu"].editor_widget = apu_combo;
  options_["apu"].label_widget = new QLabel("Audio Backend:", this);
  connect(apu_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &SimpleConfigDialogQt::OnValueChanged);
  audio_layout->addRow(options_["apu"].label_widget, apu_combo);

  auto* xma_combo = new QComboBox(this);
  auto xma_it = enum_options.find("xma_decoder");
  if (xma_it != enum_options.end()) {
    for (const auto& opt : xma_it->second) {
      xma_combo->addItem(SafeQString(opt));
    }
  }
  options_["xma_decoder"].cvar_name = "xma_decoder";
  options_["xma_decoder"].editor_widget = xma_combo;
  options_["xma_decoder"].label_widget = new QLabel("Audio Decoder:", this);
  connect(xma_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &SimpleConfigDialogQt::OnValueChanged);
  audio_layout->addRow(options_["xma_decoder"].label_widget, xma_combo);

  auto* xma_thread_check = new QCheckBox(this);
  options_["use_dedicated_xma_thread"].cvar_name = "use_dedicated_xma_thread";
  options_["use_dedicated_xma_thread"].editor_widget = xma_thread_check;
  options_["use_dedicated_xma_thread"].label_widget =
      new QLabel("Dedicated Thread:", this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(xma_thread_check, &QCheckBox::checkStateChanged, this,
          &SimpleConfigDialogQt::OnValueChanged);
#else
  connect(xma_thread_check, &QCheckBox::stateChanged, this,
          &SimpleConfigDialogQt::OnValueChanged);
#endif
  audio_layout->addRow(options_["use_dedicated_xma_thread"].label_widget,
                       xma_thread_check);

  main_layout->addWidget(audio_group);

  // Other section
  auto* other_group = new QGroupBox("Other", this);
  auto* other_layout = new QFormLayout(other_group);

  auto* license_combo = new QComboBox(this);
  license_combo->addItem("None", 0);
  license_combo->addItem("Full", 1);
  license_combo->addItem("All", -1);
  options_["license_mask"].cvar_name = "license_mask";
  options_["license_mask"].editor_widget = license_combo;
  options_["license_mask"].label_widget = new QLabel("License:", this);
  connect(license_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &SimpleConfigDialogQt::OnValueChanged);
  other_layout->addRow(options_["license_mask"].label_widget, license_combo);

  auto* discord_check = new QCheckBox(this);
  options_["discord"].cvar_name = "discord";
  options_["discord"].editor_widget = discord_check;
  options_["discord"].label_widget = new QLabel("Discord Rich Presence:", this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(discord_check, &QCheckBox::checkStateChanged, this,
          &SimpleConfigDialogQt::OnValueChanged);
#else
  connect(discord_check, &QCheckBox::stateChanged, this,
          &SimpleConfigDialogQt::OnValueChanged);
#endif
  other_layout->addRow(options_["discord"].label_widget, discord_check);

  auto* language_combo = new QComboBox(this);
  auto language_it = enum_options.find("user_language");
  if (language_it != enum_options.end()) {
    for (const auto& opt : language_it->second) {
      language_combo->addItem(SafeQString(opt));
    }
  }
  options_["user_language"].cvar_name = "user_language";
  options_["user_language"].editor_widget = language_combo;
  options_["user_language"].label_widget = new QLabel("Language:", this);
  connect(language_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &SimpleConfigDialogQt::OnValueChanged);
  other_layout->addRow(options_["user_language"].label_widget, language_combo);

  auto* country_combo = new QComboBox(this);
  auto country_it = enum_options.find("user_country");
  if (country_it != enum_options.end()) {
    for (const auto& opt : country_it->second) {
      country_combo->addItem(SafeQString(opt));
    }
  }
  options_["user_country"].cvar_name = "user_country";
  options_["user_country"].editor_widget = country_combo;
  options_["user_country"].label_widget = new QLabel("Country:", this);
  connect(country_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &SimpleConfigDialogQt::OnValueChanged);
  other_layout->addRow(options_["user_country"].label_widget, country_combo);

  main_layout->addWidget(other_group);

  // Buttons
  auto* button_layout = new QHBoxLayout();

  auto* advanced_button = new QPushButton("Advanced...", this);
  connect(advanced_button, &QPushButton::clicked, this,
          &SimpleConfigDialogQt::OnAdvancedClicked);
  button_layout->addWidget(advanced_button);

  auto* reset_button = new QPushButton("Reset to Default", this);
  connect(reset_button, &QPushButton::clicked, this,
          &SimpleConfigDialogQt::OnResetClicked);
  button_layout->addWidget(reset_button);

  button_layout->addStretch();

  auto* save_button = new QPushButton("Save", this);
  connect(save_button, &QPushButton::clicked, this,
          &SimpleConfigDialogQt::OnSaveClicked);
  button_layout->addWidget(save_button);

  auto* discard_button = new QPushButton("Cancel", this);
  connect(discard_button, &QPushButton::clicked, this,
          &SimpleConfigDialogQt::OnDiscardClicked);
  button_layout->addWidget(discard_button);

  main_layout->addLayout(button_layout);
}

void SimpleConfigDialogQt::LoadConfigValues() {
  // Reload the global config from file to ensure we're not reading stale values
  // that may have been contaminated by game-specific config dialogs
  config::ReloadConfig();

  UpdateUIFromConfigVars();

  // Reset all labels to non-bold since values now match what's in config
  for (auto& [cvar_name, option] : options_) {
    if (option.label_widget) {
      QFont font = option.label_widget->font();
      font.setBold(false);
      option.label_widget->setFont(font);
    }
  }

  has_unsaved_changes_ = false;
}

void SimpleConfigDialogQt::LoadDefaultValues() {
  if (!cvar::ConfigVars) {
    return;
  }

  // Reset all cvars to their default values in memory
  for (auto& [name, var] : *cvar::ConfigVars) {
    if (name.find("logged_profile_slot_") != std::string::npos) {
      continue;
    }
    auto config_var = static_cast<cvar::IConfigVar*>(var);
    config_var->ResetConfigValueToDefault();
  }

  // Now update the UI from the in-memory default values (without reloading from
  // disk). Note: UpdateUIFromConfigVars sets both current_value and
  // pending_value to the default, but we want current_value to remain as the
  // original file value so we can show what changed. So we need to restore
  // current_value after.

  // Save the original current_values (from file)
  std::map<std::string, std::string> original_values;
  for (auto& [cvar_name, option] : options_) {
    original_values[cvar_name] = option.current_value;
  }

  UpdateUIFromConfigVars();

  // Restore the original current_values and update label bold state
  for (auto& [cvar_name, option] : options_) {
    option.current_value = original_values[cvar_name];
    bool is_modified = (option.pending_value != option.current_value);

    if (option.label_widget) {
      QFont font = option.label_widget->font();
      font.setBold(is_modified);
      option.label_widget->setFont(font);
    }
  }

  has_unsaved_changes_ = true;
}

void SimpleConfigDialogQt::UpdateUIFromConfigVars() {
  if (!cvar::ConfigVars) {
    return;
  }

  // Block signals during programmatic UI updates to prevent OnValueChanged from
  // firing prematurely and reading partially-updated state
  for (auto& [cvar_name, option] : options_) {
    if (option.editor_widget) {
      option.editor_widget->blockSignals(true);
    }
  }

  for (auto& [cvar_name, option] : options_) {
    if (cvar_name == "draw_resolution_scale") {
      auto it_x = (*cvar::ConfigVars).find("draw_resolution_scale_x");
      if (it_x != (*cvar::ConfigVars).end()) {
        auto config_var_x = static_cast<cvar::IConfigVar*>(it_x->second);
        std::string value = config_var_x->config_value();

        if (value.length() >= 2 && value.front() == '"' &&
            value.back() == '"') {
          value = value.substr(1, value.length() - 2);
        }

        option.current_value = value;
        option.pending_value = value;

        if (auto* combo = qobject_cast<QComboBox*>(option.editor_widget)) {
          int scale_value = std::stoi(value);
          int index = combo->findData(scale_value);
          if (index >= 0) {
            combo->setCurrentIndex(index);
          }
        }
      }
      continue;
    }

    // VSync option (inverted from variable_refresh_rate cvars)
    if (cvar_name == "vsync") {
      auto it_gpu = (*cvar::ConfigVars).find("gpu");
      std::string gpu_value = "d3d12";
      if (it_gpu != (*cvar::ConfigVars).end()) {
        auto gpu_var = static_cast<cvar::IConfigVar*>(it_gpu->second);
        gpu_value = gpu_var->config_value();
        if (gpu_value.length() >= 2 && gpu_value.front() == '"' &&
            gpu_value.back() == '"') {
          gpu_value = gpu_value.substr(1, gpu_value.length() - 2);
        }
      }

      std::string actual_cvar_name =
          (gpu_value == "vulkan")
              ? "vulkan_allow_present_mode_immediate"
              : "d3d12_allow_variable_refresh_rate_and_tearing";

      auto it_vrr = (*cvar::ConfigVars).find(actual_cvar_name);
      if (it_vrr != (*cvar::ConfigVars).end()) {
        auto config_var_vrr = static_cast<cvar::IConfigVar*>(it_vrr->second);
        std::string value = config_var_vrr->config_value();

        if (value.length() >= 2 && value.front() == '"' &&
            value.back() == '"') {
          value = value.substr(1, value.length() - 2);
        }

        // Store the inverted value (VSync = !VRR)
        std::string inverted = (value == "true") ? "false" : "true";
        option.current_value = inverted;
        option.pending_value = inverted;

        if (auto* check = qobject_cast<QCheckBox*>(option.editor_widget)) {
          // VSync enabled = VRR disabled
          check->setChecked(value != "true");
        }
      }
      continue;
    }

    // Guest Refresh Rate option (from guest_display_refresh_cap +
    // use_50Hz_mode)
    if (cvar_name == "guest_refresh_rate") {
      bool cap_enabled = false;
      bool use_50hz = false;

      auto it_cap = (*cvar::ConfigVars).find("guest_display_refresh_cap");
      if (it_cap != (*cvar::ConfigVars).end()) {
        auto cap_var = static_cast<cvar::IConfigVar*>(it_cap->second);
        std::string cap_value = cap_var->config_value();
        if (cap_value.length() >= 2 && cap_value.front() == '"' &&
            cap_value.back() == '"') {
          cap_value = cap_value.substr(1, cap_value.length() - 2);
        }
        cap_enabled = (cap_value == "true");
      }

      auto it_50hz = (*cvar::ConfigVars).find("use_50Hz_mode");
      if (it_50hz != (*cvar::ConfigVars).end()) {
        auto hz_var = static_cast<cvar::IConfigVar*>(it_50hz->second);
        std::string hz_value = hz_var->config_value();
        if (hz_value.length() >= 2 && hz_value.front() == '"' &&
            hz_value.back() == '"') {
          hz_value = hz_value.substr(1, hz_value.length() - 2);
        }
        use_50hz = (hz_value == "true");
      }

      // Determine radio selection: 0=Uncapped, 1=50Hz, 2=60Hz
      int selection = 0;
      if (cap_enabled) {
        selection = use_50hz ? 1 : 2;
      }
      std::string value = std::to_string(selection);
      option.current_value = value;
      option.pending_value = value;

      // Find the button group and select the appropriate radio
      if (auto* widget = option.editor_widget) {
        auto buttons = widget->findChildren<QRadioButton*>();
        if (selection < buttons.size()) {
          buttons[selection]->setChecked(true);
        }
      }
      continue;
    }

    auto it = (*cvar::ConfigVars).find(cvar_name);
    if (it != (*cvar::ConfigVars).end()) {
      auto config_var = static_cast<cvar::IConfigVar*>(it->second);
      std::string value = config_var->config_value();

      if (value.length() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.length() - 2);
      }

      option.current_value = value;
      option.pending_value = value;

      if (auto* combo = qobject_cast<QComboBox*>(option.editor_widget)) {
        if (cvar_name == "license_mask") {
          // license_mask uses integer data, not text
          int int_value = std::stoi(value);
          int index = combo->findData(int_value);
          if (index >= 0) {
            combo->setCurrentIndex(index);
          }
        } else {
          int index = combo->findText(SafeQString(value));
          if (index >= 0) {
            combo->setCurrentIndex(index);
          }
        }
      } else if (auto* check = qobject_cast<QCheckBox*>(option.editor_widget)) {
        check->setChecked(value == "true");
      } else if (auto* spin = qobject_cast<QSpinBox*>(option.editor_widget)) {
        spin->setValue(std::stoi(value));
      }
    }
  }

  // Unblock signals after all updates are complete
  for (auto& [cvar_name, option] : options_) {
    if (option.editor_widget) {
      option.editor_widget->blockSignals(false);
    }
  }
}

void SimpleConfigDialogQt::SaveConfigChanges() {
  if (!cvar::ConfigVars) {
    return;
  }

  for (auto& [cvar_name, option] : options_) {
    UpdatePendingValueFromEditor(&option);

    if (cvar_name == "draw_resolution_scale") {
      auto it_x = (*cvar::ConfigVars).find("draw_resolution_scale_x");
      auto it_y = (*cvar::ConfigVars).find("draw_resolution_scale_y");
      if (it_x != (*cvar::ConfigVars).end() &&
          it_y != (*cvar::ConfigVars).end()) {
        auto config_var_x = static_cast<cvar::IConfigVar*>(it_x->second);
        auto config_var_y = static_cast<cvar::IConfigVar*>(it_y->second);
        toml::value new_value(std::stoi(option.pending_value));
        config_var_x->LoadConfigValue(&new_value);
        config_var_y->LoadConfigValue(&new_value);
      }
      continue;
    }

    // VSync option (inverted to variable_refresh_rate cvars)
    if (cvar_name == "vsync") {
      auto it_gpu = (*cvar::ConfigVars).find("gpu");
      std::string gpu_value = "d3d12";
      if (it_gpu != (*cvar::ConfigVars).end()) {
        auto gpu_var = static_cast<cvar::IConfigVar*>(it_gpu->second);
        gpu_value = gpu_var->config_value();
        if (gpu_value.length() >= 2 && gpu_value.front() == '"' &&
            gpu_value.back() == '"') {
          gpu_value = gpu_value.substr(1, gpu_value.length() - 2);
        }
      }

      std::string actual_cvar_name =
          (gpu_value == "vulkan")
              ? "vulkan_allow_present_mode_immediate"
              : "d3d12_allow_variable_refresh_rate_and_tearing";

      auto it_vrr = (*cvar::ConfigVars).find(actual_cvar_name);
      if (it_vrr != (*cvar::ConfigVars).end()) {
        auto config_var_vrr = static_cast<cvar::IConfigVar*>(it_vrr->second);
        // Invert: VSync enabled = VRR disabled
        toml::value new_value(option.pending_value != "true");
        config_var_vrr->LoadConfigValue(&new_value);
      }
      continue;
    }

    // Guest Refresh Rate option (saves to guest_display_refresh_cap +
    // use_50Hz_mode)
    if (cvar_name == "guest_refresh_rate") {
      int selection = std::stoi(option.pending_value);
      // 0=Uncapped, 1=50Hz, 2=60Hz
      bool cap_enabled = (selection != 0);
      bool use_50hz = (selection == 1);

      auto it_cap = (*cvar::ConfigVars).find("guest_display_refresh_cap");
      if (it_cap != (*cvar::ConfigVars).end()) {
        auto cap_var = static_cast<cvar::IConfigVar*>(it_cap->second);
        toml::value cap_value(cap_enabled);
        cap_var->LoadConfigValue(&cap_value);
      }

      auto it_50hz = (*cvar::ConfigVars).find("use_50Hz_mode");
      if (it_50hz != (*cvar::ConfigVars).end()) {
        auto hz_var = static_cast<cvar::IConfigVar*>(it_50hz->second);
        toml::value hz_value(use_50hz);
        hz_var->LoadConfigValue(&hz_value);
      }
      continue;
    }

    auto it = (*cvar::ConfigVars).find(cvar_name);
    if (it != (*cvar::ConfigVars).end()) {
      auto config_var = static_cast<cvar::IConfigVar*>(it->second);

      if (cvar_name == "framerate_limit") {
        toml::value new_value(std::stoull(option.pending_value));
        config_var->LoadConfigValue(&new_value);
      } else if (cvar_name == "license_mask") {
        toml::value new_value(std::stoi(option.pending_value));
        config_var->LoadConfigValue(&new_value);
      } else if (cvar_name == "fullscreen" ||
                 cvar_name == "present_letterbox" || cvar_name == "discord" ||
                 cvar_name == "use_dedicated_xma_thread") {
        toml::value new_value(option.pending_value == "true");
        config_var->LoadConfigValue(&new_value);
      } else {
        toml::value new_value(option.pending_value);
        config_var->LoadConfigValue(&new_value);
      }
    }
  }

  config::SaveConfig();
  has_unsaved_changes_ = false;
}

void SimpleConfigDialogQt::OnSaveClicked() {
  SaveConfigChanges();
  accept();
}

void SimpleConfigDialogQt::OnDiscardClicked() {
  if (has_unsaved_changes_) {
    auto reply = QMessageBox::question(
        this, "Unsaved Changes",
        "You have unsaved changes. Are you sure you want to discard them?",
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::No) {
      return;
    }
  }

  reject();
}

void SimpleConfigDialogQt::OnAdvancedClicked() {
  if (has_unsaved_changes_) {
    auto reply = QMessageBox::question(
        this, "Unsaved Changes",
        "You have unsaved changes. Do you want to save them before opening "
        "the advanced settings?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

    if (reply == QMessageBox::Cancel) {
      return;
    } else if (reply == QMessageBox::Save) {
      SaveConfigChanges();
    }
  }

  accept();

  config::ReloadConfig();

  auto* advanced_dialog = new ConfigDialogQt(nullptr, emulator_window_);
  advanced_dialog->exec();
  delete advanced_dialog;
}

void SimpleConfigDialogQt::OnResetClicked() {
  auto reply = QMessageBox::question(
      this, "Reset to Defaults",
      "Are you sure you want to reset all settings to their default values?",
      QMessageBox::Yes | QMessageBox::No);

  if (reply == QMessageBox::Yes) {
    LoadDefaultValues();
  }
}

void SimpleConfigDialogQt::OnValueChanged() {
  for (auto& [cvar_name, option] : options_) {
    UpdatePendingValueFromEditor(&option);
    bool is_modified = (option.pending_value != option.current_value);

    if (option.label_widget) {
      QFont font = option.label_widget->font();
      font.setBold(is_modified);
      option.label_widget->setFont(font);
    }
  }

  has_unsaved_changes_ = true;
}

void SimpleConfigDialogQt::UpdatePendingValueFromEditor(ConfigOption* option) {
  option->pending_value =
      GetEditorValue(option->editor_widget, option->cvar_name);
}

void SimpleConfigDialogQt::UpdateLabelModifiedState(ConfigOption* option) {
  if (!option || !option->label_widget) {
    return;
  }

  bool is_modified = (option->pending_value != option->current_value);
  QFont font = option->label_widget->font();
  font.setBold(is_modified);
  option->label_widget->setFont(font);
}

std::string SimpleConfigDialogQt::GetEditorValue(QWidget* editor,
                                                 const std::string& cvar_name) {
  if (auto* combo = qobject_cast<QComboBox*>(editor)) {
    if (cvar_name == "draw_resolution_scale" || cvar_name == "license_mask") {
      return std::to_string(combo->currentData().toInt());
    }
    return SafeStdString(combo->currentText());
  } else if (auto* check = qobject_cast<QCheckBox*>(editor)) {
    return check->isChecked() ? "true" : "false";
  } else if (auto* spin = qobject_cast<QSpinBox*>(editor)) {
    return std::to_string(spin->value());
  } else if (cvar_name == "guest_refresh_rate") {
    // Find which radio button is checked (0=Uncapped, 1=50Hz, 2=60Hz)
    auto buttons = editor->findChildren<QRadioButton*>();
    for (int i = 0; i < buttons.size(); ++i) {
      if (buttons[i]->isChecked()) {
        return std::to_string(i);
      }
    }
    return "2";  // Default to 60Hz
  }
  return "";
}

}  // namespace app
}  // namespace xe
