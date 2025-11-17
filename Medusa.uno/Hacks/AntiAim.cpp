// AntiAim.cpp - ÓËÓ×ØÅÍÍÀß ÂÅÐÑÈß ÄËß TOP-1 HVH

#include "../Interfaces.h"
#include "AimbotFunctions.h"
#include "AntiAim.h"
#include "../GameData.h"
#include "../Memory.h"
#include "../SDK/Engine.h"
#include "../SDK/Entity.h"
#include "../SDK/EngineTrace.h"
#include "../SDK/EntityList.h"
#include "../SDK/NetworkChannel.h"
#include "../SDK/UserCmd.h"
#include "Tickbase.h"
#include "../includes.hpp"

// Ãëîáàëüíûå ïåðåìåííûå
static bool isShooting{ false };
static bool didShoot{ false };
static float lastShotTime{ 0.f };
static bool invert = false;

// ÓËÓ×ØÅÍÈÅ: áîëåå áåçîïàñíûé random
static float safeRandomFloat(float min, float max) noexcept
{
    if (min > max)
        std::swap(min, max);

    const float random = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
    return min + random * (max - min);
}

// ÓËÓ×ØÅÍÍÀß ÔÓÍÊÖÈß: LBY update detection
bool updateLby(bool update = false) noexcept
{
    static float timer = 0.f;
    static bool lastValue = false;

    if (!update)
        return lastValue;

    if (!localPlayer || !(localPlayer->flags() & FL_ONGROUND) || !localPlayer->getAnimstate())
    {
        lastValue = false;
        return false;
    }

    const float velocity2D = localPlayer->velocity().length2D();
    const float velocityZ = std::abs(localPlayer->velocity().z);

    // ÓËÓ×ØÅÍÈÅ: áîëåå òî÷íûå óñëîâèÿ LBY update
    if (velocity2D > 0.1f || velocityZ > 100.f)
    {
        timer = memory->globalVars->serverTime() + 0.22f;
    }

    if (timer < memory->globalVars->serverTime())
    {
        timer = memory->globalVars->serverTime() + 1.1f;
        lastValue = true;
        return true;
    }

    lastValue = false;
    return false;
}

// ÓËÓ×ØÅÍÍÀß ÔÓÍÊÖÈß: Distortion ñ áîëåå ïëàâíîé àíèìàöèåé
void distortion(UserCmd* cmd, int movingFlag) noexcept
{
    auto speed = config->rageAntiAim[movingFlag].distortionSpeed;
    auto amount = config->rageAntiAim[movingFlag].distortionAmount;

    if (speed <= 0)
        speed = safeRandomFloat(50.f, 100.f);
    if (amount <= 0)
        amount = safeRandomFloat(180.f, 360.f);

    // ÓËÓ×ØÅÍÈÅ: áîëåå ñëîæíàÿ ñèíóñîèäà
    const float time = memory->globalVars->currenttime * (speed / 10.f);
    const float sine = std::sin(time);
    const float cosine = std::cos(time * 0.5f);

    const float distortionValue = ((sine * 0.7f + cosine * 0.3f + 1.f) / 2.f) * amount;

    cmd->viewangles.y += distortionValue - (amount / 2.f);
}

