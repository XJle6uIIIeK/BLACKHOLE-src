#include "../Interfaces.h"
#include <random>
#include <cmath>
#include "AimbotFunctions.h"
#include "AntiAim.h"

#include "../imgui/imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "../imgui/imgui_internal.h"
#include "../GameData.h"
#include "../Memory.h"
#include "../SDK/Engine.h"
#include "../Netvars.h"
#include "../SDK/Entity.h"
#include "../SDK/EngineTrace.h"
#include "../SDK/EntityList.h"
#include "../SDK/NetworkChannel.h"
#include "../SDK/UserCmd.h"
#include "Tickbase.h"
#include "../Config.h"
#include "../SDK/LocalPlayer.h"

// ============================================
// UTILITY FUNCTIONS
// ============================================

float AntiAim::normalizeYaw(float yaw) noexcept {
    while (yaw > 180.f) yaw -= 360.f;
    while (yaw < -180.f) yaw += 360.f;
    return yaw;
}

float AntiAim::randomFloat(float min, float max) noexcept {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(min, max);
    return dis(gen);
}

// Возвращает int для совместимости с config->rageAntiAim[index]
int AntiAim::getMovingFlag(UserCmd* cmd) noexcept {
    if (!localPlayer)
        return 0; // STANDING

    const float speed = localPlayer->velocity().length2D();
    const bool onGround = (localPlayer->flags() & FL_ONGROUND) != 0;
    const bool ducking = (cmd->buttons & UserCmd::IN_DUCK) != 0 || (localPlayer->flags() & FL_DUCKING) != 0;

    if (!onGround)
        return 6; // IN_AIR

    if (ducking) {
        if (speed > 3.f)
            return 5; // DUCKING_MOVING
        return 4; // DUCKING
    }

    if (speed < 3.f)
        return 0; // STANDING

    if (speed > 110.f)
        return 3; // RUNNING

    if (speed > 50.f)
        return 1; // MOVING

    return 2; // SLOW_WALKING
}

void AntiAim::reset() noexcept {
    state = AntiAimState{};
    auto_direction_yaw = 0;
    isShooting = false;
    didShoot = false;
    lastShotTime = 0.f;
    invert = true;
    r8Working = false;
}

// ============================================
// LBY FUNCTIONS
// ============================================

bool AntiAim::updateLBY(bool update) noexcept {
    static float timer = 0.f;
    static bool lastValue = false;

    if (!update)
        return lastValue;

    if (!localPlayer || !(localPlayer->flags() & FL_ONGROUND) || !localPlayer->getAnimstate()) {
        lastValue = false;
        state.lby.standingTime = 0.f;
        return false;
    }

    const float speed = localPlayer->velocity().length2D();
    const float verticalSpeed = std::fabsf(localPlayer->velocity().z);

    if (speed > 0.1f || verticalSpeed > 100.f) {
        timer = memory->globalVars->serverTime() + 0.22f;
        state.lby.standingTime = 0.f;
        state.lby.justUpdated = false;
    }
    else {
        state.lby.standingTime += memory->globalVars->intervalPerTick;
    }

    state.lby.nextUpdateTime = timer;
    state.lby.willUpdate = timer < memory->globalVars->serverTime();

    if (timer < memory->globalVars->serverTime()) {
        timer = memory->globalVars->serverTime() + 1.1f;
        lastValue = true;
        state.lby.justUpdated = true;
        return true;
    }

    lastValue = false;
    state.lby.justUpdated = false;
    return false;
}

bool AntiAim::predictLBYUpdate() noexcept {
    if (!localPlayer || !localPlayer->getAnimstate())
        return false;

    const float timeUntilUpdate = state.lby.nextUpdateTime - memory->globalVars->serverTime();
    return timeUntilUpdate > 0.f && timeUntilUpdate < memory->globalVars->intervalPerTick * 2.f;
}

