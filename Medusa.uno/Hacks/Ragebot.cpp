#include "../Config.h"
#include "../Interfaces.h"
#include "../Memory.h"

#include "AimbotFunctions.h"
#include "Animations.h"
#include "Backtrack.h"
#include "Ragebot.h"
#include "EnginePrediction.h"
#include "Resolver.h"
#include "Tickbase.h"
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
#include <algorithm>
#include <cmath>
#include "../xor.h"
#include "../Logger.h"
#include "../VectorSIMD.h"

#define TICK_INTERVAL (memory->globalVars->intervalPerTick)
#define TIME_TO_TICKS(dt) ((int)(0.5f + (float)(dt) / TICK_INTERVAL))
#define TICKS_TO_TIME(t) (TICK_INTERVAL * (float)(t))

// ============================================
// HELPER FUNCTIONS
// ============================================

namespace {
    std::unordered_map<int, Ragebot::TargetMovement> targetMovements;

    void updateTargetMovement(Entity* target) noexcept {
        const auto index = target->index();
        const auto currentPos = target->getAbsOrigin();
        const auto currentTime = target->simulationTime();

        if (targetMovements.find(index) != targetMovements.end()) {
            auto& record = targetMovements[index];
            const float deltaTime = currentTime - record.simulationTime;

            if (deltaTime > 0.0f) {
                record.velocity = (currentPos - target->getAbsOrigin()) / deltaTime;
                record.simulationTime = currentTime;
            }
        }
        else {
            targetMovements[index] = { Vector{0.f, 0.f, 0.f}, currentTime };
        }
    }

    Vector predictTargetPosition(Entity* target, float predictionTime) noexcept {
        const auto index = target->index();
        if (targetMovements.find(index) == targetMovements.end())
            return target->getAbsOrigin();

        const auto& movement = targetMovements[index];
        return target->getAbsOrigin() + movement.velocity * predictionTime;
    }

    int getHitgroup(int hitbox) noexcept {
        switch (hitbox) {
        case Hitboxes::Head:
            return 1; // HITGROUP_HEAD
        case Hitboxes::UpperChest:
        case Hitboxes::Thorax:
        case Hitboxes::LowerChest:
            return 2; // HITGROUP_CHEST
        case Hitboxes::Belly:
        case Hitboxes::Pelvis:
            return 3; // HITGROUP_STOMACH
        case Hitboxes::RightUpperArm:
        case Hitboxes::RightForearm:
            return 4; // HITGROUP_RIGHTARM
        case Hitboxes::LeftUpperArm:
        case Hitboxes::LeftForearm:
            return 5; // HITGROUP_LEFTARM
        case Hitboxes::RightThigh:
        case Hitboxes::RightCalf:
            return 6; // HITGROUP_RIGHTLEG
        case Hitboxes::LeftThigh:
        case Hitboxes::LeftCalf:
            return 7; // HITGROUP_LEFTLEG
        default:
            return 0;
        }
    }

    float getHitgroupDamageMultiplier(int hitgroup) noexcept {
        switch (hitgroup) {
        case 1: return 4.0f;  // Head
        case 3: return 1.25f; // Stomach
        case 4: case 5: return 1.0f; // Arms
        case 6: case 7: return 0.75f; // Legs
        default: return 1.0f;
        }
    }
}

// ============================================
// INPUT HANDLING
// ============================================

void Ragebot::updateInput() noexcept {
    config->ragebotKey.handleToggle();
    config->hitchanceOverride.handleToggle();
    config->minDamageOverrideKey.handleToggle();
    config->forceBaim.handleToggle();

    // === НОВОЕ: Force Shot Key ===
    config->forceShotKey.handleToggle();
    forceShotActive = config->forceShotKey.isActive();
}

void Ragebot::reset() noexcept {
    delayShotData.reset();
    forceShotActive = false;
    targetMovements.clear();
    currentTick = 0;
}

// ============================================
// WEAPON INDEX
// ============================================

int Ragebot::getWeaponIndex(WeaponId weaponId) noexcept {
    const auto& cfg1 = config->rageBot;

    switch (weaponId) {
    case WeaponId::Elite:
    case WeaponId::Hkp2000:
    case WeaponId::P250:
    case WeaponId::Usp_s:
    case WeaponId::Cz75a:
    case WeaponId::Tec9:
    case WeaponId::Fiveseven:
    case WeaponId::Glock:
        return cfg1[1].enabled ? 1 : 0;

    case WeaponId::Deagle:
        return cfg1[2].enabled ? 2 : 0;

    case WeaponId::Revolver:
        return cfg1[3].enabled ? 3 : 0;

    case WeaponId::Mac10:
    case WeaponId::Mp9:
    case WeaponId::Mp7:
    case WeaponId::Mp5sd:
    case WeaponId::Ump45:
    case WeaponId::P90:
    case WeaponId::Bizon:
        return cfg1[4].enabled ? 4 : 0;

    case WeaponId::Ak47:
    case WeaponId::M4A1:
    case WeaponId::M4a1_s:
    case WeaponId::GalilAr:
    case WeaponId::Aug:
    case WeaponId::Sg553:
    case WeaponId::Famas:
        return cfg1[5].enabled ? 5 : 0;

    case WeaponId::Ssg08:
        return cfg1[6].enabled ? 6 : 0;

    case WeaponId::Awp:
        return cfg1[7].enabled ? 7 : 0;

    case WeaponId::G3SG1:
    case WeaponId::Scar20:
        return cfg1[8].enabled ? 8 : 0;

    case WeaponId::Taser:
        return cfg1[9].enabled ? 9 : 0;

    default:
        return 0;
    }
}

// ============================================
// MATRIX RESET
// ============================================

void Ragebot::resetMatrix(Entity* entity, matrix3x4* backup, const Vector& origin,
    const Vector& absAngle, const Vector& mins, const Vector& maxs) noexcept {

    memcpy(entity->getBoneCache().memory, backup,
        std::clamp(entity->getBoneCache().size, 0, MAXSTUDIOBONES) * sizeof(matrix3x4));
    memory->setAbsOrigin(entity, origin);
    memory->setAbsAngle(entity, absAngle);
    memory->setCollisionBounds(entity->getCollideable(), mins, maxs);
}

// ============================================
// HITBOX PRIORITIZATION
// ============================================

float Ragebot::getHitboxBasePriority(int hitbox, int weaponIndex) noexcept {
    const auto& cfg1 = config->rageBot[weaponIndex];

    // Base priorities (higher = better)
    switch (hitbox) {
    case Hitboxes::Head:
        return 70.f;
    case Hitboxes::UpperChest:
    case Hitboxes::Thorax:
        return 80.f;
    case Hitboxes::LowerChest:
        return 72.f;
    case Hitboxes::Belly:
        return 75.f;
    case Hitboxes::Pelvis:
        return 71.f;
    case Hitboxes::RightUpperArm:
    case Hitboxes::LeftUpperArm:
        return 40.f;
    case Hitboxes::RightForearm:
    case Hitboxes::LeftForearm:
        return 32.f;
    case Hitboxes::RightThigh:
    case Hitboxes::LeftThigh:
        return 50.f;
    case Hitboxes::RightCalf:
    case Hitboxes::LeftCalf:
        return 45.f;
    case Hitboxes::RightFoot:
    case Hitboxes::LeftFoot:
        return 30.f;
    default:
        return 10.f;
    }
}

