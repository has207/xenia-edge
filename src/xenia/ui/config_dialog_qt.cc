/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/config_dialog_qt.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSpinBox>
#include <algorithm>
#include <filesystem>
#include <sstream>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/app/emulator_window.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/ui/config_helpers.h"
#include "xenia/ui/qt_util.h"

#if XE_PLATFORM_LINUX
#include <unistd.h>
#endif

namespace xe {
namespace app {

namespace {

// Use the shared enum options from config_helpers.h
using xe::ui::GetKnownEnumOptions;
using xe::ui::SafeQString;
using xe::ui::SafeStdString;

#if XE_PLATFORM_LINUX
// Check if a command exists in PATH by searching each directory
bool IsCommandAvailable(const std::string& command) {
  std::string path_env = std::getenv("PATH") ? std::getenv("PATH") : "";
  if (path_env.empty()) {
    return false;
  }

  // Split PATH by ':'
  std::istringstream path_stream(path_env);
  std::string path_dir;
  while (std::getline(path_stream, path_dir, ':')) {
    std::filesystem::path full_path = std::filesystem::path(path_dir) / command;
    if (std::filesystem::exists(full_path) &&
        access(full_path.c_str(), X_OK) == 0) {
      return true;
    }
  }
  return false;
}
#endif

}  // namespace

ConfigDialogQt::ConfigDialogQt(QWidget* parent, EmulatorWindow* emulator_window)
    : QDialog(parent), emulator_window_(emulator_window) {
  setWindowTitle("Configuration Manager");
  resize(800, 600);

  LoadConfigValues();
  SetupUI();
}

ConfigDialogQt::~ConfigDialogQt() = default;

void ConfigDialogQt::SelectCategory(const std::string& category_name) {
  // Find the index of the category in our ordered list
  for (size_t i = 0; i < category_order_.size(); ++i) {
    if (category_order_[i] == category_name) {
      category_list_->setCurrentRow(static_cast<int>(i));
      return;
    }
  }
}

void ConfigDialogQt::LoadConfigValues() {
  config_vars_.clear();
  categories_.clear();
  has_unsaved_changes_ = false;

  if (!cvar::ConfigVars) {
    XELOGW("ConfigVars not initialized");
    return;
  }

  // Collect all config variables that are actually saved to config.toml
  // Skip transient variables as they don't belong in the config file
  for (const auto& [name, var] : *cvar::ConfigVars) {
    if (var->is_transient()) {
      continue;  // Skip transient variables entirely
    }

    // Skip the [Config] category - it contains internal variables like
    // defaults_date
    if (var->category() == "Config") {
      continue;
    }

    ConfigVarInfo info;
    info.var = var;
    info.name = var->name();
    info.description = var->description();
    info.category = var->category();

    // Get the actual value without TOML formatting
    if (auto* string_var = dynamic_cast<cvar::ConfigVar<std::string>*>(var)) {
      info.current_value = string_var->GetTypedConfigValue();
    } else if (auto* path_var =
                   dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(var)) {
      info.current_value = xe::path_to_utf8(path_var->GetTypedConfigValue());
    } else {
      info.current_value = var->config_value();
    }

    info.pending_value = info.current_value;
    info.is_modified = false;

    config_vars_.push_back(std::move(info));
  }

  // Organize by category and build ordered list
  category_order_.clear();
  for (auto& var_info : config_vars_) {
    auto& category_vars = categories_[var_info.category];
    if (category_vars.empty()) {
      // First time seeing this category, add to order
      category_order_.push_back(var_info.category);
    }
    category_vars.push_back(&var_info);
  }

  // Sort category order alphabetically
  std::sort(category_order_.begin(), category_order_.end());

  // Sort variables within each category
  for (auto& [category_name, vars] : categories_) {
    std::sort(vars.begin(), vars.end(),
              [](const ConfigVarInfo* a, const ConfigVarInfo* b) {
                return a->name < b->name;
              });
  }
}

void ConfigDialogQt::SetupUI() {
  auto* main_layout = new QVBoxLayout(this);

  // Info text
  auto* info_label = new QLabel(
      "Manage emulator configuration settings. Changes will be saved to "
      "config.toml. Some settings may require a restart to take effect.");
  info_label->setWordWrap(true);
  main_layout->addWidget(info_label);

  // Create horizontal layout for category list and settings
  auto* content_layout = new QHBoxLayout();

  // Category list on the left
  category_list_ = new QListWidget(this);
  category_list_->setMaximumWidth(150);
  connect(category_list_, &QListWidget::currentRowChanged, this,
          &ConfigDialogQt::OnCategorySelected);

  // Settings stack on the right
  settings_stack_ = new QStackedWidget(this);

  // Add categories in order
  for (const auto& category_name : category_order_) {
    category_list_->addItem(SafeQString(category_name));
    auto it = categories_.find(category_name);
    if (it != categories_.end()) {
      CreateCategoryPage(category_name, it->second);
    }
  }

  // Select first category by default
  if (category_list_->count() > 0) {
    category_list_->setCurrentRow(0);
  }

  content_layout->addWidget(category_list_);
  content_layout->addWidget(settings_stack_, 1);  // Give settings more space
  main_layout->addLayout(content_layout);

  // Button layout
  auto* button_layout = new QHBoxLayout();

  save_button_ = new QPushButton("Save Changes");
  connect(save_button_, &QPushButton::clicked, this,
          &ConfigDialogQt::OnSaveClicked);
  button_layout->addWidget(save_button_);

  discard_button_ = new QPushButton("Discard Changes");
  connect(discard_button_, &QPushButton::clicked, this,
          &ConfigDialogQt::OnDiscardClicked);
  button_layout->addWidget(discard_button_);

  reset_button_ = new QPushButton("Reset to Defaults");
  connect(reset_button_, &QPushButton::clicked, this,
          &ConfigDialogQt::OnResetToDefaultsClicked);
  button_layout->addWidget(reset_button_);

  button_layout->addStretch();
  main_layout->addLayout(button_layout);
}

void ConfigDialogQt::CreateCategoryPage(
    const std::string& category_name, const std::vector<ConfigVarInfo*>& vars) {
  auto* scroll_area = new QScrollArea();
  scroll_area->setWidgetResizable(true);

  auto* container = new QWidget();
  auto* form_layout = new QFormLayout(container);
  form_layout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

  for (auto* var_info : vars) {
    // Create label with tooltip - left aligned
    auto* label = new QLabel(SafeQString(var_info->name));
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    if (!var_info->description.empty()) {
      // Format tooltip with rich text to enable word wrapping
      QString tooltip =
          QString("<p style='white-space: pre-wrap; max-width: 400px;'>%1</p>")
              .arg(SafeQString(var_info->description).toHtmlEscaped());
      label->setToolTip(tooltip);
      label->setToolTipDuration(60000);  // Show for 60 seconds
    }

    // Store label reference for bold state updates
    var_info->label_widget = label;

    // Create editor widget - right aligned
    QWidget* editor = CreateEditorWidget(var_info);
    var_info->editor_widget = editor;

    form_layout->addRow(label, editor);
  }

  // Set label alignment for the form
  form_layout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  form_layout->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

  scroll_area->setWidget(container);
  settings_stack_->addWidget(scroll_area);
}

QWidget* ConfigDialogQt::CreateEditorWidget(ConfigVarInfo* var_info) {
  std::string trimmed_current = var_info->current_value;
  trimmed_current.erase(0, trimmed_current.find_first_not_of(" \t\n\r"));
  trimmed_current.erase(trimmed_current.find_last_not_of(" \t\n\r") + 1);

  // Check if this is an enum-like cvar with known options
  const auto& enum_options = GetKnownEnumOptions();
  auto enum_it = enum_options.find(var_info->name);

  if (trimmed_current == "true" || trimmed_current == "false") {
    // Boolean checkbox
    auto* checkbox = new QCheckBox();
    checkbox->setChecked(var_info->pending_value == "true");
    connect(checkbox, &QCheckBox::checkStateChanged, this,
            &ConfigDialogQt::OnValueChanged);

#if XE_PLATFORM_LINUX
    // Disable Linux-specific options if the required tools aren't installed
    if (var_info->name == "use_mangohud" && !IsCommandAvailable("mangohud")) {
      checkbox->setEnabled(false);
      checkbox->setToolTip(
          "MangoHUD is not installed or not found in PATH. Install MangoHUD "
          "to use this feature.");
    } else if (var_info->name == "use_gamemode" &&
               !IsCommandAvailable("gamemoderun")) {
      checkbox->setEnabled(false);
      checkbox->setToolTip(
          "GameMode is not installed or not found in PATH. Install GameMode "
          "to use this feature.");
    }
#endif

    return checkbox;
  } else if (enum_it != enum_options.end()) {
    // Dropdown for enum-like cvars
    auto* combo = new QComboBox();
    const auto& options = enum_it->second;

    int current_index = -1;
    for (size_t i = 0; i < options.size(); ++i) {
      combo->addItem(SafeQString(options[i]));
      if (options[i] == var_info->pending_value) {
        current_index = static_cast<int>(i);
      }
    }

    if (current_index >= 0) {
      combo->setCurrentIndex(current_index);
    }

    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigDialogQt::OnValueChanged);
    return combo;
  } else if (auto* path_var =
                 dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(
                     var_info->var)) {
    // Path input with browse button
    auto* container = new QWidget();
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* line_edit = new QLineEdit();
    line_edit->setText(SafeQString(var_info->pending_value));
    connect(line_edit, &QLineEdit::textChanged, this,
            &ConfigDialogQt::OnValueChanged);
    layout->addWidget(line_edit);

    auto* browse_button = new QPushButton("Browse...");
    connect(browse_button, &QPushButton::clicked,
            [this, line_edit, var_info]() {
              QString path;
              // File pickers for specific path types
              if (var_info->name == "achievement_sound_path") {
                path = QFileDialog::getOpenFileName(
                    this, "Select Achievement Sound", line_edit->text(),
                    "Audio Files (*.wav *.mp3 *.ogg *.flac *.m4a *.aac *.wma "
                    "*.opus);;All Files (*)");
              } else if (var_info->name == "custom_font_path") {
                path = QFileDialog::getOpenFileName(
                    this, "Select Custom Font", line_edit->text(),
                    "Font Files (*.ttf *.otf *.ttc);;All Files (*)");
              } else if (var_info->name == "mappings_file") {
                path = QFileDialog::getOpenFileName(
                    this, "Select Controller Mappings File", line_edit->text(),
                    "Text Files (*.txt);;All Files (*)");
              } else if (var_info->name == "log_file") {
                path = QFileDialog::getSaveFileName(
                    this, "Select Log File", line_edit->text(),
                    "Log Files (*.log *.txt);;All Files (*)");
              } else {
                // Default: directory picker
                path = QFileDialog::getExistingDirectory(
                    this, SafeQString("Select Directory for " + var_info->name),
                    line_edit->text());
              }
              if (!path.isEmpty()) {
                line_edit->setText(path);
              }
            });
    layout->addWidget(browse_button);

    return container;
  } else if (dynamic_cast<cvar::ConfigVar<int32_t>*>(var_info->var) ||
             dynamic_cast<cvar::ConfigVar<uint32_t>*>(var_info->var) ||
             dynamic_cast<cvar::ConfigVar<int64_t>*>(var_info->var) ||
             dynamic_cast<cvar::ConfigVar<uint64_t>*>(var_info->var)) {
    // Integer spin box
    auto* spinbox = new QSpinBox();
    spinbox->setRange(std::numeric_limits<int>::min(),
                      std::numeric_limits<int>::max());
    try {
      int value = std::stoi(var_info->pending_value);
      spinbox->setValue(value);
    } catch (...) {
      // If conversion fails, use 0
      spinbox->setValue(0);
    }
    connect(spinbox, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &ConfigDialogQt::OnValueChanged);
    return spinbox;
  } else {
    // Text input for all other types (string, double, etc.)
    auto* line_edit = new QLineEdit();
    line_edit->setText(SafeQString(var_info->pending_value));
    connect(line_edit, &QLineEdit::textChanged, this,
            &ConfigDialogQt::OnValueChanged);
    return line_edit;
  }
}

std::string ConfigDialogQt::GetEditorValue(QWidget* editor,
                                           const std::string& var_name) {
  if (auto* checkbox = qobject_cast<QCheckBox*>(editor)) {
    return checkbox->isChecked() ? "true" : "false";
  } else if (auto* combo = qobject_cast<QComboBox*>(editor)) {
    return SafeStdString(combo->currentText());
  } else if (auto* spinbox = qobject_cast<QSpinBox*>(editor)) {
    return std::to_string(spinbox->value());
  } else if (auto* line_edit = qobject_cast<QLineEdit*>(editor)) {
    return SafeStdString(line_edit->text());
  } else if (auto* container = qobject_cast<QWidget*>(editor)) {
    // For path containers, find the QLineEdit child
    auto* line_edit = container->findChild<QLineEdit*>();
    if (line_edit) {
      return SafeStdString(line_edit->text());
    }
  }
  return "";
}

void ConfigDialogQt::OnValueChanged() {
  // Update pending values from all editors
  for (auto& var_info : config_vars_) {
    if (var_info.editor_widget) {
      std::string new_value =
          GetEditorValue(var_info.editor_widget, var_info.name);
      var_info.pending_value = new_value;
      var_info.is_modified = (var_info.pending_value != var_info.current_value);

      // Update label bold state
      UpdateLabelModifiedState(&var_info);
    }
  }

  has_unsaved_changes_ =
      std::any_of(config_vars_.begin(), config_vars_.end(),
                  [](const ConfigVarInfo& v) { return v.is_modified; });

  // Update button states or window title if needed
  if (has_unsaved_changes_) {
    setWindowTitle("Configuration Manager *");
  } else {
    setWindowTitle("Configuration Manager");
  }
}

void ConfigDialogQt::UpdateLabelModifiedState(ConfigVarInfo* var_info) {
  if (!var_info || !var_info->label_widget) {
    return;
  }

  QFont font = var_info->label_widget->font();
  font.setBold(var_info->is_modified);
  var_info->label_widget->setFont(font);
}

void ConfigDialogQt::SaveConfigChanges() {
  XELOGI("Saving config changes");

  bool any_changed = false;
  for (auto& var_info : config_vars_) {
    if (var_info.is_modified) {
      try {
        auto* var = var_info.var;
        toml::table temp_table;
        std::string trimmed_value = var_info.pending_value;

        // Trim whitespace
        trimmed_value.erase(0, trimmed_value.find_first_not_of(" \t\n\r"));
        trimmed_value.erase(trimmed_value.find_last_not_of(" \t\n\r") + 1);

        // Check the actual type of the ConfigVar and parse accordingly
        if (dynamic_cast<cvar::ConfigVar<bool>*>(var)) {
          temp_table.insert(var->name(), trimmed_value == "true");
        } else if (dynamic_cast<cvar::ConfigVar<std::string>*>(var)) {
          temp_table.insert(var->name(), trimmed_value);
        } else if (dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(var)) {
          temp_table.insert(var->name(), trimmed_value);
        } else if (dynamic_cast<cvar::ConfigVar<int32_t>*>(var) ||
                   dynamic_cast<cvar::ConfigVar<uint32_t>*>(var) ||
                   dynamic_cast<cvar::ConfigVar<int64_t>*>(var) ||
                   dynamic_cast<cvar::ConfigVar<uint64_t>*>(var)) {
          try {
            if (trimmed_value.size() > 2 && trimmed_value[0] == '0' &&
                (trimmed_value[1] == 'x' || trimmed_value[1] == 'X')) {
              int64_t val = std::stoll(trimmed_value, nullptr, 16);
              temp_table.insert(var->name(), val);
            } else {
              int64_t val = std::stoll(trimmed_value);
              temp_table.insert(var->name(), val);
            }
          } catch (...) {
            XELOGE("Failed to parse integer value for {}: {}", var_info.name,
                   trimmed_value);
            continue;
          }
        } else if (dynamic_cast<cvar::ConfigVar<float>*>(var) ||
                   dynamic_cast<cvar::ConfigVar<double>*>(var)) {
          try {
            double val = std::stod(trimmed_value);
            temp_table.insert(var->name(), val);
          } catch (...) {
            XELOGE("Failed to parse float value for {}: {}", var_info.name,
                   trimmed_value);
            continue;
          }
        } else {
          XELOGW("Unknown config var type for {}, attempting generic parse",
                 var_info.name);
          temp_table.insert(var->name(), trimmed_value);
        }

        auto* node = temp_table.get(var->name());
        if (node) {
          var->LoadConfigValue(node);
          // Get the updated value properly based on type
          if (auto* string_var =
                  dynamic_cast<cvar::ConfigVar<std::string>*>(var)) {
            var_info.current_value = string_var->GetTypedConfigValue();
          } else if (auto* path_var =
                         dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(
                             var)) {
            var_info.current_value =
                xe::path_to_utf8(path_var->GetTypedConfigValue());
          } else {
            var_info.current_value = var->config_value();
          }
          var_info.pending_value = var_info.current_value;
          var_info.is_modified = false;
          any_changed = true;
        }
      } catch (const std::exception& e) {
        XELOGE("Failed to apply config value for {}: {}", var_info.name,
               e.what());
      }
    }
  }

  if (any_changed) {
    config::SaveConfig();
    has_unsaved_changes_ = false;
    setWindowTitle("Configuration Manager");
    XELOGI("Config saved successfully");

    QMessageBox::information(this, "Success",
                             "Configuration saved successfully!");
  }
}

void ConfigDialogQt::ResetToDefaults() {
  auto reply = QMessageBox::question(
      this, "Reset to Defaults",
      "Are you sure you want to reset all settings to their default values?",
      QMessageBox::Yes | QMessageBox::No);

  if (reply == QMessageBox::Yes) {
    XELOGI("Resetting config to defaults (UI only)");

    for (auto& var_info : config_vars_) {
      if (var_info.name.find("logged_profile_slot_") != std::string::npos) {
        continue;
      }
      var_info.var->ResetConfigValueToDefault();
      std::string default_value;

      if (auto* string_var =
              dynamic_cast<cvar::ConfigVar<std::string>*>(var_info.var)) {
        default_value = string_var->GetTypedConfigValue();
      } else if (auto* path_var =
                     dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(
                         var_info.var)) {
        default_value = xe::path_to_utf8(path_var->GetTypedConfigValue());
      } else {
        default_value = var_info.var->config_value();
      }

      var_info.pending_value = default_value;
      var_info.is_modified = (var_info.pending_value != var_info.current_value);

      // Update editor widget to show default value
      if (var_info.editor_widget) {
        if (auto* checkbox = qobject_cast<QCheckBox*>(var_info.editor_widget)) {
          checkbox->setChecked(default_value == "true");
        } else if (auto* combo =
                       qobject_cast<QComboBox*>(var_info.editor_widget)) {
          int index = combo->findText(SafeQString(default_value));
          if (index >= 0) {
            combo->setCurrentIndex(index);
          }
        } else if (auto* spinbox =
                       qobject_cast<QSpinBox*>(var_info.editor_widget)) {
          try {
            int value = std::stoi(default_value);
            spinbox->setValue(value);
          } catch (...) {
          }
        } else if (auto* line_edit =
                       qobject_cast<QLineEdit*>(var_info.editor_widget)) {
          line_edit->setText(SafeQString(default_value));
        } else if (auto* container =
                       qobject_cast<QWidget*>(var_info.editor_widget)) {
          auto* line_edit = container->findChild<QLineEdit*>();
          if (line_edit) {
            line_edit->setText(SafeQString(default_value));
          }
        }
      }

      // Restore the actual config variable to its original value
      try {
        toml::table temp_table;
        if (dynamic_cast<cvar::ConfigVar<bool>*>(var_info.var)) {
          temp_table.insert(var_info.var->name(),
                            var_info.current_value == "true");
        } else if (dynamic_cast<cvar::ConfigVar<std::string>*>(var_info.var) ||
                   dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(
                       var_info.var)) {
          temp_table.insert(var_info.var->name(), var_info.current_value);
        } else if (dynamic_cast<cvar::ConfigVar<int32_t>*>(var_info.var) ||
                   dynamic_cast<cvar::ConfigVar<uint32_t>*>(var_info.var) ||
                   dynamic_cast<cvar::ConfigVar<int64_t>*>(var_info.var) ||
                   dynamic_cast<cvar::ConfigVar<uint64_t>*>(var_info.var)) {
          std::string trimmed = var_info.current_value;
          trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
          trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
          if (trimmed.size() > 2 && trimmed[0] == '0' &&
              (trimmed[1] == 'x' || trimmed[1] == 'X')) {
            int64_t val = std::stoll(trimmed, nullptr, 16);
            temp_table.insert(var_info.var->name(), val);
          } else {
            int64_t val = std::stoll(trimmed);
            temp_table.insert(var_info.var->name(), val);
          }
        } else if (dynamic_cast<cvar::ConfigVar<float>*>(var_info.var) ||
                   dynamic_cast<cvar::ConfigVar<double>*>(var_info.var)) {
          double val = std::stod(var_info.current_value);
          temp_table.insert(var_info.var->name(), val);
        } else {
          temp_table.insert(var_info.var->name(), var_info.current_value);
        }

        auto* node = temp_table.get(var_info.var->name());
        if (node) {
          var_info.var->LoadConfigValue(node);
        }
      } catch (...) {
      }
    }

    has_unsaved_changes_ =
        std::any_of(config_vars_.begin(), config_vars_.end(),
                    [](const ConfigVarInfo& v) { return v.is_modified; });

    if (has_unsaved_changes_) {
      setWindowTitle("Configuration Manager *");
    } else {
      setWindowTitle("Configuration Manager");
    }
  }
}

void ConfigDialogQt::OnSaveClicked() {
  SaveConfigChanges();
  if (!has_unsaved_changes_) {
    accept();  // Close dialog after successful save
  }
}

void ConfigDialogQt::OnCategorySelected(int index) {
  if (index >= 0 && index < settings_stack_->count()) {
    settings_stack_->setCurrentIndex(index);
  }
}

void ConfigDialogQt::OnDiscardClicked() {
  if (has_unsaved_changes_) {
    auto reply = QMessageBox::question(
        this, "Discard Changes",
        "Are you sure you want to discard all unsaved changes?",
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
      LoadConfigValues();

      // Update all editor widgets with current values
      for (auto& var_info : config_vars_) {
        if (var_info.editor_widget) {
          if (auto* checkbox =
                  qobject_cast<QCheckBox*>(var_info.editor_widget)) {
            checkbox->setChecked(var_info.current_value == "true");
          } else if (auto* combo =
                         qobject_cast<QComboBox*>(var_info.editor_widget)) {
            int index = combo->findText(SafeQString(var_info.current_value));
            if (index >= 0) {
              combo->setCurrentIndex(index);
            }
          } else if (auto* spinbox =
                         qobject_cast<QSpinBox*>(var_info.editor_widget)) {
            try {
              int value = std::stoi(var_info.current_value);
              spinbox->setValue(value);
            } catch (...) {
            }
          } else if (auto* line_edit =
                         qobject_cast<QLineEdit*>(var_info.editor_widget)) {
            line_edit->setText(SafeQString(var_info.current_value));
          } else if (auto* container =
                         qobject_cast<QWidget*>(var_info.editor_widget)) {
            auto* line_edit = container->findChild<QLineEdit*>();
            if (line_edit) {
              line_edit->setText(SafeQString(var_info.current_value));
            }
          }
        }
      }

      has_unsaved_changes_ = false;
      setWindowTitle("Configuration Manager");
    }
  }
}

void ConfigDialogQt::OnResetToDefaultsClicked() { ResetToDefaults(); }

}  // namespace app
}  // namespace xe
