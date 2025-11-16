// BLACKHOLE Enhanced Tickbase Manipulation v2.0
// Advanced doubletap/hideshots with defensive capabilities
// Optimized for CS:GO HvH competitive play

#include "../Config.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "../includes.hpp"
#include "Tickbase.h"
#include "AntiAim.h"
#include "../SDK/ClientState.h"
#include "../SDK/Entity.h"
#include "../SDK/UserCmd.h"
#include "../SDK/NetworkChannel.h"
#include "../SDK/Input.h"
#include "../xor.h"

#include <algorithm>
#include <cmath>

namespace TickbaseV2 {

// ============================================================================
// TICKBASE STATE
// ============================================================================

struct TickbaseState {
    // Shift tracking
    int ticksToShift = 0;
    int ticksShifted = 0;
    int maxShiftAmount = 14;
    
    // Recharge tracking
    int ticksAllowed = 0;
    int ticksPaused = 0;
    float lastRechargeTime = 0.0f;
    
    // Command tracking
    int shiftCommandNumber = 0;
    float realTimeAtShift = 0.0f;
    
    // State flags
    bool isShifting = false;
    bool isRecharging = false;
    bool canShiftDefensive = false;
    bool hasHadActiveThisLife = false;
    
    // Network
    int lastChokedPackets = 0;
    
    void reset() {
        ticksToShift = 0;
        ticksShifted = 0;
        ticksAllowed = 0;
        ticksPaused = 0;
        lastRechargeTime = 0.0f;
        shiftCommandNumber = 0;
        realTimeAtShift = 0.0f;
        isShifting = false;
        isRecharging = false;
        canShiftDefensive = false;
        hasHadActiveThisLife = false;
        lastChokedPackets = 0;
    }
};

static TickbaseState state{};

// ============================================================================
// TICKBASE UTILITIES
// ============================================================================

// Calculate optimal shift amount based on server settings
int calculateOptimalShift(bool isDoubletap) noexcept
{
    static auto sv_maxusrcmdprocessticks = interfaces->cvar->findVar(skCrypt("sv_maxusrcmdprocessticks"));
    
    int maxTicks = sv_maxusrcmdprocessticks ? sv_maxusrcmdprocessticks->getInt() : 16;
    
    // Valve servers have lower limit
    if ((*memory->gameRules)->isValveDS())
        maxTicks = std::min(maxTicks, 6);
    
    if (isDoubletap)
    {
        // Doubletap: maximum safe shift (14 ticks for non-valve, 6 for valve)
        return std::min(maxTicks - 2, 14);
    }
    else
    {
        // Hideshots: moderate shift (9 ticks for non-valve, 6 for valve)
        return std::min(maxTicks - 1, 9);
    }
}

// Calculate minimum recharge time based on ping/choke
float calculateRechargeTime() noexcept
{
    constexpr float BASE_RECHARGE = 0.24825f;
    
    auto netChannel = interfaces->engine->getNetworkChannel();
    if (!netChannel)
        return BASE_RECHARGE;
    
    // Add latency compensation
    const float latency = netChannel->getLatency(0);
    const float adjustedRecharge = BASE_RECHARGE + latency * 0.5f;
    
    return std::clamp(adjustedRecharge, BASE_RECHARGE, 0.5f);
}

// Check if weapon can be shifted
bool canShiftWeapon(Entity* weapon) noexcept
{
    if (!weapon)
        return false;
    
    const auto weaponId = weapon->itemDefinitionIndex2();
    
    // Blocked weapons
    if (weaponId == WeaponId::Revolver ||      // Timing issues
        weaponId == WeaponId::C4 ||            // Can't shoot
        weapon->isGrenade() ||                 // Throws are weird
        weapon->isKnife())                     // Not needed
    {
        return false;
    }
    
    return true;
}

// ============================================================================
// DEFENSIVE TICKBASE - Break lag compensation
// ============================================================================

bool tryDefensiveTeleport(UserCmd* cmd) noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return false;
    