bool Ragebot::isLethalHitbox(int hitbox, Entity* entity, Entity* weapon) noexcept {
    if (!entity || !weapon)
        return false;

    const auto weaponData = weapon->getWeaponData();
    if (!weaponData)
        return false;

    float damage = static_cast<float>(weaponData->damage);
    float multiplier = getHitgroupDamageMultiplier(getHitgroup(hitbox));
    float potentialDamage = damage * multiplier;

    // Account for armor
    if (entity->armor() > 0) {
        bool hasHelmet = entity->hasHelmet();
        if (hitbox == Hitboxes::Head && hasHelmet) {
            potentialDamage *= 0.5f; // Rough helmet reduction
        }
        else if (hitbox != Hitboxes::Head) {
            potentialDamage *= 0.5f; // Rough armor reduction
        }
    }

    return potentialDamage >= entity->health();
}

std::vector<Ragebot::HitboxPriority> Ragebot::calculateHitboxPriorities(
    Entity* entity, Entity* weapon, int minDamage, int weaponIndex) noexcept {

    std::vector<HitboxPriority> priorities;
    const auto& cfg1 = config->rageBot[weaponIndex];

    for (int i = 0; i < Hitboxes::Max; i++) {
        HitboxPriority hp;
        hp.hitbox = i;
        hp.basePriority = getHitboxBasePriority(i, weaponIndex);
        hp.damageMultiplier = getHitgroupDamageMultiplier(getHitgroup(i));
        hp.isLethal = isLethalHitbox(i, entity, weapon);
        hp.isPreferred = false;

        // Check if hitbox is enabled in config
        switch (i) {
        case Hitboxes::Head:
            hp.isPreferred = (cfg1.hitboxes & (1 << 0)) != 0;
            break;
        case Hitboxes::UpperChest:
            hp.isPreferred = (cfg1.hitboxes & (1 << 1)) != 0;
            break;
        case Hitboxes::Thorax:
            hp.isPreferred = (cfg1.hitboxes & (1 << 2)) != 0;
            break;
        case Hitboxes::LowerChest:
            hp.isPreferred = (cfg1.hitboxes & (1 << 3)) != 0;
            break;
        case Hitboxes::Belly:
            hp.isPreferred = (cfg1.hitboxes & (1 << 4)) != 0;
            break;
        case Hitboxes::Pelvis:
            hp.isPreferred = (cfg1.hitboxes & (1 << 5)) != 0;
            break;
        case Hitboxes::RightUpperArm:
        case Hitboxes::RightForearm:
        case Hitboxes::LeftUpperArm:
        case Hitboxes::LeftForearm:
            hp.isPreferred = (cfg1.hitboxes & (1 << 6)) != 0;
            break;
        case Hitboxes::RightThigh:
        case Hitboxes::RightCalf:
        case Hitboxes::LeftThigh:
        case Hitboxes::LeftCalf:
            hp.isPreferred = (cfg1.hitboxes & (1 << 7)) != 0;
            break;
        case Hitboxes::RightFoot:
        case Hitboxes::LeftFoot:
            hp.isPreferred = (cfg1.hitboxes & (1 << 8)) != 0;
            break;
        }

        // Boost priority for lethal hitboxes
        if (hp.isLethal) {
            hp.basePriority *= 1.5f;
        }

        if (hp.isPreferred) {
            priorities.push_back(hp);
        }
    }

 
    constexpr size_t TOP_HITBOXES = 5;

    if (priorities.size() > TOP_HITBOXES) {
        std::partial_sort(priorities.begin(),
            priorities.begin() + TOP_HITBOXES,
            priorities.end(),
            [](const HitboxPriority& a, const HitboxPriority& b) {
                return a.basePriority > b.basePriority;
            });
        priorities.resize(TOP_HITBOXES);
    }
    else {
        std::sort(priorities.begin(), priorities.end(),
            [](const HitboxPriority& a, const HitboxPriority& b) {
                return a.basePriority > b.basePriority;
            });
    }

    return priorities;
}

// ============================================
// SAFE POINTS
// ============================================

bool Ragebot::isSafePoint(Entity* entity, const Vector& point, int hitbox,
    matrix3x4* matrix, StudioHitboxSet* set) noexcept {

    if (!set)
        return false;

    StudioBbox* bbox = set->getHitbox(hitbox);
    if (!bbox)
        return false;

    // Get hitbox center
    Vector mins = bbox->bbMin;
    Vector maxs = bbox->bbMax;
    Vector center = (mins + maxs) * 0.5f;

    // Transform to world space
    Vector worldCenter;
    worldCenter.x = matrix[bbox->bone][0][0] * center.x + matrix[bbox->bone][0][1] * center.y +
        matrix[bbox->bone][0][2] * center.z + matrix[bbox->bone][0][3];
    worldCenter.y = matrix[bbox->bone][1][0] * center.x + matrix[bbox->bone][1][1] * center.y +
        matrix[bbox->bone][1][2] * center.z + matrix[bbox->bone][1][3];
    worldCenter.z = matrix[bbox->bone][2][0] * center.x + matrix[bbox->bone][2][1] * center.y +
        matrix[bbox->bone][2][2] * center.z + matrix[bbox->bone][2][3];

    // Calculate hitbox radius
    float radius = (maxs - mins).length() * 0.5f;

    // Point is safe if it's within threshold of center
    float distanceFromCenter = point.distTo(worldCenter);

    return distanceFromCenter <= radius * SAFE_POINT_THRESHOLD;
}

std::vector<Vector> Ragebot::getSafePoints(Entity* entity, int hitbox,
    matrix3x4* matrix, StudioHitboxSet* set, const Vector& eyePos) noexcept {

    std::vector<Vector> safePoints;

    if (!set)
        return safePoints;

    StudioBbox* bbox = set->getHitbox(hitbox);
    if (!bbox)
        return safePoints;

    Vector mins = bbox->bbMin;
    Vector maxs = bbox->bbMax;
    Vector center = (mins + maxs) * 0.5f;

    // Transform center to world space
    Vector worldCenter;
    worldCenter.x = matrix[bbox->bone][0][0] * center.x + matrix[bbox->bone][0][1] * center.y +
        matrix[bbox->bone][0][2] * center.z + matrix[bbox->bone][0][3];
    worldCenter.y = matrix[bbox->bone][1][0] * center.x + matrix[bbox->bone][1][1] * center.y +
        matrix[bbox->bone][1][2] * center.z + matrix[bbox->bone][1][3];
    worldCenter.z = matrix[bbox->bone][2][0] * center.x + matrix[bbox->bone][2][1] * center.y +
        matrix[bbox->bone][2][2] * center.z + matrix[bbox->bone][2][3];

    // Center is always a safe point
    safePoints.push_back(worldCenter);

    // Add points close to center (safe zone)
    float radius = (maxs - mins).length() * 0.5f;
    float safeRadius = radius * 0.3f; // 30% of radius is definitely safe

    // Generate safe points in a cross pattern
    Vector directions[] = {
        Vector(1, 0, 0), Vector(-1, 0, 0),
        Vector(0, 1, 0), Vector(0, -1, 0),
        Vector(0, 0, 1), Vector(0, 0, -1)
    };

    for (const auto& dir : directions) {
        Vector safePoint = worldCenter + dir * safeRadius;

        // Verify it's still inside hitbox with trace
        Trace trace;
        interfaces->engineTrace->traceRay(
            { eyePos, safePoint },
            MASK_SHOT, { localPlayer.get() }, trace
        );

        if (trace.entity == entity) {
            safePoints.push_back(safePoint);
        }
    }

    return safePoints;
}

