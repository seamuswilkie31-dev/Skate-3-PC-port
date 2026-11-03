#include "apu/xma.h"

#include "kernel/kernel.h"
#include "memory.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace s3n::apu {
namespace {

// The input: 2048-byte packets, each starting with a 32-bit header (6 bits frame count, 15
// bits where the first frame starting in the packet begins, after the header; 3 bits
// metadata; 8 bits how many packets of other streams follow before this stream's next).
// Frames run on across packets and start with their own length in bits, 15 bits that
// include themselves; their last bit says whether another frame follows in the packet.
constexpr uint32_t kPacketBytes = 2048;
constexpr uint32_t kPacketBits = kPacketBytes * 8;
constexpr uint32_t kHeaderBits = 32;
constexpr uint32_t kLengthBits = 15;
constexpr uint32_t kNoFrame = 0x7FFF;  // as a length: the rest of the packet is padding
constexpr uint32_t kMaxFrameBytes = 4096;

// The output: 512 samples a frame, in blocks of 256 bytes (128 samples of one channel).
constexpr uint32_t kContexts = 256;
constexpr uint32_t kBlockBytes = 256;
constexpr uint32_t kSamplesPerFrame = 512;
constexpr uint32_t kBlocksPerChannel = kSamplesPerFrame * 2 / kBlockBytes;
constexpr uint32_t kRates[4] = {24000, 32000, 44100, 48000};

// Each frame goes to FFmpeg as a WMA Pro packet of its own: a 4-bit sequence number, 2 unused
// bits and 15 bits counting the bits that finish the previous packet's last frame, then the
// frame. A frame too long for one packet is finished in a second.
constexpr uint32_t kWrapHeaderBits = 21;
constexpr uint32_t kWrapRoom = kPacketBits - kWrapHeaderBits;
constexpr uint16_t kXmaDecodeFlags = 0x10D6;  // what FFmpeg's own XMA decoders configure

// The host side of a context: its codec, and the decoder's progress between steps.
struct Stream {
    AVCodecContext* codec = nullptr;
    uint32_t rate = 0, channels = 0;
    uint8_t sequence = 0;
    bool reset = true;  // flush the codec before the next frame
    // The frame being collected, which may continue in a later packet, and where it does.
    std::array<uint8_t, kMaxFrameBytes + 8> frame{};
    uint32_t frameBits = 0;
    uint32_t resumeOffset = 0;
    bool resumeBuffer = false;
    // The last decoded frame, as the output ring takes it: 16-bit big-endian samples with
    // the channels interleaved, and how many of its blocks are already in the ring.
    std::array<uint8_t, kSamplesPerFrame * 2 * 2> pcm{};
    uint32_t pcmBlocks = 0, pcmWritten = 0;
};

GuestMemory* gMemory = nullptr;
std::mutex gMutex;  // also serializes every FFmpeg call: the build has no threading
uint32_t gPool = 0;  // guest address of the context array, 0 until first use
std::vector<bool> gInUse(kContexts);
std::vector<bool> gEnabled(kContexts);
std::vector<Stream> gStreams(kContexts);
AVPacket* gPacket = nullptr;
AVFrame* gFrame = nullptr;
std::array<uint8_t, kPacketBytes + AV_INPUT_BUFFER_PADDING_SIZE> gWrap{};
std::atomic<uint32_t> gCreated{0};
std::atomic<uint64_t> gFrames{0};      // decoded to sound
std::atomic<uint64_t> gPriming{0};     // the first after a reset, which a codec only primes on
std::atomic<uint64_t> gFailed{0};      // FFmpeg refused them
std::atomic<uint64_t> gBadFrames{0};   // broken framing in the input
std::atomic<uint64_t> gLoops{0};
std::atomic<uint32_t> gCodecMessages{0};
std::atomic<int64_t> gBusyTicks{0};  // QueryPerformanceCounter ticks spent decoding

uint8_t* Base() { return gMemory->base(); }

uint32_t Get(uint32_t context, XmaField f) {
    uint32_t word = LoadBE32(Base(), context + f.word * 4);
    return f.bits == 32 ? word : (word >> f.shift) & ((1u << f.bits) - 1);
}

void Set(uint32_t context, XmaField f, uint32_t value) {
    uint32_t address = context + f.word * 4;
    if (f.bits == 32) {
        StoreBE32(Base(), address, value);
        return;
    }
    uint32_t mask = ((1u << f.bits) - 1) << f.shift;
    StoreBE32(Base(), address, (LoadBE32(Base(), address) & ~mask) | ((value << f.shift) & mask));
}

int32_t IndexOf(uint32_t context) {
    if (!gPool || context < gPool || context >= gPool + kContexts * kContextSize) return -1;
    if ((context - gPool) % kContextSize) return -1;
    return int32_t((context - gPool) / kContextSize);
}

// Bits are numbered from the most significant bit of the first byte, as the hardware reads
// them. Reads touch only the bytes holding the bits asked for (count <= 25).
uint32_t ReadBits(const uint8_t* data, uint32_t bit, uint32_t count) {
    uint32_t first = bit >> 3, last = (bit + count - 1) >> 3, word = 0;
    for (uint32_t i = first; i <= last; ++i) word = word << 8 | data[i];
    return (word >> ((last + 1) * 8 - bit - count)) & ((1u << count) - 1);
}

// ORs bits into `to`, which must be zero where they land.
void PutBits(uint8_t* to, uint32_t bit, uint32_t count, uint32_t value) {
    while (count) {
        uint32_t n = std::min(count, 8 - (bit & 7));
        uint32_t part = (value >> (count - n)) & ((1u << n) - 1);
        to[bit >> 3] |= uint8_t(part << (8 - (bit & 7) - n));
        bit += n;
        count -= n;
    }
}

void CopyBits(uint8_t* to, uint32_t toBit, const uint8_t* from, uint32_t fromBit, uint32_t count) {
    while (count) {
        uint32_t n = std::min(count, 8 - (toBit & 7));
        PutBits(to, toBit, n, ReadBits(from, fromBit, n));
        toBit += n;
        fromBit += n;
        count -= n;
    }
}

// XAudio2 loops: from loopEnd back to loopStart, loopCount more times (255: for ever). Both
// are bit offsets in the input buffer.
bool LoopBack(uint32_t context) {
    uint32_t count = Get(context, kLoopCount), start = Get(context, kLoopStart);
    if (!count || start >= Get(context, kLoopEnd)) return false;
    Set(context, kInputReadOffset, start);
    if (count < 255) Set(context, kLoopCount, count - 1);
    ++gLoops;
    return true;
}

// Collects the next whole frame of a context's input into stream.frame and moves the read
// offset past it, as the hardware does: a frame continues in the stream's next packet (past
// other streams' packets) and from the end of one input buffer into the start of the other.
// A used-up buffer is handed back by clearing its valid bit. False when the input runs out;
// a frame cut off there resumes when the title refills the buffer, unless it moves the offset.
bool NextFrame(uint32_t context, Stream& s, uint32_t& length) {
    for (uint32_t guard = 0; guard < 16384; ++guard) {
        bool second = Get(context, kCurrentBuffer);
        if (!Get(context, second ? kInput1Valid : kInput0Valid)) {
            if (!Get(context, second ? kInput0Valid : kInput1Valid)) return false;
            second = !second;  // only the other one is filled: start there
            Set(context, kCurrentBuffer, second);
            Set(context, kInputReadOffset, 0);
            s.frameBits = 0;
        }
        uint32_t offset = Get(context, kInputReadOffset);
        if (s.frameBits && (offset != s.resumeOffset || second != s.resumeBuffer)) s.frameBits = 0;
        if (!s.frameBits && offset >= Get(context, kLoopEnd) && LoopBack(context)) continue;

        uint32_t packet = offset / kPacketBits, at = offset % kPacketBits;
        if (packet >= Get(context, second ? kInput1PacketCount : kInput0PacketCount)) {
            if (LoopBack(context)) {
                s.frameBits = 0;
                continue;
            }
            Set(context, second ? kInput1Valid : kInput0Valid, 0);
            Set(context, kCurrentBuffer, !second);
            Set(context, kInputReadOffset, 0);
            s.resumeOffset = 0;
            s.resumeBuffer = !second;
            continue;
        }
        const uint8_t* data =
            gMemory->Physical(Get(context, second ? kInput1Address : kInput0Address)) + packet * kPacketBytes;
        auto toNextPacket = [&] {
            uint32_t next = (packet + 1 + data[3]) * kPacketBits;
            Set(context, kInputReadOffset, next);
            s.resumeOffset = next;
            s.resumeBuffer = second;
        };
        if (at < kHeaderBits) {
            at = s.frameBits ? kHeaderBits : kHeaderBits + ReadBits(data, 6, kLengthBits);
            if (at >= kPacketBits) {  // no frame starts in this packet
                toNextPacket();
                continue;
            }
        }

        uint32_t room = kPacketBits - at;
        if (!s.frameBits) s.frame.fill(0);
        if (s.frameBits + room < kLengthBits) {  // even the length goes on in the next packet
            CopyBits(s.frame.data(), s.frameBits, data, at, room);
            s.frameBits += room;
            toNextPacket();
            continue;
        }
        uint32_t lengthHere = kLengthBits - std::min(s.frameBits, kLengthBits);
        length = s.frameBits >= kLengthBits ? ReadBits(s.frame.data(), 0, kLengthBits)
                                            : (s.frameBits ? ReadBits(s.frame.data(), 0, s.frameBits) << lengthHere : 0) |
                                                  ReadBits(data, at, lengthHere);
        if (length == kNoFrame || length <= kLengthBits) {
            if (s.frameBits) ++gBadFrames;  // a frame that doesn't go on as it said: drop it
            s.frameBits = 0;
            toNextPacket();
            continue;
        }
        uint32_t take = std::min(length - s.frameBits, room);
        CopyBits(s.frame.data(), s.frameBits, data, at, take);
        s.frameBits += take;
        at += take;
        if (s.frameBits < length) {
            toNextPacket();
            continue;
        }
        s.frameBits = 0;
        if (ReadBits(s.frame.data(), length - 1, 1) && at < kPacketBits)
            Set(context, kInputReadOffset, packet * kPacketBits + at);
        else
            toNextPacket();
        return true;
    }
    return false;
}

// A WMA Pro decoder set up the way FFmpeg sets one up for XMA: 512-sample frames with length
// prefixes, at the context's rate and channel count.
bool Prepare(Stream& s, uint32_t rate, uint32_t channels) {
    if (s.codec && s.rate == rate && s.channels == channels) {
        if (s.reset) avcodec_flush_buffers(s.codec);
        s.reset = false;
        return true;
    }
    avcodec_free_context(&s.codec);
    s.reset = false;
    const AVCodec* wmapro = avcodec_find_decoder(AV_CODEC_ID_WMAPRO);
    AVCodecContext* codec = wmapro ? avcodec_alloc_context3(wmapro) : nullptr;
    if (!codec) return false;
    codec->sample_rate = int(rate);
    av_channel_layout_default(&codec->ch_layout, int(channels));
    codec->block_align = kPacketBytes;  // frame lengths are 15 bits
    // WAVEFORMATEX extra bytes: bits per sample, channel mask (none: the layout's count), decode flags.
    codec->extradata = static_cast<uint8_t*>(av_mallocz(18 + AV_INPUT_BUFFER_PADDING_SIZE));
    codec->extradata_size = 18;
    if (codec->extradata) {
        codec->extradata[0] = 16;
        codec->extradata[14] = uint8_t(kXmaDecodeFlags);
        codec->extradata[15] = uint8_t(kXmaDecodeFlags >> 8);
    }
    if (!codec->extradata || avcodec_open2(codec, wmapro, nullptr) < 0) {
        avcodec_free_context(&codec);
        return false;
    }
    s.codec = codec;
    s.rate = rate;
    s.channels = channels;
    s.sequence = 0;
    return true;
}

void ToPcm(Stream& s, const AVFrame* frame) {
    uint32_t samples = std::min<uint32_t>(frame->nb_samples, kSamplesPerFrame);
    uint8_t* out = s.pcm.data();
    for (uint32_t i = 0; i < kSamplesPerFrame; ++i) {
        for (uint32_t c = 0; c < s.channels; ++c) {
            float value = i < samples ? reinterpret_cast<const float*>(frame->extended_data[c])[i] : 0.0f;
            auto sample = uint16_t(int16_t(std::lrintf(std::clamp(value, -1.0f, 1.0f) * 32767.0f)));
            *out++ = uint8_t(sample >> 8);
            *out++ = uint8_t(sample);
        }
    }
}

// Decodes stream.frame into stream.pcm. False when no sound came out (silence is left there).
bool Decode(Stream& s, uint32_t length) {
    bool decoded = false;
    for (uint32_t sent = 0; sent < length;) {
        uint32_t take = std::min(length - sent, kWrapRoom);
        gWrap.fill(0);
        gWrap[0] = uint8_t(s.sequence++ << 4);
        if (sent) PutBits(gWrap.data(), 6, kLengthBits, take);
        CopyBits(gWrap.data(), kWrapHeaderBits, s.frame.data(), sent, take);
        sent += take;
        gPacket->data = gWrap.data();
        gPacket->size = int(kPacketBytes);
        if (avcodec_send_packet(s.codec, gPacket) < 0) return false;
        while (avcodec_receive_frame(s.codec, gFrame) == 0) {
            if (gFrame->format == AV_SAMPLE_FMT_FLTP && uint32_t(gFrame->ch_layout.nb_channels) == s.channels) {
                ToPcm(s, gFrame);
                decoded = true;
            }
            av_frame_unref(gFrame);
        }
    }
    return decoded;
}

bool DecodeNext(uint32_t context, Stream& s) {
    uint32_t length = 0;
    if (!NextFrame(context, s, length)) return false;
    uint32_t channels = Get(context, kStereo) ? 2 : 1;
    s.pcmBlocks = kBlocksPerChannel * channels;
    s.pcmWritten = 0;
    bool primed = s.reset || !s.codec;
    if (Prepare(s, kRates[Get(context, kSampleRate)], channels) && Decode(s, length)) {
        ++gFrames;
        return true;
    }
    s.pcm.fill(0);
    if (primed)
        ++gPriming;
    else
        ++gFailed;
    return true;
}

// Fills as much of a context's output ring as the input allows. Equal read and write offsets
// mean an empty ring: a full one is marked by clearing the output's valid bit, which the title
// sets again once it has read. The valid bit is cleared too when there's no input left.
void Step(uint32_t context, Stream& s) {
    if (!Get(context, kOutputValid)) return;
    uint32_t blocks = Get(context, kOutputBlockCount);
    if (!blocks) blocks = 32;
    uint32_t write = Get(context, kOutputWriteOffset), read = Get(context, kOutputReadOffset);
    uint32_t space = write == read ? blocks : (read + blocks - write) % blocks;
    uint32_t output = Get(context, kOutputAddress);
    bool written = false;
    while (space) {
        if (s.pcmWritten == s.pcmBlocks && !DecodeNext(context, s)) break;
        uint32_t n = std::min(space, s.pcmBlocks - s.pcmWritten);
        for (uint32_t b = 0; b < n; ++b)
            std::memcpy(gMemory->Physical(output + (write + b) % blocks * kBlockBytes),
                        s.pcm.data() + (s.pcmWritten + b) * kBlockBytes, kBlockBytes);
        write = (write + n) % blocks;
        space -= n;
        s.pcmWritten += n;
        written = true;
    }
    if (written) Set(context, kOutputWriteOffset, write);
    bool starved = !Get(context, kInput0Valid) && !Get(context, kInput1Valid) && s.pcmWritten == s.pcmBlocks;
    if ((written && !space) || starved) Set(context, kOutputValid, 0);
}

void ResetStream(Stream& s) {
    s.reset = true;
    s.frameBits = 0;
    s.pcmBlocks = s.pcmWritten = 0;
}

void LogCodec(void*, int level, const char* format, va_list args) {
    if (level > AV_LOG_ERROR || ++gCodecMessages > 20) return;
    char line[256];
    std::vsnprintf(line, sizeof line, format, args);
    kernel::Log("  xma: ffmpeg: %s", line);
}

void Decoder() {
    for (;;) {
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        for (uint32_t i = 0; i < kContexts; ++i) {
            std::lock_guard lock(gMutex);
            if (gPool && gEnabled[i]) Step(gPool + i * kContextSize, gStreams[i]);
        }
        QueryPerformanceCounter(&end);
        gBusyTicks += end.QuadPart - start.QuadPart;
        Sleep(1);
    }
}

std::string Describe() {
    uint32_t inUse = 0, enabled = 0;
    {
        std::unique_lock lock(gMutex, std::try_to_lock);
        for (uint32_t i = 0; lock.owns_lock() && i < kContexts; ++i) {
            inUse += gInUse[i];
            enabled += gEnabled[i];
        }
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    double seconds = double(gBusyTicks.load()) / double(frequency.QuadPart);
    char line[256];
    std::snprintf(line, sizeof line,
                  "xma: %u contexts created, %u in use, %u enabled; %llu frames decoded, %llu priming, %llu failed, "
                  "%llu broken, %llu loops; %u FFmpeg errors; %.1f s decoding\n",
                  gCreated.load(), inUse, enabled, gFrames.load(), gPriming.load(), gFailed.load(), gBadFrames.load(),
                  gLoops.load(), gCodecMessages.load(), seconds);
    return line;
}

}  // namespace

void InitXma(GuestMemory& memory) {
    gMemory = &memory;
    av_log_set_callback(LogCodec);
    gPacket = av_packet_alloc();
    gFrame = av_frame_alloc();
    kernel::AddReportSection(Describe);
    std::thread(Decoder).detach();
}

uint32_t CreateContext() {
    std::lock_guard lock(gMutex);
    if (!gPool) {
        uint32_t offset = gMemory->AllocPhysical(0x10000, 0x10000, GuestMemory::kPhysicalSize, false);
        if (offset == GuestMemory::kNoPhysical) return 0;
        gPool = GuestMemory::kPhysicalWindows[0] + offset;
    }
    for (uint32_t i = 0; i < kContexts; ++i) {
        if (gInUse[i]) continue;
        gInUse[i] = true;
        gEnabled[i] = false;
        ResetStream(gStreams[i]);
        uint32_t context = gPool + i * kContextSize;
        std::memset(Base() + context, 0, kContextSize);
        ++gCreated;
        return context;
    }
    return 0;
}

void ReleaseContext(uint32_t context) {
    std::lock_guard lock(gMutex);
    if (int32_t i = IndexOf(context); i >= 0) {
        gInUse[i] = gEnabled[i] = false;
        avcodec_free_context(&gStreams[i].codec);
        ResetStream(gStreams[i]);
    }
}

uint32_t GetField(uint32_t context, XmaField field) {
    std::lock_guard lock(gMutex);
    return IndexOf(context) >= 0 ? Get(context, field) : 0;
}

void SetField(uint32_t context, XmaField field, uint32_t value) {
    std::lock_guard lock(gMutex);
    if (IndexOf(context) >= 0) Set(context, field, value);
}

// XMA_CONTEXT_INIT: input buffer 0 and 1 (address, packet count), input read offset,
// output buffer (address, block count), work buffer, subframes to decode, channel count,
// sample rate, then loop data (start, end, count, subframe end, subframe skip).
void InitializeContext(uint32_t context, uint32_t init) {
    std::lock_guard lock(gMutex);
    int32_t index = IndexOf(context);
    if (index < 0) return;
    ResetStream(gStreams[index]);
    const uint8_t* base = Base();
    auto physical = [&](uint32_t address) { return address ? gMemory->ConsolePhysical(address) : 0; };
    std::memset(Base() + context, 0, kContextSize);
    Set(context, kInput0Address, physical(LoadBE32(base, init + 0x00)));
    Set(context, kInput0PacketCount, LoadBE32(base, init + 0x04));
    Set(context, kInput1Address, physical(LoadBE32(base, init + 0x08)));
    Set(context, kInput1PacketCount, LoadBE32(base, init + 0x0C));
    Set(context, kInputReadOffset, LoadBE32(base, init + 0x10));
    Set(context, kOutputAddress, physical(LoadBE32(base, init + 0x14)));
    Set(context, kOutputBlockCount, LoadBE32(base, init + 0x18));
    Set(context, kSubframeDecodeCount, LoadBE32(base, init + 0x20));
    Set(context, kStereo, LoadBE32(base, init + 0x24) >= 1);
    Set(context, kSampleRate, LoadBE32(base, init + 0x28));
    Set(context, kLoopStart, LoadBE32(base, init + 0x2C));
    Set(context, kLoopEnd, LoadBE32(base, init + 0x30));
    Set(context, kLoopCount, base[init + 0x34]);
    Set(context, kLoopSubframeEnd, base[init + 0x35]);
    Set(context, kLoopSubframeSkip, base[init + 0x36]);
}

void EnableContext(uint32_t context, bool enabled) {
    std::lock_guard lock(gMutex);
    if (int32_t i = IndexOf(context); i >= 0) gEnabled[i] = enabled;
}

}  // namespace s3n::apu
