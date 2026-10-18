/**
 * @file        http.h
 * @brief       The online client's HTTP requests (libcurl).
 *
 * After the Xenia netplay fork's XLiveAPI::Get/Post/Delete (src/xenia/kernel/XLiveAPI.cpp,
 * Copyright 2026 Xenia Canary, BSD license).
 */

#pragma once

#include <string>

namespace rex::kernel::xam::live {

struct HttpResponse {
  long status = 0;  // 0 when the request failed
  std::string body;
  std::string local_ip;  // the address the request left from
};

// `method` GET, POST or DELETE; `body` is sent as JSON. Requests take the default route
// (a loopback --wwe_online_ip cannot reach a server on the internet).
HttpResponse HttpRequest(const std::string& method, const std::string& url,
                         const std::string& body = {});

}  // namespace rex::kernel::xam::live
