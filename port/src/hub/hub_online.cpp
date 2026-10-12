// WWE SmackDown vs. Raw 2011 - the PS5 hub's Online page (the account on the
// Community Creations server, as the PC launcher's Online tab) and its
// on-screen keyboard.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <mutex>
#include <thread>

#include "hub_style.h"
#include "hub_ui.h"

namespace svr2011::hub {

namespace {

constexpr const char* kDefaultServer = "https://sho-ti.me/svr";

std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) >= 0x20) out += c;
  }
  return out + "\"";
}

// A string field of a flat JSON answer ("" if none).
std::string Field(const std::string& json, const std::string& key) {
  const size_t k = json.find("\"" + key + "\"");
  if (k == std::string::npos) return {};
  size_t i = json.find(':', k);
  if (i == std::string::npos) return {};
  i = json.find_first_not_of(" \t\r\n", i + 1);
  if (i == std::string::npos) return {};
  if (json[i] != '"') {  // (a number)
    const size_t e = json.find_first_of(",}", i);
    return json.substr(i, e - i);
  }
  std::string out;
  for (++i; i < json.size() && json[i] != '"'; ++i) {
    if (json[i] == '\\' && i + 1 < json.size()) ++i;
    out += json[i];
  }
  return out;
}

std::string Unquoted(std::string v) {
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  return v;
}

// The keyboard's keys: 4 rows of 10, then the bottom row (widths in units).
const char* const kRows[2][4] = {{"1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm_-."},
                                 {"!#$%&*()+=", "QWERTYUIOP", "ASDFGHJKL?", "ZXCVBNM,;'"}};
struct BottomKey {
  const char* label;
  int units;
  int what;  // 0 shift, 1 ':', 2 '/', 3 space, 4 delete, 5 done
};
const BottomKey kBottom[] = {{"SHIFT", 2, 0}, {":", 1, 1}, {"/", 1, 2}, {"SPACE", 2, 3}, {"DELETE", 2, 4}, {"DONE", 2, 5}};

int BottomAt(int col) {  // the bottom key under a unit column
  int u = 0;
  for (int k = 0; k < 6; ++k) {
    if (col < u + kBottom[k].units) return k;
    u += kBottom[k].units;
  }
  return 5;
}
int BottomStart(int k) {
  int u = 0;
  for (int i = 0; i < k; ++i) u += kBottom[i].units;
  return u;
}

}  // namespace

struct Hub::Job {
  std::thread thread;
  std::atomic<bool> done{false};
  int status = 0;
  std::string answer;
  std::function<void(int, const std::string&)> then;
};

Hub::~Hub() {
  if (job_ && job_->thread.joinable()) job_->thread.detach();  // (it finishes on its own)
}

std::string Hub::ServerUrl() const {
  std::string base = Value("online_server", kDefaultServer);
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (base.find("://") == std::string::npos) base = "http://" + base;  // (a bare host:port)
  return base;
}

void Hub::Call(std::string method, std::string path, std::string body, std::string token,
               std::function<void(int, const std::string&)> then) {
  if (!request_) {
    then(0, "");
    return;
  }
  auto job = std::make_shared<Job>();
  job->then = std::move(then);
  job_ = job;
  job->thread = std::thread([job, req = request_, method, url = ServerUrl() + path, body, token] {
    job->status = req(method, url, body, token, &job->answer);
    job->done = true;
  });
}

void Hub::Ask(std::string title, std::string text, bool secret, std::function<void(const std::string&)> done) {
  kb_.open = true, kb_.secret = secret, kb_.shift = false;
  kb_.title = std::move(title), kb_.text = std::move(text);
  kb_.row = 1, kb_.col = 0, kb_.in = 0, kb_.fx = kb_.fy = -1;
  kb_.done = std::move(done);
}

