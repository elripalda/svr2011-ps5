/**
 * @file        net_trace.h
 * @brief       --wwe_net_trace: a rate-limited log of the guest's network, XMsg and
 *              session calls.
 *
 * A diagnostic for online play. Off by default; off, the wrapped exports behave exactly
 * like REX_EXPORT / REX_EXPORT_STUB.
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DECLARE(bool, wwe_net_trace);

namespace rex::kernel::xam::net_trace {

// Busy calls (a socket poll loop runs thousands a second) would rotate the log away:
// each export logs its first 32 calls, then each power-of-two call.
inline bool ShouldLog(const char* name, uint64_t* count) {
  static std::mutex mutex;
  static std::unordered_map<const char*, uint64_t> counts;
  std::lock_guard lock(mutex);
  *count = ++counts[name];
  return *count <= 32 || (*count & (*count - 1)) == 0;
}

// A guest sockaddr_in as "a.b.c.d:port", for the export's address argument.
inline std::string SockaddrText(uint32_t guest_address) {
  if (!guest_address) {
    return "null";
  }
  const uint8_t* p = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(guest_address);
  return fmt::format("{}.{}.{}.{}:{} (family {})", p[4], p[5], p[6], p[7], (p[2] << 8) | p[3],
                     (p[0] << 8) | p[1]);
}

// Raw argument registers before the call, r3 after it. NetDll_* imports take the
// caller type in r3, so their real arguments start at r4. `sockaddr_reg` names the
// register holding a sockaddr_in pointer to decode, or 0 for none.
inline bool LogEnter(const char* name, const PPCContext& ctx, int sockaddr_reg = 0) {
  uint64_t count = 0;
  if (!ShouldLog(name, &count)) {
    return false;
  }
  std::string address;
  if (sockaddr_reg) {
    const uint32_t regs[] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                             ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
    address = " addr=" + SockaddrText(regs[sockaddr_reg - 3]);
  }
  REXKRNL_INFO("[net] {}({:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}){} #{}", name,
               ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
               address, count);
  return true;
}

inline void LogLeave(const char* name, const PPCContext& ctx) {
  REXKRNL_INFO("[net] {} -> {:08X}", name, ctx.r3.u32);
}

}  // namespace rex::kernel::xam::net_trace

// REX_EXPORT with trace logging around the call.
#define REX_TRACE_EXPORT_SOCKADDR(name, function, sockaddr_reg)                              \
  extern "C" REX_FUNC(name) {                                                                \
    const bool logged = REXCVAR_GET(wwe_net_trace) &&                                        \
                        rex::kernel::xam::net_trace::LogEnter(#name, ctx, sockaddr_reg);     \
    rex::ppc::HostToGuestFunction<function>(ctx, base);                                      \
    if (logged) rex::kernel::xam::net_trace::LogLeave(#name, ctx);                           \
  }                                                                                          \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);

#define REX_TRACE_EXPORT(name, function) REX_TRACE_EXPORT_SOCKADDR(name, function, 0)

// REX_EXPORT_STUB whose warning, with the trace on, goes through the trace's rate limit.
#define REX_TRACE_EXPORT_STUB(name)                                              \
  extern "C" REX_FUNC(name) {                                                    \
    (void)base;                                                                  \
    if (!REXCVAR_GET(wwe_net_trace)) {                                           \
      REXKRNL_WARN("{} STUB", #name);                                            \
    } else if (rex::kernel::xam::net_trace::LogEnter(#name " STUB", ctx)) {      \
      rex::kernel::xam::net_trace::LogLeave(#name " STUB", ctx);                 \
    }                                                                            \
  }                                                                              \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);
