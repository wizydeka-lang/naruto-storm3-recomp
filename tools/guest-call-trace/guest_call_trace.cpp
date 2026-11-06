// Per-function guest call counts for the NARUTO_TRACE_GUEST_CALLS build.
//
// Counts calls to guest functions between NARUTO_TRACE_LOW and
// NARUTO_TRACE_HIGH and, once a second, appends "seconds<TAB>function<TAB>
// total calls" for each one called so far to $NARUTO_TRACE_FILE (default
// guest_calls.tsv in the working directory).

#include <tracy/Tracy.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace naruto_trace {

namespace {

constexpr uint32_t kLow = 0x82700000;
constexpr uint32_t kHigh = 0x82810000;

std::atomic<Slot*> g_slots{nullptr};
std::mutex g_register_mutex;

// "__imp__sub_82746748" -> 0x82746748; 0 for any other name.
uint32_t GuestAddress(const char* name) {
  const char* sub = std::strstr(name, "sub_");
  return sub ? uint32_t(std::strtoul(sub + 4, nullptr, 16)) : 0;
}

void WriterLoop() {
  const char* path = std::getenv("NARUTO_TRACE_FILE");
  FILE* out = std::fopen(path ? path : "guest_calls.tsv", "w");
  if (!out) {
    return;
  }
  auto start = std::chrono::steady_clock::now();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    for (Slot* s = g_slots.load(std::memory_order_acquire); s; s = s->next) {
      std::fprintf(out, "%.1f\t%s\t%llu\n", seconds, s->name,
                   static_cast<unsigned long long>(s->calls.load(std::memory_order_relaxed)));
    }
    std::fflush(out);
  }
}

}  // namespace

void Register(Slot* slot) {
  std::lock_guard<std::mutex> lock(g_register_mutex);
  if (slot->state.load(std::memory_order_relaxed) != 0) {
    return;
  }
  uint32_t address = GuestAddress(slot->name);
  bool traced = address >= kLow && address < kHigh;
  if (traced) {
    static std::once_flag writer_started;
    std::call_once(writer_started, [] { std::thread(WriterLoop).detach(); });
    slot->next = g_slots.load(std::memory_order_relaxed);
    g_slots.store(slot, std::memory_order_release);
  }
  slot->state.store(traced ? 1 : 2, std::memory_order_relaxed);
}

}  // namespace naruto_trace
