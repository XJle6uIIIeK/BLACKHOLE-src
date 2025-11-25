

#include "../Config.h"
#include "../Interfaces.h"
#include "../Memory.h"

#include "AimbotFunctions.h"
#include "Animations.h"
#include "Backtrack.h"
#include "Ragebot.h"
#include "EnginePrediction.h"
#include "Resolver.h"
#include "../GameData.h"
#include "../SDK/Entity.h"
#include "../SDK/UserCmd.h"
#include "../SDK/Utils.h"
#include "../SDK/Vector.h"
#include "../SDK/WeaponId.h"
#include "../SDK/GlobalVars.h"
#include "../SDK/LocalPlayer.h"
#include "../SDK/ModelInfo.h"
#include <DirectXMath.h>
#include <iostream>
#include <cstdlib>
#include "../xor.h"
#include "../Logger.h"

static bool keyPressed = false;
#define TICK_INTERVAL (memory->globalVars->currenttime)
#define TIME_TO_TICKS(dt) ((int)(0.5f + (float)(dt) / TICK_INTERVAL))
//std::deque<adjust_data> player_records[65];

void Ragebot::updateInput() noexcept
{
    config->ragebotKey.handleToggle();
    config->hitchanceOverride.handleToggle();
    config->minDamageOverrideKey.handleToggle();
    config->forceBaim.handleToggle();
}

static std::unordered_map<int, Ragebot::TargetMovement> targetMovements;


void updateTargetMovement(Entity* target) noexcept
{
    if (!target)
        return;

    const auto index = target->index();
    const auto currentPos = target->getAbsOrigin();
    const auto currentTime = target->simulationTime();

    if (targetMovements.find(index) != targetMovements.end())
    {
        auto& record = targetMovements[index];
        const float deltaTime = currentTime - record.simulationTime;

        if (deltaTime > 0.0f && deltaTime < 1.0f)
        {
         
            const Vector newVelocity = (currentPos - target->getAbsOrigin()) / deltaTime;
            record.velocity = Helpers::lerp(0.7f, record.velocity, newVelocity); // Smooth transition
            record.simulationTime = currentTime;
        }
    }
    else
    {
        targetMovements[index] = { Vector{0.f, 0.f, 0.f}, currentTime };
    }
}


Vector predictTargetPosition(Entity* target, float predictionTime) noexcept
{
    if (!target)
        return Vector{};

    const auto index = target->index();
    if (targetMovements.find(index) == targetMovements.end())
        return target->getAbsOrigin();

    const auto& movement = targetMovements[index];
    Vector predictedPos = target->getAbsOrigin();


    predictedPos = predictedPos + movement.velocity * predictionTime;

    if (!(target->flags() & FL_ONGROUND))
    {
        static auto gravity = interfaces->cvar->findVar(skCrypt("sv_gravity"));
        if (gravity)
        {
            const float gravityValue = gravity->getFloat();
            predictedPos.z -= 0.5f * gravityValue * predictionTime * predictionTime;
        }
    }

    return predictedPos;
}


void handlePredictiveAutoStop(UserCmd* cmd, Entity* target, float accuracyBoost) noexcept
{
    if (!target || !localPlayer)
        return;

    const auto predictedPos = predictTargetPosition(target, TICK_INTERVAL);
    const auto localPos = localPlayer->getAbsOrigin();

    const float distanceToTarget = localPos.distTo(predictedPos);
    const auto activeWeapon = localPlayer->getActiveWeapon();
    if (!activeWeapon)
        return;

    const auto weaponData = activeWeapon->getWeaponData();
    if (!weaponData)
        return;

    const float optimalDistance = weaponData->range * 0.85f;

    if (distanceToTarget > optimalDistance)
        return;

    const Vector velocity = EnginePrediction::getVelocity();
    const float speed = velocity.length2D();
    const float targetSpeed = targetMovements[target->index()].velocity.length2D();
    const float maxspeed = (localPlayer->isScoped() ? weaponData->maxSpeedAlt : weaponData->maxSpeed);

    const float timeToStop = speed / maxspeed * (1.0f - accuracyBoost);
    const float predictionFactor = std::clamp(targetSpeed / 300.0f, 0.5f, 1.5f);

    const float requiredSpeed = (distanceToTarget - (targetSpeed * TICK_INTERVAL)) /
        (timeToStop * predictionFactor);

    if (requiredSpeed < maxspeed)
    {
        Vector velAngles = velocity.toAngle();
        Vector forward, right, up;
        Helpers::AngleVectors(velAngles, &forward, &right, &up);

        const Vector dirVector = forward * requiredSpeed;
        const Vector adjustedVelocity = velocity - dirVector;

        cmd->forwardmove = adjustedVelocity.x;
        cmd->sidemove = adjustedVelocity.y;
    }
}


