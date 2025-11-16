// BLACKHOLE Enhanced Resolver v2.0
// Optimized resolver with ML-inspired adaptive system
// Focus: HvH competitive play

#include "AimbotFunctions.h"
#include "Animations.h"
#include "Resolver.h"
#include "../GameData.h"
#include "../SDK/Entity.h"
#include "../xor.h"

#include <array>
#include <deque>
#include <algorithm>
#include <cmath>

// ============================================================================
// RESOLVER DATA STRUCTURES
// ============================================================================

struct ResolverPlayer {
    // Historical data
    std::array<float, 16> yawHistory{};      // Last 16 yaw values
    std::array<float, 16> pitchHistory{};    // Last 16 pitch values
    int historyIndex = 0;
    
    // Statistics
    int totalShots = 0;
    int hitsOnLeft = 0;
    int hitsOnRight = 0;
    int hitsOnCenter = 0;
    
    // Detection flags
    bool isJittering = false;
    bool isLowDelta = false;
    bool isExtended = false;        // Using >60° desync
    bool isFakewalking = false;
    
    // Timing
    float lastUpdateTime = 0.0f;
    float lastShotTime = 0.0f;
    
    // Resolver state
    int currentBrutePhase = 0;
    float lastResolvedYaw = 0.0f;
    
    void reset() {
        yawHistory.fill(0.0f);
        pitchHistory.fill(0.0f);
        historyIndex = 0;
        totalShots = 0;
        hitsOnLeft = hitsOnRight = hitsOnCenter = 0;
        isJittering = isLowDelta = isExtended = isFakewalking = false;
        lastUpdateTime = lastShotTime = 0.0f;
        currentBrutePhase = 0;
        lastResolvedYaw = 0.0f;
    }
};

static std::array<ResolverPlayer, 65> resolverData{};

// ============================================================================
// RESOLVER CORE - Detection Functions
// ============================================================================

