/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"

#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// A stand-in for an object of a type the title defines itself, which has no
// dispatch header for a host object to keep its handle in. The handle table
// only names it. The guest header keeps its counts.
class XGuestObject : public XObject {
 public:
  XGuestObject(KernelState* kernel_state, uint32_t body)
      : XObject(kernel_state, Type::Undefined), body_(body) {
    auto global_lock = global_critical_region::AcquireDirect();
    by_body_[body] = this;
  }
  ~XGuestObject() override {
    auto global_lock = global_critical_region::AcquireDirect();
    // A new object at the same address may have replaced it.
    auto it = by_body_.find(body_);
    if (it != by_body_.end() && it->second == this) {
      by_body_.erase(it);
    }
  }

  uint32_t body() const { return body_; }

  static object_ref<XObject> FromBody(uint32_t body) {
    auto global_lock = global_critical_region::AcquireDirect();
    // One whose last reference just went is still mapped until its destructor
    // runs.
    auto it = by_body_.find(body);
    if (it == by_body_.end() || !it->second->TryRetain()) {
      return object_ref<XObject>();
    }
    return object_ref<XObject>(it->second);
  }
  static bool HasBody(uint32_t body) {
    auto global_lock = global_critical_region::AcquireDirect();
    return by_body_.count(body) != 0;
  }

 private:
  uint32_t body_;
  static inline std::unordered_map<uint32_t, XGuestObject*> by_body_;
};

