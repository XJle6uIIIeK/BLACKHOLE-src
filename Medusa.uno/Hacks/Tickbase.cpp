#include "Tickbase.h"
#include "AntiAim.h"
#include "EnginePrediction.h"
#include "../Config.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "../SDK/ClientState.h"
#include "../SDK/Entity.h"
#include "../SDK/UserCmd.h"
#include "../SDK/NetworkChannel.h"
#include "../SDK/LocalPlayer.h"
#include "../SDK/Cvar.h"
#include "../SDK/ConVar.h"
#include "../xor.h"
#include <algorithm>
#include <cmath>

// ============================================
// HELPER FUNCTIONS
// ============================================

namespace {
    // Cached ConVars
    ConVar* sv_maxusrcmdprocessticks = nullptr;
    ConVar* sv_clockcorrection_msecs = nullptr;

    void cacheConVars() noexcept {
        if (!sv_maxusrcmdprocessticks)
            sv_maxusrcmdprocessticks = interfaces->cvar->findVar("sv_maxusrcmdprocessticks");

        if (!sv_clockcorrection_msecs)
            sv_clockcorrection_msecs = interfaces->cvar->findVar("sv_clockcorrection_msecs");
    }

    int getServerMaxTicks() noexcept {
        cacheConVars();

        if (sv_maxusrcmdprocessticks)
            return sv_maxusrcmdprocessticks->getInt() - 1;

        return Tickbase::MAX_USERCMD_PROCESS_TICKS - 1;
    }

    bool isValveServer() noexcept {
        if (!memory->gameRules || !*memory->gameRules)
            return false;

        return (*memory->gameRules)->isValveDS();
    }

    float getServerTime() noexcept {
        if (!localPlayer)
            return 0.f;

        return static_cast<float>(localPlayer->tickBase()) * memory->globalVars->intervalPerTick;
    }

    float getCurrentTime() noexcept {
        return memory->globalVars->realtime;
    }

    bool isWeaponBlocked(int weaponId) noexcept {
        switch (static_cast<WeaponId>(weaponId)) {
        case WeaponId::Revolver:
        case WeaponId::C4:
        case WeaponId::Healthshot:
            return true;
        default:
            return false;
        }
    }
}

// ============================================
// INITIALIZATION
// ============================================

void Tickbase::reset() noexcept {
    data.reset();
    currentCmd = nullptr;
    sendPacket = true;
}

void Tickbase::updateInput() noexcept {
    config->tickbase.doubletap.handleToggle();
    config->tickbase.hideshots.handleToggle();
}

void Tickbase::getCmd(UserCmd* cmd) noexcept {
    currentCmd = cmd;
}

// ============================================
// MAIN TICK PROCESSING
// ============================================

void Tickbase::start(UserCmd* cmd) noexcept {
    // Validate local player
    if (!localPlayer || !localPlayer->isAlive()) {
        data.wasActive = data.isActive;
        data.isActive = false;
        data.ticksCharged = 0;
        return;
    }

    // Update server info
    data.serverMaxTicks = getServerMaxTicks();
    data.isValveServer = isValveServer();

    // Track choked packets
    if (auto* netChannel = interfaces->engine->getNetworkChannel()) {
        data.chokedPackets = (std::max)(data.chokedPackets, netChannel->chokedPackets);
    }

    // Get weapon
    auto* weapon = localPlayer->getActiveWeapon();
    if (!weapon) {
        data.wasActive = data.isActive;
        data.isActive = false;
        return;
    }

    // Check if weapon is allowed
    if (weapon->isGrenade() || isWeaponBlocked(weapon->itemDefinitionIndex())) {
        return;
    }

    // Determine mode
    const bool dtActive = config->tickbase.doubletap.isActive();
    const bool hsActive = config->tickbase.hideshots.isActive();

    if (!dtActive && !hsActive) {
        // Deactivated - discharge if was active
        if (data.wasActive && data.ticksCharged > 0) {
            shiftOffensive(cmd, data.ticksCharged, true);
        }

        data.wasActive = data.isActive;
        data.isActive = false;
        data.mode = Mode::NONE;
        data.targetShift = 0;
        return;
    }

    // Set mode and target shift
    if (dtActive) {
        data.mode = Mode::DOUBLETAP;
        data.targetShift = data.isValveServer ? MAX_SHIFT_VALVE : MAX_SHIFT_COMMUNITY;

        // Defensive DT
        if (config->tickbase.defensive_dt) {
            breakLagComp(cmd, data.targetShift);
        }
    }
    else if (hsActive) {
        data.mode = Mode::HIDESHOTS;
        data.targetShift = data.isValveServer ? 6 : MAX_SHIFT_HIDESHOTS;
    }

    // Clamp to server limits
    data.targetShift = std::clamp(data.targetShift, 0, (std::min)(data.serverMaxTicks, MAX_USERCMD_PROCESS_TICKS - 1));

    data.wasActive = data.isActive;
    data.isActive = true;
}

