/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <forward_list>

#include "third_party/disruptorplus/include/disruptorplus/blocking_wait_strategy.hpp"
#include "third_party/disruptorplus/include/disruptorplus/multi_threaded_claim_strategy.hpp"
#include "third_party/disruptorplus/include/disruptorplus/ring_buffer.hpp"
#include "third_party/disruptorplus/include/disruptorplus/sequence_barrier.hpp"
#include "third_party/disruptorplus/include/disruptorplus/spin_wait.hpp"
#include "third_party/disruptorplus/include/disruptorplus/spin_wait_strategy.hpp"
#include "xenia/base/assert.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"
#include "xenia/base/threading_timer_queue.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif
#if XE_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

namespace dp = disruptorplus;

namespace xe {
namespace threading {

using WaitItem = TimerQueueWaitItem;
/*
        chrispy: changed this to a blocking wait from a spin-wait, the spin was
   monopolizing a ton of cpu time (depending on the game 2-4% of total cpu time)
   on my 3990x no complaints since that change
*/

/*
        edit: actually had to change it back, when i was testing it only worked
   because i fixed disruptorplus' code to compile (it gives wrong args to
   condition_variable::wait_until) but now builds

*/

/*
    edit2: (30.12.2024) After uplifting version of MSVC compiler Xenia cannot be
   correctly initialized if you're using proton.
*/
// Windows producers keep spinning: blocking waits deadlocked initialisation
// under Proton (c3301d928).
// The dispatch thread waits on Win32 handles instead, since spin waits sleep
// 1 ms at a time and fire timers 1-3 ms late.
#if XE_PLATFORM_WIN32
using WaitStrat = dp::spin_wait_strategy;
#else
using WaitStrat = dp::blocking_wait_strategy;
#endif

class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

