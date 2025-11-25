#include "Resolver.h"
#include "AimbotFunctions.h"
#include "Backtrack.h"
#include "../Logger.h"
#include "../SDK/GameEvent.h"
#include "../GameData.h"
#include "../Helpers.h"
#include "../Memory.h"
#include "../Interfaces.h"
#include "../Config.h"

// ============================================
// INITIALIZATION
// ============================================

void Resolver::initialize() noexcept {
    for (auto& d : data) {
        d.reset();
    }
    snapshots.clear();
}

void Resolver::reset() noexcept {
    initialize();
}


// ============================================
// MAIN UPDATE FUNCTION
// ============================================

void Resolver::update(Entity* entity, Animations::Players& player) noexcept {
    if (!enabled || !entity || !entity->isAlive())
        return;

    if (entity->isDormant() || entity->isBot())
        return;

    if (!localPlayer || entity->team() == localPlayer->team())
        return;

    const int index = entity->index();
    if (index < 1 || index > 64)
        return;

    auto& info = data[index];
    auto* animState = entity->getAnimstate();

    if (!animState)
        return;

    // Update mode
    info.mode = detectMode(entity, info);
    info.is_on_ground = (entity->flags() & FL_ONGROUND) != 0;
    info.is_moving = entity->velocity().length2D() > MOVING_SPEED_THRESHOLD;

    // Store eye angles history
    info.eye_angles_history[info.eye_angles_index % 16] = entity->eyeAngles();
    info.eye_angles_index++;

    // Update all resolver data
    updateLBYData(entity, info);
    updateAnimStateData(entity, info);
    updatePoseParamData(entity, info);

    // Detect states
    info.jitter.is_jittering = detectJitter(entity, info);
    info.is_low_delta = detectLowDelta(entity, info);
    info.is_faking = detectFakeAngles(entity);
    info.extended_desync = detectExtendedDesync(entity, info);

    // Collect votes from all methods
    collectVotes(entity, info);

    // Process votes and determine final angle
    processVotes(entity, info);

    // Calculate and apply final yaw
    float finalYaw = calculateFinalYaw(entity, info);
    animState->footYaw = buildServerAbsYaw(entity, finalYaw);

    // Update player data
    player.side = static_cast<int>(info.side);
    info.last_resolve_time = memory->globalVars->currenttime;
}

// ============================================
// VOTING SYSTEM
// ============================================

void Resolver::collectVotes(Entity* entity, ResolverData& info) noexcept {
    // Clear previous votes
    for (auto& vote : info.votes) {
        vote = MethodVote();
    }

    // Collect vote from each method
    info.votes[static_cast<int>(ResolveMethod::LBY)] = resolveLBY(entity, info);
    info.votes[static_cast<int>(ResolveMethod::ANIMSTATE)] = resolveAnimState(entity, info);
    info.votes[static_cast<int>(ResolveMethod::POSE_PARAM)] = resolvePoseParams(entity, info);
    info.votes[static_cast<int>(ResolveMethod::TRACE)] = resolveTrace(entity, info);
    info.votes[static_cast<int>(ResolveMethod::BRUTEFORCE)] = resolveBruteforce(entity, info);
}

void Resolver::processVotes(Entity* entity, ResolverData& info) noexcept {
    // Calculate weighted scores for each side
    float leftScore = 0.f;
    float rightScore = 0.f;
    float centerScore = 0.f;

    float totalWeight = 0.f;
    float bestConfidence = 0.f;
    ResolveMethod bestMethod = ResolveMethod::LBY;
    float bestYaw = entity->eyeAngles().y;

    for (int i = 0; i < static_cast<int>(ResolveMethod::COUNT); i++) {
        const auto& vote = info.votes[i];
        if (!vote.valid)
            continue;

        float weight = getMethodWeight(static_cast<ResolveMethod>(i), info);
        float weightedConfidence = vote.confidence * weight;

        switch (vote.side) {
        case ResolveSide::LEFT:
            leftScore += weightedConfidence;
            break;
        case ResolveSide::RIGHT:
            rightScore += weightedConfidence;
            break;
        case ResolveSide::ORIGINAL:
            centerScore += weightedConfidence;
            break;
        }

        totalWeight += weight;

        // Track best individual method
        if (weightedConfidence > bestConfidence) {
            bestConfidence = weightedConfidence;
            bestMethod = static_cast<ResolveMethod>(i);
            bestYaw = vote.suggested_yaw;
        }
    }

    // Normalize scores
    if (totalWeight > 0.f) {
        leftScore /= totalWeight;
        rightScore /= totalWeight;
        centerScore /= totalWeight;
    }

    // Determine winning side
    if (leftScore > rightScore && leftScore > centerScore) {
        info.side = ResolveSide::LEFT;
        info.confidence = leftScore;
    }
    else if (rightScore > leftScore && rightScore > centerScore) {
        info.side = ResolveSide::RIGHT;
        info.confidence = rightScore;
    }
    else {
        info.side = ResolveSide::ORIGINAL;
        info.confidence = centerScore;
    }

    info.winning_method = bestMethod;

    // Calculate weighted average yaw from all valid votes
    float totalYaw = 0.f;
    float yawWeight = 0.f;

    for (int i = 0; i < static_cast<int>(ResolveMethod::COUNT); i++) {
        const auto& vote = info.votes[i];
        if (!vote.valid)
            continue;

        float weight = getMethodWeight(static_cast<ResolveMethod>(i), info) * vote.confidence;

        // Convert yaw to vector for proper averaging
        float rad = vote.suggested_yaw * (3.14159265f / 180.f);
        totalYaw += std::atan2(
            std::sin(rad) * weight,
            std::cos(rad) * weight
        );
        yawWeight += weight;
    }

    if (yawWeight > 0.f) {
        info.resolved_yaw = normalizeYaw(totalYaw * (180.f / 3.14159265f));
    }
    else {
        info.resolved_yaw = entity->eyeAngles().y;
    }
}

