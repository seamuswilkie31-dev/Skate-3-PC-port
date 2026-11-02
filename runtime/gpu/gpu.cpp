#include "gpu/gpu.h"

#include "gpu/registers.h"
#include "kernel/kernel.h"
#include "memory.h"
#include "render/draws.h"
#include "render/shader_cache.h"
#include "render/targets.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace s3n::gpu {
namespace {

using Clock = std::chrono::steady_clock;

// Register numbers.
constexpr uint32_t kCpRbRptr = 0x01C4;
constexpr uint32_t kCpRbWptr = 0x01C5;
constexpr uint32_t kScratchUmsk = 0x01DC;
constexpr uint32_t kScratchAddr = 0x01DD;
constexpr uint32_t kScratchReg0 = 0x0578;  // 8 registers; enabled ones are mirrored to memory
constexpr uint32_t kCoherStatusHost = 0x0A31;
constexpr uint32_t kDisplayInterruptStatus = 0x1951;  // bit 0: vertical blank
constexpr uint32_t kVgtEventInitiator = 0x21F9;
// Registers below this are also in the CPU's window; above are the shader constants.
constexpr uint32_t kWindowRegisters = kRegisterWindowSize / 4;
constexpr uint32_t kRegisterCount = 0x6000;

// Type-3 packet opcodes.
enum Opcode : uint32_t {
    kNop = 0x10,
    kRegRmw = 0x21,
    kDrawIndx = 0x22,
    kVizQuery = 0x23,
    kWaitForIdle = 0x26,
    kImLoad = 0x27,
    kImLoadImmediate = 0x2B,
    kSetConstant = 0x2D,
    kLoadAluConstant = 0x2F,
    kDrawIndx2 = 0x36,
    kIndirectBufferPfd = 0x37,
    kInvalidateState = 0x3B,
    kWaitRegMem = 0x3C,
    kMemWrite = 0x3D,
    kRegToMem = 0x3E,
    kIndirectBuffer = 0x3F,
    kCondWrite = 0x45,
    kEventWrite = 0x46,
    kMeInit = 0x48,
    kSetBinMask = 0x50,
    kSetBinSelect = 0x51,
    kInterrupt = 0x54,
    kSetConstant2 = 0x55,
    kSetShaderConstants = 0x56,
    kEventWriteShd = 0x58,
    kEventWriteExt = 0x5A,
    kEventWriteZpd = 0x5B,
    kContextUpdate = 0x5E,
    kSetBinMaskLo = 0x60,
    kSetBinMaskHi = 0x61,
    kSetBinSelectLo = 0x62,
    kSetBinSelectHi = 0x63,
};

GuestMemory* gMemory = nullptr;
uint8_t* gBase = nullptr;
std::vector<uint32_t> gConstants(kRegisterCount);  // registers from kWindowRegisters up

std::atomic<uint32_t> gRingBase{0};    // physical
std::atomic<uint32_t> gRingDwords{0};  // 0 until the title sets the ring up
std::atomic<uint32_t> gReadIndex{0};   // in dwords
std::atomic<uint32_t> gWriteBack{0};   // physical address the read index is mirrored to
uint64_t gBinMask = ~0ull;             // predicated packets run when mask & select != 0
uint64_t gBinSelect = ~0ull;

std::atomic<uint32_t> gCallback{0};
std::atomic<uint32_t> gCallbackData{0};
std::atomic<bool> gInterruptThreadStarted{false};
std::mutex gInterruptMutex;
std::condition_variable gInterruptWake;
std::deque<std::pair<uint32_t, uint32_t>> gInterrupts;  // (source, processor)

// Counters for the report.
std::atomic<uint32_t> gVblanks{0};
std::atomic<uint32_t> gSwaps{0};
std::atomic<uint64_t> gInterruptsDelivered{0};
uint32_t gLoggedInterrupts = 0;  // interrupt thread only
std::atomic<uint64_t> gDraws{0};
std::atomic<uint64_t> gType0{0};
std::array<std::atomic<uint64_t>, 128> gOpcodes{};
std::array<std::atomic<bool>, 128> gOpcodeLogged{};
std::atomic<uint32_t> gWaitingOn{0};  // WAIT_REG_MEM target while the processor waits, else 0
std::atomic<bool> gWaitingOnMemory{false};
uint32_t gTracePackets = 0;  // S3N_GPU_TRACE=<n>: log the first n packets in full

// Shaders and draws, as the command processor sees them. The bound shaders are its own;
// the tables are shared with the report (gStatsMutex).
struct BoundShader {
    bool loaded = false;
    uint64_t key = 0;
    render::Shader* shader = nullptr;  // nullptr: not in the shader cache
    std::vector<uint8_t> microcode;    // vertex shaders: the bound copy, for its vertex fetches
};
BoundShader gBound[2];  // vertex, pixel
struct UnknownShader {
    bool pixel;
    uint32_t dwords;
    uint64_t loads, draws;
    uint64_t primitives;  // bit per primitive type drawn with
};
std::atomic<uint64_t> gShaderLoads{0};
std::atomic<uint64_t> gShaderHits{0};
std::atomic<uint64_t> gDrawsWithShaders{0};
std::atomic<uint64_t> gDrawsWithUnknown{0};
std::atomic<uint64_t> gResolveDraws{0};  // draws in copy mode
std::atomic<uint64_t> gClearDraws{0};    // Direct3D's clear rectangles
std::array<std::atomic<uint64_t>, 64> gPrimitives{};  // draws by primitive type
std::mutex gStatsMutex;
std::unordered_map<uint64_t, uint64_t> gShaderPairs;  // vertex * 31 + pixel container hash -> draws
std::unordered_map<uint64_t, UnknownShader> gUnknownShaders;  // by key (pixel shaders' keys inverted)

void LoadShader(uint32_t type, const uint8_t* microcode, uint32_t dwords) {
    ++gShaderLoads;
    bool pixel = type != 0;
    BoundShader& bound = gBound[pixel];
    bound.loaded = true;
    bound.key = render::ShaderKey(microcode, size_t(dwords) * 4);
    bound.shader = render::FindShader(bound.key, pixel);
    if (!pixel) bound.microcode.assign(microcode, microcode + size_t(dwords) * 4);
    if (bound.shader) {
        ++gShaderHits;
        return;
    }
    std::lock_guard lock(gStatsMutex);
    auto [it, inserted] = gUnknownShaders.try_emplace(pixel ? ~bound.key : bound.key, UnknownShader{pixel, dwords, 0, 0, 0});
    ++it->second.loads;
    if (inserted && gUnknownShaders.size() <= 8)
        kernel::Log("  gpu: %s shader %016llX (%u dwords) isn't in the shader cache\n", pixel ? "pixel" : "vertex",
                    bound.key, dwords);
}

uint32_t ReadRegister(uint32_t index);

// Direct3D's clear shader: bare microcode in its library (drawn by 82B53D40), so the
// translated shaders don't have it. Its rectangles are cleared natively. (Its ShaderKey:
// a change to the key changes this.)
constexpr uint64_t kDirect3DClearShader = 0x95FA1EC89F62BB53ull;

// VGT_DRAW_INITIATOR: primitive type (bits 0-5), index source (6-7: 0 index buffer,
// 1 immediate, 2 auto), 32-bit indices (bit 11), index or vertex count (16-31). Draws from an
// index buffer also have VGT_DMA_BASE and VGT_DMA_SIZE.
void RecordDraw(uint32_t initiator, uint32_t indexBase, uint32_t indexSize) {
    ++gDraws;
    ++gPrimitives[initiator & 0x3F];
    const BoundShader &vertex = gBound[0], &pixel = gBound[1];
    if ((ReadRegister(reg::kRbModeControl) & 7) == reg::kEdramModeCopy) {  // a resolve, not a draw
        ++gResolveDraws;
        render::Resolve();
        return;
    }
    if (vertex.loaded && !vertex.shader && vertex.key == kDirect3DClearShader) {
        ++gClearDraws;
        render::ClearRectangle();
        return;
    }
    if (!vertex.loaded || !pixel.loaded) return;
    if (vertex.shader && pixel.shader) {
        ++gDrawsWithShaders;
        {
            std::lock_guard lock(gStatsMutex);
            ++gShaderPairs[vertex.shader->containerHash * 31 + pixel.shader->containerHash];
        }
        render::DrawCall call;
        call.initiator = initiator;
        call.indexBase = indexBase;
        call.indexSize = indexSize;
        call.vertexShader = vertex.shader;
        call.pixelShader = pixel.shader;
        call.vertexMicrocode = vertex.microcode.data();
        call.vertexMicrocodeSize = uint32_t(vertex.microcode.size());
        render::Draw(call);
        return;
    }
    std::lock_guard lock(gStatsMutex);
    ++gDrawsWithUnknown;
    for (UnknownShader* u : {vertex.shader ? nullptr : &gUnknownShaders[vertex.key],
                             pixel.shader ? nullptr : &gUnknownShaders[~pixel.key]}) {
        if (!u) continue;
        ++u->draws;
        u->primitives |= 1ull << (initiator & 0x3F);
    }
}

uint8_t* Physical(uint32_t address) { return gMemory->Physical(address & 0x1FFFFFFF); }

uint32_t ReadRegister(uint32_t index) {
    if (index < kWindowRegisters) return LoadBE32(gBase, kRegisterWindow + index * 4);
    return index < kRegisterCount ? gConstants[index] : 0;
}

// Scratch registers enabled in SCRATCH_UMSK are mirrored to memory at SCRATCH_ADDR on
// every write, the GPU's or the CPU's. The CPU's writes land in the register window
// without the runtime seeing them, so the command processor copies any changed value
// across whenever it looks (each batch, and while it waits).
std::array<uint32_t, 8> gScratchSeen{};

void MirrorScratch(uint32_t n, uint32_t value) {
    gScratchSeen[n] = value;
    if (ReadRegister(kScratchUmsk) & (1u << n))
        *reinterpret_cast<uint32_t*>(Physical(ReadRegister(kScratchAddr) + n * 4)) = __builtin_bswap32(value);
}

void SyncScratchRegisters() {
    for (uint32_t n = 0; n < 8; ++n) {
        uint32_t value = ReadRegister(kScratchReg0 + n);
        if (value != gScratchSeen[n]) MirrorScratch(n, value);
    }
}

void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) return;
    if (index < kWindowRegisters) StoreBE32(gBase, kRegisterWindow + index * 4, value);
    else gConstants[index] = value;

    if (index >= kScratchReg0 && index < kScratchReg0 + 8) {
        MirrorScratch(index - kScratchReg0, value);
    } else if (index == kCoherStatusHost) {
        // Marks a cache flush as pending; WAIT_REG_MEM on it completes the flush.
        StoreBE32(gBase, kRegisterWindow + index * 4, value | 0x80000000);
    }
}