// ============================================
// AUTO DIRECTION
// ============================================

bool AntiAim::autoDirection(const Vector& eyeAngle) noexcept {
    constexpr float maxRange = 8192.0f;

    Vector eye = eyeAngle;
    eye.x = 0.f;

    Vector eyeAnglesLeft45 = eye;
    Vector eyeAnglesRight45 = eye;
    eyeAnglesLeft45.y += 45.f;
    eyeAnglesRight45.y -= 45.f;

    Vector viewAnglesLeft45 = Vector::fromAngle(eyeAnglesLeft45) * maxRange;
    Vector viewAnglesRight45 = Vector::fromAngle(eyeAnglesRight45) * maxRange;

    Trace traceLeft45, traceRight45;
    Vector startPosition = localPlayer->getEyePosition();

    interfaces->engineTrace->traceRay(
        { startPosition, startPosition + viewAnglesLeft45 },
        0x4600400B, { localPlayer.get() }, traceLeft45
    );
    interfaces->engineTrace->traceRay(
        { startPosition, startPosition + viewAnglesRight45 },
        0x4600400B, { localPlayer.get() }, traceRight45
    );

    float distanceLeft45 = startPosition.distTo(traceRight45.endpos);
    float distanceRight45 = startPosition.distTo(traceLeft45.endpos);

    return distanceLeft45 < distanceRight45;
}

int AntiAim::getAutoDirectionSide(const Vector& eyeAngle) noexcept {
    return autoDirection(eyeAngle) ? 1 : -1;
}

// ============================================
// FREESTAND
// ============================================

void AntiAim::applyFreestand(UserCmd* cmd, int movingFlag) noexcept {
    if (!config->freestandKey.isActive())
        return;

    const auto& cfg = config->rageAntiAim[movingFlag];
    if (!cfg.freestand)
        return;

    constexpr std::array positions = { -30.0f, 0.0f, 30.0f };
    std::array<bool, 3> active = { false, false, false };

    const auto fwd = Vector::fromAngle2D(cmd->viewangles.y);
    const auto side = fwd.crossProduct(Vector::up());
    const Vector eyePos = localPlayer->getEyePosition();

    for (std::size_t i = 0; i < positions.size(); i++) {
        const auto start = eyePos + side * positions[i];
        const auto end = start + fwd * 100.0f;

        Trace trace{};
        interfaces->engineTrace->traceRay({ start, end }, 0x1 | 0x2, nullptr, trace);

        if (trace.fraction != 1.0f)
            active[i] = true;
    }

    if (active[0] && active[1] && !active[2]) {
        state.freestand.side = -1;
        state.freestand.isActive = true;
        auto_direction_yaw = -1;
    }
    else if (!active[0] && active[1] && active[2]) {
        state.freestand.side = 1;
        state.freestand.isActive = true;
        auto_direction_yaw = 1;
    }
    else {
        state.freestand.side = 0;
        state.freestand.isActive = false;
        auto_direction_yaw = 0;
    }
}

// ============================================
// KNIFE THREAT DETECTION
// ============================================

bool AntiAim::detectKnifeThreat(UserCmd* cmd) noexcept {
    if (!localPlayer)
        return false;

    const Vector localPos = localPlayer->getAbsOrigin();
    const Vector eyePos = localPlayer->getEyePosition();

    for (int i = 1; i <= interfaces->engine->getMaxClients(); ++i) {
        const auto entity = interfaces->entityList->getEntity(i);
        if (!entity || entity == localPlayer.get() || entity->isDormant() || !entity->isAlive())
            continue;

        if (!entity->isOtherEnemy(localPlayer.get()))
            continue;

        float distance = entity->getAbsOrigin().distTo(localPos);
        if (distance > 350.f)
            continue;

        const auto weapon = entity->getActiveWeapon();
        if (!weapon || !weapon->isKnife())
            continue;

        state.willGetStabbed = true;
        state.knifeAttacker = entity;
        return true;
    }

    state.willGetStabbed = false;
    state.knifeAttacker = nullptr;
    return false;
}