float Resolver::calculateFinalYaw(Entity* entity, ResolverData& info) noexcept {
    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);

    // If high confidence, use resolved yaw directly
    if (info.confidence > 0.7f) {
        return info.resolved_yaw;
    }

    // Otherwise, use side-based calculation
    float desyncAngle = maxDesync;

    if (info.is_low_delta) {
        desyncAngle *= 0.5f;
    }

    if (info.extended_desync) {
        desyncAngle = maxDesync; // Full desync
    }

    return eyeYaw + desyncAngle * static_cast<float>(info.side);
}

float Resolver::getMethodWeight(ResolveMethod method, ResolverData& info) noexcept {
    // Base weights
    float weight = 1.f;

    switch (method) {
    case ResolveMethod::LBY:
        weight = WEIGHT_LBY;
        // LBY is more reliable when standing still
        if (!info.is_moving && info.lby.updated_this_tick)
            weight *= 1.5f;
        // Less reliable when jittering
        if (info.jitter.is_jittering)
            weight *= 0.6f;
        break;

    case ResolveMethod::ANIMSTATE:
        weight = WEIGHT_ANIMSTATE;
        // AnimState is very reliable when moving
        if (info.is_moving)
            weight *= 1.4f;
        break;

    case ResolveMethod::POSE_PARAM:
        weight = WEIGHT_POSE;
        // Pose params are good for detecting extended desync
        if (info.extended_desync)
            weight *= 1.3f;
        break;

    case ResolveMethod::TRACE:
        weight = WEIGHT_TRACE;
        // Trace is good for freestanding detection
        break;

    case ResolveMethod::BRUTEFORCE:
        weight = WEIGHT_BRUTE;
        // Bruteforce gets more weight after misses
        weight += info.misses * 0.15f;
        break;

    default:
        break;
    }

    return weight;
}

// ============================================
// LBY RESOLVER
// ============================================

void Resolver::updateLBYData(Entity* entity, ResolverData& info) noexcept {
    auto& lby = info.lby;

    lby.last_lby = lby.current_lby;
    lby.current_lby = entity->lby();
    lby.lby_delta = angleDiff(lby.current_lby, lby.last_lby);

    // Store LBY history
    lby.lby_history[lby.history_index % HISTORY_SIZE] = lby.current_lby;
    lby.history_index++;

    // Detect LBY update
    lby.updated_this_tick = std::fabsf(lby.lby_delta) > 1.f;

    if (lby.updated_this_tick) {
        lby.last_update_time = memory->globalVars->currenttime;
        lby.flick_count++;
    }

    // Track standing time for LBY prediction
    if (!info.is_moving && info.is_on_ground) {
        lby.standing_time += memory->globalVars->intervalPerTick;
    }
    else {
        lby.standing_time = 0.f;
        lby.last_moving_lby = lby.current_lby;
        info.last_moving_time = memory->globalVars->currenttime;
    }

    // Predict next LBY update
    lby.next_update_time = lby.last_update_time + LBY_UPDATE_TIME;

    // Detect LBY flicking
    lby.is_flicking = detectLBYFlick(entity, info);
}

bool Resolver::predictLBYUpdate(Entity* entity, ResolverData& info) noexcept {
    if (info.is_moving)
        return false;

    const float currentTime = memory->globalVars->currenttime;
    const float timeSinceUpdate = currentTime - info.lby.last_update_time;

    // LBY updates every 1.1 seconds when standing still
    return timeSinceUpdate >= LBY_UPDATE_TIME - memory->globalVars->intervalPerTick;
}