// The low 2 bits of a GPU memory address choose a byte order for the access.
uint32_t Swap(uint32_t value, uint32_t endian) {
    switch (endian & 3) {
    case 1: return (value & 0x00FF00FF) << 8 | (value >> 8 & 0x00FF00FF);  // 8 in 16
    case 2: return __builtin_bswap32(value);                                // 8 in 32
    case 3: return value << 16 | value >> 16;                               // 16 in 32
    default: return value;
    }
}
uint32_t ReadMemory(uint32_t address) {
    return Swap(*reinterpret_cast<const uint32_t*>(Physical(address & ~3u)), address);
}
void WriteMemory(uint32_t address, uint32_t value) {
    *reinterpret_cast<uint32_t*>(Physical(address & ~3u)) = Swap(value, address);
}

bool Compare(uint32_t function, uint32_t value, uint32_t reference) {
    switch (function & 7) {
    case 1: return value < reference;
    case 2: return value <= reference;
    case 3: return value == reference;
    case 4: return value != reference;
    case 5: return value >= reference;
    case 6: return value > reference;
    case 7: return true;
    default: return false;  // never
    }
}

// Packets come from the ring buffer (which wraps) or from indirect buffers.
struct Stream {
    const uint8_t* data;
    uint32_t sizeDwords;
    uint32_t position;
    uint32_t remaining;