void AntiAim::handleKnifeThreat(UserCmd* cmd, float& yaw, float& pitch) noexcept {
    if (!state.willGetStabbed || !state.knifeAttacker)
        return;

    const Vector localPos = localPlayer->getEyePosition();
    const Vector enemyPos = state.knifeAttacker->getAbsOrigin();

    Vector angle = AimbotFunction::calculateRelativeAngle(localPos, enemyPos, cmd->viewangles);
    yaw = angle.y + 180.f;
    pitch = 0.f;
}

// ============================================
// ANIM BREAKERS
// ============================================

void AntiAim::JitterMove(UserCmd* cmd) noexcept {
    if (!localPlayer || !(localPlayer->flags() & FL_ONGROUND))
        return;

    if (localPlayer->getActiveWeapon() && localPlayer->getActiveWeapon()->isGrenade())
        return;

    if (std::abs(cmd->sidemove) + std::abs(cmd->forwardmove) < 10.f)
        return;

    if (localPlayer->velocity().length2D() < 140.f)
        return;

    float factor = 0.95f + std::fmod(memory->globalVars->currenttime, 0.2f) * 0.25f;

    cmd->sidemove = std::clamp(cmd->sidemove, -250.f, 250.f) * factor;
    cmd->forwardmove = std::clamp(cmd->forwardmove, -250.f, 250.f) * factor;
}

void AntiAim::microMovement(UserCmd* cmd) noexcept {
    if (!localPlayer || !(localPlayer->flags() & FL_ONGROUND))
        return;

    if (std::fabsf(cmd->sidemove) >= 5.0f)
        return;

    float moveAmount = (cmd->buttons & UserCmd::IN_DUCK) ? 3.25f : 1.1f;
    cmd->sidemove = (cmd->tickCount & 1) ? moveAmount : -moveAmount;
}

void AntiAim::applyAnimBreakers(UserCmd* cmd) noexcept {
    if (!config->condAA.animBreakers)
        return;

    if ((config->condAA.animBreakers & (1 << 4)) != 0)
        JitterMove(cmd);

    if ((config->condAA.animBreakers & (1 << 0)) != 0)
        microMovement(cmd);
}

// ============================================
// DISTORTION
// ============================================

void AntiAim::applyDistortion(UserCmd* cmd, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];

    if (!cfg.distortion)
        return;

    float speed = cfg.distortionSpeed;
    float amount = cfg.distortionAmount;

    if (speed <= 0.f)
        speed = randomFloat(10.f, 100.f);
    if (amount <= 0.f)
        amount = randomFloat(10.f, 60.f);

    const float sine = ((std::sin(memory->globalVars->currenttime * (speed / 10.f)) + 1.f) / 2.f) * amount;
    cmd->viewangles.y += sine - (amount / 2.f);
}

// ============================================
// PITCH
// ============================================

float AntiAim::calculatePitch(UserCmd* cmd, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];

    if (state.willGetStabbed)
        return 0.f;

    switch (cfg.pitch) {
    case 0: // None
        return cmd->viewangles.x;
    case 1: // Down
        return 89.f;
    case 2: // Zero
        return 0.f;
    case 3: // Up
        return -89.f;
    case 4: // Fake pitch
        return (memory->globalVars->tickCount % 20 == 0) ? -89.f : 89.f;
    case 5: // Random
        return randomFloat(-89.f, 89.f);
    default:
        return 89.f;
    }
}

// ============================================
// YAW BASE
// ============================================

