// BLACKHOLE Enhanced Ragebot v2.0
// Advanced targeting system with smart selection
// Optimized for CS:GO HvH competitive play

#include "../Config.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "AimbotFunctions.h"
#include "Animations.h"
#include "Tickbase.h"
#include "../SDK/Entity.h"
#include "../SDK/UserCmd.h"
#include "../SDK/WeaponId.h"
#include "../xor.h"

#include <vector>
#include <algorithm>
#include <execution>

namespace RagebotV2 {

// ============================================================================
// TARGET SELECTION STRUCTURES
// ============================================================================

struct ScanPoint {
    Vector position;
    int hitbox;
    float damage;
    float safety;  // 0.0 = unsafe, 1.0 = safe
};

struct Target {
    Entity* entity = nullptr;
    int entityIndex = 0;
    
    std::vector<ScanPoint> scanPoints;
    
    float bestDamage = 0.0f;
    Vector bestPosition;
    int bestHitbox = 0;
    
    float fov = 999.0f;
    float distance = 0.0f;
    float priority = 0.0f;
    
    bool canHit = false;
    bool isSafe = false;
};

// ============================================================================
// HITBOX CONFIGURATION
// ============================================================================

struct HitboxConfig {
    int id;
    float priorityMultiplier;
    float minDamage;
    bool enabled;
};

static const std::array<HitboxConfig, 8> hitboxConfigs = {{
    {Hitboxes::Head,        1.5f,  40.0f, true},   // Prefer head
    {Hitboxes::Neck,        1.3f,  35.0f, true},
    {Hitboxes::UpperChest,  1.2f,  30.0f, true},
    {Hitboxes::Chest,       1.0f,  25.0f, true},
    {Hitboxes::LowerChest,  0.9f,  25.0f, true},
    {Hitboxes::Stomach,     0.8f,  20.0f, true},
    {Hitboxes::Pelvis,      0.7f,  20.0f, true},
    {Hitboxes::Legs,        0.5f,  15.0f, false}   // Disabled by default
}};

// ============================================================================
// TARGET SCANNING
// ============================================================================

// Scan a single hitbox and generate points
void scanHitbox(
    Target& target,
    int hitboxId,
    StudioHitboxSet* set,
    const matrix3x4 matrix[MAXSTUDIOBONES],
    const WeaponInfo* weaponData,
    int minDamage) noexcept
{
    if (!set || !weaponData)
        return;
    
    auto hitbox = set->getHitbox(hitboxId);
    if (!hitbox)
        return;
    
    // Find hitbox config
    const HitboxConfig* config = nullptr;
    for (const auto& cfg : hitboxConfigs)
    {
        if (cfg.id == hitboxId)
        {
            config = &cfg;
            break;
        }
    }
    
    if (!config || !config->enabled)
        return;
    
    // Generate multipoint
    const auto points = AimbotFunctionV2::advancedMultiPoint(
        target.entity,
        matrix,
        hitbox,
        localPlayer->getEyePosition(),
        hitboxId,
        config->headScale,
        config->bodyScale
    );
    
    // Scan each point
    for (const auto& point : points)
    {
        // Calculate damage
        auto scanResult = AimbotFunctionV2::advancedDamageScan(
            target.entity,
            point,
            weaponData,
            std::max(minDamage, static_cast<int>(config->minDamage)),
            false
        );
        
        if (!scanResult.canHit)
            continue;
        
        // Calculate safety (how exposed we are)
        float safety = 1.0f;
        
        // Lower safety for head shots (more vulnerable)
        if (hitboxId == Hitboxes::Head)
            safety *= 0.8f;
        
        // Higher safety for body shots
        if (hitboxId >= Hitboxes::Chest && hitboxId <= Hitboxes::Stomach)
            safety *= 1.2f;
        
        // Add scan point
        ScanPoint scanPoint{
            point,
            hitboxId,
            scanResult.damage * config->priorityMultiplier,
            std::clamp(safety, 0.0f, 1.0f)
        };
        
        target.scanPoints.push_back(scanPoint);
        
        // Update best point
        if (scanPoint.damage > target.bestDamage)
        {
            target.bestDamage = scanPoint.damage;
            target.bestPosition = point;
            target.bestHitbox = hitboxId;
        }
    }
}

// Scan all hitboxes for a target
void scanTarget(
    Target& target,
    const WeaponInfo* weaponData,
    int minDamage) noexcept
{
    if (!target.entity || !weaponData)
        return;
    
    // Get player data
    const auto& player = Animations::getPlayer(target.entityIndex);
    if (!player.gotMatrix)
        return;
    
    // Get model
    const auto model = target.entity->getModel();
    if (!model)
        return;
    
    auto hdr = interfaces->modelInfo->getStudioModel(model);
    if (!hdr)
        return;
    
    auto set = hdr->getHitboxSet(0);
    if (!set)
        return;
    
    // Scan each hitbox
    for (const auto& hitboxConfig : hitboxConfigs)
    {
        if (!hitboxConfig.enabled)
            continue;
        
        scanHitbox(
            target,
            hitboxConfig.id,
            set,
            player.matrix.data(),
            weaponData,
            minDamage
        );
    }
    
    target.canHit = !target.scanPoints.empty();
}

// ============================================================================
// TARGET SELECTION
// ============================================================================

// Calculate target priority
float calculatePriority(
    const Target& target,
    bool preferSafe) noexcept
{
    float priority = 0.0f;
    
    // FOV weight (closer to crosshair = better)
    priority += (180.0f - target.fov) * 2.0f;
    
    // Distance weight (closer = better)
    priority += (8192.0f - target.distance) * 0.01f;
    
    // Damage weight (more damage = better)
    priority += target.bestDamage * 10.0f;
    
    // Safety weight (if prefer safe)
    if (preferSafe)
    {
        float avgSafety = 0.0f;
        for (const auto& point : target.scanPoints)
            avgSafety += point.safety;
        
        if (!target.scanPoints.empty())
            avgSafety /= target.scanPoints.size();
        
        priority += avgSafety * 100.0f;
    }
    
    return priority;
}

// Select best target from list
Target* selectBestTarget(
    std::vector<Target>& targets,
    bool preferSafe) noexcept
{
    if (targets.empty())
        return nullptr;
    
    // Calculate priorities
    for (auto& target : targets)
    {
        target.priority = calculatePriority(target, preferSafe);
    }
    
    // Sort by priority
    std::sort(
        std::execution::par_unseq,
        targets.begin(),
        targets.end(),
        [](const Target& a, const Target& b) {
            return a.priority > b.priority;
        }
    );
    
    return &targets.front();
}

// ============================================================================
// AIMBOT EXECUTION
// ============================================================================

// Calculate aim angles
Vector calculateAimAngles(
    const Vector& source,
    const Vector& destination,
    const Vector& viewAngles,
    Entity* weapon) noexcept
{
    Vector angles = AimbotFunction::calculateRelativeAngle(source, destination, viewAngles);
    
    // Apply recoil compensation
    if (localPlayer && weapon)
    {
        const auto aimPunch = localPlayer->getAimPunch();
        
        if (config->ragebot.recoilControl)
        {
            angles -= aimPunch * 2.0f;
        }
    }
    
    return angles;
}

// Smooth aim angles (optional)
Vector smoothAim(
    const Vector& currentAngles,
    const Vector& targetAngles,
    float smoothAmount) noexcept
{
    if (smoothAmount <= 0.0f)
        return targetAngles;
    
    Vector delta = targetAngles - currentAngles;
    delta.normalize();
    
    const float smoothFactor = std::clamp(1.0f / smoothAmount, 0.0f, 1.0f);
    
    return currentAngles + delta * smoothFactor;
}

// Execute aimbot
bool execute(
    UserCmd* cmd,
    Target* target,
    const WeaponInfo* weaponData) noexcept
{
    if (!target || !target->canHit || !weaponData)
        return false;
    
    auto weapon = localPlayer->getActiveWeapon();
    if (!weapon)
        return false;
    
    // Get player data for hitchance
    const auto& player = Animations::getPlayer(target->entityIndex);
    if (!player.gotMatrix)
        return false;
    
    // Get model
    const auto model = target->entity->getModel();
    if (!model)
        return false;
    
    auto hdr = interfaces->modelInfo->getStudioModel(model);
    if (!hdr)
        return false;
    
    auto set = hdr->getHitboxSet(0);
    if (!set)
        return false;
    
    // Check hitchance
    const int requiredHitchance = config->ragebot.hitChance;
    
    if (requiredHitchance > 0)
    {
        const bool passedHitchance = AimbotFunctionV2::enhancedHitChance(
            localPlayer.get(),
            target->entity,
            set,
            player.matrix.data(),
            weapon,
            target->bestPosition,
            cmd,
            requiredHitchance
        );
        
        if (!passedHitchance)
            return false;
    }
    
    // Calculate aim angles
    Vector aimAngles = calculateAimAngles(
        localPlayer->getEyePosition(),
        target->bestPosition,
        cmd->viewangles,
        weapon
    );
    
    // Apply smoothing if needed
    if (config->ragebot.smooth > 0.0f)
    {
        aimAngles = smoothAim(cmd->viewangles, aimAngles, config->ragebot.smooth);
    }
    
    // Set angles
    cmd->viewangles = aimAngles;
    
    // Auto shoot
    if (config->ragebot.autoShoot)
    {
        cmd->buttons |= UserCmd::IN_ATTACK;
    }
    
    // Auto stop (for accuracy)
    if (config->ragebot.autoStop)
    {
        cmd->forwardmove = 0.0f;
        cmd->sidemove = 0.0f;
    }
    
    return true;
}

// ============================================================================
// MAIN RAGEBOT FUNCTION
// ============================================================================

void run(UserCmd* cmd) noexcept
{
    if (!config->ragebot.enabled)
        return;
    
    if (!localPlayer || !localPlayer->isAlive())
        return;
    
    auto weapon = localPlayer->getActiveWeapon();
    if (!weapon || !weapon->clip())
        return;
    
    auto weaponData = weapon->getWeaponData();
    if (!weaponData)
        return;
    
    // Check if we can shoot
    const float serverTime = memory->globalVars->serverTime();
    
    if (localPlayer->nextAttack() > serverTime)
        return;
    
    if (weapon->nextPrimaryAttack() > serverTime)
        return;
    
    if (weapon->isGrenade() || weapon->isKnife())
        return;
    
    // Build target list
    std::vector<Target> targets;
    targets.reserve(64);
    
    for (int i = 1; i <= interfaces->engine->getMaxClients(); ++i)
    {
        auto entity = interfaces->entityList->getEntity(i);
        if (!entity || entity == localPlayer.get())
            continue;
        
        if (!entity->isAlive() || entity->isDormant())
            continue;
        
        if (!entity->isOtherEnemy(localPlayer.get()))
            continue;
        
        // Calculate FOV
        const Vector eyePos = localPlayer->getEyePosition();
        const Vector targetPos = entity->getEyePosition();
        const Vector angle = AimbotFunction::calculateRelativeAngle(eyePos, targetPos, cmd->viewangles);
        const float fov = std::hypot(angle.x, angle.y);
        
        // FOV check
        if (fov > config->ragebot.fov)
            continue;
        
        // Create target
        Target target;
        target.entity = entity;
        target.entityIndex = i;
        target.fov = fov;
        target.distance = (targetPos - eyePos).length();
        
        // Scan target
        scanTarget(target, weaponData, config->ragebot.minDamage);
        
        if (target.canHit)
            targets.push_back(std::move(target));
    }
    
    // Select best target
    auto bestTarget = selectBestTarget(targets, config->ragebot.safePoint);
    
    if (!bestTarget)
        return;
    
    // Execute aimbot
    execute(cmd, bestTarget, weaponData);
}

} // namespace RagebotV2