namespace ResolverV2 {

// Detect jitter through yaw variance analysis
bool detectJitter(Entity* entity, ResolverPlayer& data) noexcept
{
    if (!entity || !entity->getAnimstate())
        return false;
    
    const float currentYaw = entity->eyeAngles().y;
    
    // Update history
    data.yawHistory[data.historyIndex % 16] = currentYaw;
    data.historyIndex++;
    
    // Need at least 8 samples
    if (data.historyIndex < 8)
        return false;
    
    // Calculate variance in recent yaw values
    float mean = 0.0f;
    constexpr int samples = 8;
    
    for (int i = 0; i < samples; ++i) {
        const int idx = (data.historyIndex - 1 - i) % 16;
        mean += data.yawHistory[idx];
    }
    mean /= samples;
    
    // Calculate standard deviation
    float variance = 0.0f;
    int significantChanges = 0;
    
    for (int i = 0; i < samples - 1; ++i) {
        const int idx1 = (data.historyIndex - 1 - i) % 16;
        const int idx2 = (data.historyIndex - 2 - i) % 16;
        
        const float diff = std::abs(Helpers::angleDiff(data.yawHistory[idx1], data.yawHistory[idx2]));
        variance += diff * diff;
        
        if (diff > 15.0f)
            ++significantChanges;
    }
    
    variance /= (samples - 1);
    const float stdDev = std::sqrt(variance);
    
    // High variance + frequent changes = jitter
    data.isJittering = (stdDev > 20.0f && significantChanges >= 3);
    return data.isJittering;
}

// Detect low delta (legit AA style)
bool detectLowDelta(Entity* entity) noexcept
{
    if (!entity || !entity->getAnimstate())
        return false;
    
    const float eyeYaw = entity->eyeAngles().y;
    const float footYaw = entity->getAnimstate()->footYaw;
    
    const float delta = std::abs(Helpers::angleDiff(eyeYaw, footYaw));
    
    return (delta < 35.0f);
}

// Detect extended desync (>60°)
bool detectExtendedDesync(Entity* entity, const Animations::Players& player) noexcept
{
    if (!entity || !entity->getAnimstate())
        return false;
    
    // Check animation layers for extended angle indicators
    const auto& layers = player.layers;
    
    // Layer 3 (ANIMATION_LAYER_MOVEMENT_MOVE) analysis
    if (layers[3].cycle == 0.0f && layers[3].weight == 0.0f)
        return true;
    
    // Check actual desync angle
    const float maxDesync = entity->getMaxDesyncAngle();
    return (maxDesync > 58.0f);
}

// Detect fakewalking through animation analysis
bool detectFakewalk(const Animations::Players& player) noexcept
{
    // Check movement layer weight and velocity mismatch
    const auto& moveLayer = player.layers[ANIMATION_LAYER_MOVEMENT_MOVE];
    
    const float velocityLength = player.velocity.length2D();
    const bool hasVelocity = (velocityLength > 0.1f);
    const bool hasMovementAnim = (moveLayer.weight > 0.0f);
    
    // Fakewalking: movement animation without actual velocity
    return (hasMovementAnim && !hasVelocity) || 
           (velocityLength > 0.1f && velocityLength < 100.0f && moveLayer.playbackRate < 0.5f);
}

// ============================================================================
// RESOLVER CORE - Side Detection
// ============================================================================

float detectSideViaLayers(
    const Animations::Players& player,
    Entity* entity) noexcept
{
    if (!entity || !entity->getAnimstate())
        return 0.0f;
    
    // Compare animation layer deltas to determine side
    constexpr int MOVE_LAYER = ANIMATION_LAYER_MOVEMENT_MOVE;
    
    if (player.oldlayers.empty())
        return 0.0f;
    
    const float deltaLeft = std::abs(
        player.layers[MOVE_LAYER].playbackRate - 
        player.oldlayers[MOVE_LAYER].playbackRate
    );
    
    const float deltaRight = deltaLeft; // Simplified for now
    const float deltaCenter = 0.0f;     // Needs proper calculation
    
    // Find minimum delta (most likely side)
    float minDelta = std::min({deltaLeft, deltaRight, deltaCenter});
    
    if (minDelta == deltaLeft)
        return -1.0f;  // Left
    else if (minDelta == deltaRight)
        return 1.0f;   // Right
    
    return 0.0f;  // Center
}

float detectSideViaFreestand(Entity* entity) noexcept
{
    if (!entity || !localPlayer)
        return 0.0f;
    
    // Trace rays to detect wall proximity
    Vector forward, right, up;
    const Vector eyePos = entity->getEyePosition();
    const float backwardYaw = Helpers::calculate_angle(
        localPlayer->origin(), 
        entity->origin()
    ).y;
    
    Helpers::AngleVectors(Vector(0, backwardYaw, 0), &forward, &right, &up);
    
    constexpr float traceDistance = 384.0f;
    
    // Trace left and right
    Trace traceLeft, traceRight;
    interfaces->engineTrace->traceRay(
        {eyePos, eyePos + forward * traceDistance - right * 35.0f},
        MASK_SHOT,
        {entity},
        traceLeft
    );
    
    interfaces->engineTrace->traceRay(
        {eyePos, eyePos + forward * traceDistance + right * 35.0f},
        MASK_SHOT,
        {entity},
        traceRight
    );
    
    const float leftDist = (traceLeft.endpos - traceLeft.startpos).length();
    const float rightDist = (traceRight.endpos - traceRight.startpos).length();
    
    // More space on left = hide to left
    if (leftDist > rightDist + 50.0f)
        return -1.0f;
    else if (rightDist > leftDist + 50.0f)
        return 1.0f;
    
    return 0.0f;
}

// ============================================================================
// RESOLVER CORE - Angle Calculation
// ============================================================================

float calculateResolvedYaw(
    Entity* entity,
    const Animations::Players& player,
    ResolverPlayer& data,
    bool& shouldBruteforce) noexcept
{
    if (!entity || !entity->getAnimstate())
        return 0.0f;
    
    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = entity->getMaxDesyncAngle();
    
    // Priority 1: Shot detection (most reliable)
    if (player.shot)
    {
        // Use shot matrix analysis
        const Vector muzzlePos = player.matrix[8].origin();
        const float shotYaw = Helpers::calculate_angle(
            localPlayer->origin(),
            muzzlePos
        ).y;
        
        data.lastShotTime = memory->globalVars->currenttime;
        return shotYaw;
    }
    
    // Priority 2: Animation layer resolution
    if (entity->flags() & FL_ONGROUND && player.velocity.length2D() > 0.1f)
    {
        const float layerSide = detectSideViaLayers(player, entity);
        if (layerSide != 0.0f)
        {
            return Helpers::normalizeYaw(eyeYaw + maxDesync * layerSide);
        }
    }
    
    // Priority 3: Low delta detection
    if (detectLowDelta(entity))
    {
        data.isLowDelta = true;
        const float lowDeltaAngle = data.isExtended ? 29.0f : 35.0f;
        
        // Use statistical preference
        float side = 0.0f;
        if (data.hitsOnLeft > data.hitsOnRight)
            side = -1.0f;
        else if (data.hitsOnRight > data.hitsOnLeft)
            side = 1.0f;
        else
            side = detectSideViaFreestand(entity);
        
        return Helpers::normalizeYaw(eyeYaw + lowDeltaAngle * side);
    }
    
    // Priority 4: Jitter detection
    if (detectJitter(entity, data))
    {
        // Predict opposite of last resolved angle
        const float jitterSide = (data.lastResolvedYaw > eyeYaw) ? -1.0f : 1.0f;
        return Helpers::normalizeYaw(eyeYaw + maxDesync * jitterSide);
    }
    
    // Priority 5: Freestanding
    const float freestandSide = detectSideViaFreestand(entity);
    if (freestandSide != 0.0f)
    {
        return Helpers::normalizeYaw(eyeYaw + maxDesync * freestandSide);
    }
    
    // Priority 6: Bruteforce
    shouldBruteforce = true;
    return eyeYaw; // Will be adjusted by bruteforce
}

// ============================================================================
// RESOLVER CORE - Adaptive Bruteforce
// ============================================================================

float adaptiveBruteforce(
    Entity* entity,
    ResolverPlayer& data,
    bool isJittering) noexcept
{
    if (!entity || !entity->getAnimstate())
        return 0.0f;
    
    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = entity->getMaxDesyncAngle();
    
    // Adaptive bruteforce based on hit history
    const int totalHits = data.hitsOnLeft + data.hitsOnRight + data.hitsOnCenter;
    
    if (totalHits > 0)
    {
        // Use statistical weighting
        const float leftWeight = static_cast<float>(data.hitsOnLeft) / totalHits;
        const float rightWeight = static_cast<float>(data.hitsOnRight) / totalHits;
        const float centerWeight = static_cast<float>(data.hitsOnCenter) / totalHits;
        
        // Choose most successful angle
        if (leftWeight > rightWeight && leftWeight > centerWeight)
            return Helpers::normalizeYaw(eyeYaw - maxDesync);
        else if (rightWeight > leftWeight && rightWeight > centerWeight)
            return Helpers::normalizeYaw(eyeYaw + maxDesync);
    }
    
    // Standard bruteforce pattern
    if (isJittering)
    {
        // 3-way bruteforce for jitter
        switch (data.currentBrutePhase % 3)
        {
        case 0: return Helpers::normalizeYaw(eyeYaw + maxDesync);
        case 1: return Helpers::normalizeYaw(eyeYaw - maxDesync);
        case 2: return eyeYaw;
        }
    }
    else
    {
        // 5-way bruteforce for standard
        switch (data.currentBrutePhase % 5)
        {
        case 0: return Helpers::normalizeYaw(eyeYaw + maxDesync);
        case 1: return Helpers::normalizeYaw(eyeYaw - maxDesync);
        case 2: return eyeYaw;
        case 3: return Helpers::normalizeYaw(eyeYaw + maxDesync * 0.5f);
        case 4: return Helpers::normalizeYaw(eyeYaw - maxDesync * 0.5f);
        }
    }
    
    return eyeYaw;
}

// ============================================================================
// RESOLVER CORE - Main Resolution Function
// ============================================================================

void resolvePlayer(
    Entity* entity,
    Animations::Players& player,
    int entityIndex) noexcept
{
    if (!entity || !entity->isAlive() || !entity->getAnimstate())
        return;
    
    if (entity->isDormant() || entity == localPlayer.get())
        return;
    
    if (!entity->isOtherEnemy(localPlayer.get()))
        return;
    
    auto& data = resolverData[entityIndex];
    
    // Update detection flags
    data.isExtended = detectExtendedDesync(entity, player);
    data.isFakewalking = detectFakewalk(player);
    data.isLowDelta = detectLowDelta(entity);
    detectJitter(entity, data);
    
    // Calculate resolved angle
    bool shouldBrute = false;
    float resolvedYaw = calculateResolvedYaw(entity, player, data, shouldBrute);
    
    // Apply bruteforce if needed
    if (shouldBrute)
    {
        resolvedYaw = adaptiveBruteforce(entity, data, data.isJittering);
    }
    
    // Apply resolved angle to animstate
    if (entity->getAnimstate())
    {
        entity->getAnimstate()->footYaw = resolvedYaw;
        data.lastResolvedYaw = resolvedYaw;
    }
    
    // Force pitch down for accuracy
    entity->eyeAngles().x = 89.0f;
}

// ============================================================================
// RESOLVER CORE - Statistics Update
// ============================================================================

void updateResolverStats(
    int entityIndex,
    float resolvedYaw,
    bool didHit) noexcept
{
    if (entityIndex < 0 || entityIndex >= 65)
        return;
    
    auto& data = resolverData[entityIndex];
    data.totalShots++;
    
    if (!didHit)
    {
        // Miss: increment bruteforce phase
        data.currentBrutePhase++;
        return;
    }
    
    // Hit: update statistics
    const auto entity = interfaces->entityList->getEntity(entityIndex);
    if (!entity || !entity->getAnimstate())
        return;
    
    const float eyeYaw = entity->eyeAngles().y;
    const float delta = Helpers::angleDiff(resolvedYaw, eyeYaw);
    
    // Categorize hit
    if (std::abs(delta) < 15.0f)
        data.hitsOnCenter++;
    else if (delta < 0.0f)
        data.hitsOnLeft++;
    else
        data.hitsOnRight++;
    
    // Reset bruteforce phase on hit
    data.currentBrutePhase = 0;
}

// ============================================================================
// RESOLVER CORE - Public Interface
// ============================================================================

void processPlayers(FrameStage stage) noexcept
{
    if (stage != FrameStage::NET_UPDATE_END)
        return;
    
    if (!localPlayer || !localPlayer->isAlive())
    {
        // Reset all data
        for (auto& data : resolverData)
            data.reset();
        return;
    }
    
    // Process each player
    for (int i = 1; i <= interfaces->engine->getMaxClients(); ++i)
    {
        auto entity = interfaces->entityList->getEntity(i);
        if (!entity || entity == localPlayer.get())
            continue;
        
        auto& player = *Animations::setPlayer(i);
        resolvePlayer(entity, player, i);
    }
}

void onPlayerHurt(int attackerId, int victimId, int hitgroup) noexcept
{
    if (attackerId != localPlayer->getUserId())
        return;
    
    const int entityIndex = interfaces->engine->getPlayerFromUserID(victimId);
    if (entityIndex < 0 || entityIndex >= 65)
        return;
    
    auto& data = resolverData[entityIndex];
    updateResolverStats(entityIndex, data.lastResolvedYaw, true);
}

void onPlayerMiss(int entityIndex) noexcept
{
    if (entityIndex < 0 || entityIndex >= 65)
        return;
    
    auto& data = resolverData[entityIndex];
    updateResolverStats(entityIndex, data.lastResolvedYaw, false);
}

void reset() noexcept
{
    for (auto& data : resolverData)
        data.reset();
}

} // namespace ResolverV2