void Hub::SignIn(bool create) {
  Ask("Account Name", Unquoted(Value("online_name", "")), false, [this, create](const std::string& name) {
    if (name.empty()) return;
    Ask("Password", "", true, [this, create, name](const std::string& password) {
      if (password.empty()) return;
      online_msg_ = create ? "Creating your account..." : "Signing in...";
      auto finish = [this, name](int status, const std::string& answer) {
        const std::string token = Field(answer, "token");
        if (status != 200 || token.empty()) {
          const std::string error = Field(answer, "error");
          online_msg_ = !status          ? "Couldn't reach the server: " + (answer.empty() ? std::string("no answer") : answer)
                        : !error.empty() ? error
                                         : "The server refused (HTTP " + std::to_string(status) + ").";
          Play(Sound::kError);
          return;
        }
        const std::string account = Field(answer, "name").empty() ? name : Field(answer, "name");
        Save("online_name", Quote(account));
        Save("online_token", Quote(token));
        if (!Field(answer, "xuid").empty()) Save("online_xuid", Quote(Field(answer, "xuid")));
        Save("online_enabled", "true");
        online_msg_.clear();
        Play(Sound::kOn);
        Toast("Signed in as " + account);
      };
      const std::string body = "{\"name\": " + Quote(name) + ", \"password\": " + Quote(password) + "}";
      Call("POST", create ? "/api/register" : "/api/login", body, "",
           [this, create, body, finish](int status, const std::string& answer) {
             // a new account: signed in with it straight away (a server that
             // makes the account without a session: then the login)
             if (create && status == 200 && Field(answer, "token").empty()) {
               online_msg_ = "Account made. Signing in...";
               Call("POST", "/api/login", body, "", finish);
               return;
             }
             finish(status, answer);
           });
    });
  });
}

void Hub::SignOut() {
  const std::string token = Unquoted(Value("online_token", ""));
  if (!token.empty()) Call("POST", "/api/logout", "{}", token, [](int, const std::string&) {});
  Save("online_token", "\"\"");
  online_msg_.clear();
  Play(Sound::kOff);
  Toast("Signed out");
}

void Hub::PollJob() {
  // a request done: its answer (on this thread)
  if (job_ && job_->done) {
    auto job = std::move(job_);
    if (job->thread.joinable()) job->thread.join();
    job->then(job->status, job->answer);
  }
}

void Hub::DrawOnline() {  const bool signed_in = !Unquoted(Value("online_token", "")).empty();
  const std::string name = Unquoted(Value("online_name", ""));
  const bool on = Value("online_enabled", "false") == "true";
  PageTitle("ONLINE", signed_in ? "Signed in as " + name + (on ? "." : ". Online play is off.")
                                : std::string("Community Creations, leaderboards and online matches."));
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, 3);
  std::vector<Row> rows = {{-1, "ACCOUNT"}, {0}, {1}, {-1, "SETTINGS"}, {2}, {3}};
  const float x = kListX, w = kListW;
  List(rows, sel, 76, [&](int i, float ry, float over) {
    std::string label, sub, value;
    switch (i) {
      case 0:
        label = signed_in ? "Sign Out" : "Sign In";
        sub = signed_in ? "Signed in as " + name : "With your SmackDown vs. Raw Recomp account";
        break;
      case 1:
        label = "Create Account", sub = "A new account on the server";
        break;
      case 2:
        label = "Play Online", sub = "The game's online menus and matches";
        Switch(x + w - 24 - 58, ry + 19, Knob(i, on), over > 0.5f);
        break;
      case 3:
        label = "Server", sub = Fit(a_.body, 18, ServerUrl(), w - 120);
        break;
    }
    Text(a_.body_bold, 24, x + 24, ry + 9, Mix(White(0.94f), kInk, over), label);
    Text(a_.body, 18, x + 24, ry + 41, Mix(White(0.42f), Black(0.55f), over), sub);
  });
  Panel([&](float px, float py, float pw) {
    const char* title = sel == 0 ? (signed_in ? "Your Account" : "Sign In") : sel == 1 ? "Create Account"
                        : sel == 2                                         ? "Play Online"
                                                                           : "Server";
    Text(a_.display, 44, px, py, kText, title);
    py += 72;
    std::string text;
    switch (sel) {
      case 0:
        text = signed_in ? "You're signed in as " + name +
                               ". Your Created Superstars, logos and entrances can be shared through Community "
                               "Creations, and friends can find you for online matches."
                         : "Sign in with the account you made in the PC launcher (or make one here). The game uses it "
                           "for Community Creations, leaderboards and online matches.";
        break;
      case 1:
        text = "Pick a name and a password for a new account. The same account works on PC, Android and PS5.";
        break;
      case 2:
        text = "Turns the game's online features on: Community Creations, leaderboards, and matches with friends - "
               "straight between consoles and PCs, through the server's relay when they can't reach each other.";
        break;
      case 3:
        text = std::string("The Community Creations server. The default is ") + kDefaultServer +
               ". https and plain http servers both work.";
        break;
    }
    py += Wrapped(a_.body, 22, px, py, pw, White(0.65f), text);
    if (!online_msg_.empty() && sel <= 1) {
      py += 24;
      const bool busy = job_ != nullptr;
      if (busy) {  // a spinner
        const ImVec2 c = P(px + 12, py + 14);
        dl_->PathArcTo(c, 10 * s_, time_ * 6, time_ * 6 + 4.2f, 24);
        dl_->PathStroke(kAccent, 0, 3 * s_);
      }
      Wrapped(a_.body_bold, 21, px + (busy ? 36 : 0), py, pw - 36, busy ? kText : kAccent, online_msg_);
    }
  });
}