    uint32_t Read() {
        uint32_t value = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(data + position * 4));
        position = (position + 1) % sizeDwords;
        --remaining;
        return value;
    }
    void Skip(uint32_t dwords) {
        position = (position + dwords) % sizeDwords;
        remaining -= dwords;
    }
};

bool ExecuteStream(Stream& s, int depth);

void QueueInterrupt(uint32_t source, uint32_t processor) {
    {
        std::lock_guard lock(gInterruptMutex);
        gInterrupts.push_back({source, processor});
    }
    gInterruptWake.notify_one();
}

// WAIT_REG_MEM: blocks the command processor until a register or memory word compares
// true against the reference.
void WaitRegMem(uint32_t info, uint32_t target, uint32_t reference, uint32_t mask, uint32_t interval) {
    bool memory = info & 0x10;
    Clock::time_point start = Clock::now();
    bool warned = false;
    for (;;) {
        SyncScratchRegisters();
        uint32_t value;
        if (memory) {
            value = ReadMemory(target);
        } else {
            if (target == kCoherStatusHost)  // nothing to flush: complete it now
                StoreBE32(gBase, kRegisterWindow + target * 4, ReadRegister(target) & ~0x80000000u);
            value = ReadRegister(target);
        }
        if (Compare(info, value & mask, reference)) break;
        gWaitingOn = target;
        gWaitingOnMemory = memory;
        if (!warned && Clock::now() - start > std::chrono::seconds(2)) {
            kernel::Log("  gpu: still waiting for %s %08X & %08X to compare %u against %08X (now %08X)\n",
                        memory ? "memory" : "register", target, mask, info & 7, reference, value);
            warned = true;
        }
        if (interval >= 0x100) Sleep(1);
        else SwitchToThread();
    }
    gWaitingOn = 0;
}

