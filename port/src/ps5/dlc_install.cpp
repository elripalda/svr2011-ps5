// [ps5 port] DLC installed from the hub's DLC page. The game installs a
// package it finds in DLC/ while it starts (dlc.cpp: ContentManager::
// InstallContent), on a black screen that looks frozen for a big pack. The
// hub does the same thing here first, with progress: the files into
// <name>.installing, then put in place, then the .header (the game lists DLC
// by its headers), so the game never sees half a pack and skips it at start.
#include "dlc_install.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <queue>
#include <span>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xtypes.h>

namespace fs = std::filesystem;

namespace svr2011::ps5 {

using rex::X_STATUS;  // (X_STATUS_SUCCESS)

namespace {
constexpr uint32_t kTitleId = 0x5451085D;
}

bool InstallDlcPackage(const fs::path& package, const fs::path& user_data, const std::string& name,
                       hub::install::Progress& p, std::string& error) {
  const fs::path title = user_data / "0000000000000000" / "5451085D";
  const fs::path installed = title / "00000002";
  const fs::path header = title / "Headers" / "00000002" / (name + ".header");
  const fs::path dest = installed / name, work = installed / (name + ".installing");
  std::error_code ec;
  REXLOG_INFO("[dlc] hub: installing {} ({} MB) as {}", package.filename().string(), fs::file_size(package, ec) >> 20,
              name);
  const auto start = std::chrono::steady_clock::now();

  auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", package);
  if (!device->Initialize()) {
    error = "It can't be read as a DLC package. It may not have finished copying: copy it again.";
    REXLOG_ERROR("[dlc] hub: {} isn't a readable package", package.filename().string());
    return false;
  }
  rex::filesystem::Entry* root = device->ResolvePath("");
  if (!root) {
    error = "The package has no files.";
    return false;
  }
  std::vector<rex::filesystem::Entry*> entries;
  uint64_t total = 0;
  for (std::queue<rex::filesystem::Entry*> q(std::deque<rex::filesystem::Entry*>{root}); !q.empty(); q.pop()) {
    for (const auto& child : q.front()->children()) q.push(child.get());
    if (q.front() != root) entries.push_back(q.front());
    if (!(q.front()->attributes() & rex::filesystem::kFileAttributeDirectory)) total += q.front()->size();
  }
  p.total = total;
  p.done = 0;

  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  if (ec) {
    error = "Its folder couldn't be made (" + ec.message() + ").";
    return false;
  }
  auto fail = [&](std::string why) {
    std::error_code e2;
    fs::remove_all(work, e2);
    error = std::move(why);
    REXLOG_ERROR("[dlc] hub: {}: {}", name, error);
    return false;
  };
  std::vector<uint8_t> buffer(4u << 20);
  for (rex::filesystem::Entry* e : entries) {
    const fs::path out = work / rex::to_path(rex::string::utf8_fix_path_separators(e->path()));
    if (e->attributes() & rex::filesystem::kFileAttributeDirectory) {
      fs::create_directories(out, ec);
      continue;
    }
    fs::create_directories(out.parent_path(), ec);
    rex::filesystem::File* in = nullptr;
    if (e->Open(rex::filesystem::FileAccess::kFileReadData, &in) != X_STATUS_SUCCESS || !in)
      return fail("Its file " + e->path() + " can't be read.");
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    bool ok = bool(o);
    for (size_t at = 0, left = e->size(); ok && left;) {
      if (p.cancel) {
        in->Destroy();
        return fail("stopped");
      }
      size_t got = 0;
      in->ReadSync(std::span<uint8_t>(buffer.data(), std::min(left, buffer.size())), at, &got);
      if (!got) {
        ok = false;
        break;
      }
      ok = bool(o.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(got)));
      at += got, left -= got;
      p.done += got;
    }
    in->Destroy();
    o.close();
    if (!ok || !o) return fail("It couldn't be written. Is the console's storage full?");
  }

  // into place, then the .header the game finds it by
  fs::remove(header, ec);
  fs::remove_all(dest, ec);
  fs::rename(work, dest, ec);
  if (ec) return fail("Its folder couldn't be put in place (" + ec.message() + ").");
  rex::system::xam::XCONTENT_AGGREGATE_DATA data;
  std::memset(static_cast<void*>(&data), 0, sizeof(data));
  data.device_id = 1;  // (the hard drive: ContentManager's DummyDeviceId::HDD)
  data.content_type = rex::system::XContentType::kMarketplaceContent;
  data.title_id = kTitleId;
  data.xuid = 0;
  data.set_file_name(name);
  const std::u16string shown = device->header().metadata.display_name(rex::system::XLanguage::kEnglish);
  data.set_display_name(shown.empty() ? rex::path_to_utf16(package.filename()) : shown);
  uint32_t license_mask = 0;
  for (const auto& license : device->header().header.licenses)
    if (license.license_flags) license_mask |= license.license_bits;
  fs::create_directories(header.parent_path(), ec);
  {
    std::ofstream h(header, std::ios::binary | std::ios::trunc);
    h.write(reinterpret_cast<const char*>(&data), sizeof(data));
    if (license_mask) h.write(reinterpret_cast<const char*>(&license_mask), sizeof(license_mask));
    if (!h) {
      std::error_code e2;
      fs::remove_all(dest, e2);
      error = "Its .header file couldn't be written.";
      return false;
    }
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  REXLOG_INFO("[dlc] hub: installed {} ({} files, {} MB) in {:.1f} s", name, entries.size(), total >> 20, seconds);
  rex::FlushLogging();
  return true;
}

}  // namespace svr2011::ps5
