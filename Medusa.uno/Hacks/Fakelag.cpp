#include "Fakelag.h"
#include "EnginePrediction.h"
#include "Tickbase.h"
#include "AntiAim.h"
#include "Animations.h"
#include "../Config.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "../SDK/Entity.h"
#include "../SDK/UserCmd.h"
#include "../SDK/NetworkChannel.h"
#include "../SDK/LocalPlayer.h"
#include "../SDK/GlobalVars.h"
#include "../GameData.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <random>

// ============================================
// HELPER FUNCTIONS
// ============================================

namespace {
    // Random number generator
    std::mt19937 rng{ std::random_device{}() };

    float getServerTime() noexcept {
        if (!localPlayer)
            return 0.f;
        return static_cast<float>(localPlayer->tickBase()) * memory->globalVars->intervalPerTick;
    }

    float getCurrentTime() noexcept {
        return memory->globalVars->realtime;
    }

    bool isOnGround() noexcept {
        if (!localPlayer)
            return false;
        return (localPlayer->flags() & FL_ONGROUND) != 0;
    }

    float getSpeed() noexcept {
        return EnginePrediction::getVelocity().length2D();
    }

    bool hasVisibleEnemy() noexcept {
        if (!localPlayer)
            return false;

        const Vector eyePos = localPlayer->getEyePosition();

        for (int i = 1; i <= interfaces->engine->getMaxClients(); i++) {
            const auto entity = interfaces->entityList->getEntity(i);
            if (!entity || entity == localPlayer.get())
                continue;

            if (entity->isDormant() || !entity->isAlive())
                continue;

            if (!entity->isOtherEnemy(localPlayer.get()))
                continue;

            // Simple visibility check to chest
            const auto& player = Animations::getPlayer(i);
            if (!player.gotMatrix)
                continue;

            const Vector chestPos = player.matrix[6].origin();

            Trace trace;
            interfaces->engineTrace->traceRay(
                { eyePos, chestPos },
                0x46004009,
                localPlayer.get(),
                trace
            );

            if (trace.entity == entity || trace.fraction >= 0.97f)
                return true;
        }

        return false;
    }

    int getClosestEnemyIndex() noexcept {
        if (!localPlayer)
            return -1;

        int closest = -1;
        float closestDist = FLT_MAX;
        const Vector localPos = localPlayer->getAbsOrigin();

        for (int i = 1; i <= interfaces->engine->getMaxClients(); i++) {
            const auto entity = interfaces->entityList->getEntity(i);
            if (!entity || entity == localPlayer.get())
                continue;

            if (entity->isDormant() || !entity->isAlive())
                continue;

            if (!entity->isOtherEnemy(localPlayer.get()))
                continue;

            const float dist = localPos.distTo(entity->getAbsOrigin());
            if (dist < closestDist) {
                closestDist = dist;
                closest = i;
            }
        }

        return closest;
    }

    float lastGrenadeActionTime = 0.f;
    constexpr float GRENADE_FAKELAG_BLOCK_TIME = 0.3f;
}


// ============================================
// INITIALIZATION
// ============================================

void Fakelag::reset() noexcept {
    data.reset();
    peekData = PeekData{};
    lastGrenadeActionTime = 0.f;
}

void Fakelag::update() noexcept {
    if (!localPlayer || !localPlayer->isAlive()) {
        reset();
        return;
    }

    // Update last origin
    const Vector currentOrigin = localPlayer->getAbsOrigin();

    if (data.lastOrigin.null()) {
        data.lastOrigin = currentOrigin;
        data.lastSentOrigin = currentOrigin;
    }

    // Update peek detection
    updatePeekDetection();

    // Update random seed periodically
    if (memory->globalVars->tickCount % 64 == 0) {
        data.randomSeed = static_cast<unsigned int>(std::time(nullptr));
    }
}

// ============================================
// PEEK DETECTION
// ============================================

void Fakelag::updatePeekDetection() noexcept {
    if (!localPlayer || !localPlayer->isAlive()) {
        peekData.isPeeking = false;
        return;
    }

    const Vector currentPos = localPlayer->getAbsOrigin();
    const float speed = getSpeed();

    // Check if we have a visible enemy now
    const bool hasVisible = hasVisibleEnemy();

    // Detect peek start
    if (!peekData.isPeeking && speed > 50.f && !peekData.wasVisible && hasVisible) {
        peekData.isPeeking = true;
        peekData.startPosition = currentPos;
        peekData.peekStartTime = getCurrentTime();
        peekData.targetIndex = getClosestEnemyIndex();

        // Calculate peek direction
        if (peekData.targetIndex > 0) {
            const auto target = interfaces->entityList->getEntity(peekData.targetIndex);
            if (target) {
                peekData.peekDirection = (target->getAbsOrigin() - currentPos).normalized();
            }
        }
    }

    // Detect peek end
    if (peekData.isPeeking) {
        const float peekTime = getCurrentTime() - peekData.peekStartTime;
        const float peekDistance = currentPos.distTo(peekData.startPosition);

        // End peek if:
        // - Took too long (> 1 second)
        // - Moved too far (> 200 units)
        // - Stopped moving
        // - Lost visibility for too long
        if (peekTime > 1.f || peekDistance > 200.f || speed < 10.f) {
            peekData.isPeeking = false;
        }
    }

    peekData.wasVisible = hasVisible;
}