void applyEarlyAutostop(UserCmd* cmd, Entity* weapon, float accuracyBoost) noexcept
{
    if (!localPlayer || !weapon)
        return;

    const auto weaponData = weapon->getWeaponData();
    if (!weaponData)
        return;

    const Vector velocity = localPlayer->velocity();
    const float speed = velocity.length2D();
    const float maxSpeed = (localPlayer->isScoped() ? weaponData->maxSpeedAlt : weaponData->maxSpeed) * (1.0f - accuracyBoost);


    if (speed < 10.0f || !(localPlayer->flags() & FL_ONGROUND))
        return;


    constexpr float friction = 5.0f;
    constexpr float acceleration = 5.5f;


    const float targetSpeed = maxSpeed * 0.95f;
    const float speedReduction = speed - targetSpeed;

    if (speedReduction <= 0.0f)
        return;


    Vector moveDir = velocity.toAngle();
    moveDir.y = cmd->viewangles.y - moveDir.y;

    Vector forward, right, up;
    Helpers::AngleVectors(moveDir, &forward, &right, &up);

    const Vector negatedDir = forward * -450.0f;


    const float brakeFactor = std::clamp(speedReduction / maxSpeed, 0.0f, 1.0f);
    cmd->forwardmove = negatedDir.x * brakeFactor;
    cmd->sidemove = negatedDir.y * brakeFactor;
}


float calculateDynamicHitchance(
    Entity* localPlayer,
    Entity* target,
    Entity* weapon,
    const Vector& angle,
    int baseHitchance
) noexcept {

    float hitchance = static_cast<float>(baseHitchance);

    // === БОНУСЫ К ТОЧНОСТИ ===

    // 1. Crouching bonus (+10%)
    if (localPlayer->flags() & FL_DUCKING) {
        hitchance *= 1.10f;
    }

    // 2. Scoped bonus (+15%)
    if (weapon->isSniperRifle() && localPlayer->isScoped()) {
        hitchance *= 1.15f;
    }

    // 3. Standing still bonus (+20%)
    const float velocity = localPlayer->velocity().length2D();
    if (velocity < 5.0f) {
        hitchance *= 1.20f;
    }
    // Partial bonus for slow movement
    else if (velocity < 34.0f) { // walking speed
        hitchance *= 1.10f;
    }

    // 4. First shot accuracy bonus (+25% for first 2 shots)
    const int shotsFired = localPlayer->shotsFired();
    if (shotsFired == 0) {
        hitchance *= 1.25f;
    }
    else if (shotsFired == 1) {
        hitchance *= 1.15f;
    }

    // 5. Close range bonus (< 15m = +10%)
    const float distance = localPlayer->getAbsOrigin().distTo(target->getAbsOrigin());
    if (distance < 500.0f) { // ~15 meters
        hitchance *= 1.10f;
    }

    // === ПЕНАЛЬТИ К ТОЧНОСТИ ===

    // 1. Recoil penalty (после 3 выстрелов)
    if (shotsFired > 3) {
        const float recoilPenalty = 0.9f - ((std::min)(shotsFired - 3, 7) * 0.01f);
        hitchance *= recoilPenalty; // -10% до -17%
    }

    // 2. Moving target penalty
    const float targetVelocity = target->velocity().length2D();
    if (targetVelocity > 100.0f) {
        hitchance *= 0.90f;
    }
    if (targetVelocity > 200.0f) {
        hitchance *= 0.85f; // итого -15% при быстром движении
    }

    // 3. Air target penalty
    if (!(target->flags() & FL_ONGROUND)) {
        hitchance *= 0.92f;
    }

    // 4. Long range penalty (> 30m = -15%)
    if (distance > 1000.0f) { // ~30 meters
        hitchance *= 0.85f;
    }

    // 5. Wide angle penalty (поворот > 30° = -10%)
    const float angleDiff = angle.length2D();
    if (angleDiff > 30.0f) {
        hitchance *= 0.90f;
    }

    // Clamp результат
    return std::clamp(hitchance, 0.0f, 100.0f);
}