void LogOpcodeOnce(uint32_t opcode, uint32_t count, const char* what) {
    if (!gOpcodeLogged[opcode].exchange(true))
        kernel::Log("  gpu: first %s packet (opcode %02X, %u dwords)\n", what, opcode, count);
}

bool ExecuteType3(Stream& s, uint32_t header, int depth) {
    uint32_t opcode = (header >> 8) & 0x7F;
    uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    if (count > s.remaining) return false;
    ++gOpcodes[opcode];
    uint32_t end = s.remaining - count;
    if ((header & 1) && !(gBinMask & gBinSelect)) {  // predicated off
        s.Skip(count);
        return true;
    }

    switch (opcode) {
    case kIndirectBuffer:
    case kIndirectBufferPfd: {
        uint32_t address = s.Read();
        uint32_t length = s.Read() & 0xFFFFF;
        if (gTracePackets) kernel::Log("  gpu indirect buffer %08X, %u dwords\n", address, length);
        if (depth < 4 && length) {
            Stream indirect{Physical(address), length, 0, length};
            ExecuteStream(indirect, depth + 1);
        }
        break;
    }
    case kWaitRegMem: {
        uint32_t info = s.Read(), target = s.Read(), reference = s.Read(), mask = s.Read(), interval = s.Read();
        WaitRegMem(info, target, reference, mask, interval);
        break;
    }
    case kRegRmw: {
        uint32_t info = s.Read(), andMask = s.Read(), orMask = s.Read();
        uint32_t index = info & 0x1FFF;
        uint32_t value = ReadRegister(index);
        value &= (info >> 31) ? ReadRegister(andMask & 0x1FFF) : andMask;
        value |= (info >> 30 & 1) ? ReadRegister(orMask & 0x1FFF) : orMask;
        WriteRegister(index, value);
        break;
    }
    case kRegToMem: {
        uint32_t index = s.Read(), address = s.Read();
        WriteMemory(address, ReadRegister(index));
        break;
    }
    case kMemWrite: {
        uint32_t address = s.Read();
        for (uint32_t i = 1; i < count; ++i, address += 4) WriteMemory(address, s.Read());
        break;
    }
    case kCondWrite: {
        uint32_t info = s.Read(), target = s.Read(), reference = s.Read(), mask = s.Read();
        uint32_t destination = s.Read(), data = s.Read();
        uint32_t value = (info & 0x10) ? ReadMemory(target) : ReadRegister(target);
        if (Compare(info, value & mask, reference)) {
            if (info & 0x100) WriteMemory(destination, data);
            else WriteRegister(destination, data);
        }
        break;
    }
    case kEventWrite:
    case kEventWriteZpd:
        WriteRegister(kVgtEventInitiator, s.Read() & 0x3F);
        break;
    case kEventWriteShd: {  // a fence: writes a value (or the frame counter) when work is done
        uint32_t initiator = s.Read(), address = s.Read(), value = s.Read();
        WriteRegister(kVgtEventInitiator, initiator & 0x3F);
        WriteMemory(address, (initiator >> 31) ? gVblanks.load() : value);
        break;
    }
    case kEventWriteExt: {  // the screen area the last draws touched: report all of it
        uint32_t initiator = s.Read(), address = s.Read();
        WriteRegister(kVgtEventInitiator, initiator & 0x3F);
        static constexpr uint16_t kExtents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};  // min/max x, y, z
        uint8_t* out = Physical(address & ~3u);
        for (int i = 0; i < 6; ++i) {
            out[i * 2] = uint8_t(kExtents[i] >> 8);
            out[i * 2 + 1] = uint8_t(kExtents[i]);
        }
        break;
    }
    case kInterrupt: {
        uint32_t processors = s.Read();
        for (uint32_t n = 0; n < 6; ++n)
            if (processors & (1u << n)) QueueInterrupt(1, n);
        break;
    }
    case kSetConstant: {
        uint32_t offsetType = s.Read();
        static constexpr uint32_t kBases[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};  // ALU, fetch, bool, loop, registers
        uint32_t type = (offsetType >> 16) & 0xFF;
        if (type < 5) {
            uint32_t index = kBases[type] + (offsetType & 0x7FF);
            for (uint32_t i = 1; i < count; ++i) WriteRegister(index++, s.Read());
        }
        break;
    }
    case kSetConstant2:
    case kSetShaderConstants: {
        uint32_t index = s.Read() & 0xFFFF;
        for (uint32_t i = 1; i < count; ++i) WriteRegister(index++, s.Read());
        break;
    }
    case kLoadAluConstant: {
        uint32_t address = s.Read() & 0x3FFFFFFF, offsetType = s.Read(), size = s.Read() & 0xFFF;
        static constexpr uint32_t kBases[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
        uint32_t type = (offsetType >> 16) & 0xFF;
        if (type < 5) {
            uint32_t index = kBases[type] + (offsetType & 0x7FF);
            const uint8_t* source = Physical(address);
            for (uint32_t i = 0; i < size; ++i)
                WriteRegister(index + i, __builtin_bswap32(*reinterpret_cast<const uint32_t*>(source + i * 4)));
        }
        break;
    }
    case kSetBinMask: {
        uint64_t high = s.Read(), low = s.Read();
        gBinMask = high << 32 | low;
        break;
    }
    case kSetBinSelect: {
        uint64_t high = s.Read(), low = s.Read();
        gBinSelect = high << 32 | low;
        break;
    }
    case kSetBinMaskLo: gBinMask = (gBinMask & ~0xFFFFFFFFull) | s.Read(); break;
    case kSetBinMaskHi: gBinMask = (gBinMask & 0xFFFFFFFFull) | uint64_t(s.Read()) << 32; break;
    case kSetBinSelectLo: gBinSelect = (gBinSelect & ~0xFFFFFFFFull) | s.Read(); break;
    case kSetBinSelectHi: gBinSelect = (gBinSelect & 0xFFFFFFFFull) | uint64_t(s.Read()) << 32; break;
    case kDrawIndx:
    case kDrawIndx2: {
        if (opcode == kDrawIndx) s.Read();  // visibility query condition
        uint32_t initiator = s.Read(), indexBase = 0, indexSize = 0;
        if ((initiator >> 6 & 3) == 0 && count >= (opcode == kDrawIndx ? 4u : 3u)) indexBase = s.Read(), indexSize = s.Read();
        RecordDraw(initiator, indexBase, indexSize);
        LogOpcodeOnce(opcode, count, "draw");
        break;
    }
    case kImLoad: {  // shader microcode in memory: address | type, then start << 16 | dwords
        uint32_t addressType = s.Read(), startSize = s.Read();
        LoadShader(addressType & 3, Physical(addressType & ~3u), startSize & 0xFFFF);
        break;
    }
    case kImLoadImmediate: {  // the same, with the microcode in the packet
        uint32_t type = s.Read(), startSize = s.Read();
        uint32_t dwords = std::min(startSize & 0xFFFF, count - 2);
        std::vector<uint32_t> code(dwords);
        for (uint32_t& word : code) word = __builtin_bswap32(s.Read());  // back to the stored byte order
        LoadShader(type, reinterpret_cast<const uint8_t*>(code.data()), dwords);
        break;
    }
    case kNop:  // VdSwap's marker (gpu.h) ends a frame
        if (count >= 4 && s.Read() == kSwapMarker) {
            uint32_t frontBuffer = s.Read(), width = s.Read(), height = s.Read();
            render::Swap(frontBuffer, width, height);
        }
        break;
    case kMeInit:
    case kWaitForIdle:
    case kInvalidateState:
    case kVizQuery:
    case kContextUpdate:
        break;  // nothing to do until there's a renderer
    default:
        LogOpcodeOnce(opcode, count, "unhandled");
        break;
    }
    if (s.remaining > end) s.Skip(s.remaining - end);  // whatever the handler didn't read
    return true;
}