bool Resolver::detectLBYFlick(Entity* entity, ResolverData& info) noexcept {
    auto& lby = info.lby;

    // Check for rapid LBY changes (flicking)
    int flickCount = 0;
    for (int i = 0; i < 8; i++) {
        int idx1 = (lby.history_index - i - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        int idx2 = (lby.history_index - i - 2 + HISTORY_SIZE) % HISTORY_SIZE;

        float delta = std::fabsf(angleDiff(lby.lby_history[idx1], lby.lby_history[idx2]));
        if (delta > 30.f) {
            flickCount++;
        }
    }

    return flickCount >= 3;
}

Resolver::ResolveSide Resolver::getLBYSide(Entity* entity, ResolverData& info) noexcept {
    const float eyeYaw = entity->eyeAngles().y;
    const float lbyDelta = angleDiff(info.lby.current_lby, eyeYaw);

    if (std::fabsf(lbyDelta) < 10.f)
        return ResolveSide::ORIGINAL;

    return lbyDelta > 0.f ? ResolveSide::LEFT : ResolveSide::RIGHT;
}

Resolver::MethodVote Resolver::resolveLBY(Entity* entity, ResolverData& info) noexcept {
    MethodVote vote;
    vote.method = ResolveMethod::LBY;
    vote.valid = true;

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);

    // When moving, LBY is reliable
    if (info.is_moving) {
        vote.side = ResolveSide::ORIGINAL;
        vote.suggested_yaw = info.lby.current_lby;
        vote.confidence = 0.9f;
        return vote;
    }

    // Check if LBY just updated
    if (info.lby.updated_this_tick && !info.lby.is_flicking) {
        vote.side = getLBYSide(entity, info);
        vote.suggested_yaw = info.lby.current_lby;
        vote.confidence = 0.85f;
        return vote;
    }

    // Predict LBY update
    if (predictLBYUpdate(entity, info)) {
        vote.side = getLBYSide(entity, info);
        vote.suggested_yaw = eyeYaw + maxDesync * static_cast<float>(vote.side);
        vote.confidence = 0.7f;
        return vote;
    }

    // LBY flicking - less reliable
    if (info.lby.is_flicking) {
        vote.side = getLBYSide(entity, info);
        vote.confidence = 0.4f;
    }
    else {
        // Use last known LBY
        const float lbyDelta = angleDiff(info.lby.last_moving_lby, eyeYaw);
        vote.side = lbyDelta > 0.f ? ResolveSide::LEFT : ResolveSide::RIGHT;
        vote.confidence = 0.5f;
    }

    vote.suggested_yaw = eyeYaw + maxDesync * static_cast<float>(vote.side);
    return vote;
}

// ============================================
// ANIMSTATE RESOLVER
// ============================================

void Resolver::updateAnimStateData(Entity* entity, ResolverData& info) noexcept {
    auto* animState = entity->getAnimstate();
    if (!animState)
        return;

    auto& as = info.animstate;

    // Store previous values
    as.last_foot_yaw = as.foot_yaw;
    std::memcpy(as.prev_layers.data(), as.layers.data(), sizeof(as.layers));

    // Update current values
    as.foot_yaw = animState->footYaw;
    as.goal_feet_yaw = animState->eyeYaw; // Goal feet yaw is based on eye yaw
    as.eye_yaw = animState->eyeYaw;
    as.duck_amount = animState->animDuckAmount;
    as.velocity_length = entity->velocity().length2D();

    // Copy current layers
    std::memcpy(as.layers.data(), entity->get_animlayers(), sizeof(as.layers));

    // Calculate layer deltas
    for (int i = 0; i < 13; i++) {
        as.layer_deltas[i].weight_delta = as.layers[i].weight - as.prev_layers[i].weight;
        as.layer_deltas[i].cycle_delta = as.layers[i].cycle - as.prev_layers[i].cycle;
        as.layer_deltas[i].rate_delta = as.layers[i].playbackRate - as.prev_layers[i].playbackRate;
    }

    // Calculate move yaw
    as.move_yaw = getMoveYaw(entity, info);
}

float Resolver::getMoveYaw(Entity* entity, ResolverData& info) noexcept {
    const Vector velocity = entity->velocity();
    const float speed = velocity.length2D();

    if (speed < 0.1f)
        return info.animstate.foot_yaw;

    // Calculate velocity direction
    float velYaw = std::atan2(velocity.y, velocity.x) * (180.f / 3.14159265f);

    // Move yaw is relative to foot yaw
    return normalizeYaw(velYaw - info.animstate.foot_yaw);
}

bool Resolver::detectExtendedDesync(Entity* entity, ResolverData& info) noexcept {
    // Check animation layer 3 for extended desync detection
    const auto& layer3 = info.animstate.layers[3];

    if (layer3.cycle == 0.f && layer3.weight == 0.f) {
        return true; // Extended desync detected
    }

    return false;
}

float Resolver::compareAnimLayers(Entity* entity, ResolverData& info) noexcept {
    // Compare movement layer (layer 6) for side detection
    const auto& moveLayer = info.animstate.layers[6];
    const auto& prevMoveLayer = info.animstate.prev_layers[6];

    if (moveLayer.weight < 0.01f)
        return 0.f;

    // Simulate layers for left and right
    // This is simplified - in reality you'd need to fully simulate the animstate
    float leftDelta = std::fabsf(moveLayer.playbackRate - prevMoveLayer.playbackRate);
    float rightDelta = std::fabsf(moveLayer.playbackRate - prevMoveLayer.playbackRate);

    return leftDelta - rightDelta;
}

Resolver::ResolveSide Resolver::getAnimStateSide(Entity* entity, ResolverData& info) noexcept {
    const float eyeYaw = entity->eyeAngles().y;
    const float footYaw = info.animstate.foot_yaw;
    const float delta = angleDiff(footYaw, eyeYaw);

    if (std::fabsf(delta) < 10.f)
        return ResolveSide::ORIGINAL;

    return delta > 0.f ? ResolveSide::RIGHT : ResolveSide::LEFT;
}