// ------------------------------------------------------------------ keyboard

bool Hub::KeyboardInput(uint32_t p) {
  if (!kb_.open) return false;
  auto type = [&](const std::string& s) {
    if (kb_.text.size() < 64) kb_.text += s;
    if (kb_.shift) kb_.shift = false;  // (one capital, then back)
  };
  auto finish = [&](bool ok) {
    kb_.open = false;
    auto done = std::move(kb_.done);
    const std::string text = kb_.text;
    kb_.text.clear();
    if (ok && done) done(text);
  };
  if (p & kCircle) {
    Play(Sound::kBack);
    finish(false);
    return true;
  }
  if (p & kOptions) {
    Play(Sound::kSelect);
    finish(true);
    return true;
  }
  if (p & kSquare) {
    if (!kb_.text.empty()) kb_.text.pop_back();
    Play(Sound::kMove);
    return true;
  }
  if (p & kTriangle) {
    type(" ");
    Play(Sound::kMove);
    return true;
  }
  if (p & kL1) {
    kb_.shift = !kb_.shift;
    Play(Sound::kMove);
    return true;
  }
  const int before_r = kb_.row, before_c = kb_.col;
  if (p & kUp) kb_.row = (kb_.row + 4) % 5;
  if (p & kDown) kb_.row = (kb_.row + 1) % 5;
  if (p & kLeft) kb_.col = kb_.row == 4 ? (BottomStart(BottomAt(kb_.col)) + 9) % 10 : (kb_.col + 9) % 10;
  if (p & kRight)
    kb_.col = kb_.row == 4 ? (BottomStart(BottomAt(kb_.col)) + kBottom[BottomAt(kb_.col)].units) % 10 : (kb_.col + 1) % 10;
  if (kb_.row == 4 && (p & (kLeft))) kb_.col = BottomStart(BottomAt(kb_.col));
  if (before_r != kb_.row || before_c != kb_.col) Play(Sound::kMove);
  if (p & kCross) {
    kb_.press = 1;
    if (kb_.row < 4) {
      type(std::string(1, kRows[kb_.shift ? 1 : 0][kb_.row][kb_.col]));
      Play(Sound::kMove);
    } else {
      switch (kBottom[BottomAt(kb_.col)].what) {
        case 0: kb_.shift = !kb_.shift; Play(Sound::kMove); break;
        case 1: type(":"); Play(Sound::kMove); break;
        case 2: type("/"); Play(Sound::kMove); break;
        case 3: type(" "); Play(Sound::kMove); break;
        case 4:
          if (!kb_.text.empty()) kb_.text.pop_back();
          Play(Sound::kMove);
          break;
        case 5: Play(Sound::kSelect); finish(true); break;
      }
    }
  }
  return true;
}