// ÓËÓ×ØÅÍÍÀß ÔÓÍÊÖÈß: Auto direction (freestanding)
bool autoDirection(const Vector& eyeAngle) noexcept
{
    if (!localPlayer)
        return false;

    constexpr float maxRange = 8192.0f;

    Vector eye = eyeAngle;
    eye.x = 0.f;

    // Left 45 degrees
    Vector eyeAnglesLeft45 = eye;
    eyeAnglesLeft45.y += 45.f;

    // Right 45 degrees
    Vector eyeAnglesRight45 = eye;
    eyeAnglesRight45.y -= 45.f;

    // Convert to forward vectors
    Vector forwardLeft, rightLeft, upLeft;
    Vector forwardRight, rightRight, upRight;
    Helpers::AngleVectors(eyeAnglesLeft45, &forwardLeft, &rightLeft, &upLeft);
    Helpers::AngleVectors(eyeAnglesRight45, &forwardRight, &rightRight, &upRight);

    const Vector viewAnglesLeft45 = forwardLeft * maxRange;
    const Vector viewAnglesRight45 = forwardRight * maxRange;

    const Vector startPosition = localPlayer->getEyePosition();

    Trace traceLeft45, traceRight45;
    interfaces->engineTrace->traceRay({ startPosition, startPosition + viewAnglesLeft45 }, 0x4600400B, { localPlayer.get() }, traceLeft45);
    interfaces->engineTrace->traceRay({ startPosition, startPosition + viewAnglesRight45 }, 0x4600400B, { localPlayer.get() }, traceRight45);

    const float distanceLeft45 = (startPosition - traceLeft45.endpos).length();
    const float distanceRight45 = (startPosition - traceRight45.endpos).length();

    // ÓËÓ×ØÅÍÈÅ: âîçâðàùàåì ñòîðîíó ñ áîëüøèì ïðîñòðàíñòâîì
    return distanceLeft45 < distanceRight45;
}

// ÓËÓ×ØÅÍÍÀß ÔÓÍÊÖÈß: JitterMove
void AntiAim::JitterMove(UserCmd* cmd) noexcept
{
    if (!localPlayer || !(localPlayer->flags() & FL_ONGROUND))
        return;

    const auto activeWeapon = localPlayer->getActiveWeapon();
    if (activeWeapon && activeWeapon->isGrenade())
        return;

    const float totalMove = std::abs(cmd->sidemove) + std::abs(cmd->forwardmove);
    if (totalMove < 10.f)
        return;

    const float velocity2D = localPlayer->velocity().length2D();
    if (velocity2D < 140.f)
        return;

    // ÓËÓ×ØÅÍÈÅ: áîëåå ïëàâíûé jitter factor
    const float timeFactor = std::fmod(memory->globalVars->currenttime, 0.2f);
    const float factor = 0.95f + timeFactor * 0.25f;

    cmd->sidemove = std::clamp(cmd->sidemove, -250.f, 250.f) * factor;
    cmd->forwardmove = std::clamp(cmd->forwardmove, -250.f, 250.f) * factor;
}

// ÍÎÂÀß ÔÓÍÊÖÈß: Knife detection (anti-backstab)
static bool detectKnifeThreat(float& yawToKnifer) noexcept
{
    if (!localPlayer)
        return false;

    const Vector localEye = localPlayer->getEyePosition();
    const Vector localOrigin = localPlayer->getAbsOrigin();

    float closestDistance = FLT_MAX;
    bool threatDetected = false;

    for (int i = 1; i <= interfaces->engine->getMaxClients(); ++i)
    {
        const auto entity = interfaces->entityList->getEntity(i);
        if (!entity || entity == localPlayer.get() || entity->isDormant() ||
            !entity->isAlive() || !entity->isOtherEnemy(localPlayer.get()))
        {
            continue;
        }

        const auto weapon = entity->getActiveWeapon();
        if (!weapon || !weapon->isKnife())
            continue;

        const float distance = (entity->getAbsOrigin() - localOrigin).length();

        // ÊÐÈÒÈ×ÅÑÊÀß ÄÈÑÒÀÍÖÈß äëÿ backstab
        if (distance > 469.f)
            continue;

        const auto angle = AimbotFunction::calculateRelativeAngle(
            localEye,
            entity->getAbsOrigin(),
            Vector{}
        );

        const float fov = angle.length2D();

        if (distance < closestDistance)
        {
            closestDistance = distance;
            yawToKnifer = angle.y;
            threatDetected = true;
        }
    }

    return threatDetected;
}

