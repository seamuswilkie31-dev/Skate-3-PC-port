#include "hooks/hooks.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace s3n::hooks {
namespace {

std::mutex& RegistryMutex() {
    static std::mutex mutex;
    return mutex;
}
std::vector<Counter*>& Registry() {
    static std::vector<Counter*> counters;
    return counters;
}

std::string Describe() {
    std::vector<Counter*> counters;
    {
        std::unique_lock lock(RegistryMutex(), std::try_to_lock);
        if (!lock.owns_lock()) return "hooks: (busy)\n";
        counters = Registry();
    }
    std::sort(counters.begin(), counters.end(), [](Counter* a, Counter* b) { return a->calls > b->calls; });
    std::string out = "call counts (game functions; callers by return address):\n";
    char line[160];
    for (Counter* c : counters) {
        std::snprintf(line, sizeof line, "  %10llu  %08X %-28s", c->calls.load(), c->address, c->label);
        out += line;
        for (int i = 0; i < Counter::kCallers; ++i) {
            uint32_t caller = c->callers[i];
            if (!caller) break;
            std::snprintf(line, sizeof line, " %08X:%llu", caller, c->callerCalls[i].load());
            out += line;
        }
        out += "\n";
    }
    return out;
}

}  // namespace

Counter::Counter(uint32_t address, const char* label) : address(address), label(label) {
    std::lock_guard lock(RegistryMutex());
    if (Registry().empty()) kernel::AddReportSection(Describe);
    Registry().push_back(this);
}

void Counter::Hit(uint32_t returnAddress) {
    ++calls;
    for (int i = 0; i < kCallers; ++i) {
        uint32_t seen = callers[i].load(std::memory_order_relaxed);
        if (seen == returnAddress) {
            ++callerCalls[i];
            return;
        }
        if (seen == 0) {
            uint32_t expected = 0;
            if (callers[i].compare_exchange_strong(expected, returnAddress) || expected == returnAddress) {
                ++callerCalls[i];
                return;
            }
        }
    }
}

}  // namespace s3n::hooks
