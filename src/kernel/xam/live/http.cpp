/**
 * @file        http.cpp
 * @brief       The online client's HTTP requests (see http.h).
 *
 * After the Xenia netplay fork's XLiveAPI::Get/Post/Delete (src/xenia/kernel/XLiveAPI.cpp,
 * Copyright 2026 Xenia Canary, BSD license).
 */

#include <rex/kernel/xam/live/http.h>

#include <mutex>

#include <curl/curl.h>

#include <rex/logging.h>

namespace rex::kernel::xam::live {

namespace {

size_t Append(void* data, size_t size, size_t count, void* body) {
  static_cast<std::string*>(body)->append(static_cast<const char*>(data), size * count);
  return size * count;
}

}  // namespace

HttpResponse HttpRequest(const std::string& method, const std::string& url,
                         const std::string& body) {
  static std::once_flag global_init;
  std::call_once(global_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

  HttpResponse response;
  CURL* curl = curl_easy_init();
  if (!curl) {
    REXKRNL_ERROR("[online] cannot initialize libcurl");
    return response;
  }
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "wweallstars");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Append);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(body.size()));
  }
  const CURLcode result = curl_easy_perform(curl);
  if (result == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    char* local_ip = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_LOCAL_IP, &local_ip) == CURLE_OK && local_ip) {
      response.local_ip = local_ip;
    }
  } else {
    REXKRNL_ERROR("[online] {} {}: {}", method, url, curl_easy_strerror(result));
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

}  // namespace rex::kernel::xam::live
