// [ps5 port] See host_case.h.
#include "host_case.h"

#include <cctype>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace svr2011 {

namespace {
std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

fs::path HostCase(const fs::path& p) {
#if defined(_WIN32)
  return p;
#else
  std::error_code ec;
  if (p.empty() || fs::exists(p, ec)) return p;
  // the longest part that exists, then the rest matched one part at a time
  fs::path base = p;
  std::vector<fs::path> rest;
  while (!base.empty() && !fs::exists(base, ec)) {
    if (!base.has_filename()) {
      base = base.parent_path();
      continue;
    }
    rest.push_back(base.filename());
    const fs::path parent = base.parent_path();
    if (parent == base) return p;
    base = parent;
  }
  if (base.empty()) base = ".";
  for (auto it = rest.rbegin(); it != rest.rend(); ++it) {
    const std::string want = Lower(it->string());
    fs::path found;
    for (fs::directory_iterator d(base, ec), end; !ec && d != end; d.increment(ec)) {
      if (Lower(d->path().filename().string()) == want) {
        found = d->path();
        break;
      }
    }
    if (found.empty()) return p;
    base = found;
  }
  return base;
#endif
}

fs::path RelativePath(const fs::path& p, const fs::path& base) {
  const fs::path r = p.lexically_normal().lexically_relative(base.lexically_normal());
  if (!r.empty() && *r.begin() != "..") return r;
  std::error_code ec;
  const fs::path q = fs::relative(p, base, ec);
  return ec || q.empty() ? r : q;
}

}  // namespace svr2011