bool Fakelag::detectPeek() noexcept {
    return peekData.isPeeking;
}

bool Fakelag::isCurrentlyPeeking() noexcept {
    return peekData.isPeeking;
}

// ============================================
// CHOKE CALCULATIONS
// ============================================

int Fakelag::calculateStaticChoke() noexcept {
    return config->fakelag.limit;
}

int Fakelag::calculateAdaptiveChoke(float speed) noexcept {
    if (speed < MIN_SPEED_FOR_FAKELAG)
        return MIN_CHOKE;

    // Calculate ticks needed to move 64 units (break lag compensation)
    // distance = speed * time
    // time = ticks * intervalPerTick
    // ticks = distance / (speed * intervalPerTick)

    const float intervalPerTick = memory->globalVars->intervalPerTick;
    if (intervalPerTick <= 0.f)
        return config->fakelag.limit;

    const int ticksNeeded = static_cast<int>(std::ceilf(
        ADAPTIVE_BASE_DISTANCE / (speed * intervalPerTick)
    ));

    return std::clamp(ticksNeeded, MIN_CHOKE, config->fakelag.limit);
}

int Fakelag::calculateRandomChoke() noexcept {
    // Use seeded random for more consistent behavior
    std::uniform_int_distribution<int> dist(1, config->fakelag.limit);
    return dist(rng);
}

int Fakelag::calculateBreakLCChoke() noexcept {
    // Break lag compensation pattern
    // Alternate between high choke and low choke

    const int tickCount = memory->globalVars->tickCount;
    const int cycle = tickCount % 34; // ~0.5 second cycle at 64 tick

    // High choke for most of the cycle, low choke briefly
    if (cycle >= 4) {
        return config->fakelag.limit;
    }

    return MIN_CHOKE;
}

int Fakelag::calculatePeekChoke() noexcept {
    // When peeking, use maximum choke to hide movement
    if (peekData.isPeeking) {
        const float peekTime = getCurrentTime() - peekData.peekStartTime;

        // First 0.3 seconds of peek - max choke
        if (peekTime < 0.3f) {
            return config->fakelag.limit;
        }

        // After initial peek, reduce to allow shooting
        return (std::max)(MIN_CHOKE, config->fakelag.limit / 2);
    }

    // Not peeking - use adaptive
    return calculateAdaptiveChoke(getSpeed());
}

int Fakelag::calculateLegitChoke(float speed) noexcept {
    // Legit-looking fakelag that mimics network jitter

    // Base choke on speed
    int baseChoke = 1;

    if (speed > 200.f) {
        baseChoke = 2;
    }
    else if (speed > 100.f) {
        baseChoke = 1;
    }

    // Add small random variation (1-2 ticks)
    std::uniform_int_distribution<int> variation(0, 1);
    baseChoke += variation(rng);

    return std::clamp(baseChoke, 1, 3);
}

// ============================================
// HELPER FUNCTIONS
// ============================================

bool Fakelag::shouldChoke() noexcept {
    if (!localPlayer || !localPlayer->isAlive())
        return false;

    // Don't choke if AA is disabled
    if (!config->condAA.global)
        return false;

    // Don't choke if we just shot
    if (AntiAim::getDidShoot())
        return false;

    // Check trigger conditions
    const float speed = getSpeed();
    const bool onGround = isOnGround();
    const bool isPeeking = isCurrentlyPeeking();

    // Based on trigger setting (if implemented in config)
    // For now, always allow if moving
    if (speed < MIN_SPEED_FOR_FAKELAG && !isPeeking)
        return false;

    return true;
}

bool Fakelag::canChoke() noexcept {
    const auto netChannel = interfaces->engine->getNetworkChannel();
    if (!netChannel)
        return false;


    if (config->tickbase.doubletap.isActive() && !Tickbase::isReady())
        return false;

    // Check tickbase constraints
    const int maxAllowed = getMaxChoke();
    if (netChannel->chokedPackets >= maxAllowed)
        return false;

    return true;
}

bool Fakelag::isBreakingLC() noexcept {
    if (!localPlayer)
        return false;

    // Check if we've moved enough to break lag compensation
    const Vector currentOrigin = localPlayer->getAbsOrigin();
    const float distance = currentOrigin.distTo(data.lastSentOrigin);

    return distance >= BREAK_LC_DISTANCE;
}

float Fakelag::getDistanceMoved() noexcept {
    if (!localPlayer)
        return 0.f;

    return localPlayer->getAbsOrigin().distTo(data.lastSentOrigin);
}