// ÎÑÍÎÂÍÀß ÔÓÍÊÖÈß ANTIAIM
void AntiAim::rage(UserCmd* cmd, const Vector& previousViewAngles, const Vector& currentViewAngles, bool& sendPacket) noexcept
{
    if (!config->condAA.global)
        return;

    static float knifeYaw = 0.f;
    const bool knifeThre = detectKnifeThreat(knifeYaw);

    // Animation breakers
    if ((config->condAA.animBreakers & (1 << 4)) == (1 << 4))
        JitterMove(cmd);

    const auto moving_flag = get_moving_flag(cmd);

    // ===== PITCH =====
    if (cmd->viewangles.x == currentViewAngles.x || Tickbase::isShifting())
    {
        // Anti-backstab: look at knifer
        if (knifeThre)
        {
            cmd->viewangles.x = 0.f;
        }
        else
        {
            switch (config->rageAntiAim[static_cast<int>(moving_flag)].pitch)
            {
            case 0: // None
                break;
            case 1: // Down
                cmd->viewangles.x = 89.f;
                break;
            case 2: // Zero
                cmd->viewangles.x = 0.f;
                break;
            case 3: // Up
                cmd->viewangles.x = -89.f;
                break;
            case 4: // Fake pitch (jitter)
            {
                const bool jitterDown = (memory->globalVars->tickCount % 20) == 0;
                cmd->viewangles.x = jitterDown ? -89.f : 89.f;
                break;
            }
            case 5: // Random
            {
                cmd->viewangles.x = std::round(safeRandomFloat(-89.f, 89.f));
                break;
            }
            default:
                break;
            }
        }
    }

    // ===== YAW =====
    if (cmd->viewangles.y == currentViewAngles.y || Tickbase::isShifting())
    {
        if (config->rageAntiAim[static_cast<int>(moving_flag)].yawBase != Yaw::off)
        {
            // Manual AA
            const bool forward = config->manualForward.isActive();
            const bool back = config->manualBackward.isActive();
            const bool right = config->manualRight.isActive();
            const bool left = config->manualLeft.isActive();
            const bool isManualSet = forward || back || right || left;

            float yaw = 0.f;
            static bool flipJitter = false;
            bool isFreestanding = false;

            // At targets (lock onto closest enemy)
            if (config->rageAntiAim[static_cast<int>(moving_flag)].atTargets &&
                localPlayer->moveType() != MoveType::LADDER && !knifeThre)
            {
                const Vector localEye = localPlayer->getEyePosition();
                const Vector aimPunch = localPlayer->getAimPunch();

                float bestFov = FLT_MAX;
                float yawAngle = 0.f;

                for (int i = 1; i <= interfaces->engine->getMaxClients(); ++i)
                {
                    const auto entity = interfaces->entityList->getEntity(i);
                    if (!entity || entity == localPlayer.get() || entity->isDormant() ||
                        !entity->isAlive() || !entity->isOtherEnemy(localPlayer.get()))
                    {
                        continue;
                    }

                    const auto angle = AimbotFunction::calculateRelativeAngle(
                        localEye,
                        entity->getAbsOrigin(),
                        cmd->viewangles + aimPunch
                    );

                    const float fov = angle.length2D();
                    if (fov < bestFov)
                    {
                        yawAngle = angle.y;
                        bestFov = fov;
                    }
                }

                yaw = yawAngle;
            }

            // Anti-backstab: face the knifer
            if (knifeThre)
            {
                yaw = knifeYaw;
            }
            else
            {
                // Yaw base
                switch (config->rageAntiAim[static_cast<int>(moving_flag)].yawBase)
                {
                case Yaw::forward:
                    yaw += 0.f;
                    break;
                case Yaw::backward:
                    yaw += 180.f;
                    break;
                case Yaw::right:
                    yaw += -90.f;
                    break;
                case Yaw::left:
                    yaw += 90.f;
                    break;
                default:
                    break;
                }
            }

            // Manual overrides (if not knife threat)
            if (!knifeThre)
            {
                if (back)
                {
                    yaw = 180.f;
                    if (left)
                        yaw += 45.f;
                    else if (right)
                        yaw -= 45.f;
                }
                else if (left)
                {
                    yaw = 90.f;
                    if (back)
                        yaw += 45.f;
                    else if (forward)
                        yaw -= 45.f;
                }
                else if (right)
                {
                    yaw = -90.f;
                    if (back)
                        yaw -= 45.f;
                    else if (forward)
                        yaw += 45.f;
                }
                else if (forward)
                {
                    yaw = 0.f;
                }
            }

            // Fake flick
            if (config->rageAntiAim[static_cast<int>(moving_flag)].fakeFlick &&
                config->fakeFlickOnKey.isActive() && !knifeThre)
            {
                const int rate = config->rageAntiAim[static_cast<int>(moving_flag)].fakeFlickRate;
                const int tick = memory->globalVars->tickCount % rate;

                if (tick == 0)
                {
                    yaw += config->flipFlick.isActive() ? -90.f : 90.f;
                }
            }

            // Freestanding
            if (config->rageAntiAim[static_cast<int>(moving_flag)].freestand &&
                config->freestandKey.isActive() && !knifeThre)
            {
                constexpr std::array positions = { -30.0f, 0.0f, 30.0f };
                std::array<bool, 3> active = { false, false, false };

                Vector forward, right, up;
                Vector rightVec, upVec, dummy;
                Helpers::AngleVectors(Vector{ 0.f, cmd->viewangles.y, 0.f }, & forward, & right, & up);
                Helpers::AngleVectors(Vector{ 0.f, cmd->viewangles.y + 90.f, 0.f }, & rightVec, & dummy, & dummy);

                const Vector side = right;

                for (size_t i = 0; i < positions.size(); i++)
                {
                    const Vector start = localPlayer->getEyePosition() + side * positions[i];
                    const Vector end = start + forward * 100.0f;

                    Trace trace;
                    interfaces->engineTrace->traceRay({ start, end }, 0x4600400B, nullptr, trace);

                    if (trace.fraction != 1.0f)
                        active[i] = true;
                }

                // Left side blocked
                if (active[0] && active[1] && !active[2])
                {
                    yaw = 90.f;
                    AntiAim::auto_direction_yaw = -1;
                    isFreestanding = true;
                }
                // Right side blocked
                else if (!active[0] && active[1] && active[2])
                {
                    yaw = -90.f;
                    AntiAim::auto_direction_yaw = 1;
                    isFreestanding = true;
                }
                else
                {
                    AntiAim::auto_direction_yaw = 0;
                    isFreestanding = false;
                }
            }

            // Flip jitter
            if (sendPacket && !AntiAim::getDidShoot())
                flipJitter = !flipJitter;

            // Yaw modifier
            if (!isManualSet && !isFreestanding && !knifeThre)
            {
                const float jitterMin = config->rageAntiAim[static_cast<int>(moving_flag)].jitterMin;
                const float jitterRange = config->rageAntiAim[static_cast<int>(moving_flag)].jitterRange;
                const float randomRange = config->rageAntiAim[static_cast<int>(moving_flag)].randomRange;
                const float spinBase = config->rageAntiAim[static_cast<int>(moving_flag)].spinBase;

                switch (config->rageAntiAim[static_cast<int>(moving_flag)].yawModifier)
                {
                case 0: // None
                    break;

                case 1: // Jitter centered
                {
                    const float jitterValue = safeRandomFloat(jitterMin, jitterRange);
                    yaw += flipJitter ? jitterValue : -jitterValue;
                    break;
                }

                case 2: // Jitter offset
                {
                    if (flipJitter)
                    {
                        const float jitterValue = safeRandomFloat(jitterMin, jitterRange);
                        yaw += config->invert.isActive() ? jitterValue : -jitterValue;
                    }
                    break;
                }

                case 3: // Random
                {
                    yaw += std::round(safeRandomFloat(-randomRange, randomRange));
                    break;
                }

                case 4: // 3-way jitter
                {
                    static int stage = 0;
                    switch (stage)
                    {
                    case 0:
                        yaw -= jitterRange;
                        stage = 1;
                        break;
                    case 1:
                        yaw += 0.f;
                        stage = 2;
                        break;
                    case 2:
                        yaw += jitterRange;
                        stage = 0;
                        break;
                    }
                    break;
                }

                case 5: // 5-way jitter
                {
                    static int stage = 0;
                    const float halfRange = jitterRange / 2.f;

                    switch (stage)
                    {
                    case 0: yaw -= jitterRange; stage = 1; break;
                    case 1: yaw -= halfRange; stage = 2; break;
                    case 2: yaw += 0.f; stage = 3; break;
                    case 3: yaw += halfRange; stage = 4; break;
                    case 4: yaw += jitterRange; stage = 0; break;
                    }
                    break;
                }

                case 6: // Spin
                {
                    yaw = -180.0f + (cmd->tickCount % 360) * (spinBase / 40.f);
                    break;
                }

                default:
                    break;
                }
            }

            // Yaw add
            if (!isManualSet && !isFreestanding && !knifeThre)
            {
                yaw += static_cast<float>(config->rageAntiAim[static_cast<int>(moving_flag)].yawAdd);
            }

            // Distortion
            if (!isManualSet && !isFreestanding && !knifeThre)
            {
                if (config->rageAntiAim[static_cast<int>(moving_flag)].distortion)
                {
                    distortion(cmd, static_cast<int>(moving_flag));
                }
            }

            cmd->viewangles.y += yaw;
        }
    }

    // ===== DESYNC (FAKE YAW) =====
    if (config->rageAntiAim[static_cast<int>(moving_flag)].desync && !Tickbase::isShifting())
    {
        // Roll
        if (config->rageAntiAim[static_cast<int>(moving_flag)].roll.enabled &&
            localPlayer->velocity().length2D() < 100.f)
        {
            const float rollAdd = config->rageAntiAim[static_cast<int>(moving_flag)].roll.add;
            cmd->viewangles.z = invert ? rollAdd : -rollAdd;
        }
        else
        {
            cmd->viewangles.z = 0.f;
        }

        // Invert management
        const bool isInvertToggled = config->invert.isActive();

        if (config->rageAntiAim[static_cast<int>(moving_flag)].peekMode != 3)
        {
            invert = isInvertToggled;
        }

        const float leftMin = config->rageAntiAim[static_cast<int>(moving_flag)].leftMin;
        const float leftLimit = config->rageAntiAim[static_cast<int>(moving_flag)].leftLimit;
        const float rightMin = config->rageAntiAim[static_cast<int>(moving_flag)].rightMin;
        const float rightLimit = config->rageAntiAim[static_cast<int>(moving_flag)].rightLimit;

        const float leftDesyncAngle = safeRandomFloat(leftMin, leftLimit) * 2.f;
        const float rightDesyncAngle = safeRandomFloat(rightMin, rightLimit) * -2.f;

        // Peek mode
        switch (config->rageAntiAim[static_cast<int>(moving_flag)].peekMode)
        {
        case 0: // None
            break;

        case 1: // Peek real
            if (!isInvertToggled)
                invert = !autoDirection(cmd->viewangles);
            else
                invert = autoDirection(cmd->viewangles);
            break;

        case 2: // Peek fake
            if (isInvertToggled)
                invert = !autoDirection(cmd->viewangles);
            else
                invert = autoDirection(cmd->viewangles);
            break;

        case 3: // Jitter
            if (sendPacket)
            {
                const int yawMod = config->rageAntiAim[static_cast<int>(moving_flag)].yawModifier;

                if (yawMod != 7)
                {
                    invert = !invert;
                }
                else
                {
                    const int tickDelay = config->rageAntiAim[static_cast<int>(moving_flag)].tickDelays;
                    if ((memory->globalVars->tickCount % tickDelay) == 0)
                        invert = !invert;
                }
            }
            break;

        case 4: // Switch (on move)
            if (sendPacket && localPlayer->velocity().length2D() > 5.0f)
            {
                invert = !invert;
            }
            break;

        default:
            break;
        }

        // LBY mode
        switch (config->rageAntiAim[static_cast<int>(moving_flag)].lbyMode)
        {
        case 0: // Normal (sidemove)
        {
            if (std::abs(cmd->sidemove) < 5.0f)
            {
                const bool duckJitter = cmd->buttons & UserCmd::IN_DUCK;
                cmd->sidemove = (cmd->tickCount & 1) ?
                    (duckJitter ? 3.25f : 1.1f) :
                    (duckJitter ? -3.25f : -1.1f);
            }
            break;
        }

        case 1: // Opposite (LBY break)
        {
            if (updateLby())
            {
                cmd->viewangles.y += invert ? rightDesyncAngle : leftDesyncAngle;
                sendPacket = false;
                return;
            }
            break;
        }

        case 2: // Sway (flip every LBY update)
        {
            static bool flip = false;

            if (updateLby())
            {
                cmd->viewangles.y += flip ? rightDesyncAngle : leftDesyncAngle;
                sendPacket = false;
                flip = !flip;
                return;
            }

            if (!sendPacket)
            {
                cmd->viewangles.y += flip ? leftDesyncAngle : rightDesyncAngle;
            }
            break;
        }

        case 3: // Fake
        {
            if (updateLby())
            {
                cmd->viewangles.y += invert ? rightDesyncAngle : leftDesyncAngle;
                sendPacket = false;
                return;
            }

            if (!sendPacket)
            {
                cmd->viewangles.y += invert ? leftDesyncAngle : rightDesyncAngle;
            }
            break;
        }

        default:
            break;
        }

        // ÊÐÈÒÈ×ÅÑÊÈ ÂÀÆÍÎ: fake angles òîëüêî íà choked packets
        if (!sendPacket)
        {
            cmd->viewangles.y += invert ? leftDesyncAngle : rightDesyncAngle;
        }
    }
}

