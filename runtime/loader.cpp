#include "loader.h"

#include <file.h>
#include <image.h>
#include <ppc_config.h>
#include <ppc_context.h>

#include <cstring>
#include <unordered_map>

namespace s3n {
namespace {

// Export ordinal -> name for the two system modules, from XenonUtils' export tables.
enum ExportType { kFunction, kVariable };
struct Export {
    const char* name;
    ExportType type;
};
#define XE_EXPORT(module, ordinal, name, type) { ordinal, Export{#name, type} }
const std::unordered_map<uint32_t, Export> kKernelExports = {
#include "xbox/xboxkrnl_table.inc"
};
const std::unordered_map<uint32_t, Export> kXamExports = {
#include "xbox/xam_table.inc"
};
#undef XE_EXPORT

// XenonUtils replaces each function import's 16-byte thunk with nop, nop, nop, blr and
// names it __imp__<Name>; the recompiled code calls that symbol directly.
bool IsPatchedThunk(const uint8_t* base, uint32_t address) {
    return LoadBE32(base, address) == 0x60000000 && LoadBE32(base, address + 4) == 0x60000000 &&
           LoadBE32(base, address + 8) == 0x60000000 && LoadBE32(base, address + 12) == 0x4E800020;
}

}  // namespace

bool LoadGame(GuestMemory& memory, const std::filesystem::path& xexPath, LoadedGame& game, std::string& error) {
    std::vector<uint8_t> file = LoadFile(xexPath);
    if (file.empty()) {
        error = "can't read " + xexPath.string();
        return false;
    }
    uint32_t headerSize = XexHeaderSize(file.data(), file.size());
    if (!headerSize || headerSize > file.size() || !ParseXexHeaders(file.data(), headerSize, game.xex, &error))
        return error.empty() ? (error = "not an XEX2 executable", false) : false;
    std::vector<XexImportLibrary> imports;
    if (!ParseXexImports(file.data(), headerSize, imports)) {
        error = "corrupt import table";
        return false;
    }

    Image image = Image::ParseImage(file.data(), file.size());
    if (!image.data) {
        error = "XenonUtils couldn't load the image";
        return false;
    }
    if (image.base != PPC_IMAGE_BASE || image.size > PPC_IMAGE_SIZE) {
        error = "this default.xex doesn't match the recompiled code (different image base or size)";
        return false;
    }
    uint8_t* base = memory.base();
    if (!memory.Commit(uint32_t(image.base), image.size)) {
        error = "can't commit guest memory for the image";
        return false;
    }
    std::memcpy(base + image.base, image.data.get(), image.size);
    game.entryPoint = uint32_t(image.entry_point);

    // Indirect calls look up host functions in a table that sits right after the image.
    if (!memory.Commit(uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE), uint32_t(PPC_CODE_SIZE * 2))) {
        error = "can't commit the indirect-call table";
        return false;
    }
    for (const PPCFuncMapping* m = PPCFuncMappings; m->guest; ++m, ++game.functionsMapped)
        PPC_LOOKUP_FUNC(base, m->guest) = m->host;

    // Import slots. XenonUtils byte-swapped every record in place and overwrote function
    // thunks; what's left in a variable slot is its record in host order.
    std::unordered_map<std::string, uint32_t> thunks;  // "__imp__Name" -> thunk address
    for (const Symbol& s : image.symbols)
        if (s.name.rfind("__imp__", 0) == 0) thunks.emplace(s.name, uint32_t(s.address));

    for (const XexImportLibrary& library : imports) {
        const auto& exports = library.name == "xam.xex" ? kXamExports : kKernelExports;
        for (uint32_t slot : library.records) {
            if (IsPatchedThunk(base, slot)) {
                ++game.functionImports;
                continue;
            }
            uint32_t ordinal = *reinterpret_cast<const uint32_t*>(base + slot) & 0xFFFF;
            auto e = exports.find(ordinal);
            std::string name = e != exports.end() ? e->second.name : library.name + "#" + std::to_string(ordinal);

            // A function's address slot: point it at the function's thunk.
            if (auto t = thunks.find("__imp__" + name); t != thunks.end()) {
                StoreBE32(base, slot, t->second);
                continue;
            }
            // A kernel variable: zeroed storage until the kernel defines it.
            uint32_t storage = memory.AllocSystem(0x1000, 0x1000);
            if (!storage) {
                error = "out of system memory for kernel variables";
                return false;
            }
            StoreBE32(base, slot, storage);
            game.variables.push_back(name);
        }
    }
    return true;
}

}  // namespace s3n