float AntiAim::calculateBaseYaw(UserCmd* cmd, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];
    float yaw = 0.f;

    switch (cfg.yawBase) {
    case Yaw::off:
        return cmd->viewangles.y;
    case Yaw::forward:
        yaw = 0.f;
        break;
    case Yaw::backward:
        yaw = 180.f;
        break;
    case Yaw::right:
        yaw = -90.f;
        break;
    case Yaw::left:
        yaw = 90.f;
        break;
    case Yaw::spin:
        yaw = std::fmod(memory->globalVars->currenttime * cfg.spinBase * 10.f, 360.f) - 180.f;
        break;
    default:
        yaw = 180.f;
        break;
    }

    return yaw;
}

float AntiAim::applyAtTargets(float yaw, UserCmd* cmd) noexcept {
    if (!localPlayer)
        return yaw;

    const Vector eyePos = localPlayer->getEyePosition();
    const Vector aimPunch = localPlayer->getAimPunch();

    float bestFov = 255.f;
    float targetYaw = 0.f;
    bool foundTarget = false;

    for (int i = 1; i <= interfaces->engine->getMaxClients(); ++i) {
        const auto entity = interfaces->entityList->getEntity(i);
        if (!entity || entity == localPlayer.get() || entity->isDormant() || !entity->isAlive())
            continue;

        if (!entity->isOtherEnemy(localPlayer.get()))
            continue;

        const auto angle = AimbotFunction::calculateRelativeAngle(
            eyePos, entity->getAbsOrigin(), cmd->viewangles + aimPunch);
        const float fov = angle.length2D();

        if (fov < bestFov) {
            targetYaw = angle.y;
            bestFov = fov;
            foundTarget = true;
        }
    }

    if (foundTarget)
        return yaw + targetYaw;

    return yaw;
}

float AntiAim::applyManualYaw(float yaw) noexcept {
    const bool forward = config->manualForward.isActive();
    const bool back = config->manualBackward.isActive();
    const bool right = config->manualRight.isActive();
    const bool left = config->manualLeft.isActive();

    if (!forward && !back && !right && !left) {
        state.isManualOverride = false;
        return yaw;
    }

    state.isManualOverride = true;
    float manualYaw = 0.f;

    if (back) {
        manualYaw = -180.f;
        if (left) manualYaw -= 45.f;
        else if (right) manualYaw += 45.f;
    }
    else if (left) {
        manualYaw = 90.f;
        if (back) manualYaw += 45.f;
        else if (forward) manualYaw -= 45.f;
    }
    else if (right) {
        manualYaw = -90.f;
        if (back) manualYaw -= 45.f;
        else if (forward) manualYaw += 45.f;
    }
    else if (forward) {
        manualYaw = 0.f;
    }

    return manualYaw;
}

// ============================================
// JITTER MODIFIERS
// ============================================

float AntiAim::applyJitterCentered(float yaw, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];

    float jitterAmount = randomFloat(cfg.jitterMin, cfg.jitterRange);

    if (cfg.desync && cfg.peekMode == 3) {
        return yaw + (invert ? jitterAmount : -jitterAmount);
    }

    return yaw + (state.jitterFlip ? jitterAmount : -jitterAmount);
}

float AntiAim::applyJitterOffset(float yaw, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];

    if (!state.jitterFlip)
        return yaw;

    float jitterAmount = randomFloat(cfg.jitterMin, cfg.jitterRange);
    return yaw + (config->invert.isActive() ? jitterAmount : -jitterAmount);
}

float AntiAim::applyJitterRandom(float yaw, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];
    return yaw + randomFloat(-cfg.randomRange, cfg.randomRange);
}

float AntiAim::applyJitter3Way(float yaw, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];

    static int stage = 0;
    float offset = 0.f;

    switch (stage) {
    case 0: offset = -cfg.jitterRange; break;
    case 1: offset = 0.f; break;
    case 2: offset = cfg.jitterRange; break;
    case 3: offset = 0.f; break;
    }

    stage = (stage + 1) % 4;
    return yaw + offset;
}

