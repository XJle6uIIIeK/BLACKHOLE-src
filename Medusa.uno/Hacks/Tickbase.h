#pragma once

#include "../SDK/Vector.h"
#include <array>

struct UserCmd;
class Entity;

namespace Tickbase {

    // ============================================
    // CONSTANTS
    // ============================================

    constexpr int MAX_USERCMD_PROCESS_TICKS = 17;
    constexpr int MAX_SHIFT_VALVE = 6;
    constexpr int MAX_SHIFT_COMMUNITY = 13;
    constexpr int MAX_SHIFT_HIDESHOTS = 9;
    constexpr float RECHARGE_COOLDOWN = 0.24825f;
    constexpr float SHOT_COOLDOWN = 1.0f;
    constexpr float TOLERANCE = 0.03125f;

    // ============================================
    // ENUMS
    // ============================================

    enum class Mode {
        NONE,
        DOUBLETAP,
        HIDESHOTS
    };

    enum class State {
        IDLE,
        RECHARGING,
        READY,
        SHIFTING,
        COOLDOWN
    };

    // ============================================
    // STRUCTURES
    // ============================================

    struct TickbaseData {
        // Current state
        State state = State::IDLE;
        Mode mode = Mode::NONE;

        // Tick management
        int ticksCharged = 0;
        int targetShift = 0;
        int currentShift = 0;
        int shiftCommand = 0;
        int pauseTicks = 0;
        int chokedPackets = 0;

        // Timing
        float lastShiftTime = 0.f;
        float lastShotTime = 0.f;
        float rechargeStartTime = 0.f;

        // Flags
        bool isActive = false;
        bool isShifting = false;
        bool isFinalTick = false;
        bool shouldRecharge = false;
        bool wasActive = false;

        // Server info
        int serverMaxTicks = MAX_USERCMD_PROCESS_TICKS;
        bool isValveServer = false;

        void reset() noexcept {
            state = State::IDLE;
            mode = Mode::NONE;
            ticksCharged = 0;
            targetShift = 0;
            currentShift = 0;
            shiftCommand = 0;
            pauseTicks = 0;
            chokedPackets = 0;
            lastShiftTime = 0.f;
            lastShotTime = 0.f;
            rechargeStartTime = 0.f;
            isActive = false;
            isShifting = false;
            isFinalTick = false;
            shouldRecharge = false;
            wasActive = false;
        }
    };

    // ============================================
    // MAIN FUNCTIONS
    // ============================================

    // Initialization
    void reset() noexcept;
    void updateInput() noexcept;

    // Main tick processing
    void start(UserCmd* cmd) noexcept;
    void end(UserCmd* cmd, bool sendPacket) noexcept;

    // Shift operations
    bool shiftOffensive(UserCmd* cmd, int shiftAmount, bool force = false) noexcept;
    bool shiftDefensive(UserCmd* cmd, int shiftAmount) noexcept;
    bool shiftHideShots(UserCmd* cmd, int shiftAmount) noexcept;

    // Recharge management
    bool canRun() noexcept;
    void updateRecharge() noexcept;

    // Validation
    bool canShift(int amount) noexcept;
    bool canShiftDT(int amount, bool force = false) noexcept;
    bool canShiftHS(int amount, bool force = false) noexcept;
    bool isWeaponAllowed() noexcept;

    // Tickbase calculation
    int getCorrectTickbase(int commandNumber) noexcept;
    int adjustTickbase(int oldCmds, int totalCmds, int delta) noexcept;
    int calculateCorrectionTicks() noexcept;

    // Break lag compensation
    void breakLagComp(UserCmd* cmd, int amount) noexcept;

    // ============================================
    // GETTERS
    // ============================================

    int getTargetShift() noexcept;
    int getCurrentShift() noexcept;
    int getTicksCharged() noexcept;
    int& pausedTicks() noexcept;

    bool isRecharging() noexcept;
    bool& isShifting() noexcept;
    bool& isFinalTick() noexcept;
    bool isReady() noexcept;

    State getState() noexcept;
    Mode getMode() noexcept;

    // ============================================
    // SETTERS
    // ============================================

    void resetShift() noexcept;
    void setLastShotTime(float time) noexcept;
    void getCmd(UserCmd* cmd) noexcept;

    // ============================================
    // GLOBAL DATA
    // ============================================

    inline TickbaseData data{};
    inline UserCmd* currentCmd = nullptr;
    inline bool sendPacket = true;
}