bool ExecutePacket(Stream& s, int depth) {
    uint32_t header = s.Read();
    switch (header >> 30) {
    case 0: {  // write consecutive registers (or one register repeatedly)
        if (header == 0) return true;  // padding
        uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        if (count > s.remaining) return false;
        uint32_t index = header & 0x7FFF;
        bool oneRegister = header & 0x8000;
        for (uint32_t i = 0; i < count; ++i) WriteRegister(oneRegister ? index : index + i, s.Read());
        ++gType0;
        return true;
    }
    case 1: {  // two registers
        if (s.remaining < 2) return false;
        WriteRegister(header & 0x7FF, s.Read());
        WriteRegister((header >> 11) & 0x7FF, s.Read());
        return true;
    }
    case 2:  // filler
        return true;
    default:
        return ExecuteType3(s, header, depth);
    }
}

void TracePacket(const Stream& s, int depth) {
    uint32_t header = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(s.data + s.position * 4));
    std::string line;
    char word[16];
    uint32_t words = header >> 30 == 3 || header >> 30 == 0 ? ((header >> 16) & 0x3FFF) + 1 : header >> 30 == 1 ? 2 : 0;
    for (uint32_t i = 1; i <= words && i < s.remaining && i <= 8; ++i) {
        std::snprintf(word, sizeof word, " %08X",
                      __builtin_bswap32(*reinterpret_cast<const uint32_t*>(s.data + ((s.position + i) % s.sizeDwords) * 4)));
        line += word;
    }
    kernel::Log("  gpu %s%u @%u/%u: %08X%s%s\n", depth ? "ib" : "rb", depth, s.position, s.remaining, header,
                line.c_str(), words > 8 ? " ..." : "");
}

