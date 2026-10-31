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
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

// Alias XContentCreateCrossTitleEnumerator
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

  auto e = make_object<XStaticEnumerator<XCONTENT_CROSS_TITLE_DATA>>(
      kernel_state(), 1);
  X_KENUMERATOR_CONTENT_AGGREGATE* extra;
  auto result = e->Initialize(XUserIndexAny, 0xFE, 0x2000E, 0x20010, 0, &extra);
  if (XFAILED(result)) {
    return result;
  }

  extra->magic = kXObjSignature;
  extra->handle = e->handle();

  const XContentType content_type_enum =
      static_cast<XContentType>(content_type.value());

  // Fetch any alternate title IDs defined in the XEX header
  // (used by games to load saves from other titles, etc)
  std::vector<uint32_t> title_ids{title_id ? title_id.value()
                                           : kCurrentlyRunningTitleId};
  auto exe_module = kernel_state()->GetExecutableModule();
  if (exe_module && exe_module->xex_module()) {
    const auto& alt_ids = exe_module->xex_module()->opt_alternate_title_ids();
    std::ranges::copy(alt_ids, std::back_inserter(title_ids));
  }

  auto append_items =
      [&e](const std::vector<XCONTENT_AGGREGATE_DATA>& content_datas) {
        for (const auto& content_data : content_datas) {
          auto item = e->AppendItem();
          assert_not_null(item);
          if (item) {
            item->content_data.device_id = content_data.device_id;
            item->content_data.content_type = content_data.content_type;
            item->content_data.display_name_raw = content_data.display_name_raw;
            std::memcpy(item->content_data.file_name_raw,
                        content_data.file_name_raw,
                        sizeof(content_data.file_name_raw));
            item->content_data.padding[0] = 0;
            item->content_data.padding[1] = 0;

            item->title_id = content_data.title_id;
          }
        }
      };

  auto content_manager = kernel_state()->content_manager();
  if (!device_info || device_info->device_type == DeviceType::HDD) {
    for (const auto& title_id : title_ids) {
      append_items(content_manager->ListContent(
          static_cast<uint32_t>(DummyDeviceId::HDD),
          xuid == -1 ? 0 : static_cast<uint64_t>(xuid), title_id,
          content_type_enum));
    }
  }

  // Disc content is stored under XUID 0.
  if (!device_info || device_info->device_type == DeviceType::ODD) {
    for (const auto& title_id : title_ids) {
      append_items(content_manager->ListContentODD(
          static_cast<uint32_t>(DummyDeviceId::ODD), 0, title_id,
          content_type_enum));
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
