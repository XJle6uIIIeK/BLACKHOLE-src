#pragma once
#include "../SDK/UserCmd.h"

namespace TickbaseV2 {
    
    // Main tickbase functions
    void initialize(UserCmd* cmd) noexcept;
    void process(UserCmd* cmd, bool sendPacket) noexcept;
    void finalize() noexcept;
    
    // Getters
    int getShiftAmount() noexcept;
    int getAvailableTicks() noexcept;
    bool isCurrentlyShifting() noexcept;
    bool isCurrentlyRecharging() noexcept;
    float getRechargeProgress() noexcept;
    
    // Tickbase correction for animations
    int getCorrectTickbase(int commandNumber) noexcept;
    
    // Lifecycle
    void reset() noexcept;
}