Resolver::MethodVote Resolver::resolveAnimState(Entity* entity, ResolverData& info) noexcept {
    MethodVote vote;
    vote.method = ResolveMethod::ANIMSTATE;
    vote.valid = true;

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);

    // Check movement layer for direction
    const auto& moveLayer = info.animstate.layers[ANIMATION_LAYER_MOVEMENT_MOVE];
    const auto& prevMoveLayer = info.animstate.prev_layers[ANIMATION_LAYER_MOVEMENT_MOVE];

    if (info.is_moving && moveLayer.weight > 0.1f) {
        // When moving, compare playback rates
        float rateDelta = moveLayer.playbackRate - prevMoveLayer.playbackRate;

        if (std::fabsf(rateDelta) > 0.001f) {
            vote.side = rateDelta > 0.f ? ResolveSide::RIGHT : ResolveSide::LEFT;
            vote.confidence = (std::min)(std::fabsf(rateDelta) * 10.f, 0.9f);
        }
        else {
            vote.side = getAnimStateSide(entity, info);
            vote.confidence = 0.6f;
        }
    }
    else {
        // Standing - use adjust layer (layer 3)
        const auto& adjustLayer = info.animstate.layers[ANIMATION_LAYER_ADJUST];

        if (adjustLayer.weight > 0.f && adjustLayer.cycle > 0.f) {
            // Adjusting balance - can determine side from this
            vote.side = adjustLayer.cycle > 0.5f ? ResolveSide::LEFT : ResolveSide::RIGHT;
            vote.confidence = 0.7f;
        }
        else {
            vote.side = getAnimStateSide(entity, info);
            vote.confidence = 0.5f;
        }
    }

    vote.suggested_yaw = eyeYaw + maxDesync * static_cast<float>(vote.side);
    return vote;
}

// ============================================
// POSE PARAMETER RESOLVER
// ============================================

void Resolver::updatePoseParamData(Entity* entity, ResolverData& info) noexcept {
    auto& pose = info.pose;

    // Store previous values
    std::memcpy(pose.previous.data(), pose.current.data(), sizeof(pose.current));

    // Get current pose parameters
    for (int i = 0; i < POSE_PARAM_COUNT && i < 24; i++) {
        pose.current[i] = entity->poseParameters()[i];
        pose.deltas[i] = pose.current[i] - pose.previous[i];
    }

    // Extract key pose parameters
    pose.body_yaw = pose.current[BODY_YAW];
    pose.lean_yaw = pose.current[LEAN_YAW];
    pose.move_yaw = pose.current[MOVE_YAW];
}

void Resolver::simulatePoseParams(Entity* entity, ResolverData& info, float yaw,
    std::array<float, POSE_PARAM_COUNT>& out) noexcept {
    auto* animState = entity->getAnimstate();
    if (!animState)
        return;

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);

    // Simulate body_yaw pose parameter
    float bodyYaw = angleDiff(yaw, eyeYaw);
    bodyYaw = std::clamp(bodyYaw, -maxDesync, maxDesync);

    // Normalize to 0-1 range (pose parameters are normalized)
    out[BODY_YAW] = (bodyYaw + 60.f) / 120.f;

    // Simulate lean_yaw
    out[LEAN_YAW] = info.pose.current[LEAN_YAW]; // Keep current lean

    // Simulate move_yaw
    if (info.is_moving) {
        const Vector velocity = entity->velocity();
        float velYaw = std::atan2(velocity.y, velocity.x) * (180.f / 3.14159265f);
        float moveYaw = angleDiff(velYaw, yaw);
        out[MOVE_YAW] = (moveYaw + 180.f) / 360.f;
    }
    else {
        out[MOVE_YAW] = 0.5f;
    }
}

float Resolver::comparePoseParams(const std::array<float, POSE_PARAM_COUNT>& a,
    const std::array<float, POSE_PARAM_COUNT>& b) noexcept {
    float diff = 0.f;

    // Weight important pose parameters more heavily
    const float weights[POSE_PARAM_COUNT] = {
        1.5f, // LEAN_YAW
        0.5f, // SPEED
        0.0f, // LADDER_SPEED
        0.0f, // LADDER_YAW
        1.2f, // MOVE_YAW
        0.5f, // RUN
        2.0f, // BODY_YAW (most important)
        1.0f, // BODY_PITCH
        0.0f, // DEATH_YAW
        0.5f, // STAND
        0.0f, // JUMP_FALL
        0.3f, // AIM_BLEND_STAND_IDLE
        0.3f, // AIM_BLEND_CROUCH_IDLE
        0.8f, // STRAFE_DIR
    };

    for (int i = 0; i < 14; i++) {
        diff += std::fabsf(a[i] - b[i]) * weights[i];
    }

    return diff;
}

float Resolver::extractBodyYawFromPose(Entity* entity, ResolverData& info) noexcept {
    // Body yaw pose parameter is normalized 0-1 representing -60 to +60 degrees
    float bodyYawNorm = info.pose.current[BODY_YAW];
    float bodyYaw = (bodyYawNorm * 120.f) - 60.f;

    return bodyYaw;
}