 public:
  TimerQueue()
      : buffer_(kWaitCount),
        wait_strategy_(),
        claim_strategy_(kWaitCount, wait_strategy_),
        consumed_(wait_strategy_),
        shutdown_(false) {
    claim_strategy_.add_claim_barrier(consumed_);
#if XE_PLATFORM_WIN32
    wake_event_ = CreateEventW(nullptr, false, false, nullptr);
    wait_timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
    if (!wait_timer_) {
      // Before Windows 10 1803.
      wait_timer_ =
          CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
#endif
    dispatch_thread_ = std::thread(&TimerQueue::TimerThreadMain, this);
  }

  ~TimerQueue() {
    shutdown_.store(true, std::memory_order_release);

    // Kick dispatch thread to check shutdown flag
    auto wait_item = std::make_shared<WaitItem>(nullptr, nullptr, this,
                                                clock::time_point::min(),
                                                clock::duration::zero());
    wait_item->Disarm();
    QueueTimer(std::move(wait_item));

    dispatch_thread_.join();
#if XE_PLATFORM_WIN32
    CloseHandle(wait_timer_);
    CloseHandle(wake_event_);
#endif
  }

  void TimerThreadMain() {
    dp::sequence_t next_sequence = 0;
    const auto comp = [](const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
      return left->due_ < right->due_;
    };

    xe::threading::set_name("xe::threading::TimerQueue");
#if XE_PLATFORM_MAC
    // Timer coalescing wakes an ordinary thread a quarter of the wait late, 1
    // ms on a 4 ms timer. A time-constraint thread wakes within tens of us.
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    const auto ms_to_abs = [&](uint64_t ms) {
      return uint32_t(ms * 1000000 * timebase.denom / timebase.numer);
    };
    thread_time_constraint_policy_data_t policy;
    policy.period = 0;
    policy.computation = ms_to_abs(1);
    policy.constraint = ms_to_abs(2);
    policy.preemptible = 1;
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy),
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#endif

    while (!shutdown_.load(std::memory_order_relaxed)) {
      {
        // Consume new wait items and add them to sorted wait queue
        dp::sequence_t available = WaitUntilPublished(
            next_sequence, wait_queue_.empty() ? clock::now() + kIdleWait
                                               : wait_queue_.front()->due_);

        // Check for timeout
        if (available != next_sequence - 1) {
          std::forward_list<std::shared_ptr<WaitItem>> wait_items;
          do {
            wait_items.push_front(std::move(buffer_[next_sequence]));
          } while (next_sequence++ != available);

          consumed_.publish(available);

          wait_items.sort(comp);
          wait_queue_.merge(wait_items, comp);
        }
      }

      {
        // Check wait queue, invoke callbacks and reschedule
        std::forward_list<std::shared_ptr<WaitItem>> wait_items;
        while (!wait_queue_.empty() &&
               wait_queue_.front()->due_ <= clock::now()) {
          auto wait_item = std::move(wait_queue_.front());
          wait_queue_.pop_front();

          // Ensure that it isn't disarmed
          auto state = WaitItem::State::kIdle;
          if (wait_item->state_.compare_exchange_strong(
                  state, WaitItem::State::kInCallback,
                  std::memory_order_acq_rel)) {
            // Possibility to dispatch to a thread pool here
            assert_not_null(wait_item->callback_);
            wait_item->callback_(wait_item->userdata_);

            if (wait_item->interval_ != clock::duration::zero() &&
                wait_item->state_.load(std::memory_order_acquire) !=
                    WaitItem::State::kInCallbackSelfDisarmed) {
              // Item is recurring and didn't self-disarm during callback:
              wait_item->due_ += wait_item->interval_;
              wait_item->state_.store(WaitItem::State::kIdle,
                                      std::memory_order_release);
              wait_items.push_front(std::move(wait_item));
            } else {
              wait_item->state_.store(WaitItem::State::kDisarmed,
                                      std::memory_order_release);
            }
          } else {
            // Specifically, kInCallback is illegal here
            assert_true(WaitItem::State::kDisarmed == state);
          }
        }
        wait_items.sort(comp);
        wait_queue_.merge(wait_items, comp);
      }
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ =
        std::max(clock::now() - wait_item->interval_, wait_item->due_);

    auto sequence = claim_strategy_.claim_one();
    buffer_[sequence] = std::move(wait_item);
    claim_strategy_.publish(sequence);
#if XE_PLATFORM_WIN32
    // The dispatch thread looks for new items before it waits, so a callback
    // re-arming its own timer needs no wake.
    if (std::this_thread::get_id() != dispatch_thread_.get_id()) {
      SetEvent(wake_event_);
    }
#endif

    return wait_item_weak;
  }

  const std::thread& dispatch_thread() const { return dispatch_thread_; }

 private:
  // Returns the last published sequence, or next_sequence - 1 if nothing new
  // was published.
  dp::sequence_t WaitUntilPublished(dp::sequence_t next_sequence,
                                    clock::time_point timeout) {
#if XE_PLATFORM_WIN32
    dp::sequence_t available =
        claim_strategy_.last_published_after(next_sequence - 1);
    if (available != next_sequence - 1) {
      return available;
    }
    auto wait = timeout - clock::now();
    if (wait <= clock::duration::zero()) {
      return available;
    }
    // A negative due time is relative, in 100 ns units.
    LARGE_INTEGER due_time;
    due_time.QuadPart =
        -std::chrono::ceil<
             std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(wait)
             .count();
    HANDLE handles[] = {wake_event_, wait_timer_};
    DWORD handle_count = 1;
    if (wait_timer_ &&
        SetWaitableTimer(wait_timer_, &due_time, 0, nullptr, nullptr, false)) {
      handle_count = 2;
    }
    WaitForMultipleObjects(
        handle_count, handles, false,
        handle_count == 2
            ? INFINITE
            : DWORD(
                  std::chrono::ceil<std::chrono::milliseconds>(wait).count()));
    // A publish from another thread sets the event once it is visible. This
    // thread's own publishes came before the check above.
    return claim_strategy_.last_published_after(next_sequence - 1);
#else
    return claim_strategy_.wait_until_published(next_sequence,
                                                next_sequence - 1, timeout);
#endif
  }

  static constexpr clock::duration kIdleWait = std::chrono::seconds(60);

  // This ring buffer will be used to introduce timers queued by the public API
  static constexpr size_t kWaitCount = 512;
  dp::ring_buffer<std::shared_ptr<WaitItem>> buffer_;

  WaitStrat wait_strategy_;
  dp::multi_threaded_claim_strategy<WaitStrat> claim_strategy_;
  dp::sequence_barrier<WaitStrat> consumed_;

  // This is a _sorted_ (ascending due_) list of active timers managed by a
  // dedicated thread
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;
  std::atomic_bool shutdown_;
  std::thread dispatch_thread_;
#if XE_PLATFORM_WIN32
  // Set after a publish from another thread.
  HANDLE wake_event_ = nullptr;
  HANDLE wait_timer_ = nullptr;
#endif
};

xe::threading::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread().get_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  dp::spin_wait spinner;
  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed,
                                       std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      // Do not break for kInCallbackSelfDisarmed and keep spinning in order to
      // meet guarantees
      break;
    }
    state = State::kIdle;
    spinner.spin_once();
  }
}
// Used by one-shot Timers
std::weak_ptr<WaitItem> QueueTimerOnce(std::function<void(void*)> callback,
                                       void* userdata,
                                       WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_,
                                 due, WaitItem::clock::duration::zero()));
}
// Used by HighResolutionTimer and periodic Timers
std::weak_ptr<WaitItem> QueueTimerRecurring(
    std::function<void(void*)> callback, void* userdata,
    WaitItem::clock::time_point due, WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, interval));
}

}  // namespace threading
}  // namespace xe