float AntiAim::applyJitter5Way(float yaw, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];

    static int stage = 0;
    float offset = 0.f;

    switch (stage) {
    case 0: offset = -cfg.jitterRange; break;
    case 1: offset = -cfg.jitterRange / 2.f; break;
    case 2: offset = 0.f; break;
    case 3: offset = cfg.jitterRange / 2.f; break;
    case 4: offset = cfg.jitterRange; break;
    case 5: offset = cfg.jitterRange / 2.f; break;
    case 6: offset = 0.f; break;
    case 7: offset = -cfg.jitterRange / 2.f; break;
    }

    stage = (stage + 1) % 8;
    return yaw + offset;
}

float AntiAim::applySpin(float yaw, UserCmd* cmd, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];
    return -180.0f + (cmd->tickCount % 9) * cfg.spinBase;
}

float AntiAim::applyYawModifier(float yaw, UserCmd* cmd, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];

    if (state.isManualOverride || state.freestand.isActive || state.willGetStabbed)
        return yaw;

    switch (cfg.yawModifier) {
    case 0: // None
        break;
    case 1: // Jitter Centered
        yaw = applyJitterCentered(yaw, movingFlag);
        break;
    case 2: // Jitter Offset
        yaw = applyJitterOffset(yaw, movingFlag);
        break;
    case 3: // Random
        yaw = applyJitterRandom(yaw, movingFlag);
        break;
    case 4: // 3-Way
        yaw = applyJitter3Way(yaw, movingFlag);
        break;
    case 5: // 5-Way
        yaw = applyJitter5Way(yaw, movingFlag);
        break;
    case 6: // Spin
        yaw = applySpin(yaw, cmd, movingFlag);
        break;
    default:
        break;
    }

    return yaw;
}

// ============================================
// FAKE FLICK
// ============================================

void AntiAim::applyFakeFlick(float& yaw, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];

    if (!cfg.fakeFlick || !config->fakeFlickOnKey.isActive())
        return;

    if (state.willGetStabbed)
        return;

    int rate = cfg.fakeFlickRate;
    if (rate <= 0) rate = 10;

    int tick = memory->globalVars->tickCount % rate;

    if (tick == 0) {
        yaw += config->flipFlick.isActive() ? -90.f : 90.f;
    }
}

// ============================================
// ROLL
// ============================================

float AntiAim::calculateRoll(int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];

    if (!cfg.roll.enabled)
        return 0.f;

    if (!localPlayer || localPlayer->velocity().length2D() >= 100.f)
        return 0.f;

    return invert ? cfg.roll.add : -cfg.roll.add;
}

// ============================================
// DESYNC
// ============================================

float AntiAim::getDesyncDelta(bool inverted, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];

    if (inverted) {
        return randomFloat(cfg.leftMin, cfg.leftLimit) * 2.f;
    }
    return randomFloat(cfg.rightMin, cfg.rightLimit) * -2.f;
}

void AntiAim::handleLBYBreak(UserCmd* cmd, bool& sendPacket, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];

    float leftDelta = getDesyncDelta(false, movingFlag);
    float rightDelta = getDesyncDelta(true, movingFlag);

    switch (cfg.lbyMode) {
    case 0: // Normal (sidemove)
        microMovement(cmd);
        break;

    case 1: // Opposite (LBY break)
        if (updateLBY()) {
            cmd->viewangles.y += !invert ? leftDelta : rightDelta;
            sendPacket = false;
            return;
        }
        break;

    case 2: // Sway
    {
        static bool flip = false;
        if (updateLBY()) {
            cmd->viewangles.y += !flip ? leftDelta : rightDelta;
            sendPacket = false;
            flip = !flip;
            return;
        }
        if (!sendPacket)
            cmd->viewangles.y += flip ? leftDelta : rightDelta;
        break;
    }

    case 3: // Fake
        if (updateLBY()) {
            cmd->viewangles.y += !invert ? leftDelta : rightDelta;
            sendPacket = false;
            return;
        }
        if (!sendPacket)
            cmd->viewangles.y += invert ? leftDelta : rightDelta;
        break;
    }
}