Resolver::ResolveSide Resolver::getPoseParamSide(Entity* entity, ResolverData& info) noexcept {
    float bodyYaw = extractBodyYawFromPose(entity, info);

    if (std::fabsf(bodyYaw) < 10.f)
        return ResolveSide::ORIGINAL;

    return bodyYaw > 0.f ? ResolveSide::RIGHT : ResolveSide::LEFT;
}

Resolver::MethodVote Resolver::resolvePoseParams(Entity* entity, ResolverData& info) noexcept {
    MethodVote vote;
    vote.method = ResolveMethod::POSE_PARAM;
    vote.valid = true;

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);

    // Simulate pose parameters for different yaws
    simulatePoseParams(entity, info, eyeYaw + maxDesync, info.pose.simulated_left);
    simulatePoseParams(entity, info, eyeYaw - maxDesync, info.pose.simulated_right);
    simulatePoseParams(entity, info, eyeYaw, info.pose.simulated_center);

    // Compare with actual pose parameters
    float leftDiff = comparePoseParams(info.pose.current, info.pose.simulated_left);
    float rightDiff = comparePoseParams(info.pose.current, info.pose.simulated_right);
    float centerDiff = comparePoseParams(info.pose.current, info.pose.simulated_center);

    // Find best match
    float minDiff = (std::min)({ leftDiff, rightDiff, centerDiff });

    if (minDiff == leftDiff) {
        vote.side = ResolveSide::LEFT;
        vote.suggested_yaw = eyeYaw + maxDesync;
    }
    else if (minDiff == rightDiff) {
        vote.side = ResolveSide::RIGHT;
        vote.suggested_yaw = eyeYaw - maxDesync;
    }
    else {
        vote.side = ResolveSide::ORIGINAL;
        vote.suggested_yaw = eyeYaw;
    }

    // Calculate confidence based on difference magnitude
    float totalDiff = leftDiff + rightDiff + centerDiff;
    if (totalDiff > 0.f) {
        float winnerDiff = minDiff;
        vote.confidence = 1.f - (winnerDiff / totalDiff);
    }
    else {
        vote.confidence = 0.3f;
    }

    // Use body_yaw pose parameter as additional validation
    ResolveSide poseBasedSide = getPoseParamSide(entity, info);
    if (poseBasedSide == vote.side) {
        vote.confidence = (std::min)(vote.confidence + 0.15f, 1.f);
    }

    return vote;
}

// ============================================
// TRACE RESOLVER
// ============================================

Resolver::MethodVote Resolver::resolveTrace(Entity* entity, ResolverData& info) noexcept {
    MethodVote vote;
    vote.method = ResolveMethod::TRACE;
    vote.valid = true;

    if (!localPlayer) {
        vote.valid = false;
        return vote;
    }

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);
    const float atTarget = getAtTargetYaw(entity);

    Vector forward, right, up;
    Helpers::AngleVectors(Vector(0, atTarget, 0), &forward, &right, &up);

    const Vector src = entity->getEyePosition();
    const Vector dst = src + forward * 384.f;

    Trace trLeft, trRight, trCenter;

    // Trace left
    interfaces->engineTrace->traceRay(
        Ray(src - right * 35.f, dst - right * 35.f),
        MASK_SHOT, { entity }, trLeft
    );
    float leftDist = (trLeft.endpos - trLeft.startpos).length();

    // Trace right
    interfaces->engineTrace->traceRay(
        Ray(src + right * 35.f, dst + right * 35.f),
        MASK_SHOT, { entity }, trRight
    );
    float rightDist = (trRight.endpos - trRight.startpos).length();

    // Trace center
    interfaces->engineTrace->traceRay(
        Ray(src, dst),
        MASK_SHOT, { entity }, trCenter
    );
    float centerDist = (trCenter.endpos - trCenter.startpos).length();

    // Determine freestanding side
    float maxDist = (std::max)({leftDist, rightDist, centerDist});
    float distDiff = std::fabsf(leftDist - rightDist);

    if (distDiff < 20.f) {
        // No significant difference - can't determine side
        vote.side = ResolveSide::ORIGINAL;
        vote.confidence = 0.3f;
    }
    else if (leftDist > rightDist) {
        // Left side is more open - they're likely showing right
        vote.side = ResolveSide::LEFT;
        vote.confidence = (std::min)(distDiff / 100.f, 0.8f);
    }
    else {
        vote.side = ResolveSide::RIGHT;
        vote.confidence = (std::min)(distDiff / 100.f, 0.8f);
    }

    vote.suggested_yaw = eyeYaw + maxDesync * static_cast<float>(vote.side);
    return vote;
}

// ============================================
// BRUTEFORCE RESOLVER
// ============================================