void Hub::DrawKeyboard() {
  Ease(kb_.in, kb_.open ? 1.0f : 0.0f, 14);
  Ease(kb_.press, 0, 12);
  if (kb_.in < 0.01f) return;
  const float a = Smooth(kb_.in);
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  dl_->AddRectFilled(ImVec2(0, 0), size, Black(0.7f * a));
  const int v0 = dl_->VtxBuffer.Size;
  const float kw = 76, kh = 64, gap = 8, unit = kw + gap;
  const float w = unit * 10 - gap, x = (kW - w) * 0.5f, y = 330 + (1 - a) * 40;
  // a card under it all, then the title and the text so far
  Rect(x - 44, y - 190, w + 88, 5 * (kh + gap) + 230, Rgba(19, 20, 24), 20);
  dl_->AddRectFilled(P(x - 44, y - 190), P(x - 38, y - 190 + 5 * (kh + gap) + 230), kAccent, 20 * s_,
                     ImDrawFlags_RoundCornersLeft);
  Text(a_.display, 40, x, y - 160, kText, kb_.title);
  Rect(x, y - 86, w, 64, White(0.1f), 12);
  std::string shown = kb_.secret ? std::string(kb_.text.size(), '*') : kb_.text;
  shown = Fit(a_.body_bold, 28, shown, w - 60);
  Text(a_.body_bold, 28, x + 22, y - 70, kText, shown);
  if (std::fmod(time_, 1.0f) < 0.55f) {  // the caret
    const float cx = x + 24 + TextWidth(a_.body_bold, 28, shown);
    Rect(cx, y - 72, 2.5f, 36, kAccent, 1);
  }
  // the keys
  float fx = 0, fy = 0, fw = kw;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 10; ++c) {
      const float kx = x + c * unit, ky = y + r * (kh + gap);
      if (r == kb_.row && c == kb_.col) fx = kx, fy = ky, fw = kw;
    }
  if (kb_.row == 4) {
    const int k = BottomAt(kb_.col);
    fx = x + BottomStart(k) * unit, fy = y + 4 * (kh + gap), fw = kBottom[k].units * unit - gap;
  }
  Ease(kb_.fx, fx, 24);
  Ease(kb_.fy, fy, 24);
  static float s_fw = -1;
  Ease(s_fw, fw, 24);
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 10; ++c) {
      const float kx = x + c * unit, ky = y + r * (kh + gap);
      Rect(kx, ky, kw, kh, White(0.08f), 10);
    }
  for (int k = 0; k < 6; ++k) {
    const float kx = x + BottomStart(k) * unit, ky = y + 4 * (kh + gap), bw = kBottom[k].units * unit - gap;
    const bool lit = (kBottom[k].what == 0 && kb_.shift) || kBottom[k].what == 5;
    Rect(kx, ky, bw, kh, lit ? WithAlpha(kAccent, 0.85f) : White(0.08f), 10);
  }
  const float push = kb_.press * 3;
  Rect(kb_.fx + push, kb_.fy + push, s_fw - push * 2, kh - push * 2, kText, 10);
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 10; ++c) {
      const float kx = x + c * unit, ky = y + r * (kh + gap);
      const float over = std::clamp(1 - std::hypot(kb_.fx - kx, kb_.fy - ky) / (kw * 0.6f), 0.0f, 1.0f);
      Text(a_.body_bold, 26, kx + kw * 0.5f, ky + 16, Mix(kText, kInk, over),
           std::string(1, kRows[kb_.shift ? 1 : 0][r][c]), 0.5f);
    }
  for (int k = 0; k < 6; ++k) {
    const float kx = x + BottomStart(k) * unit, ky = y + 4 * (kh + gap), bw = kBottom[k].units * unit - gap;
    const float over = std::clamp(1 - std::hypot(kb_.fx - kx, kb_.fy - ky) / (kw * 0.6f), 0.0f, 1.0f);
    const bool small = std::strlen(kBottom[k].label) > 1;
    Text(small ? a_.nav : a_.body_bold, small ? 18 : 26, kx + bw * 0.5f, ky + (small ? 21 : 16),
         Mix(kText, kInk, over), kBottom[k].label, 0.5f, small ? 2 : 0);
  }
  Fade(v0, a);
}

}  // namespace svr2011::hub