    // Check if we have defensive opportunity
    static int lastServerTick = 0;
    const int currentServerTick = memory->clientState->m_clock_drift_mgr.m_server_tick;
    
    if (std::abs(currentServerTick - lastServerTick) > 1)
    {
        lastServerTick = currentServerTick;
        state.canShiftDefensive = true;
    }
    else
    {
        state.canShiftDefensive = false;
    }
    
    if (!state.canShiftDefensive)
        return false;
    
    // Only do defensive when peeking
    if (!config->tickbase.defensiveOnPeek)
        return false;
    
    // Shift backwards to break lag compensation
    constexpr int DEFENSIVE_SHIFT = 14;
    
    for (int i = 0; i < DEFENSIVE_SHIFT; ++i)
    {
        ++memory->clientState->netChannel->chokedPackets;
    }
    
    state.canShiftDefensive = false;
    return true;
}

// ============================================================================
// TICKBASE RECHARGE - Smart recharge system
// ============================================================================

bool shouldRecharge() noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return false;
    
    // Don't recharge during freeze period
    if ((*memory->gameRules)->freezePeriod())
        return false;
    
    // Check if we have enough ticks
    if (state.ticksAllowed >= state.maxShiftAmount)
        return false;
    
    // Check recharge cooldown
    const float rechargeTime = calculateRechargeTime();
    const float timeSinceRecharge = memory->globalVars->realtime - state.lastRechargeTime;
    
    if (timeSinceRecharge < rechargeTime)
        return false;
    
    // Check network conditions
    auto netChannel = interfaces->engine->getNetworkChannel();
    if (netChannel && netChannel->chokedPackets >= state.maxShiftAmount)
        return false;
    
    return true;
}

void performRecharge() noexcept
{
    state.isRecharging = true;
    state.ticksAllowed++;
    state.ticksPaused++;
    
    // Update last recharge time
    state.lastRechargeTime = memory->globalVars->realtime;
    
    // Clamp to max
    state.ticksAllowed = std::min(state.ticksAllowed, state.maxShiftAmount);
}

// ============================================================================
// TICKBASE SHIFT - Execute the shift
// ============================================================================

bool executeShift(UserCmd* cmd, int shiftAmount) noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return false;
    
    // Validate shift amount
    if (shiftAmount <= 0 || shiftAmount > state.ticksAllowed)
        return false;
    
    // Check weapon
    auto weapon = localPlayer->getActiveWeapon();
    if (!canShiftWeapon(weapon))
        return false;
    
    // Check attack timing
    const float shiftTime = (localPlayer->tickBase() - shiftAmount) * memory->globalVars->intervalPerTick;
    
    if (localPlayer->nextAttack() > shiftTime)
        return false;
    
    if (!weapon || weapon->nextPrimaryAttack() > shiftTime)
        return false;
    
    if (!weapon->clip())
        return false;
    
    // Don't shift during non-auto weapon burst
    if (localPlayer->shotsFired() > 0 && !weapon->isFullAuto())
        return false;
    
    // Execute shift
    state.isShifting = true;
    state.ticksShifted = shiftAmount;
    state.shiftCommandNumber = cmd->commandNumber;
    state.realTimeAtShift = memory->globalVars->realtime;
    
    return true;
}

// ============================================================================
// TICKBASE CORRECTION - Adjust tickbase for animations
// ============================================================================

int getCorrectTickbase(int commandNumber) noexcept
{
    if (!localPlayer)
        return 0;
    
    const int baseTick = localPlayer->tickBase();
    
    // During recharge
    if (state.isRecharging && state.ticksPaused > 0)
        return baseTick + state.ticksPaused;
    
    // During shift
    if (commandNumber == state.shiftCommandNumber)
    {
        // First shifted command
        return baseTick - state.ticksShifted + memory->globalVars->m_simticksthisframe + 1;
    }
    else if (commandNumber == state.shiftCommandNumber + 1)
    {
        // Second shifted command (teleport mode)
        if (config->tickbase.teleport)
            return baseTick;
        
        return baseTick + state.ticksShifted - memory->globalVars->m_simticksthisframe + 1;
    }
    
    return baseTick;
}