std::vector<Vector> Ragebot::getUnsafePoints(Entity* entity, int hitbox,
    matrix3x4* matrix, StudioHitboxSet* set, const Vector& eyePos) noexcept {

    std::vector<Vector> unsafePoints;

    if (!set)
        return unsafePoints;

    StudioBbox* bbox = set->getHitbox(hitbox);
    if (!bbox)
        return unsafePoints;

    Vector mins = bbox->bbMin;
    Vector maxs = bbox->bbMax;
    Vector center = (mins + maxs) * 0.5f;
    float radius = (maxs - mins).length() * 0.5f;

    // Transform center
    Vector worldCenter;
    worldCenter.x = matrix[bbox->bone][0][0] * center.x + matrix[bbox->bone][0][1] * center.y +
        matrix[bbox->bone][0][2] * center.z + matrix[bbox->bone][0][3];
    worldCenter.y = matrix[bbox->bone][1][0] * center.x + matrix[bbox->bone][1][1] * center.y +
        matrix[bbox->bone][1][2] * center.z + matrix[bbox->bone][1][3];
    worldCenter.z = matrix[bbox->bone][2][0] * center.x + matrix[bbox->bone][2][1] * center.y +
        matrix[bbox->bone][2][2] * center.z + matrix[bbox->bone][2][3];

    // Generate edge points (unsafe but more damage potential on resolver misses)
    float edgeRadius = radius * 0.85f;

    Vector directions[] = {
        Vector(1, 0, 0), Vector(-1, 0, 0),
        Vector(0, 1, 0), Vector(0, -1, 0),
        Vector(0.707f, 0.707f, 0), Vector(-0.707f, 0.707f, 0),
        Vector(0.707f, -0.707f, 0), Vector(-0.707f, -0.707f, 0)
    };

    for (const auto& dir : directions) {
        Vector edgePoint = worldCenter + dir * edgeRadius;

        Trace trace;
        interfaces->engineTrace->traceRay(
            { eyePos, edgePoint },
            MASK_SHOT, { localPlayer.get() }, trace
        );

        if (trace.entity == entity) {
            unsafePoints.push_back(edgePoint);
        }
    }

    return unsafePoints;
}

bool Ragebot::hasMinimumSafePoints(Entity* entity, int hitbox,
    matrix3x4* matrix, StudioHitboxSet* set, const Vector& eyePos, int required) noexcept {

    auto safePoints = getSafePoints(entity, hitbox, matrix, set, eyePos);

    int validCount = 0;
    for (const auto& point : safePoints) {
        Trace trace;
        interfaces->engineTrace->traceRay(
            { eyePos, point },
            MASK_SHOT, { localPlayer.get() }, trace
        );

        if (trace.entity == entity && trace.hitgroup != 0) {
            validCount++;
            if (validCount >= required)
                return true;
        }
    }

    return false;
}

// ============================================
// AIM POINT GENERATION
// ============================================

float Ragebot::calculatePointPriority(const AimPoint& point, int weaponIndex,
    int targetHealth, bool preferSafe) noexcept {

    float priority = 0.f;

    // Base priority from hitbox
    priority += getHitboxBasePriority(point.hitbox, weaponIndex);

    // Damage factor (higher damage = higher priority)
    float damageFactor = point.damage / static_cast<float>(targetHealth);
    priority += damageFactor * 50.f;

    // Lethal shot bonus
    if (point.damage >= targetHealth) {
        priority += 100.f;
    }

    // Hitchance factor
    priority += point.hitchance * 0.5f;

    // Safe point bonus/penalty
    if (preferSafe) {
        if (point.isSafe) {
            priority += 30.f;
        }
        else {
            priority -= 20.f;
        }
    }

    // Multipoint penalty (center is usually more reliable)
    if (point.isMultipoint) {
        priority -= 5.f;
    }

    // Distance from center factor
    priority -= point.distanceFromCenter * 0.1f;

    return priority;
}