void Tickbase::end(UserCmd* cmd, bool sendPacketState) noexcept {
    sendPacket = sendPacketState;

    if (!localPlayer || !localPlayer->isAlive())
        return;

    auto* weapon = localPlayer->getActiveWeapon();
    if (!weapon)
        return;

    // Check if disabled
    if (!config->tickbase.doubletap.isActive() && !config->tickbase.hideshots.isActive()) {
        data.targetShift = 0;
        return;
    }

    // Doubletap logic
    if (data.mode == Mode::DOUBLETAP) {
        bool shouldShift = false;

        // Knife backstab
        if (weapon->isKnife() && (cmd->buttons & UserCmd::IN_ATTACK2)) {
            shouldShift = true;
        }
        // Normal attack
        else if (cmd->buttons & UserCmd::IN_ATTACK) {
            shouldShift = true;
        }

        if (shouldShift && canShiftDT(data.targetShift, false)) {
            shiftOffensive(cmd, data.targetShift);
        }
    }
    // Hideshots logic
    else if (data.mode == Mode::HIDESHOTS) {
        if ((cmd->buttons & UserCmd::IN_ATTACK) && canShiftHS(data.targetShift, false)) {
            shiftHideShots(cmd, data.targetShift);
        }
    }
}

// ============================================
// SHIFT OPERATIONS
// ============================================

bool Tickbase::shiftOffensive(UserCmd* cmd, int shiftAmount, bool force) noexcept {
    if (!canShiftDT(shiftAmount, force))
        return false;

    auto* weapon = localPlayer->getActiveWeapon();
    if (!weapon || weapon->itemDefinitionIndex2() == WeaponId::Revolver)
        return false;

    // Record shift
    data.lastShiftTime = getCurrentTime();
    data.currentShift = shiftAmount;
    data.shiftCommand = cmd->commandNumber;
    data.isShifting = true;
    data.state = State::SHIFTING;

    return true;
}

bool Tickbase::shiftDefensive(UserCmd* cmd, int shiftAmount) noexcept {
    if (!canShiftDT(shiftAmount, false))
        return false;

    auto* weapon = localPlayer->getActiveWeapon();
    if (!weapon || weapon->itemDefinitionIndex2() == WeaponId::Revolver)
        return false;

    data.lastShiftTime = getCurrentTime();
    data.currentShift = shiftAmount;
    data.shiftCommand = cmd->commandNumber;
    data.isShifting = true;

    // Defensive: increase choked packets
    if (memory->clientState && memory->clientState->netChannel) {
        memory->clientState->netChannel->chokedPackets += shiftAmount;
    }

    return true;
}

bool Tickbase::shiftHideShots(UserCmd* cmd, int shiftAmount) noexcept {
    if (!canShiftHS(shiftAmount, false))
        return false;

    data.currentShift = shiftAmount;
    data.shiftCommand = cmd->commandNumber;
    data.isShifting = true;

    return true;
}

// ============================================
// BREAK LAG COMPENSATION
// ============================================

void Tickbase::breakLagComp(UserCmd* cmd, int amount) noexcept {
    static int defensiveTick = 0;
    static bool canDefensive = false;

    if (!memory->clientState)
        return;

    const int serverTick = memory->clientState->m_clock_drift_mgr.m_server_tick;

    if (std::abs(serverTick - defensiveTick) > 1) {
        defensiveTick = serverTick;

        // Defensive exploit conditions (peeking detection)
        // This would need integration with peek detection system
        canDefensive = false;
        data.targetShift = amount;
    }
    else {
        data.targetShift = amount;
    }
}

// ============================================
// RECHARGE MANAGEMENT
// ============================================

