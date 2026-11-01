// xexscan: finds what XenonRecomp's config needs but can't detect itself.
//
//   xexscan <default.xex>                      helper + setjmp/longjmp addresses as TOML
//   xexscan <default.xex> --dis <addr> [n]     disassemble n instructions (default 32)
//   xexscan <default.xex> --find <word> [n]    list code addresses holding a 32-bit word
//   xexscan <default.xex> --bctr-shapes [n]    group bctr sites by the n instructions before them
//   xexscan <default.xex> --generate <tables.toml> <main.toml>
//                                              jump tables (replaces XenonAnalyse), plus [main]
//                                              keys: leaf-function boundaries and non-code
//                                              blocks inside .text for XenonRecomp to skip
//
// Pass the patched XEX (XenonRecomp's patched_file_path output) when building on
// a title update.

#include <disasm.h>
#include <file.h>
#include <image.h>
#include <xbox.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct HelperPattern {
    const char* key;
    uint32_t words[2];
    uint32_t span;  // bytes of the save/restore run; callers branch anywhere inside it
};

// Byte patterns from XenonRecomp's README, extended to two instructions where the
// README gives one so a lone inlined `ld r14, -0x98(r1)` can't match.
constexpr HelperPattern kHelpers[] = {
    {"restgprlr_14_address", {0xE9C1FF68, 0xE9E1FF70}, 18 * 4},  // ld   r14,-0x98(r1); ld r15,-0x90(r1)
    {"savegprlr_14_address", {0xF9C1FF68, 0xF9E1FF70}, 18 * 4},  // std  r14,-0x98(r1); std r15,-0x90(r1)
    {"restfpr_14_address", {0xC9CCFF70, 0xC9ECFF78}, 18 * 4},    // lfd  f14,-0x90(r12); lfd f15,-0x88(r12)
    {"savefpr_14_address", {0xD9CCFF70, 0xD9ECFF78}, 18 * 4},    // stfd f14,-0x90(r12); stfd f15,-0x88(r12)
    {"restvmx_14_address", {0x3960FEE0, 0x7DCB60CE}, 18 * 8},    // li r11,-0x120; lvx v14,r11,r12
    {"savevmx_14_address", {0x3960FEE0, 0x7DCB61CE}, 18 * 8},    // li r11,-0x120; stvx v14,r11,r12
    {"restvmx_64_address", {0x3960FC00, 0x100B60CB}, 64 * 8},    // li r11,-0x400; lvx128 v64,r11,r12
    {"savevmx_64_address", {0x3960FC00, 0x100B61CB}, 64 * 8},    // li r11,-0x400; stvx128 v64,r11,r12
};

constexpr uint32_t kBlr = 0x4E800020;
constexpr uint32_t kBctr = 0x4E800420;
constexpr uint32_t kEndOfTextMarker = 0x00485645;  // "\0HVE"

struct Function {
    uint32_t begin;
    uint32_t size;
};

uint32_t Be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