void runRagebot(
    UserCmd* cmd,
    Entity* entity,
    matrix3x4* matrix,
    Ragebot::Enemies target,
    std::array<bool, Hitboxes::Max> hitbox,
    Entity* activeWeapon,
    int weaponIndex,
    Vector localPlayerEyePosition,
    Vector aimPunch,
    int multiPointHead,
    int multiPointBody,
    int minDamage,
    float& damageDiff,
    Vector& bestAngle,
    Vector& bestTarget) noexcept
{
    Ragebot::latest_player = entity->getPlayerName();
    const auto& cfg = config->ragebot;
    const auto& cfg1 = config->rageBot;
    damageDiff = FLT_MAX;

    const Model* model = entity->getModel();
    if (!model)
        return;

    StudioHdr* hdr = interfaces->modelInfo->getStudioModel(model);
    if (!hdr)
        return;

    StudioHitboxSet* set = hdr->getHitboxSet(0);
    if (!set)
        return;

    static auto isSpreadEnabled = interfaces->cvar->findVar(skCrypt("weapon_accuracy_nospread"));


    struct HitboxPriority
    {
        int index;
        float priority;

        bool operator<(const HitboxPriority& other) const
        {
            return priority > other.priority;
        }
    };

    std::vector<HitboxPriority> prioritizedHitboxes;

    for (size_t i = 0; i < hitbox.size(); i++)
    {
        if (!hitbox[i])
            continue;

        float priority = 0.0f;


        switch (i)
        {
        case Hitboxes::Head:
            priority = 100.0f;
            break;
        case Hitboxes::UpperChest:
        case Hitboxes::Thorax:
            priority = 80.0f;
            break;
        case Hitboxes::LowerChest:
            priority = 70.0f;
            break;
        case Hitboxes::Belly:
        case Hitboxes::Pelvis:
            priority = 60.0f;
            break;
        default:
            priority = 30.0f;
            break;
        }

        prioritizedHitboxes.push_back({ static_cast<int>(i), priority });
    }


    std::sort(prioritizedHitboxes.begin(), prioritizedHitboxes.end());


    for (const auto& prioHitbox : prioritizedHitboxes)
    {
        const int i = prioHitbox.index;

        StudioBbox* hitboxData = set->getHitbox(i);
        if (!hitboxData)
            continue;


        auto multipoints = AimbotFunction::multiPoint(
            entity,
            matrix,
            hitboxData,
            localPlayerEyePosition,
            i,
            multiPointHead,
            multiPointBody
        );


        for (auto& bonePosition : multipoints)
        {
            const auto angle = AimbotFunction::calculateRelativeAngle(
                localPlayerEyePosition,
                bonePosition,
                cmd->viewangles + aimPunch
            );

            const auto fov = angle.length2D();
            if (fov > cfg.fov)
                continue;

            float damage = AimbotFunction::getScanDamage(
                entity,
                bonePosition,
                activeWeapon->getWeaponData(),
                minDamage,
                cfg.friendlyFire
            );

            damage = std::clamp(damage, 0.0f, static_cast<float>(entity->maxHealth()));

            if (damage <= 0.5f)
                continue;

            // Auto scope
            if (cfg.autoScope && activeWeapon->isSniperRifle() &&
                !localPlayer->isScoped() && activeWeapon->zoomLevel() < 1)
            {
                cmd->buttons |= UserCmd::IN_ZOOM;
            }


            if (cfg1[weaponIndex].autoStop &&
                (localPlayer->flags() & FL_ONGROUND) &&
                !(cmd->buttons & UserCmd::IN_JUMP) &&
                isSpreadEnabled->getInt() == 0)
            {
                const auto& stopMode = cfg1[weaponIndex].autoStopMod;
                const Vector velocity = EnginePrediction::getVelocity();
                const float speed = velocity.length2D();
                const auto weaponData = activeWeapon->getWeaponData();
                const float accuracyBoost = std::clamp(cfg1[weaponIndex].accuracyBoost, 0.0f, 1.0f);
                const float maxSpeed = (localPlayer->isScoped() ? weaponData->maxSpeedAlt : weaponData->maxSpeed) *
                    (1.0f - accuracyBoost);

                // Adaptive
                if (stopMode & (1 << 0))
                {
                    if (speed > maxSpeed)
                    {
                        Vector direction = velocity.toAngle();
                        direction.y = cmd->viewangles.y - direction.y;

                        Vector forward, right, up;
                        Helpers::AngleVectors(direction, &forward, &right, &up);

                        const Vector negatedDirection = forward * -speed;
                        cmd->forwardmove = negatedDirection.x;
                        cmd->sidemove = negatedDirection.y;
                    }
                }

                // Full stop
                if (stopMode & (1 << 2))
                {
                    cmd->forwardmove = cmd->sidemove = 0;
                }

                // Duck
                if (stopMode & (1 << 3))
                {
                    cmd->buttons |= UserCmd::IN_DUCK;
                }

                // Early
                if (stopMode & (1 << 4))
                {
                    applyEarlyAutostop(cmd, activeWeapon, accuracyBoost);
                }

                // Predictive
                if (stopMode & (1 << 5))
                {
                    updateTargetMovement(entity);
                    handlePredictiveAutoStop(cmd, entity, accuracyBoost);
                }

                EnginePrediction::update();
            }


            const float damageScore = damage * 2.0f;
            const float priorityScore = prioHitbox.priority;
            const float totalScore = damageScore + priorityScore;

            const float currentDiff = std::fabsf(static_cast<float>(target.health) - damage);


            if (std::fabsf(currentDiff - damageDiff) < 5.0f)
            {
                if (prioHitbox.priority > 0.0f) // Prefer higher priority hitbox
                {
                    bestAngle = angle;
                    damageDiff = currentDiff;
                    bestTarget = bonePosition;
                }
            }
            else if (currentDiff < damageDiff)
            {
                bestAngle = angle;
                damageDiff = currentDiff;
                bestTarget = bonePosition;
            }
        }


        if (i == Hitboxes::Head && bestTarget.notNull() && damageDiff < 10.0f)
            break;
    }


    if (bestTarget.notNull())
    {
        bool hitChanceOverrideActive = config->hitchanceOverride.isActive() && cfg1[weaponIndex].hcov;
        int requiredHitChance = hitChanceOverrideActive ?
            cfg1[weaponIndex].OvrHitChance : cfg1[weaponIndex].hitChance;

        float dynamicHitchance = calculateDynamicHitchance(
            localPlayer.get(),
            entity,
            activeWeapon,
            bestAngle,
            requiredHitChance
        );

        // Затем проверяем с динамическим значением
        if (!AimbotFunction::hitChance(
            localPlayer.get(),
            entity,
            set,
            matrix,
            activeWeapon,
            bestAngle,
            cmd,
            static_cast<int>(dynamicHitchance)))
        {
            bestTarget = Vector{};
            bestAngle = Vector{};
            damageDiff = FLT_MAX;
        }
    }
}