std::vector<Ragebot::AimPoint> Ragebot::generateAimPoints(
    Entity* entity, matrix3x4* matrix, StudioHitboxSet* set,
    const Vector& eyePos, Entity* weapon, int weaponIndex,
    const std::array<bool, Hitboxes::Max>& enabledHitboxes,
    int minDamage, int multiPointHead, int multiPointBody) noexcept {

    std::vector<AimPoint> points;
    const auto& cfg = config->ragebot;
    const auto& cfg1 = config->rageBot[weaponIndex];

    if (!set || !weapon)
        return points;

    const auto weaponData = weapon->getWeaponData();
    if (!weaponData)
        return points;

    float distance = VectorSIMD::distanceFast(localPlayer->getAbsOrigin(), entity->getAbsOrigin());

    // Уменьшаем количество точек для дальних целей
    float distanceScale = 1.0f;
    if (distance > 3000.f) { // > 75м
        distanceScale = 0.5f; // 50% точек
    }
    else if (distance > 2000.f) { // > 50м
        distanceScale = 0.75f; // 75% точек
    }

    int adjustedMPHead = static_cast<int>(multiPointHead * distanceScale);
    int adjustedMPBody = static_cast<int>(multiPointBody * distanceScale);

    // Минимум 1 точка (центр хитбокса)
    adjustedMPHead = (std::max)(1, adjustedMPHead);
    adjustedMPBody = (std::max)(1, adjustedMPBody);


    constexpr int MAX_TOTAL_POINTS = 64;
    int totalPointsGenerated = 0;

    // Get hitbox priorities
    auto priorities = calculateHitboxPriorities(entity, weapon, minDamage, weaponIndex);

    for (const auto& hp : priorities) {
        if (totalPointsGenerated >= MAX_TOTAL_POINTS) {
            break;
        }

        if (!enabledHitboxes[hp.hitbox])
            continue;

        StudioBbox* bbox = set->getHitbox(hp.hitbox);
        if (!bbox)
            continue;


        int mpScale = (hp.hitbox == Hitboxes::Head) ? adjustedMPHead : adjustedMPBody;

        // Generate points for this hitbox
        auto hitboxPoints = AimbotFunction::multiPoint(entity, matrix, bbox, eyePos,
            hp.hitbox, adjustedMPHead, adjustedMPBody);

        int maxPointsFromHitbox = (std::min)(static_cast<int>(hitboxPoints.size()),
            MAX_TOTAL_POINTS - totalPointsGenerated);

        // Get safe points for comparison
        auto safePointsList = getSafePoints(entity, hp.hitbox, matrix, set, eyePos);

        for (int idx = 0; idx < maxPointsFromHitbox; idx++) {
            const auto& pos = hitboxPoints[idx];

            AimPoint ap;
            ap.position = pos;
            ap.hitbox = hp.hitbox;
            ap.hitgroup = getHitgroup(hp.hitbox);

            // Calculate damage
            ap.damage = AimbotFunction::getScanDamage(entity, pos, weaponData,
                minDamage, cfg.friendlyFire);

            if (ap.damage < minDamage)
                continue;

            // Check if this is a safe point
            ap.isSafe = isSafePoint(entity, pos, hp.hitbox, matrix, set);
            ap.isMultipoint = (idx > 0); // Первая точка - центр

            // Calculate distance from center
            if (!safePointsList.empty()) {
                ap.distanceFromCenter = VectorSIMD::distanceFast(pos, safePointsList[0]);
            }
            else {
                ap.distanceFromCenter = 0.f;
            }

            ap.hitchance = 0.f; // Will be calculated properly later

            // Calculate priority
            ap.priority = calculatePointPriority(ap, weaponIndex, entity->health(),
                cfg1.preferSafePoints);

            points.push_back(ap);
            totalPointsGenerated++;
        }

        // Unsafe hitbox points
        if (cfg1.unsafeHitbox && totalPointsGenerated < MAX_TOTAL_POINTS) {
            auto unsafePointsList = getUnsafePoints(entity, hp.hitbox, matrix, set, eyePos);

            int maxUnsafe = (std::min)(static_cast<int>(unsafePointsList.size()),
                MAX_TOTAL_POINTS - totalPointsGenerated);

            for (int idx = 0; idx < maxUnsafe; idx++) {
                const auto& pos = unsafePointsList[idx];

                AimPoint ap;
                ap.position = pos;
                ap.hitbox = hp.hitbox;
                ap.hitgroup = getHitgroup(hp.hitbox);

                ap.damage = AimbotFunction::getScanDamage(entity, pos, weaponData,
                    minDamage, cfg.friendlyFire);

                if (ap.damage < minDamage)
                    continue;

                ap.isSafe = false;
                ap.isMultipoint = true;
                ap.distanceFromCenter = safePointsList.empty() ? 0.f : pos.distTo(safePointsList[0]);
                ap.hitchance = 0.f;

                // Lower priority for unsafe points
                ap.priority = calculatePointPriority(ap, weaponIndex, entity->health(), false) * 0.7f;

                points.push_back(ap);
                totalPointsGenerated++;
            }
        }
    }

    // Нам нужны только топ-10 лучших точек
    constexpr int TOP_POINTS = 10;
    if (points.size() > TOP_POINTS) {
        std::partial_sort(points.begin(), points.begin() + TOP_POINTS, points.end(),
            [](const AimPoint& a, const AimPoint& b) {
                return a.priority > b.priority;
            });
        points.resize(TOP_POINTS); // Отбрасываем остальные
    }
    else {
        std::sort(points.begin(), points.end());
    }

    return points;
}

Ragebot::AimPoint Ragebot::selectBestAimPoint(
    const std::vector<AimPoint>& points,
    int weaponIndex, bool preferSafe, bool forceSafe) noexcept {

    if (points.empty())
        return AimPoint{};

    // If force safe, only consider safe points
    if (forceSafe) {
        for (const auto& point : points) {
            if (point.isSafe) {
                return point;
            }
        }
        // No safe points found
        return AimPoint{};
    }

    // If prefer safe, try safe points first but fall back to unsafe
    if (preferSafe) {
        for (const auto& point : points) {
            if (point.isSafe) {
                return point;
            }
        }
    }

    // Return highest priority point
    return points[0];
}

// ============================================
// DELAY SHOT
// ============================================

bool Ragebot::shouldDelayShot(UserCmd* cmd, const TargetData& target, int weaponIndex) noexcept {
    const auto& cfg1 = config->rageBot[weaponIndex];

    // Force shot overrides delay
    if (forceShotActive)
        return false;

    // Check if delay shot is enabled
    if (!cfg1.delayShot)
        return false;

    // Don't delay if we have a lethal shot
    if (target.hasValidPoint && target.bestPoint.damage >= target.health)
        return false;

    // Don't delay if already delaying too long
    if (delayShotData.isDelaying && delayShotData.delayTicks >= cfg1.delayShotTicks)
        return false;

    // Don't delay if hitchance is already at maximum
    if (target.hasValidPoint && target.bestPoint.hitchance >= 95.f)
        return false;

    return true;
}

void Ragebot::updateDelayShot(UserCmd* cmd, const TargetData& target, int weaponIndex) noexcept {
    const auto& cfg1 = config->rageBot[weaponIndex];

    if (!delayShotData.isDelaying) {
        // Start delay
        delayShotData.isDelaying = true;
        delayShotData.delayStartTick = currentTick;
        delayShotData.delayTicks = 0;
        delayShotData.maxDelayTicks = cfg1.delayShotTicks;
        delayShotData.bestTarget = target;
        delayShotData.bestDamage = target.hasValidPoint ? target.bestPoint.damage : 0.f;
        delayShotData.bestHitchance = target.hasValidPoint ? target.bestPoint.hitchance : 0.f;

        delayShotData.damageHistory.clear();
        delayShotData.hitchanceHistory.clear();
    }

    delayShotData.delayTicks = currentTick - delayShotData.delayStartTick;

    // Store current frame data
    if (target.hasValidPoint) {
        delayShotData.damageHistory.push_back({ currentTick, target.bestPoint.damage });
        delayShotData.hitchanceHistory.push_back({ currentTick, target.bestPoint.hitchance });

        // Keep history limited
        while (delayShotData.damageHistory.size() > 16)
            delayShotData.damageHistory.pop_front();
        while (delayShotData.hitchanceHistory.size() > 16)
            delayShotData.hitchanceHistory.pop_front();

        // Update best if current is better
        bool isBetter = false;

        // Prioritize lethal damage
        if (target.bestPoint.damage >= target.health &&
            delayShotData.bestDamage < target.health) {
            isBetter = true;
        }
        // Then prioritize higher hitchance
        else if (target.bestPoint.hitchance > delayShotData.bestHitchance + 5.f) {
            isBetter = true;
        }
        // Then prioritize higher damage
        else if (target.bestPoint.damage > delayShotData.bestDamage + 10.f &&
            target.bestPoint.hitchance >= delayShotData.bestHitchance - 5.f) {
            isBetter = true;
        }

        if (isBetter) {
            delayShotData.bestTarget = target;
            delayShotData.bestDamage = target.bestPoint.damage;
            delayShotData.bestHitchance = target.bestPoint.hitchance;
        }
    }
}

