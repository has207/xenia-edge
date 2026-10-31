/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

// Alias XContentCreateCrossTitleEnumerator. The console's xam builds each item
// as a 0x200-byte XCONTENT_DATA_INTERNAL, with the XUID, title ID, size and
// title name after the XCONTENT_DATA, and the dashboard reads them so. Title 0
// lists every title's content and type 0xFFFFFFFF every type; a retail console
// answers such a call with all the packages on its drives.
dword_result_t XamContentAggregateCreateEnumerator_entry(qword_t xuid,
                                                         dword_t device_id,
                                                         dword_t content_type,
                                                         dword_t title_id,
                                                         lpdword_t handle_out) {
  assert_not_null(handle_out);

  auto device_info = device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    return X_E_INVALIDARG;
  }

  auto e =
      make_object<XStaticEnumerator<XCONTENT_DATA_INTERNAL>>(kernel_state(), 1);
  X_KENUMERATOR_CONTENT_AGGREGATE* extra;
  auto result = e->Initialize(XUserIndexAny, 0xFE, 0x2000E, 0x20010, 0, &extra);
  if (XFAILED(result)) {
    return result;
  }

  extra->magic = kXObjSignature;
  extra->handle = e->handle();

  auto content_manager = kernel_state()->content_manager();
  const uint64_t content_xuid = xuid == -1 ? 0 : static_cast<uint64_t>(xuid);

  // Fetch any alternate title IDs defined in the XEX header
  // (used by games to load saves from other titles, etc)
  std::vector<uint32_t> title_ids{title_id ? title_id.value()
                                           : kCurrentlyRunningTitleId};
  auto exe_module = kernel_state()->GetExecutableModule();
  if (title_id && exe_module && exe_module->xex_module()) {
    const auto& alt_ids = exe_module->xex_module()->opt_alternate_title_ids();
    std::ranges::copy(alt_ids, std::back_inserter(title_ids));
  }

  auto content_types = [&](uint64_t types_xuid, uint32_t types_title_id) {
    if (content_type != 0xFFFFFFFF) {
      return std::vector<XContentType>{
          static_cast<XContentType>(content_type.value())};
    }
    return content_manager->ListContentTypes(types_xuid, types_title_id);
  };

  auto append_items =
      [&](uint64_t items_xuid,
          const std::vector<XCONTENT_AGGREGATE_DATA>& content_datas) {
        for (const auto& content_data : content_datas) {
          auto item = e->AppendItem();
          assert_not_null(item);
          if (!item) {
            continue;
          }
          std::memset(item, 0, sizeof(*item));
          item->device_id = content_data.device_id;
          item->content_type = content_data.content_type;
          item->display_name_raw = content_data.display_name_raw;
          std::memcpy(item->file_name_raw, content_data.file_name_raw,
                      sizeof(content_data.file_name_raw));
          item->xuid = content_data.xuid;
          item->title_id = content_data.title_id;

          // The rest comes from the package's header.
          std::vector<uint8_t> metadata;
          if (!content_manager->GetContentMetadata(items_xuid, content_data,
                                                   &metadata)) {
            const auto* header =
                reinterpret_cast<const vfs::XContentMetadata*>(metadata.data());
            item->content_size = header->content_size;
            const std::u16string title_name = header->title_name();
            xe::string_util::copy_and_swap_truncating(
                item->title_name, title_name, xe::countof(item->title_name));
          }
        }
      };

  if (!device_info || device_info->device_type == DeviceType::HDD) {
    const std::vector<uint32_t> hdd_title_ids =
        title_id ? title_ids
                 : content_manager->ListContentTitleIds(content_xuid);
    for (const auto& hdd_title_id : hdd_title_ids) {
      for (const auto type : content_types(content_xuid, hdd_title_id)) {
        append_items(content_xuid,
                     content_manager->ListContent(
                         static_cast<uint32_t>(DummyDeviceId::HDD),
                         content_xuid, hdd_title_id, type));
      }
    }
  }

  // Disc content is stored under XUID 0, for the disc's own title.
  if (!device_info || device_info->device_type == DeviceType::ODD) {
    for (const auto& odd_title_id : title_ids) {
      append_items(
          0, content_manager->ListContentODD(
                 static_cast<uint32_t>(DummyDeviceId::ODD), 0, odd_title_id,
                 static_cast<XContentType>(content_type.value())));
    }
  }

  XELOGD("XamContentAggregateCreateEnumerator: added {} items to enumerator",
         e->item_count());

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentAggregateCreateEnumerator, kContent, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(ContentAggregate);