namespace {

// Objects whose pointer and handle counts the guest header keeps, as the kernel
// does for all of them, and whose type's procedures run as those drop. Xenia's
// own objects keep theirs on the host.

// The header of an object of a type the title defines, or null.
X_OBJECT_HEADER* TitleObjectHeader(uint32_t body) {
  if (body < sizeof(X_OBJECT_HEADER)) {
    return nullptr;
  }
  const uint32_t header_ptr = body - uint32_t(sizeof(X_OBJECT_HEADER));
  if (kernel_memory()->QueryRangeAccess(header_ptr, sizeof(X_OBJECT_HEADER)) !=
      xe::memory::PageAccess::kReadWrite) {
    return nullptr;
  }
  auto header = kernel_memory()->TranslateVirtual<X_OBJECT_HEADER*>(header_ptr);
  const uint32_t type_ptr = header->object_type_ptr;
  const uint32_t globals = kernel_state()->GetKernelGuestGlobals();
  // CreateNative leaves its objects' pointer counts at 0. A guest object of one
  // of the kernel's types has its type in the kernel globals.
  if (header->pointer_count <= 0 ||
      (type_ptr >= globals &&
       type_ptr < globals + sizeof(KernelGuestGlobals)) ||
      kernel_memory()->QueryRangeAccess(type_ptr, sizeof(X_OBJECT_TYPE)) ==
          xe::memory::PageAccess::kNoAccess) {
    return nullptr;
  }
  // The kernel frees every object through its type.
  auto type = kernel_memory()->TranslateVirtual<X_OBJECT_TYPE*>(type_ptr);
  return type->free_proc ? header : nullptr;
}

// The body of a counted object, or 0.
uint32_t CountedBody(XObject* object) {
  if (auto guest_object = dynamic_cast<XGuestObject*>(object)) {
    return guest_object->body();
  }
  // Xenia's own objects, without probing guest memory.
  if (object->allocated_guest_object()) {
    return 0;
  }
  const uint32_t body = object->guest_object();
  return TitleObjectHeader(body) ? body : 0;
}
uint32_t CountedBody(uint32_t body) {
  return XGuestObject::HasBody(body) || TitleObjectHeader(body) ? body : 0;
}

X_OBJECT_HEADER* CountedHeader(uint32_t body) {
  return kernel_memory()->TranslateVirtual<X_OBJECT_HEADER*>(
      body - uint32_t(sizeof(X_OBJECT_HEADER)));
}

void ReferenceCounted(uint32_t body) {
  auto header = CountedHeader(body);
  auto global_lock = global_critical_region::AcquireDirect();
  header->pointer_count = header->pointer_count + 1;
}

// Drops a pointer reference. The last runs the type's delete procedure and
// frees the object through the type.
void DereferenceCounted(uint32_t body) {
  auto header = CountedHeader(body);
  uint32_t type_ptr;
  uint32_t allocation = body - uint32_t(sizeof(X_OBJECT_HEADER));
  {
    auto global_lock = global_critical_region::AcquireDirect();
    const int32_t pointer_count = header->pointer_count - 1;
    header->pointer_count = pointer_count;
    if (pointer_count) {
      return;
    }
    type_ptr = header->object_type_ptr;
    // A named object's allocation starts with its name info.
    if (header->flags & OBJECT_HEADER_FLAG_NAMED_OBJECT) {
      allocation -= uint32_t(sizeof(X_OBJECT_HEADER_NAME_INFO));
    }
  }
  // A host thread can't run guest code. At teardown that is fine and elsewhere
  // the guest memory leaks.
  if (!XThread::IsInThread()) {
    XELOGW("Object {:08X} deleted on a host thread, its memory leaks", body);
    return;
  }
  auto type = kernel_memory()->TranslateVirtual<X_OBJECT_TYPE*>(type_ptr);
  auto processor = kernel_state()->processor();
  auto thread_state = cpu::ThreadState::Get();
  if (type->delete_proc) {
    uint64_t args[] = {body};
    processor->Execute(thread_state, type->delete_proc, args, 1);
  }
  if (type->free_proc) {
    uint64_t args[] = {allocation};
    processor->Execute(thread_state, type->free_proc, args, 1);
  }
}

// Closes one handle's open. The type's close procedure gets the handle count
// from before it, then the handle's reference drops.
void CloseCounted(uint32_t body) {
  auto header = CountedHeader(body);
  int32_t handle_count;
  uint32_t close_proc;
  {
    auto global_lock = global_critical_region::AcquireDirect();
    handle_count = header->handle_count;
    header->handle_count = handle_count - 1;
    close_proc = kernel_memory()
                     ->TranslateVirtual<X_OBJECT_TYPE*>(header->object_type_ptr)
                     ->close_proc;
  }
  if (close_proc) {
    if (XThread::IsInThread()) {
      auto thread = XThread::GetCurrentThread();
      thread->EnterCriticalRegion();
      uint64_t args[] = {body, uint32_t(handle_count)};
      kernel_state()->processor()->Execute(thread->thread_state(), close_proc,
                                           args, 2);
      thread->LeaveCriticalRegion();
    } else {
      XELOGW("Object {:08X}: close procedure skipped on a host thread", body);
    }
  }
  // The last handle unlinks the object from its directory, which drops the
  // directory's reference.
  bool unlinked = false;
  {
    auto global_lock = global_critical_region::AcquireDirect();
    if (!header->handle_count &&
        (header->flags & (OBJECT_HEADER_FLAG_CONTAINED_IN_DIRECTORY |
                          OBJECT_HEADER_FLAG_IS_PERMANENT)) ==
            OBJECT_HEADER_FLAG_CONTAINED_IN_DIRECTORY) {
      header->flags =
          int16_t(header->flags & ~OBJECT_HEADER_FLAG_CONTAINED_IN_DIRECTORY);
      unlinked = true;
    }
  }
  if (unlinked) {
    DereferenceCounted(body);
  }
  DereferenceCounted(body);
}

// Whether a body holds a dispatcher object: one xenia already wraps, or one an
// inlined KeInitialize* laid out with its size and an empty wait list. Any
// other body would get the wrapper's handle stash written over its data.
bool IsInitializedDispatcher(uint32_t body) {
  auto header = kernel_memory()->TranslateVirtual<X_DISPATCH_HEADER*>(body);
  if (header->wait_list.flink_ptr == kXObjSignature) {
    return true;
  }
  size_t size;
  switch (header->type) {
    case X_OBJECT_TYPES::EventNotificationObject:
    case X_OBJECT_TYPES::EventSynchronizationObject:
      size = sizeof(X_KEVENT);
      break;
    case X_OBJECT_TYPES::MutantObject:
      size = sizeof(X_KMUTANT);
      break;
    case X_OBJECT_TYPES::SemaphoreObject:
      size = sizeof(X_KSEMAPHORE);
      break;
    case X_OBJECT_TYPES::TimerNotificationObject:
    case X_OBJECT_TYPES::TimerSynchronizationObject:
      size = sizeof(X_KTIMER);
      break;
    default:
      return false;
  }
  const uint32_t wait_list =
      body + uint32_t(offsetof(X_DISPATCH_HEADER, wait_list));
  return header->size == size / 4 && header->wait_list.flink_ptr == wait_list &&
         header->wait_list.blink_ptr == wait_list;
}

// The host object that names a counted object's handles. A title type's
// dispatcher body gets its wrapper when |allow_wrapper|, so waits on the handle
// work, even if a stand-in already names its other handles. Anything else gets
// the stand-in, a new one coming with its first handle and setting |created|.
object_ref<XObject> CountedObject(uint32_t body, bool allow_wrapper,
                                  bool* created) {
  *created = false;
  if (allow_wrapper && TitleObjectHeader(body) &&
      IsInitializedDispatcher(body)) {
    auto object = XObject::GetNativeObject<XObject>(
        kernel_state(), kernel_memory()->TranslateVirtual(body));
    if (object) {
      object->SetNativeType();
      return object;
    }
  }
  if (auto object = XGuestObject::FromBody(body)) {
    return object;
  }
  *created = true;
  return object_ref<XObject>(new XGuestObject(kernel_state(), body));
}

// Gives a counted object a new handle, which takes |pointer_bias| references.
X_STATUS OpenCounted(uint32_t body, uint32_t pointer_bias, bool allow_wrapper,
                     object_ref<XObject>* out_object, X_HANDLE* out_handle) {
  bool created;
  auto object = CountedObject(body, allow_wrapper, &created);
  X_HANDLE handle = object->handle();
  if (!created) {
    const X_STATUS result =
        kernel_state()->object_table()->AddHandle(object.get(), &handle, true);
    if (XFAILED(result)) {
      return result;
    }
  }
  auto header = CountedHeader(body);
  {
    auto global_lock = global_critical_region::AcquireDirect();
    header->pointer_count = header->pointer_count + int32_t(pointer_bias);
    header->handle_count = header->handle_count + 1;
  }
  *out_handle = handle;
  if (out_object) {
    *out_object = std::move(object);
  }
  return X_STATUS_SUCCESS;
}

}  // namespace