bool Ragebot::isDelayShotReady() noexcept {
    if (!delayShotData.isDelaying)
        return false;

    // Force shot overrides
    if (forceShotActive)
        return true;

    // Max delay reached
    if (delayShotData.delayTicks >= delayShotData.maxDelayTicks)
        return true;

    // Check if we found a significantly better shot
    if (delayShotData.bestTarget.hasValidPoint) {
        // Lethal shot found
        if (delayShotData.bestDamage >= delayShotData.bestTarget.health)
            return true;

        // Very high hitchance
        if (delayShotData.bestHitchance >= 95.f)
            return true;

        // Hitchance improved significantly
        if (!delayShotData.hitchanceHistory.empty()) {
            float initialHitchance = delayShotData.hitchanceHistory.front().second;
            if (delayShotData.bestHitchance >= initialHitchance + 15.f)
                return true;
        }
    }

    return false;
}

void Ragebot::executeDelayShot(UserCmd* cmd) noexcept {
    // Log delay shot execution
    std::ostringstream log;
    log << "[Delay Shot] Executed after " << delayShotData.delayTicks << " ticks";
    log << " | DMG: " << delayShotData.bestDamage;
    log << " | HC: " << static_cast<int>(delayShotData.bestHitchance) << "%";
    Logger::addLog(log.str());

    delayShotData.reset();
}

// ============================================
// FORCE SHOT
// ============================================

bool Ragebot::isForceShotActive() noexcept {
    return forceShotActive;
}

void Ragebot::handleForceShot(UserCmd* cmd, const TargetData& target) noexcept {
    // Force shot ignores hitchance requirements
    // Just shoot at the best available point

    if (!target.hasValidPoint)
        return;

    // Log force shot
    std::ostringstream log;
    log << "[Force Shot] Target: " << target.entity->getPlayerName();
    log << " | Hitbox: " << target.bestPoint.hitbox;
    log << " | DMG: " << target.bestPoint.damage;
    Logger::addLog(log.str());
}

// ============================================
// TARGET COLLECTION
// ============================================

std::vector<Ragebot::TargetData> Ragebot::collectTargets(UserCmd* cmd, Entity* weapon,
    int weaponIndex, const Vector& eyePos, const Vector& aimPunch) noexcept {

    std::vector<TargetData> targets;
    const auto& cfg = config->ragebot;
    const auto& cfg1 = config->rageBot[weaponIndex];

    const auto& localPlayerOrigin = localPlayer->getAbsOrigin();

    bool foundPerfectShot = false;

    for (int i = 1; i <= interfaces->engine->getMaxClients(); ++i) {
        if (foundPerfectShot && !targets.empty()) {
            break;
        }

        const auto player = Animations::getPlayer(i);
        if (!player.gotMatrix)
            continue;

        const auto entity = interfaces->entityList->getEntity(i);
        if (!entity || entity == localPlayer.get() || entity->isDormant() || !entity->isAlive())
            continue;

        if (!entity->isOtherEnemy(localPlayer.get()) && !cfg.friendlyFire)
            continue;

        if (entity->gunGameImmunity())
            continue;

        const Model* model = entity->getModel();
        if (!model)
            continue;

        StudioHdr* hdr = interfaces->modelInfo->getStudioModel(model);
        if (!hdr)
            continue;

        StudioHitboxSet* set = hdr->getHitboxSet(0);
        if (!set)
            continue;

        TargetData targetData;
        targetData.entity = entity;
        targetData.index = i;
        targetData.health = entity->health();
        targetData.distance = VectorSIMD::distanceFast(localPlayerOrigin, entity->getAbsOrigin());


        if (targetData.distance > 6000.f) {
            continue;
        }

        const auto angle = AimbotFunction::calculateRelativeAngle(eyePos,
            player.matrix[8].origin(), cmd->viewangles + aimPunch);
        targetData.fov = angle.length2D();

        if (targetData.fov > cfg.fov)
            continue;

        // Setup hitboxes
        std::array<bool, Hitboxes::Max> hitboxes{ false };
        auto headshotonly = interfaces->cvar->findVar(skCrypt("mp_damage_headshot_only"));

        hitboxes[Hitboxes::Head] = config->forceBaim.isActive() && headshotonly->getInt() < 1
            ? false : (cfg1.hitboxes & 1 << 0) == 1 << 0;

        if (headshotonly->getInt() < 1) {
            hitboxes[Hitboxes::UpperChest] = (cfg1.hitboxes & 1 << 1) == 1 << 1;
            hitboxes[Hitboxes::Thorax] = (cfg1.hitboxes & 1 << 2) == 1 << 2;
            hitboxes[Hitboxes::LowerChest] = (cfg1.hitboxes & 1 << 3) == 1 << 3;
            hitboxes[Hitboxes::Belly] = (cfg1.hitboxes & 1 << 4) == 1 << 4;
            hitboxes[Hitboxes::Pelvis] = (cfg1.hitboxes & 1 << 5) == 1 << 5;

            bool armsEnabled = !config->forceBaim.isActive() && (cfg1.hitboxes & 1 << 6) == 1 << 6;
            hitboxes[Hitboxes::RightUpperArm] = armsEnabled;
            hitboxes[Hitboxes::RightForearm] = armsEnabled;
            hitboxes[Hitboxes::LeftUpperArm] = armsEnabled;
            hitboxes[Hitboxes::LeftForearm] = armsEnabled;

            bool legsEnabled = !config->forceBaim.isActive() && (cfg1.hitboxes & 1 << 7) == 1 << 7;
            hitboxes[Hitboxes::RightCalf] = legsEnabled;
            hitboxes[Hitboxes::RightThigh] = legsEnabled;
            hitboxes[Hitboxes::LeftCalf] = legsEnabled;
            hitboxes[Hitboxes::LeftThigh] = legsEnabled;

            bool feetEnabled = !config->forceBaim.isActive() && (cfg1.hitboxes & 1 << 8) == 1 << 8;
            hitboxes[Hitboxes::RightFoot] = feetEnabled;
            hitboxes[Hitboxes::LeftFoot] = feetEnabled;
        }

        int minDamage = std::clamp(
            config->minDamageOverrideKey.isActive() && cfg1.dmgov
            ? cfg1.minDamageOverride : cfg1.minDamage,
            0, targetData.health + 1);

        // Process both current and backtrack positions
        matrix3x4* backupBoneCache = entity->getBoneCache().memory;
        Vector backupMins = entity->getCollideable()->obbMins();
        Vector backupMaxs = entity->getCollideable()->obbMaxs();
        Vector backupOrigin = entity->getAbsOrigin();
        Vector backupAbsAngle = entity->getAbsAngle();

        for (int cycle = 0; cycle < 2; cycle++) {
            float currentSimulationTime = -1.0f;
            bool isBacktrack = false;
            int backtrackTick = 0;

            if (config->backtrack.enabled && cycle == 1) {
                const auto records = Animations::getBacktrackRecords(entity->index());
                if (!records || records->empty())
                    continue;

                // Find best backtrack tick
                int bestTick = -1;
                for (int j = static_cast<int>(records->size() - 1); j >= 0; j--) {
                    if (Backtrack::valid(records->at(j).simulationTime)) {
                        bestTick = j;
                        break;
                    }
                }

                if (bestTick <= -1)
                    continue;

                memcpy(entity->getBoneCache().memory, records->at(bestTick).matrix,
                    std::clamp(entity->getBoneCache().size, 0, MAXSTUDIOBONES) * sizeof(matrix3x4));
                memory->setAbsOrigin(entity, records->at(bestTick).origin);
                memory->setAbsAngle(entity, Vector{ 0.f, records->at(bestTick).absAngle.y, 0.f });
                memory->setCollisionBounds(entity->getCollideable(),
                    records->at(bestTick).mins, records->at(bestTick).maxs);

                currentSimulationTime = records->at(bestTick).simulationTime;
                isBacktrack = true;
                backtrackTick = bestTick;
            }
            else if (cycle == 0) {
                memcpy(entity->getBoneCache().memory, player.matrix.data(),
                    std::clamp(entity->getBoneCache().size, 0, MAXSTUDIOBONES) * sizeof(matrix3x4));
                memory->setAbsOrigin(entity, player.origin);
                memory->setAbsAngle(entity, Vector{ 0.f, player.absAngle.y, 0.f });
                memory->setCollisionBounds(entity->getCollideable(), player.mins, player.maxs);

                currentSimulationTime = player.simulationTime;
            }
            else {
                continue;
            }

            // Generate aim points
            targetData.aimPoints = generateAimPoints(entity, entity->getBoneCache().memory, set,
                eyePos, weapon, weaponIndex, hitboxes, minDamage,
                cfg1.multiPointHead, cfg1.multiPointBody);

            // Select best point
            bool forceSafe = cfg1.forceSafePoints;
            bool preferSafe = cfg1.preferSafePoints;

            targetData.bestPoint = selectBestAimPoint(targetData.aimPoints, weaponIndex,
                preferSafe, forceSafe);
            targetData.hasValidPoint = (targetData.bestPoint.damage > 0);

            if (targetData.hasValidPoint) {
                targetData.simulationTime = currentSimulationTime;
                targetData.isBacktrack = isBacktrack;
                targetData.backtrackTick = backtrackTick;

                bool isLethalShot = targetData.bestPoint.damage >= targetData.health;
                bool isHighConfidence = targetData.bestPoint.hitchance >= 95.f;
                bool isSafeShot = targetData.bestPoint.isSafe || !cfg1.preferSafePoints;

                if (isLethalShot && isHighConfidence && isSafeShot) {
                    resetMatrix(entity, backupBoneCache, backupOrigin, backupAbsAngle,
                        backupMins, backupMaxs);

                    targets.clear(); 
                    targets.push_back(targetData);
                    foundPerfectShot = true;
                    break;
                }
            }

            resetMatrix(entity, backupBoneCache, backupOrigin, backupAbsAngle,
                backupMins, backupMaxs);

            if (targetData.hasValidPoint)
                break;
        }

        if (targetData.hasValidPoint) {
            targets.push_back(targetData);
        }

        if (foundPerfectShot) {
            break;
        }
    }

    return targets;
}


