/**
 * @file        ui/present_waiter.h
 * @brief       Follows a swapchain's presents as the display shows them (present-wait)
 *
 * One thread waits for each present id in order, as DXVK's presenter does, and records when the
 * wait returned. The pacing thread uses it to keep one frame queued with vsync (wait until the
 * previous present was shown before presenting the next) and the swap log records the times.
 * The wait itself is a function (vkWaitForPresent2KHR or vkWaitForPresentKHR in the Vulkan
 * presenter), so this has no Vulkan types. The returned time is when the driver says the present
 * was reached, not a scan-out time: it limits queued frames and is logged, it does not set targets.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace rex::ui::pacing {

class PresentWaiter {
 public:
  using Clock = std::chrono::steady_clock;
  enum class WaitResult { kShown, kTimeout, kFailed };
  // Waits until present `id` was shown, for at most `timeout`. Called only from the waiter thread,
  // and never after Detach returned.
  using WaitFn = std::function<WaitResult(uint64_t id, std::chrono::nanoseconds timeout)>;

  // How long one wait call may block; Detach waits at most this long for it.
  static constexpr std::chrono::milliseconds kPollTimeout{10};

  struct QueueWait {
    bool active = false;          // false: no present-wait on this swapchain (timer pacing)
    uint32_t unshown_before = 0;  // presents not yet shown when the wait started
    Clock::duration waited{};
    bool timed_out = false;
  };

  struct Shown {
    uint64_t id = 0;
    Clock::time_point presented;  // when vkQueuePresentKHR returned
    Clock::time_point shown;      // when the wait for it returned
  };

  PresentWaiter() : thread_([this] { Run(); }) {}
  ~PresentWaiter() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  PresentWaiter(const PresentWaiter&) = delete;
  PresentWaiter& operator=(const PresentWaiter&) = delete;

  // A new swapchain created with present ids: ids start at 1 again.
  void Attach(WaitFn fn) {
    {
      std::lock_guard lock(mutex_);
      ResetLocked();
      wait_fn_ = std::move(fn);
      attached_ = true;
    }
    cv_.notify_all();
  }

  // Call before the swapchain is retired or destroyed. Returns once no wait runs on it.
  void Detach() {
    std::unique_lock lock(mutex_);
    ResetLocked();
    cv_.notify_all();
    cv_.wait(lock, [this] { return !in_wait_; });
    wait_fn_ = nullptr;
  }

  // The id to chain into the next present; 0 means present without an id.
  uint64_t NextId() {
    std::lock_guard lock(mutex_);
    return attached_ && !failed_ ? ++last_id_ : 0;
  }

  // The id handed out last (0 before the first present of this swapchain).
  uint64_t LastId() const {
    std::lock_guard lock(mutex_);
    return last_id_;
  }

  // After a successful present carrying `id`.
  void Presented(uint64_t id, Clock::time_point when) {
    if (!id) {
      return;
    }
    {
      std::lock_guard lock(mutex_);
      if (!attached_ || failed_ || id <= shown_id_) {
        return;
      }
      pending_.emplace_back(id, when);
    }
    cv_.notify_all();
  }

  // Blocks until at most `max_unshown` presents are still unshown, or for `timeout`.
  QueueWait WaitForShown(uint32_t max_unshown, Clock::duration timeout) {
    QueueWait result;
    std::unique_lock lock(mutex_);
    if (!attached_ || failed_) {
      return result;
    }
    result.active = true;
    result.unshown_before = uint32_t(pending_.size());
    if (pending_.size() <= max_unshown) {
      return result;
    }
    const Clock::time_point start = Clock::now();
    const uint64_t generation = generation_;
    result.timed_out = !cv_.wait_until(lock, start + timeout, [&] {
      return generation_ != generation || failed_ || pending_.size() <= max_unshown;
    });
    result.waited = Clock::now() - start;
    return result;
  }

  // The newest present shown since the last call.
  std::optional<Shown> TakeLatestShown() {
    std::lock_guard lock(mutex_);
    return std::exchange(latest_shown_, std::nullopt);
  }

  bool failed() const {
    std::lock_guard lock(mutex_);
    return failed_;
  }

 private:
  void ResetLocked() {
    attached_ = failed_ = false;
    ++generation_;
    last_id_ = shown_id_ = 0;
    pending_.clear();
    latest_shown_.reset();
  }

  void Run() {
    std::unique_lock lock(mutex_);
    while (true) {
      cv_.wait(lock, [this] { return stop_ || (attached_ && !failed_ && !pending_.empty()); });
      if (stop_) {
        return;
      }
      const auto [id, presented] = pending_.front();
      const uint64_t generation = generation_;
      WaitFn fn = wait_fn_;
      in_wait_ = true;
      lock.unlock();
      const WaitResult result = fn(id, kPollTimeout);
      const Clock::time_point now = Clock::now();
      lock.lock();
      in_wait_ = false;
      if (generation == generation_) {
        if (result == WaitResult::kShown) {
          // Presents are shown in order: reaching `id` means every earlier one was reached too.
          while (!pending_.empty() && pending_.front().first <= id) {
            pending_.pop_front();
          }
          shown_id_ = id;
          latest_shown_ = Shown{id, presented, now};
        } else if (result == WaitResult::kFailed) {
          failed_ = true;
          pending_.clear();
        }
      }
      cv_.notify_all();
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  WaitFn wait_fn_;
  std::deque<std::pair<uint64_t, Clock::time_point>> pending_;
  std::optional<Shown> latest_shown_;
  uint64_t last_id_ = 0;
  uint64_t shown_id_ = 0;
  uint64_t generation_ = 0;
  bool attached_ = false;
  bool failed_ = false;
  bool in_wait_ = false;
  bool stop_ = false;
  std::thread thread_;  // last: starts after the members above exist
};

}  // namespace rex::ui::pacing