bool ExecuteStream(Stream& s, int depth) {
    while (s.remaining) {
        uint32_t position = s.position, remaining = s.remaining;
        if (gTracePackets) {
            --gTracePackets;
            TracePacket(s, depth);
        }
        if (!ExecutePacket(s, depth)) {
            uint32_t header = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(s.data + position * 4));
            kernel::Log("  gpu: packet %08X at dword %u of %s needs more than the %u dwords left; skipping them\n",
                        header, position, depth ? "an indirect buffer" : "the ring buffer", remaining - 1);
            s.Skip(s.remaining);
            return false;
        }
    }
    return true;
}

// Consumes the ring buffer: everything between our read index and the write index the
// title stores in CP_RB_WPTR, then reports the new read index.
void CommandProcessor() {
    uint32_t idle = 0;
    for (;;) {
        SyncScratchRegisters();
        uint32_t size = gRingDwords;
        uint32_t write = size ? ReadRegister(kCpRbWptr) % size : 0;
        uint32_t read = gReadIndex;
        if (!size || write == read) {
            if (++idle < 200) SwitchToThread();
            else Sleep(1);
            continue;
        }
        idle = 0;
        Stream ring{Physical(gRingBase), size, read, (write + size - read) % size};
        ExecuteStream(ring, 0);
        gReadIndex = write;
        WriteRegister(kCpRbRptr, write);
        if (uint32_t writeBack = gWriteBack) *reinterpret_cast<uint32_t*>(Physical(writeBack)) = __builtin_bswap32(write);
    }
}