void AntiAim::updateInput() noexcept
{
    config->freestandKey.handleToggle();
    config->invert.handleToggle();
    config->fakeFlickOnKey.handleToggle();
    config->flipFlick.handleToggle();
    config->manualForward.handleToggle();
    config->manualBackward.handleToggle();
    config->manualRight.handleToggle();
    config->manualLeft.handleToggle();
}

void AntiAim::run(UserCmd* cmd, const Vector& previousViewAngles, const Vector& currentViewAngles, bool& sendPacket) noexcept
{
    const auto moving_flag = get_moving_flag(cmd);

    if (cmd->buttons & UserCmd::IN_USE)
        return;

    if (localPlayer->moveType() == MoveType::LADDER || localPlayer->moveType() == MoveType::NOCLIP)
        return;

    if (config->condAA.global || config->rageAntiAim[static_cast<int>(moving_flag)].desync)
    {
        AntiAim::rage(cmd, previousViewAngles, currentViewAngles, sendPacket);
    }
}

bool AntiAim::canRun(UserCmd* cmd) noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return false;

    updateLby(true);

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

    if (localPlayer->shotsFired() > 0 && !activeWeapon->isFullAuto())
        return true;

    if (localPlayer->waitForNoAttack())
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

    if (activeWeapon->isKnife())
    {
        if (activeWeapon->nextSecondaryAttack() <= memory->globalVars->serverTime() && (cmd->buttons & UserCmd::IN_ATTACK2))
            return false;
    }

    return true;
}

// Getters/Setters
float AntiAim::getLastShotTime()
{
    return lastShotTime;
}

bool AntiAim::getIsShooting()
{
    return isShooting;
}

bool AntiAim::getDidShoot()
{
    return didShoot;
}

void AntiAim::setLastShotTime(float shotTime)
{
    lastShotTime = shotTime;
}

void AntiAim::setIsShooting(bool shooting)
{
    isShooting = shooting;
}

void AntiAim::setDidShoot(bool shot)
{
    didShoot = shot;
}
