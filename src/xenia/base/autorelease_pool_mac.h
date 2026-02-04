/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_AUTORELEASE_POOL_MAC_H_
#define XENIA_BASE_AUTORELEASE_POOL_MAC_H_

#include <mutex>
#include <string>
#include <vector>

#include "xenia/base/platform.h"

#if XE_PLATFORM_MAC

namespace xe {

// Autorelease pool management for macOS.
//
// On macOS, Objective-C objects (including those created via metal-cpp) follow
// Cocoa memory management rules. Objects returned by methods NOT beginning with
// alloc/new/copy/mutableCopy/Create are added to an autorelease pool and will
// be released when that pool is drained.
//
// From metal-cpp documentation:
// - Create an AutoreleasePool in main() and for every thread you create
// - Create additional pools to reduce memory watermark when creating many
//   autoreleased objects (e.g., during rendering)
// - Without an enclosing pool, autoreleased objects leak
//
// Debug tip: Set OBJC_DEBUG_MISSING_POOLS=YES to detect missing pools.
//
// This tracker helps debug pool lifetime issues by detecting:
// - Pools popped in wrong order (stack mismatch)
// - Pools never popped (leaks)
// - Pools held too long (>1 second warning)
class AutoreleasePoolTracker {
 public:
  static void* Push(const char* location);
  static void Pop(void* pool, const char* location);
  static void CheckLeaks();
  static int GetDepth();
  static void Reset();
  static void SetEnabled(bool enabled);

 private:
  struct PoolInfo {
    void* pool;
    std::string location;
    uint64_t push_time;
  };

  static thread_local int pool_depth_;
  static thread_local std::vector<PoolInfo> pool_stack_;
  static thread_local bool initialized_;
  static bool enabled_;
  static std::mutex tracker_mutex_;

  static void EnsureInitialized();
};

// RAII wrapper for autorelease pools.
class ScopedAutoreleasePool {
 public:
  explicit ScopedAutoreleasePool(const char* name);
  ~ScopedAutoreleasePool();

  ScopedAutoreleasePool(const ScopedAutoreleasePool&) = delete;
  ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;
  ScopedAutoreleasePool(ScopedAutoreleasePool&&) = delete;
  ScopedAutoreleasePool& operator=(ScopedAutoreleasePool&&) = delete;

 private:
  void* pool_;
  const char* name_;
};

}  // namespace xe

#define XE_AUTORELEASE_POOL_PUSH(location) \
  xe::AutoreleasePoolTracker::Push(location)

#define XE_AUTORELEASE_POOL_POP(pool, location) \
  xe::AutoreleasePoolTracker::Pop(pool, location)

#define XE_AUTORELEASE_POOL_CHECK_LEAKS() \
  xe::AutoreleasePoolTracker::CheckLeaks()

#define XE_SCOPED_AUTORELEASE_POOL(name) \
  xe::ScopedAutoreleasePool _autorelease_pool_##__LINE__(name)

#else  // !XE_PLATFORM_MAC

#define XE_AUTORELEASE_POOL_PUSH(location) nullptr
#define XE_AUTORELEASE_POOL_POP(pool, location) (void)0
#define XE_AUTORELEASE_POOL_CHECK_LEAKS() (void)0
#define XE_SCOPED_AUTORELEASE_POOL(name) (void)0

#endif  // XE_PLATFORM_MAC

#endif  // XENIA_BASE_AUTORELEASE_POOL_MAC_H_
