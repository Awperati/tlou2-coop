#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace coop {

// Hardware data watchpoints (debug registers) on every thread of the game, for research: which
// code reads or writes an address. Arm from the mod's own thread (it is skipped). Hits are counted
// per instruction in the exception handler (no logging there); HwWatchReport() disarms and returns
// one line per slot and instruction (rva, count, up to 3 return addresses found on the stack).
struct HwWatchSlot {
    uintptr_t addr = 0;
    uint8_t len = 8;        // 1, 2, 4 or 8 (addr aligned to it)
    bool writeOnly = false; // false: reads and writes
    std::string name;
};
bool HwWatchArm(const std::vector<HwWatchSlot>& slots, std::string& err);
bool HwWatchArmed();
std::vector<std::string> HwWatchReport();   // disarms
void HwWatchStop();                         // disarms without a report (unload)

} // namespace coop
