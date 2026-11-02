#pragma once

// Taking over recompiled game functions.
//
// XenonRecomp emits each game function twice: its body as __imp__sub_<address>, and
// sub_<address> as a weak alias that every call (direct, or through the indirect-call
// table) goes through. Defining sub_<address> in the runtime replaces the alias, so the
// runtime's version runs instead, and it can still call __imp__sub_<address>.

#include "kernel/kernel.h"

#include <atomic>
#include <cstdint>

namespace s3n::hooks {

// Counts calls to a game function and remembers its most frequent callers, for mapping
// what the title calls and from where. Shows up in the run report.
struct Counter {
    uint32_t address;
    const char* label;
    std::atomic<uint64_t> calls{0};
    static constexpr int kCallers = 6;
    std::atomic<uint32_t> callers[kCallers]{};  // return addresses, first seen first
    std::atomic<uint64_t> callerCalls[kCallers]{};

    Counter(uint32_t address, const char* label);
    void Hit(uint32_t returnAddress);
};

}  // namespace s3n::hooks

// Declares the original body of a game function.
#define S3N_GUEST_ORIGINAL(address) extern "C" PPC_FUNC(__imp__sub_##address)

// Wraps a game function with a call counter; the original still runs.
#define S3N_COUNT_CALLS(address, label)                                     \
    S3N_GUEST_ORIGINAL(address);                                            \
    static s3n::hooks::Counter counter_##address(0x##address, label);       \
    PPC_FUNC(sub_##address) {                                               \
        counter_##address.Hit(uint32_t(ctx.lr));                            \
        __imp__sub_##address(ctx, base);                                    \
    }