void AntiAim::applyDesync(UserCmd* cmd, bool& sendPacket, int movingFlag) noexcept {
    const auto& cfg = config->rageAntiAim[movingFlag];

    if (!cfg.desync || Tickbase::isShifting())
        return;

    // Roll
    cmd->viewangles.z = calculateRoll(movingFlag);

    // Invert handling
    bool isInvertToggled = config->invert.isActive();

    if (cfg.peekMode != 3) {
        invert = isInvertToggled;
    }

    // Peek modes
    switch (cfg.peekMode) {
    case 0: // Off
        break;
    case 1: // Peek Real
        invert = isInvertToggled ? autoDirection(cmd->viewangles) : !autoDirection(cmd->viewangles);
        break;
    case 2: // Peek Fake
        invert = isInvertToggled ? !autoDirection(cmd->viewangles) : autoDirection(cmd->viewangles);
        break;
    case 3: // Jitter
        if (sendPacket) {
            if (cfg.yawModifier == 7) {
                if (memory->globalVars->tickCount % cfg.tickDelays == 0)
                    invert = !invert;
            }
            else {
                invert = !invert;
            }
        }
        break;
    case 4: // Switch on move
        if (sendPacket && localPlayer->velocity().length2D() > 5.0f) {
            invert = !invert;
        }
        break;
    }

    // Update state
    state.desync.isInverted = invert;
    state.desync.side = invert ? 1 : -1;

    // LBY handling
    handleLBYBreak(cmd, sendPacket, movingFlag);

    if (sendPacket)
        return;

    // Apply desync delta on choked packet
    float desyncDelta = getDesyncDelta(invert, movingFlag);
    cmd->viewangles.y += desyncDelta;

    state.desync.desyncDelta = desyncDelta;
}

// ============================================
// MAIN RAGE FUNCTION
// ============================================

void AntiAim::rage(UserCmd* cmd, const Vector& previousViewAngles,
    const Vector& currentViewAngles, bool& sendPacket) noexcept {

    if (!config->condAA.global)
        return;

    int movingFlag = getMovingFlag(cmd);
    const auto& cfg = config->rageAntiAim[movingFlag];

    // Anim breakers
    applyAnimBreakers(cmd);

    // Detect knife threat
    detectKnifeThreat(cmd);

    // Update jitter flip on send packet
    if (sendPacket && !getDidShoot()) {
        state.jitterFlip = !state.jitterFlip;
    }

    // === PITCH ===
    if (cmd->viewangles.x == currentViewAngles.x || Tickbase::isShifting()) {
        cmd->viewangles.x = calculatePitch(cmd, movingFlag);
    }

    // === YAW ===
    if (cmd->viewangles.y == currentViewAngles.y || Tickbase::isShifting()) {
        if (cfg.yawBase != Yaw::off) {
            float yaw = calculateBaseYaw(cmd, movingFlag);

            // At targets
            if (cfg.atTargets && localPlayer->moveType() != MoveType::LADDER && !state.willGetStabbed) {
                yaw = applyAtTargets(yaw, cmd);
            }

            // Knife threat handling
            if (state.willGetStabbed) {
                handleKnifeThreat(cmd, yaw, cmd->viewangles.x);
            }
            else {
                // Freestand
                applyFreestand(cmd, movingFlag);

                if (state.freestand.isActive) {
                    yaw = state.freestand.side == -1 ? 90.f : -90.f;
                }
                else {
                    // Manual override
                    yaw = applyManualYaw(yaw);

                    // Fake flick
                    if (!state.isManualOverride) {
                        applyFakeFlick(yaw, movingFlag);
                    }

                    // Yaw modifier (jitter, spin, etc)
                    yaw = applyYawModifier(yaw, cmd, movingFlag);

                    // Yaw add
                    if (!state.isManualOverride && !state.freestand.isActive) {
                        yaw += static_cast<float>(cfg.yawAdd);
                    }

                    // Distortion
                    if (!state.isManualOverride && !state.freestand.isActive && cfg.distortion) {
                        applyDistortion(cmd, movingFlag);
                    }
                }
            }

            cmd->viewangles.y += yaw;
            state.currentYaw = normalizeYaw(cmd->viewangles.y);
        }

        // === DESYNC ===
        applyDesync(cmd, sendPacket, movingFlag);
    }
}