// Runs on a guest thread of its own: delivers interrupt packets as they come and a vertical
// blank every 1/60 s, each by calling the title's callback.
void InterruptThread(PPCContext& ctx, uint8_t* base) {
    constexpr auto kFrame = std::chrono::nanoseconds(1000000000 / 60);
    Clock::time_point nextVblank = Clock::now() + kFrame;
    for (;;) {
        std::pair<uint32_t, uint32_t> interrupt;
        {
            std::unique_lock lock(gInterruptMutex);
            gInterruptWake.wait_until(lock, nextVblank, [] { return !gInterrupts.empty(); });
            if (!gInterrupts.empty()) {
                interrupt = gInterrupts.front();
                gInterrupts.pop_front();
            } else {
                Clock::time_point now = Clock::now();
                if (now < nextVblank) continue;
                nextVblank += kFrame;
                if (nextVblank < now) nextVblank = now + kFrame;  // fell behind: don't burst
                ++gVblanks;
                interrupt = {0, 2};
            }
        }
        if (uint32_t callback = gCallback) {
            // The title's handler checks the display controller's interrupt status for a
            // vertical blank before doing its per-frame work.
            if (interrupt.first == 0) StoreBE32(gBase, kRegisterWindow + kDisplayInterruptStatus * 4, 1);
            kernel::SetCurrentProcessor(uint8_t(interrupt.second));
            kernel::CallGuest(ctx, base, callback, interrupt.first, gCallbackData, 0);
            ++gInterruptsDelivered;
            if (interrupt.first != 0 && gLoggedInterrupts < 8) {
                ++gLoggedInterrupts;
                kernel::Log("  gpu: interrupt for processor %u handled; scratch 0 is %08X in the register, %08X in memory\n",
                            interrupt.second, ReadRegister(kScratchReg0),
                            __builtin_bswap32(*reinterpret_cast<const uint32_t*>(Physical(ReadRegister(kScratchAddr)))));
            }
        }
    }
}