Resolver::MethodVote Resolver::resolveBruteforce(Entity* entity, ResolverData& info) noexcept {
    MethodVote vote;
    vote.method = ResolveMethod::BRUTEFORCE;
    vote.valid = true;

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);
    const int misses = info.misses;

    // Bruteforce pattern
    float bruteYaw = 0.f;

    if (info.jitter.is_jittering) {
        // For jitter, use different pattern
        switch (misses % 4) {
        case 0:
            bruteYaw = maxDesync;
            vote.side = ResolveSide::LEFT;
            break;
        case 1:
            bruteYaw = -maxDesync;
            vote.side = ResolveSide::RIGHT;
            break;
        case 2:
            bruteYaw = maxDesync * 0.5f;
            vote.side = ResolveSide::LEFT;
            break;
        case 3:
            bruteYaw = -maxDesync * 0.5f;
            vote.side = ResolveSide::RIGHT;
            break;
        }
    }
    else {
        // Standard bruteforce
        switch (misses % 6) {
        case 0:
            bruteYaw = maxDesync * static_cast<float>(info.side);
            break;
        case 1:
            bruteYaw = -maxDesync * static_cast<float>(info.side);
            vote.side = info.side == ResolveSide::LEFT ? ResolveSide::RIGHT : ResolveSide::LEFT;
            break;
        case 2:
            bruteYaw = 0.f;
            vote.side = ResolveSide::ORIGINAL;
            break;
        case 3:
            bruteYaw = maxDesync * 0.5f;
            vote.side = ResolveSide::LEFT;
            break;
        case 4:
            bruteYaw = -maxDesync * 0.5f;
            vote.side = ResolveSide::RIGHT;
            break;
        case 5:
            bruteYaw = maxDesync * 0.3f;
            vote.side = ResolveSide::LEFT;
            break;
        }
    }

    vote.suggested_yaw = eyeYaw + bruteYaw;
    vote.confidence = 0.3f + (misses > 0 ? 0.1f * (std::min)(misses, 5) : 0.f);

    return vote;
}

// ============================================
// DETECTION METHODS
// ============================================

Resolver::ResolveMode Resolver::detectMode(Entity* entity, ResolverData& info) noexcept {
    if (!entity || !entity->getAnimstate())
        return ResolveMode::NONE;

    const bool onGround = (entity->flags() & FL_ONGROUND) != 0;
    const float speed = entity->velocity().length2D();

    if (!onGround)
        return ResolveMode::AIR;

    // Check for fake walk
    if (detectFakeWalk(entity, info))
        return ResolveMode::SLOW_WALK;

    if (speed > MOVING_SPEED_THRESHOLD && speed < FAKE_WALK_THRESHOLD)
        return ResolveMode::SLOW_WALK;

    if (speed > MOVING_SPEED_THRESHOLD)
        return ResolveMode::MOVING;

    return ResolveMode::STANDING;
}