void Ragebot::run(UserCmd* cmd) noexcept
{
    if (!config->ragebot.enabled || !config->ragebotKey.isActive())
        return;

    if (!localPlayer || localPlayer->nextAttack() > memory->globalVars->serverTime() ||
        localPlayer->isDefusing() || localPlayer->waitForNoAttack())
        return;

    const auto& cfg = config->ragebot;
    const auto& cfg1 = config->rageBot;

    const auto activeWeapon = localPlayer->getActiveWeapon();
    if (!activeWeapon || !activeWeapon->clip() ||
        activeWeapon->isKnife() || activeWeapon->isBomb() || activeWeapon->isGrenade())
        return;

    // Weapon indexing
    int weaponIndex = getWeaponIndex(activeWeapon->itemDefinitionIndex2());

    {
        if (cfg1[1].enabled && (activeWeapon->itemDefinitionIndex2() == WeaponId::Elite ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Hkp2000 ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::P250 ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Usp_s ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Cz75a ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Tec9 ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Fiveseven ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Glock))
            weaponIndex = 1;
        else if (cfg1[2].enabled && activeWeapon->itemDefinitionIndex2() == WeaponId::Deagle)
            weaponIndex = 2;
        else if (cfg1[3].enabled && activeWeapon->itemDefinitionIndex2() == WeaponId::Revolver)
            weaponIndex = 3;
        else if (cfg1[4].enabled && (activeWeapon->itemDefinitionIndex2() == WeaponId::Mac10 ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Mp9 ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Mp7 ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Mp5sd ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Ump45 ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::P90 ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Bizon))
            weaponIndex = 4;
        else if (cfg1[5].enabled && (activeWeapon->itemDefinitionIndex2() == WeaponId::Ak47 ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::M4A1 ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::M4a1_s ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::GalilAr ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Aug ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Sg553 ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Famas))
            weaponIndex = 5;
        else if (cfg1[6].enabled && activeWeapon->itemDefinitionIndex2() == WeaponId::Ssg08)
            weaponIndex = 6;
        else if (cfg1[7].enabled && activeWeapon->itemDefinitionIndex2() == WeaponId::Awp)
            weaponIndex = 7;
        else if (cfg1[8].enabled && (activeWeapon->itemDefinitionIndex2() == WeaponId::G3SG1 ||
            activeWeapon->itemDefinitionIndex2() == WeaponId::Scar20))
            weaponIndex = 8;
        else if (cfg1[9].enabled && activeWeapon->itemDefinitionIndex2() == WeaponId::Taser)
            weaponIndex = 9;
        else
            weaponIndex = 0;
    }

    if (localPlayer->shotsFired() > 0 && !activeWeapon->isFullAuto())
        return;

    if ((cfg1[weaponIndex].autoStopMod & (1 << 1)) != (1 << 1) &&
        activeWeapon->nextPrimaryAttack() > memory->globalVars->serverTime())
        return;

    if (!(cfg.enabled && (cmd->buttons & UserCmd::IN_ATTACK || cfg.autoShot)))
        return;

    float damageDiff = FLT_MAX;
    Vector bestTarget{};
    Vector bestAngle{};
    int bestIndex{ -1 };
    float bestSimulationTime = 0;

    const auto localPlayerEyePosition = localPlayer->getEyePosition();
    const auto aimPunch = (!activeWeapon->isKnife() && !activeWeapon->isBomb() &&
        !activeWeapon->isGrenade()) ? localPlayer->getAimPunch() : Vector{};

    std::array<bool, Hitboxes::Max> hitbox{ false };
    auto headshotonly = interfaces->cvar->findVar(skCrypt("mp_damage_headshot_only"));

    // Hitbox setup
    hitbox[Hitboxes::Head] = config->forceBaim.isActive() && headshotonly->getInt() < 1 ?
        false : (cfg1[weaponIndex].hitboxes & (1 << 0)) == (1 << 0);

    if (headshotonly->getInt() < 1)
    {
        hitbox[Hitboxes::UpperChest] = (cfg1[weaponIndex].hitboxes & (1 << 1)) == (1 << 1);
        hitbox[Hitboxes::Thorax] = (cfg1[weaponIndex].hitboxes & (1 << 2)) == (1 << 2);
        hitbox[Hitboxes::LowerChest] = (cfg1[weaponIndex].hitboxes & (1 << 3)) == (1 << 3);
        hitbox[Hitboxes::Belly] = (cfg1[weaponIndex].hitboxes & (1 << 4)) == (1 << 4);
        hitbox[Hitboxes::Pelvis] = (cfg1[weaponIndex].hitboxes & (1 << 5)) == (1 << 5);

        hitbox[Hitboxes::RightUpperArm] = config->forceBaim.isActive() ?
            false : (cfg1[weaponIndex].hitboxes & (1 << 6)) == (1 << 6);
        hitbox[Hitboxes::RightForearm] = config->forceBaim.isActive() ?
            false : (cfg1[weaponIndex].hitboxes & (1 << 6)) == (1 << 6);
        hitbox[Hitboxes::LeftUpperArm] = config->forceBaim.isActive() ?
            false : (cfg1[weaponIndex].hitboxes & (1 << 6)) == (1 << 6);
        hitbox[Hitboxes::LeftForearm] = config->forceBaim.isActive() ?
            false : (cfg1[weaponIndex].hitboxes & (1 << 6)) == (1 << 6);

        hitbox[Hitboxes::RightCalf] = config->forceBaim.isActive() ?
            false : (cfg1[weaponIndex].hitboxes & (1 << 7)) == (1 << 7);
        hitbox[Hitboxes::RightThigh] = config->forceBaim.isActive() ?
            false : (cfg1[weaponIndex].hitboxes & (1 << 7)) == (1 << 7);
        hitbox[Hitboxes::LeftCalf] = config->forceBaim.isActive() ?
            false : (cfg1[weaponIndex].hitboxes & (1 << 7)) == (1 << 7);
        hitbox[Hitboxes::LeftThigh] = config->forceBaim.isActive() ?
            false : (cfg1[weaponIndex].hitboxes & (1 << 7)) == (1 << 7);

        hitbox[Hitboxes::RightFoot] = config->forceBaim.isActive() ?
            false : (cfg1[weaponIndex].hitboxes & (1 << 8)) == (1 << 8);
        hitbox[Hitboxes::LeftFoot] = config->forceBaim.isActive() ?
            false : (cfg1[weaponIndex].hitboxes & (1 << 8)) == (1 << 8);
    }


    std::vector<Ragebot::Enemies> enemies;
    const auto& localPlayerOrigin = localPlayer->getAbsOrigin();

    for (int i = 1; i <= interfaces->engine->getMaxClients(); ++i)
    {
        const auto player = Animations::getPlayer(i);
        if (!player.gotMatrix)
            continue;

        const auto entity = interfaces->entityList->getEntity(i);
        if (!entity || entity == localPlayer.get() || entity->isDormant() || !entity->isAlive() ||
            (!entity->isOtherEnemy(localPlayer.get()) && !cfg.friendlyFire) || entity->gunGameImmunity())
            continue;

        const auto angle = AimbotFunction::calculateRelativeAngle(
            localPlayerEyePosition,
            player.matrix[8].origin(),
            cmd->viewangles + aimPunch
        );

        const auto& origin = entity->getAbsOrigin();
        const auto fov = angle.length2D();
        const auto health = entity->health();
        const auto distance = localPlayerOrigin.distTo(origin);

        enemies.emplace_back(i, health, distance, fov);
    }

    if (enemies.empty())
        return;


    switch (cfg.priority)
    {
    case 0:
        std::sort(enemies.begin(), enemies.end(), healthSort);
        break;
    case 1:
        std::sort(enemies.begin(), enemies.end(), distanceSort);
        break;
    case 2:
        std::sort(enemies.begin(), enemies.end(), fovSort);
        break;
    default:
        break;
    }

    auto multiPointHead = cfg1[weaponIndex].multiPointHead;
    auto multiPointBody = cfg1[weaponIndex].multiPointBody;


    for (const auto& target : enemies)
    {
        auto entity = interfaces->entityList->getEntity(target.id);
        auto player = Animations::getPlayer(target.id);
        int minDamage = std::clamp(
            config->minDamageOverrideKey.isActive() && cfg1[weaponIndex].dmgov ?
            cfg1[weaponIndex].minDamageOverride : cfg1[weaponIndex].minDamage,
            0,
            target.health + 1
        );

        matrix3x4* backupBoneCache = entity->getBoneCache().memory;
        Vector backupMins = entity->getCollideable()->obbMins();
        Vector backupMaxs = entity->getCollideable()->obbMaxs();
        Vector backupOrigin = entity->getAbsOrigin();
        Vector backupAbsAngle = entity->getAbsAngle();

        for (size_t cycle = 0; cycle < 2; cycle++)
        {
            /*auto update = player_records[cycle].empty() ||
                entity->simulationTime() != entity->oldSimulationTime();

            if (update && !player_records[cycle].empty())
            {
                if (TIME_TO_TICKS(entity->oldSimulationTime()) &&
                    TIME_TO_TICKS(entity->simulationTime()))
                {
                    auto layer = &entity->get_animlayers()[11];
                    auto previous_layer = &player_records[cycle].front().layers[11];

                    if (layer->cycle == previous_layer->cycle)
                    {
                        entity->simulationTime() = entity->oldSimulationTime();
                        update = false;
                    }
                }
            }*/

            float currentSimulationTime = -1.0f;

            if (config->backtrack.enabled)
            {
                const auto records = Animations::getBacktrackRecords(entity->index());
                if (!records || records->empty())
                    continue;

                int bestTick = -1;

                if (cycle == 0)
                {

                    for (size_t i = 0; i < records->size(); i++)
                    {
                        if (Backtrack::valid(records->at(i).simulationTime))
                        {
                            bestTick = static_cast<int>(i);
                            break;
                        }
                    }
                }
                else
                {

                    for (int i = static_cast<int>(records->size() - 1U); i >= 0; i--)
                    {
                        if (Backtrack::valid(records->at(i).simulationTime))
                        {
                            bestTick = i;
                            break;
                        }
                    }
                }

                if (bestTick <= -1)
                    continue;


                memcpy(entity->getBoneCache().memory, records->at(bestTick).matrix,
                    std::clamp(entity->getBoneCache().size, 0, MAXSTUDIOBONES) * sizeof(matrix3x4));
                memory->setAbsOrigin(entity, records->at(bestTick).origin);
                memory->setAbsAngle(entity, Vector{ 0.f, records->at(bestTick).absAngle.y, 0.f });
                memory->setCollisionBounds(entity->getCollideable(),
                    records->at(bestTick).mins,
                    records->at(bestTick).maxs);

                currentSimulationTime = records->at(bestTick).simulationTime;
            }
            else
            {

                if (cycle == 1)
                    continue;

                memcpy(entity->getBoneCache().memory, player.matrix.data(),
                    std::clamp(entity->getBoneCache().size, 0, MAXSTUDIOBONES) * sizeof(matrix3x4));
                memory->setAbsOrigin(entity, player.origin);
                memory->setAbsAngle(entity, Vector{ 0.f, player.absAngle.y, 0.f });
                memory->setCollisionBounds(entity->getCollideable(), player.mins, player.maxs);

                currentSimulationTime = player.simulationTime;
            }

            // Resolver
            //Resolver::resolve_shot(const_cast<Animations::Players&>(player), entity);

            runRagebot(
                cmd,
                entity,
                entity->getBoneCache().memory,
                target,
                hitbox,
                activeWeapon,
                weaponIndex,
                localPlayerEyePosition,
                aimPunch,
                multiPointHead,
                multiPointBody,
                minDamage,
                damageDiff,
                bestAngle,
                bestTarget
            );

            // Restore �������
            resetMatrix(entity, backupBoneCache, backupOrigin, backupAbsAngle, backupMins, backupMaxs);

            if (bestTarget.notNull())
            {
                bestSimulationTime = currentSimulationTime;
                bestIndex = target.id;
                break;
            }
        }

        if (bestTarget.notNull())
            break;
    }


    if (bestTarget.notNull())
    {
        static Vector lastAngles{ cmd->viewangles };
        static int lastCommand{};

        if (lastCommand == cmd->commandNumber - 1 && lastAngles.notNull() && config->ragebot.silent)
            cmd->viewangles = lastAngles;

        Vector angle = AimbotFunction::calculateRelativeAngle(
            localPlayerEyePosition,
            bestTarget,
            cmd->viewangles + aimPunch
        );

        bool clamped{ false };

        if (std::abs(angle.x) > 90 || std::abs(angle.y) > 180)
        {
            angle.x = std::clamp(angle.x, -90.0f, 90.0f);
            angle.y = std::clamp(angle.y, -180.f, 180.f);
            clamped = true;
        }

        if (activeWeapon->nextPrimaryAttack() <= memory->globalVars->serverTime())
        {
            cmd->viewangles += angle;

            if (!config->ragebot.silent)
                interfaces->engine->setViewAngles(cmd->viewangles);

            if (config->ragebot.autoShot && !clamped)
                cmd->buttons |= UserCmd::IN_ATTACK;

            if (config->ragebot.autoScope && activeWeapon->isSniperRifle() &&
                !localPlayer->isScoped() && !activeWeapon->zoomLevel())
                cmd->buttons |= UserCmd::IN_ZOOM;
        }

        if (clamped)
            cmd->buttons &= ~UserCmd::IN_ATTACK;

        if (cmd->buttons & UserCmd::IN_ATTACK)
        {
            cmd->tickCount = timeToTicks(bestSimulationTime + Backtrack::getLerp());

            //Resolver::saveRecord(bestIndex, bestSimulationTime);
            std::string debug = Resolver::getDebugInfo(bestIndex);
            Logger::addLog(debug);
            Resolver::processMissedShots();
        }

        if (clamped)
            lastAngles = cmd->viewangles;
        else
            lastAngles = Vector{};

        lastCommand = cmd->commandNumber;
    }
}
