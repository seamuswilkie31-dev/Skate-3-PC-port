// Kernel variables the title imports. The loader gives each one zeroed storage; the ones
// the title needs filled in get their contents here.

#include "kernel/kernel.h"
#include "memory.h"

namespace s3n::kernel {

void DefineVariable(const std::string& name, uint32_t address) {
    uint8_t* base = Memory().base();
    if (name == "KeTimeStampBundle") {
        SetTimeStampBundle(address);
    } else if (name == "VdHSIOCalibrationLock") {
        InitializeCriticalSection(base, address, 0);  // the title takes it around EDRAM retraining
    } else if (name == "XboxHardwareInfo") {
        base[address + 4] = 6;  // +0 flags, +4 number of hardware threads
    }
}

}  // namespace s3n::kernel