// ============================================
// MAIN RUN FUNCTION
// ============================================

void AntiAim::run(UserCmd* cmd, const Vector& previousViewAngles,
    const Vector& currentViewAngles, bool& sendPacket) noexcept {

    int movingFlag = getMovingFlag(cmd);

    // Skip conditions
    if (cmd->buttons & UserCmd::IN_USE)
        return;

    if (localPlayer->moveType() == MoveType::LADDER || localPlayer->moveType() == MoveType::NOCLIP)
        return;

    // Run anti-aim
    if (config->condAA.global || config->rageAntiAim[movingFlag].desync) {
        rage(cmd, previousViewAngles, currentViewAngles, sendPacket);
    }
}

// ============================================
// CAN RUN CHECK
// ============================================

bool AntiAim::canRun(UserCmd* cmd) noexcept {
    if (!localPlayer || !localPlayer->isAlive())
        return false;

    updateLBY(true);

    if ((*memory->gameRules)->freezePeriod())
        return false;

    if (localPlayer->flags() & (1 << 6))
        return false;

    const auto activeWeapon = localPlayer->getActiveWeapon();
    if (!activeWeapon || !activeWeapon->clip())
        return true;

    if (activeWeapon->isThrowing())
        return false;

    if (activeWeapon->isGrenade())
        return true;

    if (localPlayer->shotsFired() > 0 && !activeWeapon->isFullAuto() || localPlayer->waitForNoAttack())
        return true;

    if (localPlayer->nextAttack() > memory->globalVars->serverTime())
        return true;

    if (activeWeapon->nextPrimaryAttack() > memory->globalVars->serverTime())
        return true;

    if (activeWeapon->nextSecondaryAttack() > memory->globalVars->serverTime())
        return true;

    if (localPlayer->nextAttack() <= memory->globalVars->serverTime() && (cmd->buttons & UserCmd::IN_ATTACK))
        return false;

    if (activeWeapon->nextPrimaryAttack() <= memory->globalVars->serverTime() && (cmd->buttons & UserCmd::IN_ATTACK))
        return false;

    if (activeWeapon->isKnife()) {
        if (activeWeapon->nextSecondaryAttack() <= memory->globalVars->serverTime() && (cmd->buttons & UserCmd::IN_ATTACK2))
            return false;
    }

    return true;
}

// ============================================
// INPUT HANDLING
// ============================================

void AntiAim::updateInput() noexcept {
    config->freestandKey.handleToggle();
    config->invert.handleToggle();
    config->fakeFlickOnKey.handleToggle();
    config->flipFlick.handleToggle();
    config->manualForward.handleToggle();
    config->manualBackward.handleToggle();
    config->manualRight.handleToggle();
    config->manualLeft.handleToggle();
}

// ============================================
// GETTERS/SETTERS
// ============================================

float AntiAim::getLastShotTime() noexcept {
    return lastShotTime;
}

bool AntiAim::getIsShooting() noexcept {
    return isShooting;
}

bool AntiAim::getDidShoot() noexcept {
    return didShoot;
}

void AntiAim::setLastShotTime(float shotTime) noexcept {
    lastShotTime = shotTime;
}

void AntiAim::setIsShooting(bool shooting) noexcept {
    isShooting = shooting;
}

void AntiAim::setDidShoot(bool shot) noexcept {
    didShoot = shot;
}