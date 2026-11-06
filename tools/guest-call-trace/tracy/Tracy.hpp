// Stand-in for Tracy, used only by the NARUTO_TRACE_GUEST_CALLS build.
//
// With REXGLUE_PROFILE_GUEST_FUNCTIONS, every generated guest function opens
// ZoneNamedN(___tracy_guest_zone, __func__, TracyIsStarted). Here that counts
// calls per function instead (see guest_call_trace.cpp); the other Tracy hooks
// the SDK headers use become no-ops.
#pragma once

#include <atomic>
#include <cstdint>

namespace naruto_trace {

struct Slot {
  const char* name;
  std::atomic<uint64_t> calls;
  // 0: not yet registered, 1: counted, 2: outside the traced address range.
  std::atomic<uint8_t> state;
  Slot* next;
};

void Register(Slot* slot);

}  // namespace naruto_trace

#define ZoneNamedN(var, name, active)                                           \
  static ::naruto_trace::Slot var##_slot{name, {0}, {0}, nullptr};              \
  do {                                                                          \
    uint8_t var##_state = var##_slot.state.load(std::memory_order_relaxed);     \
    if (__builtin_expect(var##_state == 0, 0)) {                                \
      ::naruto_trace::Register(&var##_slot);                                    \
      var##_state = var##_slot.state.load(std::memory_order_relaxed);           \
    }                                                                           \
    if (var##_state == 1) var##_slot.calls.fetch_add(1, std::memory_order_relaxed); \
  } while (0)

#define TracyIsStarted true
#define ZoneTextV(var, text, size) ((void)0)
#define TracyPlot(name, value) ((void)0)
#define FrameMark ((void)0)

namespace tracy {
inline void SetThreadName(const char*) {}
inline bool IsProfilerStarted() { return false; }
inline void StartupProfiler() {}
inline void ShutdownProfiler() {}
}  // namespace tracy