Ragebot::TargetData Ragebot::selectBestTarget(const std::vector<TargetData>& targets,
    int priorityMode) noexcept {

    if (targets.empty())
        return TargetData{};

    auto sortedTargets = targets;

    switch (priorityMode) {
    case 0: // Health
        std::sort(sortedTargets.begin(), sortedTargets.end(),
            [](const TargetData& a, const TargetData& b) {
                return a.health < b.health;
            });
        break;
    case 1: // Distance
        std::sort(sortedTargets.begin(), sortedTargets.end(),
            [](const TargetData& a, const TargetData& b) {
                return a.distance < b.distance;
            });
        break;
    case 2: // FOV
        std::sort(sortedTargets.begin(), sortedTargets.end(),
            [](const TargetData& a, const TargetData& b) {
                return a.fov < b.fov;
            });
        break;
    case 3: // Damage (NEW)
        std::sort(sortedTargets.begin(), sortedTargets.end(),
            [](const TargetData& a, const TargetData& b) {
                if (!a.hasValidPoint) return false;
                if (!b.hasValidPoint) return true;
                return a.bestPoint.damage > b.bestPoint.damage;
            });
        break;
    case 4: // Hitchance (NEW)
        std::sort(sortedTargets.begin(), sortedTargets.end(),
            [](const TargetData& a, const TargetData& b) {
                if (!a.hasValidPoint) return false;
                if (!b.hasValidPoint) return true;
                return a.bestPoint.hitchance > b.bestPoint.hitchance;
            });
        break;
    }

    // Return first valid target
    for (const auto& target : sortedTargets) {
        if (target.hasValidPoint)
            return target;
    }

    return sortedTargets[0];
}

// ============================================
// AUTO-STOP FUNCTIONS
// ============================================

namespace AutoStop {

    enum Mode {
        ADAPTIVE = 1 << 0,
        FULL_STOP = 1 << 1,
        DUCK = 1 << 2,
        EARLY = 1 << 3,
        IN_AIR = 1 << 4
    };

    // Основная функция остановки
    void applyStop(UserCmd* cmd, float targetSpeed = 0.f) noexcept {
        if (!localPlayer)
            return;

        const Vector velocity = EnginePrediction::getVelocity();
        const float speed = VectorSIMD::distance2DFast(Vector{ 0,0,0 }, velocity);

        if (speed < 1.f)
            return;

        // Направление движения относительно view angles
        Vector direction = velocity.toAngle();
        direction.y = cmd->viewangles.y - direction.y;

        // Вектор для остановки
        const Vector negatedDirection = Vector::fromAngle(direction) * -speed;


        if (targetSpeed <= 0.f) {
            // Полная остановка
            cmd->forwardmove = negatedDirection.x;
            cmd->sidemove = negatedDirection.y;
        }
        else {
            // Частичная остановка до целевой скорости
            float reduction = speed - targetSpeed;
            if (reduction > 0.f) {
                float factor = reduction / speed;
                cmd->forwardmove = negatedDirection.x * factor;
                cmd->sidemove = negatedDirection.y * factor;
            }
        }
    }

    // Адаптивный auto-stop
    void adaptive(UserCmd* cmd, Entity* weapon, float accuracyBoost) noexcept {
        if (!localPlayer || !weapon)
            return;

        const auto weaponData = weapon->getWeaponData();
        if (!weaponData)
            return;

        const Vector velocity = EnginePrediction::getVelocity();
        const float speed = velocity.length2D();

        // Максимальная скорость для точной стрельбы
        float maxSpeed = localPlayer->isScoped() ? weaponData->maxSpeedAlt : weaponData->maxSpeed;
        maxSpeed *= (1.0f - accuracyBoost);

        // Скорость для максимальной точности (обычно ~34% от максимальной)
        const float accurateSpeed = maxSpeed * 0.34f;

        if (speed > accurateSpeed) {
            applyStop(cmd, accurateSpeed);
        }
    }