void xeObSplitName(X_ANSI_STRING input_string,
                   X_ANSI_STRING* leading_path_component,
                   X_ANSI_STRING* remaining_path_components,
                   PPCContext* context) {
  // Skips one leading separator, splits off the component up to the next one
  // and leaves the rest after it.
  const uint32_t pointer = input_string.pointer;
  const uint16_t length = input_string.length;
  const char* text = context->TranslateVirtual<const char*>(pointer);
  const uint16_t start = (length && text[0] == '\\') ? 1 : 0;
  uint16_t end = start;
  while (end < length && text[end] != '\\') {
    ++end;
  }
  leading_path_component->pointer = pointer + start;
  leading_path_component->length = uint16_t(end - start);
  leading_path_component->maximum_length = uint16_t(end - start);
  const uint16_t rest = end < length ? uint16_t(end + 1) : end;
  remaining_path_components->pointer = pointer + rest;
  remaining_path_components->length = uint16_t(length - rest);
  remaining_path_components->maximum_length = uint16_t(length - rest);
}

uint32_t xeObHashObjectName(X_ANSI_STRING* ElementName, PPCContext* context) {
  uint8_t* current_character_ptr =
      context->TranslateVirtual(ElementName->pointer);
  uint32_t result = 0;
  uint8_t* name_span_end = &current_character_ptr[ElementName->length];
  while (current_character_ptr < name_span_end) {
    uint32_t current_character = *current_character_ptr++;
    if (current_character < 0x80) {
      result = (current_character | 0x20) + (result >> 1) + 3 * result;
    }
  }
  return result % 0xD;
}

