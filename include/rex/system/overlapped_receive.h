/**
 * @file        overlapped_receive.h
 * @brief       Overlapped (asynchronous) guest socket receives.
 *
 * A guest WSARecvFrom with an OVERLAPPED that finds no datagram stays pending, and
 * WSAGetOverlappedResult tries it again until a datagram arrives. Titles that poll their
 * sockets (Quazal's PRUDP) get their replies this way. Pure logic: the receive attempt,
 * the clock and the sleep are passed in, so the export code supplies the host socket and
 * the tests a fake one.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace rex::system::net {

// Guest-visible WSA error codes for these paths.
inline constexpr uint32_t kWsaIoIncomplete = 996;
inline constexpr uint32_t kWsaIoPending = 997;
inline constexpr uint32_t kWsaEWouldBlock = 10035;

// Pending receives by the guest address of their OVERLAPPED. `Request` holds what a later
// attempt needs and a `socket` member (the guest socket handle). A receive attempt is a
// callable taking the request and returning the datagram's size, or a negative value when
// none is waiting.
template <typename Request>
class OverlappedReceives {
 public:
  enum class Status {
    kCompleted,   // `bytes` received
    kPending,     // WSARecvFrom: overlapped, nothing yet (WSA_IO_PENDING)
    kWouldBlock,  // WSARecvFrom: not overlapped, nothing yet (WSAEWOULDBLOCK)
    kIncomplete,  // WSAGetOverlappedResult: still pending (WSA_IO_INCOMPLETE)
    kNotPending,  // WSAGetOverlappedResult: nothing outstanding; the OVERLAPPED holds the result
  };
  struct Outcome {
    Status status;
    int bytes = 0;
  };

  // WSARecvFrom. `overlapped` is 0 for a plain receive. A new receive on an OVERLAPPED
  // that is still pending replaces the old one.
  template <typename Receive>
  Outcome Start(uint32_t overlapped, const Request& request, Receive&& receive) {
    std::lock_guard lock(mutex_);
    if (overlapped) {
      pending_.erase(overlapped);
    }
    const int received = receive(request);
    if (received >= 0) {
      return {Status::kCompleted, received};
    }
    if (!overlapped) {
      return {Status::kWouldBlock};
    }
    pending_[overlapped] = request;
    return {Status::kPending};
  }

  // WSAGetOverlappedResult. Tries the pending receive until it completes or `wait` has
  // passed (zero: one try), sleeping between tries.
  template <typename Receive, typename Now, typename Sleep>
  Outcome Poll(uint32_t overlapped, std::chrono::steady_clock::duration wait, Receive&& receive,
               Now&& now, Sleep&& sleep) {
    const auto give_up = now() + wait;
    for (;;) {
      {
        // Under the lock, so two pollers of one OVERLAPPED can't both take a datagram.
        std::lock_guard lock(mutex_);
        auto it = pending_.find(overlapped);
        if (it == pending_.end()) {
          return {Status::kNotPending};
        }
        const int received = receive(it->second);
        if (received >= 0) {
          pending_.erase(it);
          return {Status::kCompleted, received};
        }
      }
      if (now() >= give_up) {
        return {Status::kIncomplete};
      }
      sleep();
    }
  }

  // closesocket: the socket's pending receives can never complete.
  void Forget(uint32_t socket) {
    std::lock_guard lock(mutex_);
    std::erase_if(pending_, [socket](const auto& entry) { return entry.second.socket == socket; });
  }

  size_t pending() const {
    std::lock_guard lock(mutex_);
    return pending_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, Request> pending_;
};

}  // namespace rex::system::net