// ============================================================================
// TICKBASE MAIN API
// ============================================================================

void initialize(UserCmd* cmd) noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
    {
        state.reset();
        return;
    }
    
    // Update state on new life
    static float lastSpawnTime = 0.0f;
    if (lastSpawnTime != localPlayer->spawnTime())
    {
        lastSpawnTime = localPlayer->spawnTime();
        state.reset();
    }
    
    // Check if feature is active
    const bool isDTActive = config->tickbase.doubletap.isActive();
    const bool isHSActive = config->tickbase.hideshots.isActive();
    
    if (!isDTActive && !isHSActive)
    {
        // Force shift off any remaining ticks
        if (state.hasHadActiveThisLife && state.ticksShifted > 0)
        {
            executeShift(cmd, state.ticksAllowed, true);
        }
        
        state.hasHadActiveThisLife = false;
        return;
    }
    
    state.hasHadActiveThisLife = true;
    
    // Calculate optimal shift
    state.maxShiftAmount = calculateOptimalShift(isDTActive);
    
    // Update network state
    auto netChannel = interfaces->engine->getNetworkChannel();
    if (netChannel)
    {
        if (netChannel->chokedPackets > state.lastChokedPackets)
            state.lastChokedPackets = netChannel->chokedPackets;
    }
    
    // Try defensive teleport
    if (isDTActive && config->tickbase.defensive)
    {
        tryDefensiveTeleport(cmd);
    }
}

void process(UserCmd* cmd, bool sendPacket) noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return;
    
    // Reset shift state
    state.isShifting = false;
    
    // Perform recharge if needed
    if (shouldRecharge())
    {
        performRecharge();
        return; // Skip this tick for recharge
    }
    
    state.isRecharging = false;
    state.ticksPaused = 0;
    
    // Check if we should shift
    auto weapon = localPlayer->getActiveWeapon();
    if (!weapon || !canShiftWeapon(weapon))
        return;
    
    const bool shouldShift = 
        (config->tickbase.doubletap.isActive() && (cmd->buttons & UserCmd::IN_ATTACK)) ||
        (config->tickbase.hideshots.isActive() && (cmd->buttons & UserCmd::IN_ATTACK)) ||
        (weapon->isKnife() && (cmd->buttons & UserCmd::IN_ATTACK2));
    
    if (shouldShift)
    {
        executeShift(cmd, state.maxShiftAmount);
    }
}

void finalize() noexcept
{
    // Cleanup after shift
    if (state.ticksShifted > 0)
    {
        // Deduct shifted ticks from available
        if (config->tickbase.teleport && config->tickbase.doubletap.isActive())
        {
            state.ticksAllowed = std::max(state.ticksAllowed - state.ticksShifted, 0);
        }
        
        state.ticksShifted = 0;
    }
}

// ============================================================================
// TICKBASE GETTERS
// ============================================================================

int getShiftAmount() noexcept
{
    return state.ticksShifted;
}

int getAvailableTicks() noexcept
{
    return state.ticksAllowed;
}

bool isCurrentlyShifting() noexcept
{
    return state.isShifting;
}

bool isCurrentlyRecharging() noexcept
{
    return state.isRecharging;
}

float getRechargeProgress() noexcept
{
    if (state.ticksAllowed >= state.maxShiftAmount)
        return 1.0f;
    
    const float rechargeTime = calculateRechargeTime();
    const float elapsed = memory->globalVars->realtime - state.lastRechargeTime;
    
    return std::clamp(elapsed / rechargeTime, 0.0f, 1.0f);
}

void reset() noexcept
{
    state.reset();
}

} // namespace TickbaseV2