// GPRs an instruction writes, as a bitmask; nullopt for opcodes not modelled here.
std::optional<uint32_t> GprWrites(uint32_t w) {
    uint32_t op = w >> 26;
    uint32_t rD = 1u << ((w >> 21) & 31);
    uint32_t rA = 1u << ((w >> 16) & 31);
    switch (op) {
    case 4: case 59: case 63:                            // vector, floating point
    case 10: case 11: case 16: case 18: case 19:         // compares, branches, CR ops
    case 36: case 38: case 44: case 47:                  // stw stb sth stmw
    case 48: case 50: case 52: case 54:                  // lfs lfd stfs stfd
        return 0u;
    case 7: case 8: case 12: case 13: case 14: case 15:  // mulli subfic addic addic. addi addis
    case 32: case 34: case 40: case 42:                  // lwz lbz lhz lha
        return rD;
    case 33: case 35: case 41: case 43:                  // update-form loads
        return rD | rA;
    case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30:
        return rA;                                       // rotates, logical immediates
    case 37: case 39: case 45: case 49: case 51: case 53: case 55:
        return rA;                                       // update-form stores, FP update loads
    case 58:
        return (w & 3) == 1 ? rD | rA : rD;              // ld, ldu, lwa
    case 62:
        return (w & 3) == 1 ? rA : 0u;                   // std, stdu
    case 31:
        switch ((w >> 1) & 0x3FF) {
        // compares, traps, cache, sync, stores, special-register moves, FP/vector loads
        case 0: case 32: case 4: case 68: case 54: case 86: case 278: case 246: case 1014: case 982:
        case 598: case 854: case 151: case 215: case 407: case 149: case 663: case 727: case 983:
        case 231: case 487: case 135: case 167: case 199: case 150: case 214: case 662: case 918:
        case 647: case 679: case 467: case 144: case 146: case 178: case 535: case 599: case 7:
        case 39: case 71: case 103: case 359: case 6: case 38: case 519: case 551:
            return 0u;
        case 183: case 247: case 439: case 181: case 695: case 759: case 567: case 631:
            return rA;                                   // update-form stores, FP update loads
        case 55: case 119: case 311: case 375: case 53:
            return rD | rA;                              // update-form loads
        case 23: case 87: case 279: case 343: case 21: case 341: case 20: case 84: case 534: case 790:
        case 339: case 371: case 19: case 83:
            return rD;                                   // indexed loads, mfspr, mftb, mfcr, mfmsr
        case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476: case 24:
        case 536: case 792: case 824: case 26: case 954: case 922: case 986: case 27: case 539:
        case 794: case 58:
            return rA;                                   // logical, shifts, extends, count zeros
        }
        if (((w >> 2) & 0x1FF) == 413) return rA;        // sradi
        switch ((w >> 1) & 0x1FF) {                      // XO-form arithmetic (bit 10 is OE)
        case 266: case 40: case 10: case 8: case 138: case 136: case 234: case 232: case 202:
        case 200: case 104: case 235: case 233: case 75: case 11: case 73: case 9: case 491:
        case 459: case 489: case 457:
            return rD;
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

bool IsBranch(uint32_t w) {
    uint32_t op = w >> 26;
    uint32_t xo = (w >> 1) & 0x3FF;
    return op == 16 || op == 18 || (op == 19 && (xo == 16 || xo == 528));
}

class Scanner {
public:
    explicit Scanner(const Image& image) : image_(image) {
        for (const Section& s : image.sections)
            if (s.flags & SectionFlags_Code) code_.push_back(&s);
        IndexCalls();
        ReadPdata();
    }

    void Run(const char* path) {
        size_t codeBytes = 0;
        for (const Section* s : code_) codeBytes += s->size;
        std::printf("# xexscan: %s\n", path);
        std::printf("# image base 0x%08zX, size 0x%08X, entry 0x%08zX\n", image_.base, image_.size, image_.entry_point);
        std::printf("# %zu code section(s), %zu KiB of code, %zu .pdata functions, %zu bl call sites\n\n",
                    code_.size(), codeBytes / 1024, functions_.size(), callSites_);

        for (const HelperPattern& h : kHelpers) {
            std::vector<uint32_t> hits = FindPattern(h.words);
            if (hits.empty()) {
                std::printf("# %s = ?        # NOT FOUND: may be inlined everywhere; check manually\n", h.key);
                continue;
            }
            // The real helper is the hit that the most branches land in.
            auto refs = [&](uint32_t a) { return BranchesInto(a, a + h.span); };
            auto best = *std::max_element(hits.begin(), hits.end(), [&](uint32_t a, uint32_t b) { return refs(a) < refs(b); });
            std::printf("%s = 0x%08X   # %u branches into it", h.key, best, refs(best));
            if (hits.size() > 1) std::printf(", %zu matches: CHECK", hits.size());
            std::printf("\n");
        }
        std::printf("\n");
        FindJumpBuffers();
    }

    void Disassemble(uint32_t address, int count) const {
        if (const Function* f = Containing(address))
            std::printf("# in function 0x%08X (size 0x%X)\n", f->begin, f->size);
        PrintCode(address, count);
    }

    void Find(uint32_t word, int limit) const {
        int found = 0;
        ForEachWord([&](uint32_t address, uint32_t w) {
            if (w == word && found++ < limit) std::printf("0x%08X\n", address);
        });
        std::printf("# %d occurrence(s) of %08X in code\n", found, word);
    }

    // Groups every bctr site by the mnemonics of the instructions leading up to it.
    void BctrShapes(int window) const {
        std::unordered_map<std::string, std::pair<int, uint32_t>> shapes;  // shape -> (count, example)
        ForEachWord([&](uint32_t address, uint32_t word) {
            if (word != kBctr) return;
            std::string shape;
            for (int i = window; i >= 1; --i) {
                const uint8_t* p = At(address - 4 * i);
                ppc_insn insn{};
                shape += (p && ppc::Disassemble(p, address - 4 * i, insn) && insn.opcode) ? insn.opcode->name : "?";
                shape += ' ';
            }
            auto& entry = shapes[shape];
            if (entry.first++ == 0) entry.second = address;
        });
        std::vector<std::pair<std::string, std::pair<int, uint32_t>>> sorted(shapes.begin(), shapes.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        for (const auto& [shape, entry] : sorted)
            std::printf("%5d  e.g. 0x%08X  %sbctr\n", entry.first, entry.second, shape.c_str());
    }

    // Writes every switch jump table as XenonRecomp switch-table TOML, plus [main] keys:
    // explicit boundaries for leaf functions holding them (XenonRecomp's own analysis
    // ends a function at its first bctr) and non-code blocks inside .text to skip.
    // Reports every bounds-checked bctr it couldn't use.
    bool WriteGenerated(const char* tablesPath, const char* functionsPath) const {
        int bctrs = 0;
        size_t guarded = 0, unguarded = 0;
        std::vector<SwitchTable> candidates;
        std::vector<uint32_t> unresolved;
        ForEachWord([&](uint32_t address, uint32_t word) {
            if (word != kBctr) return;
            ++bctrs;
            std::optional<BoundsCheck> check = FindBoundsCheck(address);
            if (!check) {
                // No bounds check: an unguarded switch, or a function-pointer/vtable tail call.
                if (std::optional<SwitchTable> table = ResolveUnguardedSwitch(address)) {
                    candidates.push_back(std::move(*table));
                    ++unguarded;
                }
                return;
            }
            ++guarded;
            if (std::optional<SwitchTable> table = ResolveSwitch(address, *check))
                candidates.push_back(std::move(*table));
            else
                unresolved.push_back(address);
        });

        std::unordered_map<uint32_t, const SwitchTable*> byBctr;
        std::unordered_set<uint32_t> caseLabels;
        for (const SwitchTable& t : candidates) {
            byBctr[t.bctr] = &t;
            caseLabels.insert(t.labels.begin(), t.labels.end());
        }

        std::vector<const SwitchTable*> tables;
        std::map<uint32_t, uint32_t> leaves;  // start -> size
        std::vector<uint32_t> rejected;
        for (const SwitchTable& t : candidates) {
            auto covers = [&](uint32_t begin, uint32_t size) {
                return t.bctr >= begin && t.bctr < begin + size &&
                       std::all_of(t.labels.begin(), t.labels.end(),
                                   [&](uint32_t l) { return !(l & 3) && l >= begin && l < begin + size; });
            };
            if (const Function* f = Containing(t.bctr)) {
                (covers(f->begin, f->size) ? tables.push_back(&t) : rejected.push_back(t.bctr));
                continue;
            }
            // Leaf function: candidates are the nearest bl target and the nearest entry point
            // (for leaves only reached through function pointers). Take the closest start
            // whose control flow covers the whole switch, so neighbours never get merged.
            std::optional<std::pair<uint32_t, uint32_t>> best;
            for (uint32_t begin : {LeafStart(t.bctr), EntryBefore(t.bctr, caseLabels)}) {
                uint32_t size = LeafExtent(begin, byBctr);
                if (covers(begin, size) && (!best || begin > best->first)) best = {begin, size};
            }
            if (!best) {
                rejected.push_back(t.bctr);
                continue;
            }
            leaves[best->first] = std::max(leaves[best->first], best->second);
            tables.push_back(&t);
        }
        uint32_t previousEnd = 0;
        for (const auto& [begin, size] : leaves) {
            if (begin < previousEnd) std::printf("  WARNING: leaf function 0x%08X overlaps the one before it\n", begin);
            previousEnd = begin + size;
        }

        FILE* out = std::fopen(tablesPath, "w");
        FILE* fns = std::fopen(functionsPath, "w");
        if (!out || !fns) {
            std::fprintf(stderr, "xexscan: can't write %s / %s\n", tablesPath, functionsPath);
            return false;
        }
        size_t cases = 0;
        for (const SwitchTable* t : tables) cases += t->labels.size();
        std::fprintf(out, "# Generated by xexscan --switch-tables: %zu jump tables, %zu cases.\n", tables.size(), cases);
        std::fprintf(out, "# base = the bctr itself, so a table can never arm early on another bctr.\n\n");
        for (const SwitchTable* t : tables) {
            std::fprintf(out, "[[switch]]\nbase = 0x%08X\nr = %u\ndefault = 0x%08X\nlabels = [\n", t->bctr, t->index,
                         t->defaultLabel);
            for (uint32_t label : t->labels) std::fprintf(out, "    0x%08X,\n", label);
            std::fprintf(out, "]\n\n");
        }
        std::fclose(out);

        std::fprintf(fns, "# Generated by xexscan --generate.\n\n");
        std::fprintf(fns, "# Leaf functions (no .pdata entry) that hold jump tables, measured by following\n");
        std::fprintf(fns, "# their control flow through every case.\n");
        std::fprintf(fns, "functions = [\n");
        for (const auto& [begin, size] : leaves) std::fprintf(fns, "    { address = 0x%08X, size = 0x%X },\n", begin, size);
        std::fprintf(fns, "]\n");
        std::optional<EndOfText> data = FindEndOfTextData();
        if (data) {
            std::fprintf(fns, "\n# Data, not code: the block after the import thunks at 0x%08X (marker, header,\n", data->marker);
            std::fprintf(fns, "# then a table of %zu code addresses). XenonRecomp skips size bytes from the marker.\n",
                         data->entries.size());
            std::fprintf(fns, "invalid_instructions = [\n    { data = 0x%08X, size = 0x%X },\n]\n", kEndOfTextMarker, data->size);
        }
        std::fclose(fns);

        std::printf("%d bctr sites: %zu bounds-checked switches, %zu unguarded switches (inline table), "
                    "the rest indirect calls\n",
                    bctrs, guarded, unguarded);
        std::printf("  %zu jump tables (%zu cases) -> %s\n", tables.size(), cases, tablesPath);
        std::printf("  %zu leaf function boundaries -> %s\n", leaves.size(), functionsPath);
        auto list = [](const char* what, const std::vector<uint32_t>& sites) {
            if (sites.empty()) return;
            std::printf("  %zu %s (left out):", sites.size(), what);
            for (uint32_t a : sites) std::printf(" 0x%08X", a);
            std::printf("\n");
        };
        list("tables with cases outside their function", rejected);
        list("bounds-checked sites not understood", unresolved);
        if (data) {
            // What the block's table points at decides whether XenonRecomp already knows those
            // addresses as functions (needed if the game ever calls through them).
            size_t pdata = 0, called = 0, inside = 0, other = 0;
            std::vector<uint32_t> others;
            for (uint32_t a : data->entries) {
                auto f = std::lower_bound(functions_.begin(), functions_.end(), a,
                                          [](const Function& fn, uint32_t x) { return fn.begin < x; });
                if (f != functions_.end() && f->begin == a)
                    ++pdata;
                else if (std::binary_search(callTargets_.begin(), callTargets_.end(), a))
                    ++called;
                else if (Containing(a))
                    ++inside;
                else
                    ++other, others.push_back(a);
            }
            std::printf("  non-code block at 0x%08X, 0x%X bytes: %zu-entry address table -> %s\n", data->marker, data->size,
                        data->entries.size(), functionsPath);
            std::printf("    table entries: %zu .pdata function starts, %zu other bl targets, %zu inside a function, %zu elsewhere",
                        pdata, called, inside, other);
            for (size_t i = 0; i < others.size() && i < 8; ++i) std::printf(" 0x%08X", others[i]);
            std::printf("\n");
        }
        return true;
    }

private:
    struct SwitchTable {
        uint32_t bctr;
        uint32_t index;         // register holding the case number at the bctr
        uint32_t defaultLabel;  // 0 when out-of-range cases return (bgtlr)
        std::vector<uint32_t> labels;
    };

    // The data block the linker leaves after the import thunks at the end of .text: the
    // word 0x00485645 ("\0HVE"), a 32-byte header, then a count, one more word, and
    // `count` image-relative code addresses. XenonRecomp's README skips the same marker
    // (with one entry, 44 bytes) as its example of non-code inside .text.
    struct EndOfText {
        uint32_t marker;
        uint32_t size;
        std::vector<uint32_t> entries;
    };

    std::optional<EndOfText> FindEndOfTextData() const {
        std::optional<EndOfText> found;
        ForEachWord([&](uint32_t address, uint32_t word) {
            if (word != kEndOfTextMarker || found) return;
            uint32_t count = Word(address + 0x20);
            if (count == 0 || count > 0x10000) return;
            EndOfText block{address, 0x28 + 4 * count, {}};
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t target = uint32_t(image_.base) + Word(address + 0x28 + 4 * i);
                bool code = std::any_of(code_.begin(), code_.end(), [&](const Section* s) {
                    return target >= s->base && target < s->base + s->size;
                });
                if (!code || (target & 3)) return;  // not the table we know
                block.entries.push_back(target);
            }
            found = std::move(block);
        });
        return found;
    }

    // `cmplwi crN, rIdx, max` followed by `bgt crN, default` or `bgtlr crN`.
    struct BoundsCheck {
        uint32_t compare;
        uint32_t branch;
        uint32_t index;
        uint32_t count;
        uint32_t defaultLabel;
    };

    // What the switch simulator knows about a register.
    struct Value {
        // Input: whatever the register held when simulation started (unguarded switches).
        enum Kind { Unknown, Const, Index, Entry, Input } kind = Unknown;
        uint32_t value = 0;  // Const: the constant. Index: shift applied to the case number. Entry: table address.
        uint32_t size = 0;   // Entry: element size in bytes
        uint32_t shift = 0;  // Entry: left shift applied to the loaded element
        uint32_t add = 0;    // Entry: constant added after the shift
        uint32_t reg = 0;    // Index, Entry, Input: the register holding the unscaled case number
    };

    const uint8_t* At(uint32_t address) const { return static_cast<const uint8_t*>(image_.Find(address)); }

    uint32_t Word(uint32_t address) const {
        const uint8_t* p = At(address);
        return p ? Be32(p) : 0;
    }

    template <class F> void ForEachWord(F&& visit) const {
        for (const Section* s : code_)
            for (uint32_t off = 0; off + 4 <= s->size; off += 4) visit(uint32_t(s->base + off), Be32(s->data + off));
    }

    // Records every relative branch target (b, bl and bc), and every bl call site.
    void IndexCalls() {
        ForEachWord([&](uint32_t address, uint32_t word) {
            if ((word >> 26) == 16 && !(word & 3)) {  // bc, relative, no link
                localTargets_.push_back(address + uint32_t(int32_t(int16_t(word & 0xFFFC))));
                return;
            }
            if ((word >> 26) != 18 || (word & 2)) return;  // I-form branch, AA=0
            uint32_t target = address + BranchDisplacement(word);
            branchTargets_.push_back(target);
            if (word & 1) {
                calls_[target].push_back(address);
                ++callSites_;
            } else {
                localTargets_.push_back(target);
            }
        });
        std::sort(branchTargets_.begin(), branchTargets_.end());
        std::sort(localTargets_.begin(), localTargets_.end());
        for (const auto& [target, sites] : calls_) callTargets_.push_back(target);
        std::sort(callTargets_.begin(), callTargets_.end());
    }

    static uint32_t BranchDisplacement(uint32_t w) {
        int32_t d = int32_t(w & 0x03FFFFFC);
        return uint32_t(d & 0x02000000 ? d - 0x04000000 : d);
    }

    uint32_t BranchesInto(uint32_t begin, uint32_t end) const {
        return uint32_t(std::lower_bound(branchTargets_.begin(), branchTargets_.end(), end) -
                        std::lower_bound(branchTargets_.begin(), branchTargets_.end(), begin));
    }

    void ReadPdata() {
        const Section* pdata = image_.Find(".pdata");
        if (!pdata) return;
        for (uint32_t off = 0; off + 8 <= pdata->size; off += 8) {
            uint32_t begin = Be32(pdata->data + off);
            uint32_t data = Be32(pdata->data + off + 4);
            if (begin) functions_.push_back({begin, ((data >> 8) & 0x3FFFFF) * 4});  // FunctionLength is in words
        }
        std::sort(functions_.begin(), functions_.end(), [](const Function& a, const Function& b) { return a.begin < b.begin; });
    }

    std::vector<uint32_t> FindPattern(const uint32_t (&words)[2]) const {
        std::vector<uint32_t> hits;
        ForEachWord([&](uint32_t address, uint32_t word) {
            if (word != words[0]) return;
            const uint8_t* next = At(address + 4);
            if (next && Be32(next) == words[1]) hits.push_back(address);
        });
        return hits;
    }

    const Function* Containing(uint32_t address) const {
        auto it = std::upper_bound(functions_.begin(), functions_.end(), address,
                                   [](uint32_t a, const Function& f) { return a < f.begin; });
        if (it == functions_.begin()) return nullptr;
        --it;
        return address < it->begin + it->size ? &*it : nullptr;
    }

    bool IsKnownFunction(uint32_t address) const {
        if (std::binary_search(callTargets_.begin(), callTargets_.end(), address)) return true;
        auto it = std::lower_bound(functions_.begin(), functions_.end(), address,
                                   [](const Function& f, uint32_t a) { return f.begin < a; });
        return it != functions_.end() && it->begin == address;
    }

    void PrintCode(uint32_t address, int count) const {
        for (int i = 0; i < count; ++i, address += 4) {
            const uint8_t* p = At(address);
            if (!p) return;
            ppc_insn insn{};
            if (ppc::Disassemble(p, address, insn) && insn.opcode)
                std::printf("#     %08X  %08X  %s %s\n", address, Be32(p), insn.opcode->name, insn.op_str);
            else
                std::printf("#     %08X  %08X  ?\n", address, Be32(p));
        }
    }

    // ---- switch tables ----

    std::optional<BoundsCheck> FindBoundsCheck(uint32_t bctr) const {
        for (uint32_t a = bctr - 4; a > bctr - 16 * 4; a -= 4) {
            uint32_t w = Word(a);
            bool bc = (w >> 26) == 16 && !(w & 3);       // bc, relative, no link
            bool bclr = (w & 0xFC0007FF) == 0x4C000020;  // bclr, no link
            if (!bc && !bclr) {
                if (IsBranch(w)) return std::nullopt;
                continue;
            }
            uint32_t bo = (w >> 21) & 31, bi = (w >> 16) & 31;
            if ((bo & 0x1C) != 0x0C || (bi & 3) != 1) return std::nullopt;  // must be "branch if greater than"
            uint32_t target = bc ? uint32_t(a + int16_t(w & 0xFFFC)) : 0;
            for (uint32_t c = a - 4; c > a - 8 * 4; c -= 4) {
                uint32_t cw = Word(c);
                if ((cw >> 26) == 10 && ((cw >> 23) & 7) == (bi >> 2) && !(cw & 0x00200000))  // cmplwi on that CR field
                    return BoundsCheck{c, a, (cw >> 16) & 31, (cw & 0xFFFF) + 1u, target};
                if (IsBranch(cw)) return std::nullopt;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    // Applies one instruction to the simulated registers. Returns false for an
    // instruction whose register effects aren't known.
    static bool Step(uint32_t w, Value (&regs)[32], Value& ctr) {
        uint32_t op = w >> 26, rD = (w >> 21) & 31, rA = (w >> 16) & 31, rB = (w >> 11) & 31;
        uint32_t simm = uint32_t(int32_t(int16_t(w & 0xFFFF)));
        auto plus = [](const Value& v, uint32_t c) -> Value {
            if (v.kind == Value::Const) return {Value::Const, v.value + c};
            if (v.kind == Value::Entry) {
                Value r = v;
                r.add += c;
                return r;
            }
            return {};
        };
        if ((w & 0xFC1FFFFF) == 0x7C0903A6) {  // mtctr rS
            ctr = regs[rD];
        } else if (op == 14 || op == 15) {  // addi/li, addis/lis
            uint32_t imm = op == 15 ? w << 16 : simm;
            regs[rD] = rA == 0 ? Value{Value::Const, imm} : plus(regs[rA], imm);
        } else if (op == 24) {  // ori rA, rS, uimm
            const Value s = regs[rD];
            regs[rA] = s.kind == Value::Const ? Value{Value::Const, s.value | (w & 0xFFFF)} : (w & 0xFFFF) ? Value{} : s;
        } else if (op == 21) {  // rlwinm rA, rS, sh, mb, me
            uint32_t mb = (w >> 6) & 31, me = (w >> 1) & 31;
            Value s = regs[rD];
            bool slwi = mb == 0 && me == 31 - rB, clrlwi = rB == 0 && me == 31;
            if (slwi && s.kind == Value::Index) {
                s.value += rB;
            } else if (slwi && s.kind == Value::Input) {
                s = {Value::Index, rB, 0, 0, 0, s.reg};
            } else if (slwi && s.kind == Value::Entry && s.add == 0) {
                s.shift += rB;
            } else if (!(clrlwi && (s.kind == Value::Index || s.kind == Value::Entry))) {
                s = {};  // clrlwi of a range-checked index or a zero-extended entry changes nothing
            }
            regs[rA] = s;
        } else if (op == 31 && (((w >> 1) & 0x3FF) == 23 || ((w >> 1) & 0x3FF) == 279 || ((w >> 1) & 0x3FF) == 87)) {
            uint32_t xo = (w >> 1) & 0x3FF;  // lwzx, lhzx, lbzx rD, rA, rB
            uint32_t size = xo == 23 ? 4 : xo == 279 ? 2 : 1;
            const Value x = regs[rA], y = regs[rB];
            const Value* base = x.kind == Value::Const ? &x : y.kind == Value::Const ? &y : nullptr;
            const Value* index = x.kind == Value::Index ? &x : y.kind == Value::Index ? &y : nullptr;
            bool scaled = index && (1u << index->value) == size;
            regs[rD] = base && scaled ? Value{Value::Entry, base->value, size, 0, 0, index->reg} : Value{};
        } else if (op == 31 && ((w >> 1) & 0x1FF) == 266) {  // add rD, rA, rB
            const Value x = regs[rA], y = regs[rB];
            regs[rD] = x.kind == Value::Const ? plus(y, x.value) : y.kind == Value::Const ? plus(x, y.value) : Value{};
        } else if (op == 31 && ((w >> 1) & 0x3FF) == 444 && rD == rB) {  // mr rA, rS
            regs[rA] = regs[rD];
        } else if (std::optional<uint32_t> writes = GprWrites(w)) {
            for (uint32_t r = 0; r < 32; ++r)
                if (*writes & (1u << r)) regs[r] = {};
        } else {
            return false;
        }
        return true;
    }

    // Simulates from a few instructions before the bounds check to the bctr. Gives
    // up on any instruction with unknown effects after the check rather than guessing.
    std::optional<SwitchTable> ResolveSwitch(uint32_t bctr, const BoundsCheck& check) const {
        uint32_t start = check.compare;
        for (int i = 0; i < 8; ++i) {
            uint32_t w = Word(start - 4);
            if (!w || IsBranch(w)) break;
            start -= 4;
        }
        Value regs[32];
        Value ctr;
        const Value caseNumber{Value::Index, 0, 0, 0, 0, check.index};
        regs[check.index] = caseNumber;
        for (uint32_t a = start; a < bctr; a += 4) {
            if (a == check.branch || a == check.compare) continue;
            uint32_t w = Word(a);
            bool before = a < check.compare;
            uint32_t writes = GprWrites(w).value_or(0xFFFFFFFF);
            if (!Step(w, regs, ctr)) {
                if (!before) return std::nullopt;
                for (Value& v : regs) v = {};
            }
            // Before the check, the compared register ends up holding the case number;
            // copies taken from an older value of it are not the case number.
            if (before && (writes & (1u << check.index))) {
                for (Value& v : regs)
                    if (v.kind == Value::Index) v = {};
                regs[check.index] = caseNumber;
            }
        }
        // XenonRecomp switches on the compared register, so it must still hold the case number.
        if (ctr.kind != Value::Entry || regs[check.index].kind != Value::Index || regs[check.index].value != 0)
            return std::nullopt;
        SwitchTable table{bctr, check.index, check.defaultLabel, {}};
        for (uint32_t i = 0; i < check.count; ++i) {
            const uint8_t* p = At(ctr.value + i * ctr.size);
            if (!p) return std::nullopt;
            uint32_t element = ctr.size == 4 ? Be32(p) : ctr.size == 2 ? uint32_t(p[0] << 8 | p[1]) : p[0];
            table.labels.push_back(ctr.add + (element << ctr.shift));
        }
        return table;
    }

    // A switch without a bounds check (the compiler knew the index was in range): the
    // case number arrives in some register, gets scaled, and indexes a word table that
    // sits right after the bctr. With no compare to give the case count, the table is
    // taken to run exactly up to the lowest case address, i.e. the first case's code.
    // Function-pointer arrays live in data, never inline, so they can't match.
    std::optional<SwitchTable> ResolveUnguardedSwitch(uint32_t bctr) const {
        uint32_t start = bctr;
        for (int i = 0; i < 12; ++i) {
            uint32_t w = Word(start - 4);
            if (!w || IsBranch(w)) break;
            if (const Function* f = Containing(start); f && f->begin == start) break;
            start -= 4;
        }
        Value regs[32];
        for (uint32_t r = 0; r < 32; ++r) regs[r] = {Value::Input, 0, 0, 0, 0, r};
        Value ctr;
        for (uint32_t a = start; a < bctr; a += 4)
            if (!Step(Word(a), regs, ctr)) return std::nullopt;
        uint32_t table = bctr + 4;
        if (ctr.kind != Value::Entry || ctr.size != 4 || ctr.shift || ctr.add || ctr.value != table) return std::nullopt;
        // The register the case number came from must still hold it.
        if (regs[ctr.reg].kind != Value::Input || regs[ctr.reg].reg != ctr.reg) return std::nullopt;

        SwitchTable result{bctr, ctr.reg, 0, {}};
        uint32_t lowest = UINT32_MAX;
        for (uint32_t a = table; a < lowest; a += 4) {
            uint32_t label = Word(a);
            if (label <= bctr || (label & 3) || !At(label)) break;
            result.labels.push_back(label);
            lowest = std::min(lowest, label);
        }
        if (result.labels.empty() || table + 4 * result.labels.size() != lowest) return std::nullopt;
        return result;
    }

    // Start of a leaf function (one with no .pdata entry) holding `address`: the
    // nearest bl target at or before it, unless a .pdata function ends in between.
    uint32_t LeafStart(uint32_t address) const {
        uint32_t floor = 0;
        auto prev = std::upper_bound(functions_.begin(), functions_.end(), address,
                                     [](uint32_t a, const Function& f) { return a < f.begin; });
        if (prev != functions_.begin()) floor = std::prev(prev)->begin + std::prev(prev)->size;
        auto call = std::upper_bound(callTargets_.begin(), callTargets_.end(), address);
        if (call != callTargets_.begin() && *std::prev(call) >= floor) return *std::prev(call);
        uint32_t start = FunctionStart(address);
        return std::max(start, floor);
    }

    // Nearest function entry at or before `address`: an instruction that follows an
    // unconditional end (blr, bctr, b, padding) and that no branch or switch case targets.
    // Blocks inside a function are always reached by a branch; entries are not.
    uint32_t EntryBefore(uint32_t address, const std::unordered_set<uint32_t>& caseLabels) const {
        for (uint32_t a = address; a > address - 0x4000; a -= 4) {
            uint32_t prev = Word(a - 4);
            bool ends = prev == 0 || prev == kBlr || prev == kBctr || ((prev >> 26) == 18 && !(prev & 1));
            if (ends && !std::binary_search(localTargets_.begin(), localTargets_.end(), a) && !caseLabels.count(a))
                return a;
            if (Containing(a - 4)) return a;  // ran into a .pdata function
        }
        return address;
    }

    // Start of the function holding `address`: its .pdata entry, or for leaf
    // functions without one, the first instruction after the previous blr/padding.
    uint32_t FunctionStart(uint32_t address) const {
        if (const Function* f = Containing(address)) return f->begin;
        uint32_t a = address;
        for (const uint8_t* p; (p = At(a - 4)) && Be32(p) != kBlr && Be32(p) != 0; a -= 4) {}
        return a;
    }

    // Size of a leaf function, found by following its control flow (including every
    // switch case) from `start`, without crossing into the next .pdata function.
    uint32_t LeafExtent(uint32_t start, const std::unordered_map<uint32_t, const SwitchTable*>& tables) const {
        auto next = std::upper_bound(functions_.begin(), functions_.end(), start,
                                     [](uint32_t a, const Function& f) { return a < f.begin; });
        uint32_t limit = std::min<uint32_t>(next != functions_.end() ? next->begin : UINT32_MAX, start + 0x10000);
        std::vector<uint32_t> work{start};
        std::unordered_set<uint32_t> seen;
        uint32_t end = start;
        while (!work.empty()) {
            uint32_t a = work.back();
            work.pop_back();
            for (; a >= start && a < limit && seen.insert(a).second; a += 4) {
                end = std::max(end, a + 4);
                uint32_t w = Word(a), op = w >> 26;
                bool always = ((w >> 21) & 0x14) == 0x14;
                if (w == 0) break;
                if (op == 18) {  // b / bl
                    if (w & 1) continue;
                    uint32_t target = (w & 2) ? BranchDisplacement(w) : a + BranchDisplacement(w);
                    if (!IsKnownFunction(target)) work.push_back(target);  // otherwise a tail call
                    break;
                }
                if (op == 16) {  // bc
                    if (!(w & 1)) work.push_back(a + uint32_t(int32_t(int16_t(w & 0xFFFC))));
                    if (always) break;
                    continue;
                }
                if (op == 19 && ((w >> 1) & 0x3FF) == 528) {  // bcctr
                    if (auto it = tables.find(a); it != tables.end())
                        work.insert(work.end(), it->second->labels.begin(), it->second->labels.end());
                    if (always) break;
                } else if (op == 19 && ((w >> 1) & 0x3FF) == 16 && !(w & 1) && always) {  // blr
                    break;
                }
            }
        }
        return end - start;
    }

    // ---- setjmp / longjmp ----

    struct BufferAccess {
        uint32_t function;
        uint32_t offset;  // of r14's slot
    };

    // Functions containing `<op> r14,d(rA); <op> r15,d+8(rA)` with rA != r1: the
    // non-volatile GPRs going to or from a buffer instead of the stack. `opcode` is
    // the primary opcode (62 = std, 58 = ld); base/offset of -1 match anything.
    std::vector<BufferAccess> FindRegisterBuffer(uint32_t opcode, int onlyBase, int onlyOffset) const {
        std::vector<BufferAccess> hits;
        ForEachWord([&](uint32_t address, uint32_t word) {
            if ((word >> 26) != opcode || ((word >> 21) & 31) != 14 || (word & 3)) return;  // not ldu/stdu
            uint32_t base = (word >> 16) & 31;
            uint32_t offset = word & 0xFFFC;
            if (base == 1 || (onlyBase >= 0 && base != uint32_t(onlyBase)) ||
                (onlyOffset >= 0 && offset != uint32_t(onlyOffset)))
                return;
            const uint8_t* next = At(address + 4);
            uint32_t expected = opcode << 26 | 15u << 21 | base << 16 | ((offset + 8) & 0xFFFF);
            if (!next || Be32(next) != expected) return;
            uint32_t start = FunctionStart(address);
            if (std::none_of(hits.begin(), hits.end(), [&](const BufferAccess& h) { return h.function == start; }))
                hits.push_back({start, offset});
        });
        return hits;
    }

    void Report(const char* key, const std::vector<uint32_t>& candidates, const char* why) const {
        if (candidates.size() == 1) {
            std::printf("%s = 0x%08X   # %s\n", key, candidates[0], why);
            PrintCode(candidates[0], 6);
            return;
        }
        std::printf("# %s = ?   # %zu candidates (%s): CHECK\n", key, candidates.size(), why);
        for (uint32_t c : candidates) std::printf("#   0x%08X\n", c);
    }

    void FindJumpBuffers() const {
        std::vector<uint32_t> unwinders;  // functions that call RtlUnwind
        for (const Symbol& s : image_.symbols) {
            if (s.name != "__imp__RtlUnwind") continue;
            if (auto it = calls_.find(uint32_t(s.address)); it != calls_.end())
                for (uint32_t site : it->second)
                    if (uint32_t f = FunctionStart(site); std::find(unwinders.begin(), unwinders.end(), f) == unwinders.end())
                        unwinders.push_back(f);
        }
        // setjmp stores the GPRs into the buffer passed in r3. longjmp reloads them from
        // the same slots, through whichever register it copied the buffer pointer to.
        std::vector<BufferAccess> savers = FindRegisterBuffer(62, 3, -1);
        std::vector<uint32_t> setjmps, longjmps;
        for (const BufferAccess& s : savers) setjmps.push_back(s.function);
        Report("setjmp_address", setjmps, "stores r14, r15... into the buffer at r3");
        if (savers.size() == 1) {
            for (const BufferAccess& l : FindRegisterBuffer(58, -1, int(savers[0].offset))) longjmps.push_back(l.function);
            char why[96];
            std::snprintf(why, sizeof why, "reloads r14, r15... from setjmp's slots (offset %u)", savers[0].offset);
            Report("longjmp_address", longjmps, why);
        } else {
            std::printf("# longjmp_address = ?   # resolve setjmp first\n");
        }
        if (!unwinders.empty()) {
            std::printf("# functions calling RtlUnwind:");
            for (uint32_t f : unwinders) std::printf(" 0x%08X", f);
            std::printf("\n");
        }
    }

    const Image& image_;
    std::vector<const Section*> code_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> calls_;
    std::vector<uint32_t> callTargets_;    // sorted bl targets
    std::vector<uint32_t> branchTargets_;  // sorted b and bl targets
    std::vector<uint32_t> localTargets_;   // sorted b and bc targets (jumps within functions, tail calls)
    size_t callSites_ = 0;
    std::vector<Function> functions_;
};

}  // namespace

int main(int argc, char** argv) {
    std::string mode = argc >= 3 ? argv[2] : "";
    bool known = mode == "--dis" || mode == "--find" || mode == "--bctr-shapes" || (mode == "--generate" && argc == 5);
    if (argc != 2 && !known) {
        std::fprintf(stderr, "usage: xexscan <default.xex> [--dis <address> [count] | --find <word> [count] | "
                             "--bctr-shapes [window] | --generate <tables.toml> <main.toml>]\n");
        return 2;
    }
    std::vector<uint8_t> file = LoadFile(argv[1]);
    if (file.empty()) {
        std::fprintf(stderr, "xexscan: can't read %s\n", argv[1]);
        return 1;
    }
    Image image = Image::ParseImage(file.data(), file.size());
    if (!image.data) {
        std::fprintf(stderr, "xexscan: %s is not a loadable XEX2 image\n", argv[1]);
        return 1;
    }
    Scanner scanner(image);
    uint32_t value = argc >= 4 ? uint32_t(std::strtoul(argv[3], nullptr, 16)) : 0;
    if (mode == "--dis")
        scanner.Disassemble(value, argc > 4 ? std::atoi(argv[4]) : 32);
    else if (mode == "--find")
        scanner.Find(value, argc > 4 ? std::atoi(argv[4]) : 20);
    else if (mode == "--bctr-shapes")
        scanner.BctrShapes(argc > 3 ? std::atoi(argv[3]) : 6);
    else if (mode == "--generate")
        return scanner.WriteGenerated(argv[3], argv[4]) ? 0 : 1;
    else
        scanner.Run(argv[1]);
    return 0;
}
