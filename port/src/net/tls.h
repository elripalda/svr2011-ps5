// WWE SmackDown vs. Raw 2011 - https where the platform has no TLS of its
// own (the PS5, Linux): Mbed TLS over a connected socket, the server checked
// against the Mozilla CA list built into the program (assets/certs).
#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace svr2011::net {

class Tls {
 public:
  // The handshake with `host` over `socket` (connected, blocking, with
  // timeouts); null (and the reason logged) if it fails.
  static std::unique_ptr<Tls> Start(int socket, const std::string& host);
  ~Tls();
  bool SendAll(const void* data, size_t size);
  // Up to `size` bytes; 0 at the end, -1 on an error or a timeout.
  int Recv(void* buf, size_t size);

 private:
  Tls() = default;
  struct State;
  std::unique_ptr<State> s_;
};

}  // namespace svr2011::net