    // Полная остановка
    void fullStop(UserCmd* cmd) noexcept {
        if (!localPlayer)
            return;

        const Vector velocity = EnginePrediction::getVelocity();
        const float speed = velocity.length2D();

        if (speed > 1.f) {
            applyStop(cmd, 0.f);
        }

        // Дополнительно обнуляем движение
        cmd->forwardmove = 0.f;
        cmd->sidemove = 0.f;
    }

    // Ранний auto-stop (начинает остановку заранее)
    void early(UserCmd* cmd, Entity* weapon, float accuracyBoost) noexcept {
        if (!localPlayer || !weapon)
            return;

        const auto weaponData = weapon->getWeaponData();
        if (!weaponData)
            return;

        const Vector velocity = EnginePrediction::getVelocity();
        const float speed = velocity.length2D();

        if (speed < 10.f)
            return;

        // Рассчитываем сколько тиков нужно для остановки
        const float friction = 5.2f; // CS:GO ground friction
        const float stopSpeed = 100.f; // sv_stopspeed
        const float tickInterval = memory->globalVars->intervalPerTick;

        // Примерное количество тиков для остановки
        float currentSpeed = speed;
        int ticksToStop = 0;

        while (currentSpeed > 1.f && ticksToStop < 20) {
            float control = (std::max)(currentSpeed, stopSpeed);
            float drop = control * friction * tickInterval;
            currentSpeed = (std::max)(0.f, currentSpeed - drop);
            ticksToStop++;
        }

        // Целевая скорость для точной стрельбы
        float maxSpeed = localPlayer->isScoped() ? weaponData->maxSpeedAlt : weaponData->maxSpeed;
        float targetSpeed = maxSpeed * 0.34f * (1.0f - accuracyBoost);

        // Если нужно больше 2 тиков для остановки - начинаем тормозить
        if (ticksToStop > 2 && speed > targetSpeed) {
            // Более агрессивное торможение
            Vector direction = velocity.toAngle();
            direction.y = cmd->viewangles.y - direction.y;

            const Vector stopForce = Vector::fromAngle(direction) * -(speed * 1.5f);
            cmd->forwardmove = std::clamp(stopForce.x, -450.f, 450.f);
            cmd->sidemove = std::clamp(stopForce.y, -450.f, 450.f);
        }
    }

    // === НОВОЕ: In-Air Auto-Stop ===
    void inAir(UserCmd* cmd, Entity* weapon) noexcept {
        if (!localPlayer || !weapon)
            return;

        // Проверяем что мы в воздухе
        if (localPlayer->flags() & FL_ONGROUND)
            return;

        const Vector velocity = EnginePrediction::getVelocity();
        const float speed = velocity.length2D();

        if (speed < 10.f)
            return;

        // В воздухе air-control ограничен, но мы можем замедлиться
        // используя противоположное движение

        const float airAccelerate = 10.f; // sv_airaccelerate default
        const float maxAirSpeed = 30.f; // cl_sidespeed в воздухе эффективно ограничен

        Vector direction = velocity.toAngle();
        direction.y = cmd->viewangles.y - direction.y;

        // Противоположное направление для air-strafe торможения
        Vector wishDir = Vector::fromAngle(direction);

        // Применяем противоположное движение
        cmd->forwardmove = -wishDir.x * 450.f;
        cmd->sidemove = -wishDir.y * 450.f;

        // Ограничиваем до максимальной скорости в воздухе
        float wishSpeed = std::sqrt(cmd->forwardmove * cmd->forwardmove + cmd->sidemove * cmd->sidemove);
        if (wishSpeed > 450.f) {
            float ratio = 450.f / wishSpeed;
            cmd->forwardmove *= ratio;
            cmd->sidemove *= ratio;
        }
    }

    // Основная функция выполнения auto-stop
    void run(UserCmd* cmd, Entity* weapon, int weaponIndex, bool hasTarget) noexcept {
        if (!localPlayer || !weapon)
            return;

        const auto& cfg1 = config->rageBot[weaponIndex];

        if (!cfg1.autoStop)
            return;

        // Проверяем nospread
        static auto isSpreadEnabled = interfaces->cvar->findVar(skCrypt("weapon_accuracy_nospread"));
        if (isSpreadEnabled && isSpreadEnabled->getInt() != 0)
            return;

        const int stopMode = cfg1.autoStopMod;
        const float accuracyBoost = std::clamp(cfg1.accuracyBoost, 0.0f, 1.0f);
        const bool onGround = (localPlayer->flags() & FL_ONGROUND) != 0;
        const bool inJump = (cmd->buttons & UserCmd::IN_JUMP) != 0;

        // === In-Air Auto-Stop ===
        if (stopMode & Mode::IN_AIR) {
            if (!onGround && !inJump && hasTarget) {
                inAir(cmd, weapon);
                return; // In-air stop обрабатывается отдельно
            }
        }

        // Остальные режимы только на земле
        if (!onGround || inJump)
            return;

        // === Early Auto-Stop (должен быть первым) ===
        if (stopMode & Mode::EARLY) {
            early(cmd, weapon, accuracyBoost);
        }

        // === Adaptive Auto-Stop ===
        if (stopMode & Mode::ADAPTIVE) {
            adaptive(cmd, weapon, accuracyBoost);
        }

        // === Full Stop ===
        if (stopMode & Mode::FULL_STOP) {
            fullStop(cmd);
        }

        // === Duck ===
        if (stopMode & Mode::DUCK) {
            cmd->buttons |= UserCmd::IN_DUCK;
        }

        // Обновляем prediction после изменения движения
        EnginePrediction::update();
    }

    // Проверка - можем ли мы точно стрелять
    bool canShootAccurately(Entity* weapon) noexcept {
        if (!localPlayer || !weapon)
            return false;

        const auto weaponData = weapon->getWeaponData();
        if (!weaponData)
            return true; // Если нет данных - разрешаем

        const Vector velocity = EnginePrediction::getVelocity();
        const float speed = velocity.length2D();

        float maxSpeed = localPlayer->isScoped() ? weaponData->maxSpeedAlt : weaponData->maxSpeed;
        float accurateSpeed = maxSpeed * 0.34f;

        return speed <= accurateSpeed;
    }
}



// ============================================
// MAIN RUN FUNCTION (Исправленная)
// ============================================