bool Resolver::detectJitter(Entity* entity, ResolverData& info) noexcept {
    auto& jitter = info.jitter;

    // Store current yaw
    float currentYaw = entity->eyeAngles().y;
    jitter.yaw_history[jitter.history_index % HISTORY_SIZE] = currentYaw;
    jitter.history_index++;

    if (jitter.history_index < 8)
        return false;

    // Analyze yaw history for jitter patterns
    jitter.jitter_ticks = 0;
    jitter.static_ticks = 0;
    float yawSum = 0.f;

    for (int i = 0; i < 16; i++) {
        int idx = (jitter.history_index - i - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        yawSum += jitter.yaw_history[idx];

        if (i > 0) {
            int prevIdx = (jitter.history_index - i + HISTORY_SIZE) % HISTORY_SIZE;
            float delta = std::fabsf(angleDiff(
                jitter.yaw_history[idx],
                jitter.yaw_history[prevIdx]
            ));

            if (delta < 2.f)
                jitter.static_ticks++;
            else if (delta > JITTER_THRESHOLD)
                jitter.jitter_ticks++;
        }
    }

    jitter.average_yaw = yawSum / 16.f;

    // Calculate variance
    float variance = 0.f;
    for (int i = 0; i < 16; i++) {
        int idx = (jitter.history_index - i - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        float diff = jitter.yaw_history[idx] - jitter.average_yaw;
        variance += diff * diff;
    }
    jitter.yaw_variance = variance / 16.f;

    // Jitter detection criteria
    jitter.is_jittering = (jitter.jitter_ticks > jitter.static_ticks) &&
        (jitter.jitter_ticks >= 4) &&
        (jitter.yaw_variance > 100.f);

    // Track jitter pattern for prediction
    if (jitter.is_jittering) {
        float lastDelta = angleDiff(currentYaw, jitter.yaw_history[
            (jitter.history_index - 2 + HISTORY_SIZE) % HISTORY_SIZE
        ]);
        jitter.jitter_side = lastDelta > 0.f ? 1 : -1;
    }

    return jitter.is_jittering;
}

bool Resolver::detectLowDelta(Entity* entity, ResolverData& info) noexcept {
    auto* animState = entity->getAnimstate();
    if (!animState)
        return false;

    const float eyeYaw = entity->eyeAngles().y;
    const float footYaw = animState->footYaw;
    const float delta = std::fabsf(angleDiff(eyeYaw, footYaw));

    info.is_low_delta = delta < LOW_DELTA_THRESHOLD;
    return info.is_low_delta;
}

bool Resolver::detectFakeAngles(Entity* entity) noexcept {
    const Vector& eyeAngles = entity->eyeAngles();

    // Check for impossible/extreme pitch
    if (std::fabsf(eyeAngles.x) > 89.f)
        return true;

    // Check for very high pitch (likely anti-aim)
    if (std::fabsf(eyeAngles.x) > 75.f)
        return true;

    return false;
}

bool Resolver::detectFakeWalk(Entity* entity, ResolverData& info) noexcept {
    const float speed = entity->velocity().length2D();

    if (speed < 3.f || speed > FAKE_WALK_THRESHOLD)
        return false;

    // Check movement layers
    const auto& moveLayer = info.animstate.layers[ANIMATION_LAYER_MOVEMENT_MOVE];

    // Fake walk: moving but movement layer weight is inconsistent with speed
    if (moveLayer.weight > 0.1f && moveLayer.playbackRate < 0.4f) {
        return true;
    }

    return false;
}

// ============================================
// UTILITY METHODS
// ============================================

float Resolver::getMaxDesync(Entity* entity) noexcept {
    auto* animState = entity->getAnimstate();
    if (!animState)
        return 58.f;

    float duckAmount = animState->animDuckAmount;
    float speedFraction = std::clamp(animState->speedAsPortionOfWalkTopSpeed, 0.f, 1.f);
    float speedFactor = std::clamp(animState->speedAsPortionOfCrouchTopSpeed, 0.f, 1.f);

    float modifier = ((animState->walkToRunTransition * -0.3f) - 0.2f) * speedFraction + 1.f;

    if (duckAmount > 0.f) {
        modifier += (duckAmount * speedFactor) * (0.5f - modifier);
    }

    return 58.f * modifier;
}

float Resolver::buildServerAbsYaw(Entity* entity, float yaw) noexcept {
    const float eyeYaw = entity->eyeAngles().y;
    const float maxDelta = getMaxDesync(entity);

    float goalFeetYaw = normalizeYaw(yaw);
    float eyeFeetDelta = angleDiff(eyeYaw, goalFeetYaw);

    if (eyeFeetDelta > maxDelta)
        goalFeetYaw = eyeYaw - maxDelta;
    else if (eyeFeetDelta < -maxDelta)
        goalFeetYaw = eyeYaw + maxDelta;

    return normalizeYaw(goalFeetYaw);
}

float Resolver::getAtTargetYaw(Entity* entity) noexcept {
    if (!localPlayer)
        return 0.f;

    return Helpers::calculate_angle(
        localPlayer->getAbsOrigin(),
        entity->getAbsOrigin()
    ).y;
}

float Resolver::normalizeYaw(float yaw) noexcept {
    while (yaw > 180.f) yaw -= 360.f;
    while (yaw < -180.f) yaw += 360.f;
    return yaw;
}

float Resolver::angleDiff(float a, float b) noexcept {
    float diff = normalizeYaw(a - b);
    return diff;
}

float Resolver::approachAngle(float target, float value, float speed) noexcept {
    float delta = angleDiff(target, value);

    if (delta > speed)
        return value + speed;
    else if (delta < -speed)
        return value - speed;

    return target;
}

// ============================================
// DEBUG INFO
// ============================================

std::string Resolver::getMethodName(ResolveMethod method) noexcept {
    switch (method) {
    case ResolveMethod::LBY: return "LBY";
    case ResolveMethod::ANIMSTATE: return "AnimState";
    case ResolveMethod::POSE_PARAM: return "PoseParam";
    case ResolveMethod::TRACE: return "Trace";
    case ResolveMethod::BRUTEFORCE: return "Bruteforce";
    default: return "Unknown";
    }
}

std::string Resolver::getDebugInfo(int index) noexcept {
    if (index < 1 || index > 64)
        return "";

    const auto& info = data[index];
    std::string result;

    result += "Mode: ";
    switch (info.mode) {
    case ResolveMode::STANDING: result += "Standing"; break;
    case ResolveMode::MOVING: result += "Moving"; break;
    case ResolveMode::AIR: result += "Air"; break;
    case ResolveMode::SLOW_WALK: result += "SlowWalk"; break;
    default: result += "None"; break;
    }

    result += " | Side: ";
    switch (info.side) {
    case ResolveSide::LEFT: result += "Left"; break;
    case ResolveSide::RIGHT: result += "Right"; break;
    default: result += "Original"; break;
    }

    result += " | Conf: " + std::to_string(static_cast<int>(info.confidence * 100)) + "%";
    result += " | Winner: " + getMethodName(info.winning_method);
    result += " | Jitter: " + std::string(info.jitter.is_jittering ? "Yes" : "No");
    result += " | LowD: " + std::string(info.is_low_delta ? "Yes" : "No");
    result += " | Miss: " + std::to_string(info.misses);

    return result;
}

// ============================================
// EVENT HANDLING
// ============================================

void Resolver::processEvents(GameEvent* event) noexcept {
    if (!event || !localPlayer || interfaces->engine->isHLTV())
        return;

    switch (fnv::hashRuntime(event->getName())) {
    case fnv::hash("round_start"): {
        for (auto& d : data) {
            d.misses = 0;
            d.hits = 0;
        }
        snapshots.clear();
        break;
    }
    case fnv::hash("player_hurt"): {
        if (snapshots.empty())
            break;

        if (event->getInt("attacker") != localPlayer->getUserId())
            break;

        const auto playerId = event->getInt("userid");
        const auto index = interfaces->engine->getPlayerFromUserID(playerId);

        if (index > 0 && index < 65) {
            data[index].hits++;
            data[index].total_shots++;
        }

        if (!snapshots.empty())
            snapshots.pop_front();
        break;
    }
    case fnv::hash("bullet_impact"): {
        if (snapshots.empty())
            break;

        if (event->getInt("userid") != localPlayer->getUserId())
            break;

        auto& snapshot = snapshots.front();
        if (!snapshot.got_impact) {
            snapshot.time = memory->globalVars->serverTime();
            snapshot.impact_position = Vector{
                event->getFloat("x"),
                event->getFloat("y"),
                event->getFloat("z")
            };
            snapshot.got_impact = true;
        }
        break;
    }
    case fnv::hash("weapon_fire"): {
        const auto playerId = event->getInt("userid");
        if (playerId == localPlayer->getUserId())
            break;

        const auto index = interfaces->engine->getPlayerFromUserID(playerId);
        if (index > 0 && index < 65) {
            Animations::setPlayer(index)->shot = true;
        }
        break;
    }
    }
}

void Resolver::saveShot(int playerIndex, float simTime, int backtrackTick) noexcept {
    if (!localPlayer || playerIndex < 1 || playerIndex > 64)
        return;

    const auto entity = interfaces->entityList->getEntity(playerIndex);
    const auto& player = Animations::getPlayer(playerIndex);

    if (!entity || !player.gotMatrix)
        return;

    ShotSnapshot snapshot;
    snapshot.player_index = playerIndex;
    snapshot.backtrack_tick = backtrackTick;
    snapshot.eye_position = localPlayer->getEyePosition();
    snapshot.model = entity->getModel();
    snapshot.time = -1.f;
    snapshot.got_impact = false;
    snapshot.resolver_state = data[playerIndex];

    std::memcpy(snapshot.matrix, player.matrix.data(), sizeof(snapshot.matrix));

    snapshots.push_back(std::move(snapshot));

    while (snapshots.size() > MAX_SNAPSHOTS)
        snapshots.pop_front();

    data[playerIndex].total_shots++;
}

void Resolver::processMissedShots() noexcept {
    if (!enabled || snapshots.empty())
        return;

    if (!localPlayer || !localPlayer->isAlive()) {
        snapshots.clear();
        return;
    }

    auto& snapshot = snapshots.front();

    if (snapshot.time < 0.f)
        return;

    if (memory->globalVars->serverTime() - snapshot.time > 1.f) {
        snapshots.pop_front();
        return;
    }

    if (!snapshot.got_impact) {
        snapshots.pop_front();
        return;
    }

    const auto entity = interfaces->entityList->getEntity(snapshot.player_index);
    if (!entity) {
        snapshots.pop_front();
        return;
    }

    const Model* model = snapshot.model;
    if (!model) {
        snapshots.pop_front();
        return;
    }

    StudioHdr* hdr = interfaces->modelInfo->getStudioModel(model);
    if (!hdr) {
        snapshots.pop_front();
        return;
    }

    StudioHitboxSet* set = hdr->getHitboxSet(0);
    if (!set) {
        snapshots.pop_front();
        return;
    }

    const auto angle = AimbotFunction::calculateRelativeAngle(
        snapshot.eye_position,
        snapshot.impact_position,
        Vector{}
    );
    const auto end = snapshot.impact_position + Vector::fromAngle(angle) * 2000.f;

    bool resolverMissed = false;
    for (int hitbox = 0; hitbox < Hitboxes::Max; hitbox++) {
        if (AimbotFunction::hitboxIntersection(snapshot.matrix, hitbox, set,
            snapshot.eye_position, end)) {
            resolverMissed = true;
            data[snapshot.player_index].misses++;

            const auto& savedState = snapshot.resolver_state;
            Logger::addLog("Resolver miss | Method: " + getMethodName(savedState.winning_method) +
                " | Side: " + (savedState.side == ResolveSide::LEFT ? "L" :
                    savedState.side == ResolveSide::RIGHT ? "R" : "O") +
                " | Conf: " + std::to_string(static_cast<int>(savedState.confidence * 100)) + "%");
            break;
        }
    }

    if (!resolverMissed) {
        Logger::addLog("Spread miss");
    }

    snapshots.pop_front();
}

void Resolver::updateEventListeners(bool forceRemove) noexcept {
    class ResolverEventListener : public GameEventListener {
    public:
        void fireGameEvent(GameEvent* event) override {
            Resolver::processEvents(event);
        }
    };

    static ResolverEventListener listener;
    static bool registered = false;

    if (enabled && !registered && !forceRemove) {
        interfaces->gameEventManager->addListener(&listener, "bullet_impact");
        interfaces->gameEventManager->addListener(&listener, "player_hurt");
        interfaces->gameEventManager->addListener(&listener, "round_start");
        interfaces->gameEventManager->addListener(&listener, "weapon_fire");
        registered = true;
    }
    else if ((!enabled || forceRemove) && registered) {
        interfaces->gameEventManager->removeListener(&listener);
        registered = false;
    }
}