bool Tickbase::canRun() noexcept {
    static float spawnTime = 0.f;

    // Not in game
    if (!interfaces->engine->isInGame() || !interfaces->engine->isConnected()) {
        data.ticksCharged = 0;
        data.chokedPackets = 0;
        data.pauseTicks = 0;
        data.shouldRecharge = false;
        data.state = State::IDLE;
        return true;
    }

    // Invalid player
    if (!localPlayer || !localPlayer->isAlive() || data.targetShift <= 0) {
        data.ticksCharged = 0;
        data.shouldRecharge = false;
        data.state = State::IDLE;
        return true;
    }

    // Freeze period
    if (memory->gameRules && *memory->gameRules && (*memory->gameRules)->freezePeriod()) {
        data.lastShiftTime = getCurrentTime();
        data.state = State::IDLE;
        return true;
    }

    // Spawn reset
    if (spawnTime != localPlayer->spawnTime()) {
        spawnTime = localPlayer->spawnTime();
        data.ticksCharged = 0;
        data.pauseTicks = 0;
        data.shouldRecharge = false;
        data.state = State::IDLE;
    }

    // Fakeduck blocks tickbase
    if (config->misc.fakeduck && config->misc.fakeduckKey.isActive()) {
        data.lastShiftTime = getCurrentTime();
        data.currentShift = 0;
        data.shiftCommand = 0;
        data.shouldRecharge = false;
        data.state = State::IDLE;
        return true;
    }

    // Calculate recharge conditions
    const float timeSinceShift = getCurrentTime() - data.lastShiftTime;
    const float timeSinceShot = getCurrentTime() - data.lastShotTime;

    const bool needsRecharge = (data.ticksCharged < data.targetShift) ||
        (data.chokedPackets > MAX_USERCMD_PROCESS_TICKS - data.targetShift);

    const bool cooldownPassed = timeSinceShift > RECHARGE_COOLDOWN;
    const bool shotCooldownPassed = timeSinceShot > SHOT_COOLDOWN || !data.wasActive;

    // Recharge logic
    if (needsRecharge && cooldownPassed && shotCooldownPassed) {
        data.ticksCharged = (std::min)(data.ticksCharged + 1, MAX_USERCMD_PROCESS_TICKS);
        data.chokedPackets = (std::max)(data.chokedPackets - 1, 0);
        data.pauseTicks++;
        data.shouldRecharge = true;
        data.state = State::RECHARGING;
        return false;
    }

    // Check if fully charged
    if (data.ticksCharged >= data.targetShift) {
        data.shouldRecharge = false;
        data.state = State::READY;
    }
    else {
        data.state = State::COOLDOWN;
    }

    return true;
}

void Tickbase::updateRecharge() noexcept {
    // Called every tick to update recharge state
    if (data.state != State::RECHARGING)
        return;

    const float timeSinceRechargeStart = getCurrentTime() - data.rechargeStartTime;

    // Progressive recharge with decay
    if (timeSinceRechargeStart > 0.1f) {
        data.ticksCharged = (std::min)(data.ticksCharged + 1, data.targetShift);
        data.rechargeStartTime = getCurrentTime();
    }

    // Check if done
    if (data.ticksCharged >= data.targetShift) {
        data.state = State::READY;
        data.shouldRecharge = false;
    }
}

// ============================================
// VALIDATION
// ============================================

bool Tickbase::canShift(int amount) noexcept {
    if (!localPlayer || !localPlayer->isAlive())
        return false;

    if (amount <= 0 || amount > data.ticksCharged)
        return false;

    const float timeSinceShift = getCurrentTime() - data.lastShiftTime;
    if (timeSinceShift <= RECHARGE_COOLDOWN)
        return false;

    return true;
}

bool Tickbase::canShiftDT(int amount, bool force) noexcept {
    if (!canShift(amount) && !force)
        return false;

    if (force)
        return true;

    // Fakeduck blocks
    if (config->misc.fakeduck && config->misc.fakeduckKey.isActive())
        return false;

    auto* weapon = localPlayer->getActiveWeapon();
    if (!weapon || !weapon->clip())
        return false;

    // Blocked weapons
    if (weapon->isGrenade() || weapon->isBomb() || isWeaponBlocked(weapon->itemDefinitionIndex()))
        return false;

    // Timing checks
    const float shiftTime = getServerTime() - (amount * memory->globalVars->intervalPerTick);

    if (localPlayer->nextAttack() > shiftTime)
        return false;

    if (weapon->nextPrimaryAttack() > shiftTime)
        return false;

    // Semi-auto check
    if (localPlayer->shotsFired() > 0 && !weapon->isFullAuto())
        return false;

    return true;
}

bool Tickbase::canShiftHS(int amount, bool force) noexcept {
    // Hideshots doesn't work with DT
    if (config->tickbase.doubletap.isActive())
        return false;

    if (!canShift(amount) && !force)
        return false;

    if (force)
        return true;

    if (config->misc.fakeduck && config->misc.fakeduckKey.isActive())
        return false;

    auto* weapon = localPlayer->getActiveWeapon();
    if (!weapon || !weapon->clip())
        return false;

    if (weapon->isKnife() || weapon->isGrenade() || weapon->isBomb())
        return false;

    const float shiftTime = getServerTime() - (amount * memory->globalVars->intervalPerTick);

    if (localPlayer->nextAttack() > shiftTime)
        return false;

    if (weapon->nextPrimaryAttack() > shiftTime)
        return false;

    if (localPlayer->shotsFired() > 0 && !weapon->isFullAuto())
        return false;

    return true;
}