int Fakelag::getMaxChoke() noexcept {
    int maxChoke = MAX_CHOKE;

    // Account for tickbase
    if (Tickbase::isReady()) {
        const int tickbaseShift = Tickbase::getTargetShift();
        maxChoke = (std::max)(1, maxUserCmdProcessTicks - tickbaseShift - 1);
    }

    // Account for fakeduck
    if (config->misc.fakeduck && config->misc.fakeduckKey.isActive()) {
        maxChoke = (std::min)(maxChoke, 14); // Fakeduck needs specific choke
    }

    return maxChoke;
}

// ============================================
// MAIN RUN FUNCTION
// ============================================

void Fakelag::run(bool& sendPacket) noexcept {
    update();

    if (!localPlayer || !localPlayer->isAlive()) {
        sendPacket = true;
        return;
    }

    if (!config->condAA.global) {
        sendPacket = true;
        return;
    }

    const auto netChannel = interfaces->engine->getNetworkChannel();
    if (!netChannel) {
        sendPacket = true;
        return;
    }

    // === НОВОЕ: Обработка гранат ===
    auto* weapon = localPlayer->getActiveWeapon();
    if (weapon && weapon->isGrenade()) {
        // Если граната в процессе броска
        if (weapon->isThrowing()) {
            lastGrenadeActionTime = getCurrentTime();
            // Немедленно отправляем пакет
            sendPacket = true;
            data.lastSentOrigin = localPlayer->getAbsOrigin();
            data.isActive = false;
            return;
        }

        // Если просто держим гранату - минимальный fakelag
        sendPacket = netChannel->chokedPackets >= 1;
        if (sendPacket) {
            data.lastSentOrigin = localPlayer->getAbsOrigin();
        }
        return;
    }

    // === НОВОЕ: Блокировка после броска гранаты ===
    float timeSinceGrenadeAction = getCurrentTime() - lastGrenadeActionTime;
    if (timeSinceGrenadeAction < GRENADE_FAKELAG_BLOCK_TIME) {
        sendPacket = true;
        data.lastSentOrigin = localPlayer->getAbsOrigin();
        data.isActive = false;
        return;
    }

    // Force send if we just shot
    if (AntiAim::getDidShoot()) {
        sendPacket = true;
        data.lastSentOrigin = localPlayer->getAbsOrigin();
        return;
    }

    if (!shouldChoke()) {
        sendPacket = true;
        return;
    }

    // Calculate target choke based on mode
    int targetChoke = MIN_CHOKE;
    const float speed = getSpeed();

    switch (config->fakelag.mode) {
    case 0:
        targetChoke = calculateStaticChoke();
        break;
    case 1:
        targetChoke = calculateAdaptiveChoke(speed);
        break;
    case 2:
        targetChoke = calculateRandomChoke();
        break;
    case 3:
        targetChoke = calculateBreakLCChoke();
        break;
    case 4:
        targetChoke = calculatePeekChoke();
        break;
    case 5:
        targetChoke = calculateLegitChoke(speed);
        break;
    default:
        targetChoke = calculateStaticChoke();
        break;
    }

    // ✅ ИСПРАВЛЕННАЯ логика тикбейза
    if (config->tickbase.doubletap.isActive() || config->tickbase.hideshots.isActive()) {
        // ✅ Проверка готовности перед использованием
        if (!Tickbase::canRun()) {
            // Если тикбейз недоступен - используй минимальный чок
            targetChoke = MIN_CHOKE;
        }
        else {
            const float timeSinceRecharge = getCurrentTime() - Tickbase::data.lastShiftTime;
            const bool canShift = timeSinceRecharge > 0.24625f;

            if (canShift && weapon) {
                const float shiftTime = getServerTime() - ticksToTime(Tickbase::getTargetShift());

                if (weapon->nextPrimaryAttack() <= shiftTime) {
                    if (!(config->misc.fakeduck && config->misc.fakeduckKey.isActive())) {
                        targetChoke = MIN_CHOKE;
                    }
                }
            }
        }
    }

    const int maxChoke = getMaxChoke();
    targetChoke = std::clamp(targetChoke, MIN_CHOKE, maxChoke);

    data.targetChoke = targetChoke;
    data.chokedPackets = netChannel->chokedPackets;
    data.isActive = true;

    sendPacket = netChannel->chokedPackets >= targetChoke;

    if (sendPacket) {
        data.lastSentOrigin = localPlayer->getAbsOrigin();
        data.lastChoke = netChannel->chokedPackets;
    }

    data.lastOrigin = localPlayer->getAbsOrigin();
}

// ============================================
// GETTERS
// ============================================

int Fakelag::getChokedPackets() noexcept {
    return data.chokedPackets;
}

int Fakelag::getTargetChoke() noexcept {
    return data.targetChoke;
}

const Fakelag::FakelagData& Fakelag::getData() noexcept {
    return data;
}

const Fakelag::PeekData& Fakelag::getPeekData() noexcept {
    return peekData;
}