uint32_t xeObCreateObject(X_OBJECT_TYPE* object_factory,
                          X_OBJECT_ATTRIBUTES* optional_attributes,
                          uint32_t object_size_without_headers,
                          uint32_t* out_object, cpu::ppc::PPCContext* context) {
  unsigned int resulting_header_flags = 0;
  *out_object = 0;
  unsigned int poolarg = 0;

  auto get_flags_and_poolarg_for_process_type = [&resulting_header_flags,
                                                 &poolarg, context]() {
    uint32_t process_type = xboxkrnl::xeKeGetCurrentProcessType(context);
    if (process_type == X_PROCTYPE_TITLE) {
      poolarg = 1;
      resulting_header_flags = OBJECT_HEADER_IS_TITLE_OBJECT;
    } else {
      poolarg = 2;
    }
  };
  if (!optional_attributes) {
    get_flags_and_poolarg_for_process_type();
  }

  else if ((optional_attributes->attributes & 0x1000) == 0) {
    if ((optional_attributes->attributes & 0x2000) != 0) {
      poolarg = 2;
    } else {
      get_flags_and_poolarg_for_process_type();
    }
  } else {
    poolarg = 1;
    resulting_header_flags = OBJECT_HEADER_IS_TITLE_OBJECT;
  }
  uint32_t desired_object_path_ptr;

  if (!optional_attributes ||
      (desired_object_path_ptr = optional_attributes->name_ptr) == 0) {
    /*
        object has no name provided, just allocate an object with a basic header
    */
    uint64_t allocate_args[] = {
        object_size_without_headers + sizeof(X_OBJECT_HEADER),
        object_factory->pool_tag, poolarg};
    context->processor->Execute(
        context->thread_state, object_factory->allocate_proc, allocate_args, 3);

    uint32_t allocation = static_cast<uint32_t>(context->r[3]);

    if (allocation) {
      X_OBJECT_HEADER* new_object_header =
          context->TranslateVirtual<X_OBJECT_HEADER*>(allocation);
      new_object_header->pointer_count = 1;
      new_object_header->handle_count = 0;
      new_object_header->object_type_ptr =
          context->HostToGuestVirtual(object_factory);
      new_object_header->flags = resulting_header_flags;

      *out_object = allocation + sizeof(X_OBJECT_HEADER);
      return X_STATUS_SUCCESS;
    }
    return X_STATUS_INSUFFICIENT_RESOURCES;
  }
  /*
    iterate through all path components until we obtain the final one, which is
    the objects actual name
  */
  X_ANSI_STRING trailing_path_component;
  X_ANSI_STRING remaining_path;
  X_ANSI_STRING loaded_object_name;
  loaded_object_name =
      *context->TranslateVirtual<X_ANSI_STRING*>(desired_object_path_ptr);
  trailing_path_component.pointer = 0;
  trailing_path_component.length = 0;
  remaining_path = loaded_object_name;
  while (remaining_path.length) {
    xeObSplitName(remaining_path, &trailing_path_component, &remaining_path,
                  context);
    if (remaining_path.length) {
      if (*context->TranslateVirtual<char*>(remaining_path.pointer) == '\\') {
        return X_STATUS_OBJECT_NAME_INVALID;
      }
    }
  }
  if (!trailing_path_component.length) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }
  // the object and its name are all created in a single allocation

  unsigned int aligned_object_size =
      xe::align<uint32_t>(object_size_without_headers, 4);
  {
    uint64_t allocate_args[] = {
        trailing_path_component.length + aligned_object_size +
            sizeof(X_OBJECT_HEADER_NAME_INFO) + sizeof(X_OBJECT_HEADER),
        object_factory->pool_tag, poolarg};

    context->processor->Execute(
        context->thread_state, object_factory->allocate_proc, allocate_args, 3);
  }
  uint32_t named_object_allocation = static_cast<uint32_t>(context->r[3]);
  if (!named_object_allocation) {
    return X_STATUS_INSUFFICIENT_RESOURCES;
  }

  X_OBJECT_HEADER_NAME_INFO* nameinfo =
      context->TranslateVirtual<X_OBJECT_HEADER_NAME_INFO*>(
          named_object_allocation);
  nameinfo->next_in_directory = 0;
  nameinfo->object_directory = 0;

  X_OBJECT_HEADER* header_for_named_object =
      reinterpret_cast<X_OBJECT_HEADER*>(nameinfo + 1);

  char* name_string_memory_for_named_object = &reinterpret_cast<char*>(
      header_for_named_object + 1)[aligned_object_size];
  nameinfo->name.pointer =
      context->HostToGuestVirtual(name_string_memory_for_named_object);
  nameinfo->name.length = trailing_path_component.length;
  nameinfo->name.maximum_length = trailing_path_component.length;

  memcpy(name_string_memory_for_named_object,
         context->TranslateVirtual<char*>(trailing_path_component.pointer),
         trailing_path_component.length);

  header_for_named_object->pointer_count = 1;
  header_for_named_object->handle_count = 0;
  header_for_named_object->object_type_ptr =
      context->HostToGuestVirtual(object_factory);
  header_for_named_object->flags =
      resulting_header_flags & 0xFFFE | OBJECT_HEADER_FLAG_NAMED_OBJECT;
  *out_object = context->HostToGuestVirtual(&header_for_named_object[1]);
  return X_STATUS_SUCCESS;
}
dword_result_t ObOpenObjectByName_entry(lpunknown_t obj_attributes_ptr,
                                        lpunknown_t object_type_ptr,
                                        dword_t unk, lpdword_t handle_ptr) {
  // r3 = ptr to info?
  //   +0 = -4
  //   +4 = name ptr
  //   +8 = 0
  // r4 = ExEventObjectType | ExSemaphoreObjectType | ExTimerObjectType
  // r5 = 0
  // r6 = out_ptr (handle?)

  if (!obj_attributes_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  auto obj_attributes = kernel_memory()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(
      obj_attributes_ptr);
  assert_true(obj_attributes->name_ptr != 0);
  auto name = util::TranslateAnsiStringAddress(kernel_memory(),
                                               obj_attributes->name_ptr);

  auto object_table = kernel_state()->object_table();
  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result = object_table->GetObjectByName(name, &handle);
  if (XSUCCEEDED(result)) {
    // A counted object gets a handle of its own like any open, in place of the
    // lookup's reference to the named one.
    auto object = object_table->LookupObject<XObject>(handle);
    if (uint32_t body = object ? CountedBody(object.get()) : 0) {
      object_table->ReleaseHandle(handle);
      result = OpenCounted(body, 1, true, nullptr, &handle);
    }
  }
  if (XSUCCEEDED(result)) {
    *handle_ptr = handle;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ObOpenObjectByName, kNone, kImplemented);
// chrispy: investigate this, pretty certain it does not properly emulate the
// original
dword_result_t ObOpenObjectByPointer_entry(lpvoid_t object_ptr,
                                           lpdword_t out_handle_ptr) {
  *out_handle_ptr = 0;
  if (uint32_t body = CountedBody(object_ptr.guest_address())) {
    X_HANDLE handle;
    const X_STATUS result = OpenCounted(body, 1, true, nullptr, &handle);
    if (XSUCCEEDED(result)) {
      *out_handle_ptr = handle;
    }
    return result;
  }

  auto object = XObject::GetNativeObject<XObject>(kernel_state(), object_ptr);
  if (!object) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Keep the guest type for ObReferenceObjectByHandle.
  object->SetNativeType();

  // A host object's own handle is not in the title's numbering and must never
  // reach the guest, so give it a guest one. AddHandle retains, so this needs
  // no RetainHandle of its own.
  if (object->is_host_object()) {
    X_HANDLE guest_handle = X_INVALID_HANDLE_VALUE;
    X_STATUS result = kernel_state()->object_table()->AddHandle(
        object.get(), &guest_handle, true);
    if (XFAILED(result)) {
      return result;
    }
    *out_handle_ptr = guest_handle;
    return X_STATUS_SUCCESS;
  }

  // Retain the handle. Will be released in NtClose.
  object->RetainHandle();
  *out_handle_ptr = object->handle();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObOpenObjectByPointer, kNone, kImplemented);

dword_result_t ObLookupThreadByThreadId_entry(dword_t thread_id,
                                              lpdword_t out_object_ptr) {
  auto thread = kernel_state()->GetThreadByID(thread_id);
  if (!thread) {
    *out_object_ptr = 0;
    return X_STATUS_INVALID_PARAMETER;
  }

  // Retain the object. Will be released in ObDereferenceObject.
  thread->RetainHandle();
  *out_object_ptr = thread->guest_object();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObLookupThreadByThreadId, kNone, kImplemented);

dword_result_t ObLookupAnyThreadByThreadId_entry(dword_t thread_id,
                                                 lpdword_t out_object_ptr) {
  return ObLookupThreadByThreadId_entry(thread_id, out_object_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(ObLookupAnyThreadByThreadId, kNone, kImplemented);

dword_result_t ObReferenceObjectByHandle_entry(dword_t handle,
                                               dword_t object_type_ptr,
                                               lpdword_t out_object_ptr) {
  // chrispy: gotta preinit this to 0, kernel is expected to do that
  *out_object_ptr = 0;

  // A close can't free the object between the lookup and the reference.
  auto global_lock = global_critical_region::AcquireDirect();
  auto object =
      kernel_state()->object_table()->LookupObject<XObject>(handle, true);
  if (!object) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (uint32_t body = CountedBody(object.get())) {
    if (object_type_ptr &&
        object_type_ptr != CountedHeader(body)->object_type_ptr) {
      return X_STATUS_OBJECT_TYPE_MISMATCH;
    }
    ReferenceCounted(body);
    if (out_object_ptr.guest_address()) {
      *out_object_ptr = body;
    }
    return X_STATUS_SUCCESS;
  }

  uint32_t native_ptr = object->guest_object();

  if (object_type_ptr) {
    // Use guest type recorded when opening the object,
    // or host type mapping if none was recorded.
    uint32_t actual_type_ptr = object->guest_object_type();
    if (!actual_type_ptr) {
      auto& object_types =
          kernel_state()->host_object_type_enum_to_guest_object_type_ptr_;
      auto it = object_types.find(object->type());
      if (it != object_types.end()) {
        actual_type_ptr = it->second;
      } else {
        assert_unhandled_case(object->type());
        native_ptr = 0xDEADF00D;
      }
    }
    if (actual_type_ptr && object_type_ptr != actual_type_ptr) {
      return X_STATUS_OBJECT_TYPE_MISMATCH;
    }
  }

  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  object->RetainHandle();

  assert_not_zero(native_ptr);

  if (out_object_ptr.guest_address()) {
    *out_object_ptr = native_ptr;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObReferenceObjectByHandle, kNone, kImplemented);

dword_result_t ObReferenceObjectByName_entry(pointer_t<X_ANSI_STRING> name,
                                             dword_t attributes,
                                             dword_t object_type_ptr,
                                             lpvoid_t parse_context,
                                             lpdword_t out_object_ptr,
                                             const ppc_context_t& ctx) {
  X_HANDLE handle = X_INVALID_HANDLE_VALUE;

  char* name_str = ctx.TranslateVirtual<char*>(name->pointer);
  X_STATUS result =
      kernel_state()->object_table()->GetObjectByName(name_str, &handle);
  if (XSUCCEEDED(result)) {
    result = ObReferenceObjectByHandle_entry(handle, object_type_ptr,
                                             out_object_ptr);
    // The reference is ObReferenceObjectByHandle's, not the lookup's.
    kernel_state()->object_table()->ReleaseHandle(handle);
  } else if (out_object_ptr.guest_address()) {
    *out_object_ptr = 0;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ObReferenceObjectByName, kNone, kImplemented);

void xeObDereferenceObject(PPCContext* context, uint32_t native_ptr) {
  // Check if a dummy value from ObReferenceObjectByHandle.
  if (native_ptr == 0xDEADF00D) {
    return;
  }
  if (!native_ptr) {
    XELOGE("Null native ptr in ObDereferenceObject!");
    return;
  }
  if (CountedBody(native_ptr)) {
    DereferenceCounted(native_ptr);
    return;
  }

  auto object = XObject::GetNativeObject<XObject>(
      kernel_state(), kernel_memory()->TranslateVirtual(native_ptr));
  if (object) {
    object->ReleaseHandle();

  } else {
    if (native_ptr) {
      XELOGW("Unregistered guest object provided to ObDereferenceObject {:08X}",
             native_ptr);
    }
  }
  return;
}

void ObDereferenceObject_entry(dword_t native_ptr, const ppc_context_t& ctx) {
  xeObDereferenceObject(ctx, native_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(ObDereferenceObject, kNone, kImplemented);

void ObReferenceObject_entry(dword_t native_ptr) {
  if (CountedBody(native_ptr)) {
    ReferenceCounted(native_ptr);
    return;
  }
  // Check if a dummy value from ObReferenceObjectByHandle.
  auto object = XObject::GetNativeObject<XObject>(
      kernel_state(), kernel_memory()->TranslateVirtual(native_ptr));
  if (object) {
    object->RetainHandle();
  } else {
    if (native_ptr) {
      XELOGW("Unregistered guest object provided to ObReferenceObject {:08X}",
             native_ptr.value());
    }
  }
  return;
}
DECLARE_XBOXKRNL_EXPORT1(ObReferenceObject, kNone, kImplemented);

void xeObStripDosDevicesPrefix(std::string& path) {
  // Bytes, as a guest name need not be valid UTF-8.
  if (path.starts_with("\\??\\")) {
    path.erase(0, 4);
  }
}

std::string xeObSymbolicLinkName(const std::string_view name) {
  auto path = xe::utf8::canonicalize_guest_path(name);
  xeObStripDosDevicesPrefix(path);

  if (xe::utf8::starts_with_case(path, "\\System??\\")) {
    path.erase(0, 10);
  }
  return path;
}

dword_result_t ObCreateSymbolicLink_entry(pointer_t<X_ANSI_STRING> path_ptr,
                                          pointer_t<X_ANSI_STRING> target_ptr) {
  auto path =
      xeObSymbolicLinkName(util::TranslateAnsiPath(kernel_memory(), path_ptr));
  auto target = xe::utf8::canonicalize_guest_path(
      util::TranslateAnsiPath(kernel_memory(), target_ptr));

  // 4D5307DC expects success.
  if (kernel_state()->file_system()->FindSymbolicLink(path, target)) {
    return X_STATUS_SUCCESS;
  }

  if (!kernel_state()->file_system()->RegisterSymbolicLink(path, target)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObCreateSymbolicLink, kNone, kImplemented);

dword_result_t ObDeleteSymbolicLink_entry(pointer_t<X_ANSI_STRING> path_ptr) {
  auto path =
      xeObSymbolicLinkName(util::TranslateAnsiPath(kernel_memory(), path_ptr));
  if (!kernel_state()->file_system()->UnregisterSymbolicLink(path)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObDeleteSymbolicLink, kNone, kImplemented);

// Follows the symbolic links along an object path and gives back the path it
// ends at. The root has to be the DOS devices directory (-3) or none with an
// absolute name.
dword_result_t ObTranslateSymbolicLink_entry(
    pointer_t<X_OBJECT_ATTRIBUTES> object_attributes,
    pointer_t<X_ANSI_STRING> target_ptr) {
  if (!object_attributes || !target_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const uint32_t root = object_attributes->root_directory;
  const auto name = util::TranslateAnsiStringAddress(
      kernel_memory(), object_attributes->name_ptr);
  if (root != 0xFFFFFFFD && (root || name.empty() || name[0] != '\\')) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto file_system = kernel_state()->file_system();
  std::string path = xeObSymbolicLinkName(name);

  // A link is named by a whole leading component run, so try each component
  // boundary from the shortest. The target may itself start with a link.
  bool found = false;
  for (int depth = 0; depth < 16; ++depth) {
    bool resolved = false;
    for (size_t end = 1; end <= path.size(); ++end) {
      if (end != path.size() && path[end] != '\\') {
        continue;
      }
      const std::string prefix = path.substr(0, end);
      std::string link_target;
      if (file_system->FindSymbolicLink(prefix, link_target)) {
        path = link_target + path.substr(end);
        resolved = true;
        break;
      }
      if (!found && file_system->GetDevice(prefix)) {
        found = true;
      }
    }
    if (!resolved) {
      break;
    }
    found = true;
  }
  if (!found) {
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
  }

  X_ANSI_STRING* target = target_ptr;
  if (path.size() > target->maximum_length) {
    target->length = 0;
    return X_STATUS_BUFFER_TOO_SMALL;
  }
  if (!path.empty()) {
    std::memcpy(kernel_memory()->TranslateVirtual<char*>(target->pointer),
                path.data(), path.size());
  }
  target->length = uint16_t(path.size());
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObTranslateSymbolicLink, kNone, kSketchy);

// Whether an object was created by a title rather than the system, from the
// flag ObCreateObject leaves in its header.
dword_result_t ObIsTitleObject_entry(dword_t object_body) {
  if (object_body < sizeof(X_OBJECT_HEADER)) {
    return 0;
  }
  const uint32_t header_address =
      object_body - uint32_t(sizeof(X_OBJECT_HEADER));
  // Only the flags word, the last in the header, is read.
  const uint32_t flags_address =
      header_address + uint32_t(offsetof(X_OBJECT_HEADER, flags));
  auto heap = kernel_memory()->LookupHeap(flags_address);
  uint32_t protect = 0;
  if (!heap || !heap->QueryProtect(flags_address, &protect) ||
      !(protect & kMemoryProtectRead)) {
    return 0;
  }
  auto header =
      kernel_memory()->TranslateVirtual<X_OBJECT_HEADER*>(header_address);
  return (header->flags & OBJECT_HEADER_IS_TITLE_OBJECT) ? 1 : 0;
}
DECLARE_XBOXKRNL_EXPORT1(ObIsTitleObject, kNone, kImplemented);

dword_result_t NtDuplicateObject_entry(dword_t handle, lpdword_t new_handle_ptr,
                                       dword_t options) {
  // NOTE: this function seems to be used to get the current thread handle
  //       (passed handle=-2).
  // The kernel references the source's object, closes the source if asked,
  // then opens the object again. The copy takes the slot the close frees, so a
  // closed source's value comes back. A failure writes 0.
  auto object_table = kernel_state()->object_table();
  const bool close_source = options & 1;  // DUPLICATE_CLOSE_SOURCE
  X_HANDLE new_handle = 0;
  X_STATUS result = X_STATUS_SUCCESS;
  object_ref<XObject> object;
  uint32_t body = 0;
  {
    // A close can't free the object between the lookup and the reference.
    auto global_lock = global_critical_region::AcquireDirect();
    object = object_table->LookupObject<XObject>(handle, true);
    body = object ? CountedBody(object.get()) : 0;
    if (body) {
      ReferenceCounted(body);
    }
  }
  if (!object) {
    result = X_STATUS_INVALID_HANDLE;
  } else if (body) {
    if (close_source) {
      // The slot stays for the copy.
      CloseCounted(body);
      auto header = CountedHeader(body);
      auto global_lock = global_critical_region::AcquireDirect();
      header->pointer_count = header->pointer_count + 1;
      header->handle_count = header->handle_count + 1;
      new_handle = handle;
    } else {
      result = OpenCounted(body, 1, true, nullptr, &new_handle);
    }
    DereferenceCounted(body);
  } else if (close_source && handle < 0xFFFFFFFE) {
    // Nothing to close or open again for xenia's own objects.
    new_handle = handle;
  } else {
    result = object_table->DuplicateHandle(handle, &new_handle);
  }
  if (new_handle_ptr) {
    *new_handle_ptr = XSUCCEEDED(result) ? new_handle : 0;
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtDuplicateObject, kNone, kImplemented);

uint32_t NtClose(uint32_t handle) {
  auto object_table = kernel_state()->object_table();
  object_ref<XObject> object;
  uint32_t body;
  bool closed;
  X_STATUS result;
  {
    // The slot can't be reused between the lookup and the release.
    auto global_lock = global_critical_region::AcquireDirect();
    object = object_table->LookupObject<XObject>(handle, true);
    if (!object) {
      return X_STATUS_INVALID_HANDLE;
    }
    body = CountedBody(object.get());
    result = object_table->ReleaseHandle(handle);
    // A slot an extra handle reference keeps stays open.
    closed =
        object_table->LookupObject<XObject>(handle, true).get() != object.get();
  }
  // The handle leaves the table before the close procedure runs.
  if (XSUCCEEDED(result) && body && closed) {
    CloseCounted(body);
  }
  return result;
}

dword_result_t NtClose_entry(dword_t handle) { return NtClose(handle); }
DECLARE_XBOXKRNL_EXPORT1(NtClose, kNone, kImplemented);

dword_result_t ObCreateObject_entry(
    pointer_t<X_OBJECT_TYPE> object_factory,
    pointer_t<X_OBJECT_ATTRIBUTES> optional_attributes,
    dword_t object_size_sans_headers, lpdword_t out_object,
    const ppc_context_t& context) {
  uint32_t out_object_tmp = 0;

  uint32_t result =
      xeObCreateObject(object_factory, optional_attributes,
                       object_size_sans_headers, &out_object_tmp, context);
  *out_object = out_object_tmp;
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ObCreateObject, kNone, kImplemented);

dword_result_t ObInsertObject_entry(dword_t object,
                                    pointer_t<X_OBJECT_ATTRIBUTES> attributes,
                                    dword_t pointer_bias, lpdword_t out_handle,
                                    const ppc_context_t& ctx) {
  if (!object || !out_handle) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // The creation reference passes to the handle and the caller gets
  // |pointer_bias| more, which it later drops with ObDereferenceObject.
  object_ref<XObject> inserted;
  X_HANDLE handle;
  const X_STATUS result =
      OpenCounted(object, pointer_bias, true, &inserted, &handle);
  if (XFAILED(result)) {
    return result;
  }
  inserted->SetAttributes(attributes.guest_address());
  if (!inserted->name().empty() &&
      kernel_state()->object_table()->IsNameMappedTo(inserted->name(),
                                                     handle)) {
    // The directory holds a reference until the last handle closes.
    auto header = CountedHeader(object);
    auto global_lock = global_critical_region::AcquireDirect();
    header->pointer_count = header->pointer_count + 1;
    header->flags =
        int16_t(header->flags | OBJECT_HEADER_FLAG_CONTAINED_IN_DIRECTORY);
    if (header->flags & OBJECT_HEADER_FLAG_NAMED_OBJECT) {
      // The name info before the header holds the name in the directory.
      const uint32_t name_info_ptr =
          object -
          uint32_t(sizeof(X_OBJECT_HEADER_NAME_INFO) + sizeof(X_OBJECT_HEADER));
      auto name_info =
          kernel_memory()->TranslateVirtual<X_OBJECT_HEADER_NAME_INFO*>(
              name_info_ptr);
      header->hash_index = int8_t(xeObHashObjectName(&name_info->name, ctx));
    }
  }
  *out_handle = handle;
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObInsertObject, kNone, kSketchy);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Ob);