void Ragebot::run(UserCmd* cmd) noexcept {
    currentTick = memory->globalVars->tickCount;

    if (!config->ragebot.enabled || !config->ragebotKey.isActive()) {
        delayShotData.reset();
        return;
    }

    if (!localPlayer || localPlayer->nextAttack() > memory->globalVars->serverTime() ||
        localPlayer->isDefusing() || localPlayer->waitForNoAttack()) {
        delayShotData.reset();
        return;
    }

    const auto& cfg = config->ragebot;
    const auto& cfg1 = config->rageBot;

    const auto activeWeapon = localPlayer->getActiveWeapon();
    if (!activeWeapon || !activeWeapon->clip() || activeWeapon->isKnife() ||
        activeWeapon->isBomb() || activeWeapon->isGrenade()) {
        delayShotData.reset();
        return;
    }

    int weaponIndex = getWeaponIndex(activeWeapon->itemDefinitionIndex2());

    if (localPlayer->shotsFired() > 0 && !activeWeapon->isFullAuto())
        return;

    if (!(cfg.enabled && (cmd->buttons & UserCmd::IN_ATTACK || cfg.autoShot)))
        return;

    const auto localPlayerEyePosition = localPlayer->getEyePosition();
    const auto aimPunch = (!activeWeapon->isKnife() && !activeWeapon->isBomb() &&
        !activeWeapon->isGrenade()) ? localPlayer->getAimPunch() : Vector{};

    // Collect all valid targets
    auto targets = collectTargets(cmd, activeWeapon, weaponIndex, localPlayerEyePosition, aimPunch);

    bool hasValidTarget = false;
    TargetData bestTarget;

    if (!targets.empty()) {
        bestTarget = selectBestTarget(targets, cfg.priority);
        hasValidTarget = bestTarget.hasValidPoint;
    }

    // === AUTO-STOP (вызываем даже если нет цели, но с флагом) ===
    if (hasValidTarget) {
        AutoStop::run(cmd, activeWeapon, weaponIndex, true);
    }

    if (!hasValidTarget) {
        delayShotData.reset();
        return;
    }

    // Проверяем можем ли стрелять точно (после auto-stop)
    bool canShoot = AutoStop::canShootAccurately(activeWeapon);

    // Если Early auto-stop включен - ждём пока остановимся
    if ((cfg1[weaponIndex].autoStopMod & AutoStop::Mode::EARLY) && !canShoot) {
        // Продолжаем останавливаться, но не стреляем
        return;
    }

    // Проверяем timing атаки
    if (activeWeapon->nextPrimaryAttack() > memory->globalVars->serverTime())
        return;

    // Update resolver
    auto player = Animations::getPlayer(bestTarget.index);
    Resolver::update(bestTarget.entity, player);

    // === Force Shot handling ===
    if (isForceShotActive()) {
        handleForceShot(cmd, bestTarget);
        delayShotData.reset();
    }
    // === Delay Shot handling ===
    else if (shouldDelayShot(cmd, bestTarget, weaponIndex)) {
        updateDelayShot(cmd, bestTarget, weaponIndex);

        if (!isDelayShotReady()) {
            return;
        }

        if (delayShotData.bestTarget.hasValidPoint) {
            bestTarget = delayShotData.bestTarget;
        }

        executeDelayShot(cmd);
    }

    // Perform hitchance check
    const Model* model = bestTarget.entity->getModel();
    if (!model)
        return;

    StudioHdr* hdr = interfaces->modelInfo->getStudioModel(model);
    if (!hdr)
        return;

    StudioHitboxSet* set = hdr->getHitboxSet(0);
    if (!set)
        return;

    // Hitchance check
    bool hitChanceOverrideActive = config->hitchanceOverride.isActive() && cfg1[weaponIndex].hcov;
    int requiredHitChance = hitChanceOverrideActive
        ? cfg1[weaponIndex].OvrHitChance
        : cfg1[weaponIndex].hitChance;

    // Force shot uses reduced hitchance
    if (isForceShotActive()) {
        requiredHitChance = (std::max)(1, requiredHitChance / 2);
    }

    Vector bestAngle = AimbotFunction::calculateRelativeAngle(
        localPlayerEyePosition, bestTarget.bestPoint.position, cmd->viewangles + aimPunch);

    // Restore entity matrix for hitchance check
    auto player2 = Animations::getPlayer(bestTarget.index);
    matrix3x4* matrix = player2.matrix.data();

    if (!AimbotFunction::hitChance(localPlayer.get(), bestTarget.entity, set, matrix,
        activeWeapon, bestAngle, cmd, requiredHitChance)) {

        if (!isForceShotActive())
            return;
    }

    // Auto-scope
    if (cfg.autoScope && activeWeapon->isSniperRifle() && !localPlayer->isScoped() &&
        activeWeapon->zoomLevel() < 1) {
        cmd->buttons |= UserCmd::IN_ZOOM;
    }

    // Apply angles
    static Vector lastAngles{ cmd->viewangles };
    static int lastCommand{ };

    if (lastCommand == cmd->commandNumber - 1 && lastAngles.notNull() && cfg.silent)
        cmd->viewangles = lastAngles;

    Vector angle = AimbotFunction::calculateRelativeAngle(
        localPlayerEyePosition, bestTarget.bestPoint.position, cmd->viewangles + aimPunch);

    bool clamped = false;
    if (std::abs(angle.x) > 90 || std::abs(angle.y) > 180) {
        angle.x = std::clamp(angle.x, -90.0f, 90.0f);
        angle.y = std::clamp(angle.y, -180.f, 180.f);
        clamped = true;
    }

    if (activeWeapon->nextPrimaryAttack() <= memory->globalVars->serverTime()) {
        cmd->viewangles += angle;

        if (!cfg.silent)
            interfaces->engine->setViewAngles(cmd->viewangles);

        if (cfg.autoShot && !clamped)
            cmd->buttons |= UserCmd::IN_ATTACK;
    }

    if (clamped)
        cmd->buttons &= ~UserCmd::IN_ATTACK;

    // Execute shot
    if (cmd->buttons & UserCmd::IN_ATTACK) {
        cmd->tickCount = timeToTicks(bestTarget.simulationTime + Backtrack::getLerp());

        // Save shot for resolver
        Resolver::saveShot(bestTarget.index, bestTarget.simulationTime,
            bestTarget.isBacktrack ? bestTarget.backtrackTick : 0,
            bestTarget.bestPoint.position, bestTarget.bestPoint.hitbox,
            bestTarget.bestPoint.hitchance, bestTarget.bestPoint.damage);

        // Log shot
        std::ostringstream log;
        log << "[Shot] " << bestTarget.entity->getPlayerName();
        log << " | Hitbox: " << bestTarget.bestPoint.hitbox;
        log << " | DMG: " << bestTarget.bestPoint.damage;
        log << " | Safe: " << (bestTarget.bestPoint.isSafe ? "Y" : "N");
        if (bestTarget.isBacktrack) {
            log << " | BT: " << bestTarget.backtrackTick;
        }
        if (isForceShotActive()) {
            log << " | FORCE";
        }
        Logger::addLog(log.str());

        latest_player = bestTarget.entity->getPlayerName();

        Resolver::processMissedShots();
    }

    if (clamped)
        lastAngles = cmd->viewangles;
    else
        lastAngles = Vector{};

    lastCommand = cmd->commandNumber;
}