std::string Describe() {
    char line[256];
    std::string out = "gpu:\n";
    std::snprintf(line, sizeof line, "  ring %08X, %u dwords; read %u, write %u; read pointer copied to %08X\n",
                  gRingBase.load(), gRingDwords.load(), gReadIndex.load(),
                  gRingDwords ? ReadRegister(kCpRbWptr) % gRingDwords : 0, gWriteBack.load());
    out += line;
    std::snprintf(line, sizeof line, "  %u vertical blanks, %llu interrupts delivered, %u swaps, %llu draws\n",
                  gVblanks.load(), gInterruptsDelivered.load(), gSwaps.load(), gDraws.load());
    out += line;
    if (uint32_t target = gWaitingOn) {
        std::snprintf(line, sizeof line, "  waiting on %s %08X\n", gWaitingOnMemory ? "memory" : "register", target);
        out += line;
    }
    // The tables, unless the command processor was stopped holding them.
    std::unique_lock lock(gStatsMutex, std::try_to_lock);
    std::snprintf(line, sizeof line, "  shaders: %llu loads, %llu found in the cache; %zu unknown shaders\n",
                  gShaderLoads.load(), gShaderHits.load(), lock.owns_lock() ? gUnknownShaders.size() : size_t(0));
    out += line;
    std::snprintf(line, sizeof line,
                  "  draws: %llu resolves, %llu Direct3D clears, %llu with both shaders known (%zu shader pairs), %llu "
                  "with an unknown shader; by primitive type:",
                  gResolveDraws.load(), gClearDraws.load(), gDrawsWithShaders.load(),
                  lock.owns_lock() ? gShaderPairs.size() : size_t(0), gDrawsWithUnknown.load());
    out += line;
    for (uint32_t p = 0; p < gPrimitives.size(); ++p) {
        if (uint64_t n = gPrimitives[p]) {
            std::snprintf(line, sizeof line, " %u=%llu", p, n);
            out += line;
        }
    }
    out += "\n";
    if (lock.owns_lock()) {
        std::vector<std::pair<uint64_t, UnknownShader>> unknown(gUnknownShaders.begin(), gUnknownShaders.end());
        std::sort(unknown.begin(), unknown.end(), [](auto& a, auto& b) { return a.second.draws > b.second.draws; });
        for (size_t i = 0; i < unknown.size() && i < 8 && unknown[i].second.draws; ++i) {
            const auto& [key, u] = unknown[i];
            std::snprintf(line, sizeof line, "    unknown %s shader %016llX (%u dwords): %llu loads, %llu draws; primitive types",
                          u.pixel ? "pixel" : "vertex", u.pixel ? ~key : key, u.dwords, u.loads, u.draws);
            out += line;
            for (uint32_t p = 0; p < 64; ++p) {
                if (u.primitives >> p & 1) out += " " + std::to_string(p);
            }
            out += "\n";
        }
    }
    if (lock.owns_lock()) lock.unlock();
    out += "  packets:";
    std::snprintf(line, sizeof line, " regs=%llu", gType0.load());
    out += line;
    for (uint32_t op = 0; op < gOpcodes.size(); ++op) {
        if (uint64_t n = gOpcodes[op]) {
            std::snprintf(line, sizeof line, " %02X=%llu", op, n);
            out += line;
        }
    }
    return out + "\n";
}

}  // namespace

bool Init(GuestMemory& memory) {
    gMemory = &memory;
    gBase = memory.base();
    if (const char* trace = std::getenv("S3N_GPU_TRACE")) gTracePackets = uint32_t(std::strtoul(trace, nullptr, 10));
    if (!memory.Commit(kRegisterWindow, kRegisterWindowSize)) return false;
    kernel::AddReportSection(Describe);
    std::thread(CommandProcessor).detach();
    return true;
}

void InitializeRingBuffer(uint32_t physical, uint32_t sizeLog2) {
    gReadIndex = 0;
    gRingBase = physical & 0x1FFFFFFF;
    gRingDwords = (1u << (sizeLog2 + 3)) / 4;
    kernel::Log("  gpu: ring buffer at %08X, %u bytes\n", gRingBase.load(), gRingDwords * 4);
}

void EnableReadPointerWriteBack(uint32_t physical, uint32_t blockSizeLog2) {
    gWriteBack = physical & 0x1FFFFFFF;
    kernel::Log("  gpu: read pointer copied to %08X\n", gWriteBack.load());
}

void SetInterruptCallback(uint32_t callback, uint32_t userData) {
    gCallbackData = userData;
    gCallback = callback;
    kernel::Log("  gpu: interrupt callback %08X (%08X)\n", callback, userData);
    if (callback && !gInterruptThreadStarted.exchange(true)) {
        kernel::ThreadStart start;
        start.hostBody = InterruptThread;
        start.name = "GPU interrupts";
        start.stackSize = 0x10000;
        if (uint32_t thread = kernel::CreateGuestThread(start, nullptr)) kernel::DereferenceObject(thread);
    }
}

void CountSwap() { ++gSwaps; }

uint32_t Register(uint32_t index) { return ReadRegister(index); }

uint8_t* PhysicalMemory(uint32_t address) { return Physical(address); }

const uint32_t* ConstantRegisters() { return gConstants.data(); }

}  // namespace s3n::gpu