bool Tickbase::isWeaponAllowed() noexcept {
    if (!localPlayer)
        return false;

    auto* weapon = localPlayer->getActiveWeapon();
    if (!weapon)
        return false;

    return !weapon->isGrenade() && !weapon->isBomb() && !isWeaponBlocked(weapon->itemDefinitionIndex());
}

// ============================================
// TICKBASE CALCULATION
// ============================================

int Tickbase::calculateCorrectionTicks() noexcept {
    cacheConVars();

    if (!sv_clockcorrection_msecs || !memory->globalVars || memory->globalVars->maxClients <= 1)
        return -1;

    const float correctionMs = sv_clockcorrection_msecs->getFloat();
    const float correctionSeconds = std::clamp(correctionMs / 1000.f, 0.f, 1.f);

    return static_cast<int>((correctionSeconds / memory->globalVars->intervalPerTick) + 0.5f);
}

int Tickbase::adjustTickbase(int oldCmds, int totalCmds, int delta) noexcept {
    const int correctionTicks = calculateCorrectionTicks();

    if (correctionTicks == -1)
        return localPlayer->tickBase();

    // Get stored netvar data
    const auto& netvars = EnginePrediction::currentNetvars;

    int result = 0;

    if (netvars.valid && localPlayer) {
        result = netvars.tickbase + 1;

        const int tickCount = result + oldCmds - data.targetShift;
        const int idealFinalTick = tickCount + correctionTicks;
        const int tooFastLimit = idealFinalTick + correctionTicks;
        const int tooSlowLimit = idealFinalTick - correctionTicks;
        const int adjustedFinalTick = result + totalCmds;

        if (adjustedFinalTick > tooFastLimit || adjustedFinalTick < tooSlowLimit) {
            result = idealFinalTick - totalCmds;
        }
    }

    if (result != 0)
        return result;

    // Fallback
    return localPlayer->tickBase() - delta;
}

int Tickbase::getCorrectTickbase(int commandNumber) noexcept {
    if (!localPlayer)
        return 0;

    const int tickBase = localPlayer->tickBase();

    // During recharge
    if (data.state == State::RECHARGING) {
        return tickBase + data.ticksCharged;
    }

    // Update prediction time
    memory->globalVars->currenttime = ticksToTime(tickBase);

    // During shift
    if (commandNumber == data.shiftCommand) {
        return tickBase - data.currentShift + memory->globalVars->m_simticksthisframe + 1;
    }

    // After shift
    if (commandNumber == data.shiftCommand + 1) {
        if (!config->tickbase.teleport) {
            return tickBase + data.currentShift - memory->globalVars->m_simticksthisframe + 1;
        }
        return tickBase;
    }

    // During pause
    if (data.pauseTicks > 0) {
        return tickBase + data.pauseTicks;
    }

    return tickBase;
}

// ============================================
// GETTERS
// ============================================

int Tickbase::getTargetShift() noexcept {
    return data.shouldRecharge ? data.targetShift : 1;
}

int Tickbase::getCurrentShift() noexcept {
    return data.currentShift;
}

int Tickbase::getTicksCharged() noexcept {
    return data.ticksCharged;
}

int& Tickbase::pausedTicks() noexcept {
    return data.pauseTicks;
}

bool Tickbase::isRecharging() noexcept {
    return data.state == State::RECHARGING;
}

bool& Tickbase::isShifting() noexcept {
    return data.isShifting;
}

bool& Tickbase::isFinalTick() noexcept {
    return data.isFinalTick;
}

bool Tickbase::isReady() noexcept {
    return data.state == State::READY && data.ticksCharged >= data.targetShift;
}

Tickbase::State Tickbase::getState() noexcept {
    return data.state;
}

Tickbase::Mode Tickbase::getMode() noexcept {
    return data.mode;
}

// ============================================
// SETTERS
// ============================================

void Tickbase::resetShift() noexcept {
    // Teleport mode: deduct used ticks
    if (config->tickbase.teleport && config->tickbase.doubletap.isActive()) {
        data.ticksCharged = (std::max)(data.ticksCharged - data.currentShift, 0);
    }

    data.currentShift = 0;
    data.isShifting = false;
    data.state = (data.ticksCharged >= data.targetShift) ? State::READY : State::COOLDOWN;
}

void Tickbase::setLastShotTime(float time) noexcept {
    data.lastShotTime = time;
}