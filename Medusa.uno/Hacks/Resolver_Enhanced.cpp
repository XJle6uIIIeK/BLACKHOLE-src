// ============================================================================
// BLACKHOLE ENHANCED RESOLVER IMPLEMENTATION
// Version: 2.0
// Features: ML-based bruteforce, advanced jitter detection, confidence system
// ============================================================================

#include "Resolver_Enhanced.h"
#include "AimbotFunctions.h"
#include "Animations.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "../SDK/Engine.h"
#include "../SDK/EngineTrace.h"
#include "../Helpers.h"
#include <algorithm>
#include <cmath>

namespace ResolverEnhanced {

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

float get_max_desync_delta(Entity* entity) noexcept {
    if (!entity || !entity->getAnimstate())
        return 58.0f;
    
    auto animstate = entity->getAnimstate();
    float duck_amount = animstate->animDuckAmount;
    float speed_fraction = std::clamp(animstate->speedAsPortionOfWalkTopSpeed, 0.0f, 1.0f);
    float speed_factor = std::clamp(animstate->speedAsPortionOfCrouchTopSpeed, 0.0f, 1.0f);
    
    float unk1 = ((animstate->walkToRunTransition * -0.3f) - 0.2f) * speed_fraction;
    float unk2 = unk1 + 1.0f;
    
    if (duck_amount > 0) {
        unk2 += ((duck_amount * speed_factor) * (0.5f - unk2));
    }
    
    float max_desync = 58.0f * unk2; // Use animstate yaw modifier
    return std::clamp(max_desync, 0.0f, 60.0f);
}

float get_backward_yaw(Entity* entity) noexcept {
    if (!localPlayer || !entity)
        return 0.0f;
    
    return Helpers::calculate_angle(localPlayer->getAbsOrigin(), entity->getAbsOrigin()).y;
}

float get_left_yaw(Entity* entity) noexcept {
    return Helpers::normalizeYaw(get_backward_yaw(entity) - 90.0f);
}

float get_right_yaw(Entity* entity) noexcept {
    return Helpers::normalizeYaw(get_backward_yaw(entity) + 90.0f);
}

// ============================================================================
// DETECTION FUNCTIONS
// ============================================================================

bool detect_low_delta(Entity* entity) noexcept {
    if (!entity || !entity->getAnimstate())
        return false;
    
    auto animstate = entity->getAnimstate();
    float fl_eye_yaw = animstate->eyeYaw;
    float fl_desync_delta = std::remainder(fl_eye_yaw, animstate->footYaw);
    fl_desync_delta = std::clamp(fl_desync_delta, -60.0f, 60.0f);
    
    // Low delta if abs angle is less than 35 degrees
    return std::abs(fl_desync_delta) < 35.0f;
}

bool detect_extended_desync(Entity* entity) noexcept {
    if (!entity || !entity->animOverlays())
        return false;
    
    auto layers = entity->animOverlays();
    
    // Check layer 3 for extended desync indicators
    if (layers[3].cycle == 0.0f && layers[3].weight == 0.0f) {
        return true;
    }
    
    return false;
}

bool detect_fake_angles(Entity* entity) noexcept {
    if (!entity)
        return false;
    
    float pitch = entity->eyeAngles().x;
    
    // Fake pitch detection
    if (std::abs(pitch) > 85.0f) {
        return true;
    }
    
    // Check for impossible angles
    if (pitch > 89.0f || pitch < -89.0f) {
        return true;
    }
    
    return false;
}

int detect_freestanding(Entity* entity) noexcept {
    if (!entity || !localPlayer)
        return 0;
    
    Vector src3D, dst3D, forward, right, up;
    Trace tr;
    
    float backward_yaw = get_backward_yaw(entity);
    Helpers::AngleVectors(Vector(0, backward_yaw, 0), &forward, &right, &up);
    
    src3D = entity->getEyePosition();
    dst3D = src3D + (forward * 384.0f);
    
    // Trace back
    interfaces->engineTrace->traceRay({src3D, dst3D}, MASK_SHOT, {entity}, tr);
    float back_distance = (tr.endpos - tr.startpos).length();
    
    // Trace right
    interfaces->engineTrace->traceRay(Ray(src3D + right * 35.0f, dst3D + right * 35.0f), MASK_SHOT, {entity}, tr);
    float right_distance = (tr.endpos - tr.startpos).length();
    
    // Trace left
    interfaces->engineTrace->traceRay(Ray(src3D - right * 35.0f, dst3D - right * 35.0f), MASK_SHOT, {entity}, tr);
    float left_distance = (tr.endpos - tr.startpos).length();
    
    // More space on left = body should be right
    if (left_distance > right_distance) {
        return 1;  // Right
    } else if (right_distance > left_distance) {
        return -1; // Left
    }
    
    return 0; // Center/Unknown
}

// ============================================================================
// ADVANCED ANALYSIS
// ============================================================================

bool analyze_animation_layers(Entity* entity, ResolverData& data) noexcept {
    if (!entity || !entity->animOverlays())
        return false;
    
    auto layers = entity->animOverlays();
    auto prev_layers = Animations::getPlayer(entity->index()).oldlayers;
    
    // Analyze layer 6 (movement layer) for desync detection
    float layer6_delta = std::abs(layers[6].playbackRate - prev_layers[6].playbackRate);
    
    // Analyze layer 12 (lean layer) for body direction
    float layer12_weight = layers[12].weight;
    
    // High confidence if layers are stable
    if (layer6_delta < 0.01f && layer12_weight > 0.0f) {
        data.confidence.animation_confidence = 0.85f;
        
        // Determine side from layer weight
        if (layer12_weight > 0.5f) {
            data.side = ResolverSide::RIGHT;
        } else if (layer12_weight < -0.5f) {
            data.side = ResolverSide::LEFT;
        } else {
            data.side = ResolverSide::CENTER;
        }
        
        return true;
    }
    
    // Medium confidence if only one layer is useful
    data.confidence.animation_confidence = 0.5f;
    return false;
}

float analyze_velocity_direction(Entity* entity, ResolverData& data) noexcept {
    if (!entity)
        return 0.0f;
    
    Vector velocity = entity->velocity();
    float vel_length = velocity.length2D();
    
    // Not moving = no velocity confidence
    if (vel_length < 5.0f) {
        data.confidence.velocity_confidence = 0.0f;
        return 0.0f;
    }
    
    // Calculate velocity angle
    float vel_yaw = Helpers::rad2deg(std::atan2(velocity.y, velocity.x));
    float eye_yaw = entity->eyeAngles().y;
    
    // High velocity confidence when moving fast
    data.confidence.velocity_confidence = std::min(vel_length / 250.0f, 1.0f);
    
    return Helpers::normalizeYaw(vel_yaw);
}

void detect_side_advanced(Entity* entity, ResolverData& data) noexcept {
    if (!entity || !entity->getAnimstate())
        return;
    
    // Method 1: Freestanding
    int freestand_side = detect_freestanding(entity);
    if (freestand_side != 0) {
        data.confidence.trace_confidence = 0.7f;
        data.side = static_cast<ResolverSide>(freestand_side);
        return;
    }
    
    // Method 2: Animation layers
    if (analyze_animation_layers(entity, data)) {
        return;
    }
    
    // Method 3: Eye angle delta
    auto animstate = entity->getAnimstate();
    float eye_delta = Helpers::angleDiff(entity->eyeAngles().y, animstate->footYaw);
    
    if (std::abs(eye_delta) > 35.0f) {
        data.side = eye_delta > 0 ? ResolverSide::RIGHT : ResolverSide::LEFT;
        data.confidence.trace_confidence = 0.6f;
    } else {
        data.side = ResolverSide::CENTER;
        data.confidence.trace_confidence = 0.3f;
    }
}

// ============================================================================
// ANGLE CALCULATION
// ============================================================================

float calculate_optimal_desync_angle(Entity* entity, ResolverData& data) noexcept {
    if (!entity)
        return 0.0f;
    
    float max_desync = get_max_desync_delta(entity);
    
    // Check if we're in bruteforce mode
    if (data.missed_shots >= 2) {
        data.mode = ResolverMode::BRUTEFORCE;
        float brute_angle = data.bruteforce.get_next_angle();
        return brute_angle;
    }
    
    // Low delta detection
    if (data.is_low_delta) {
        max_desync *= 0.5f; // Use 50% of max desync
    }
    
    // Extended desync detection  
    if (data.is_extended_desync) {
        max_desync = 60.0f; // Force max
    }
    
    // Apply side
    float final_angle = 0.0f;
    switch (data.side) {
        case ResolverSide::LEFT:
            final_angle = -max_desync;
            break;
        case ResolverSide::RIGHT:
            final_angle = max_desync;
            break;
        case ResolverSide::CENTER:
        case ResolverSide::UNKNOWN:
        default:
            final_angle = 0.0f;
            break;
    }
    
    return final_angle;
}

float resolve_pitch(Entity* entity, ResolverData& data) noexcept {
    if (!entity)
        return 0.0f;
    
    float current_pitch = entity->eyeAngles().x;
    
    // Fake pitch detection
    if (std::abs(current_pitch) > 85.0f) {
        // Force down
        return 89.0f;
    }
    
    // In air with weird pitch
    if (!(entity->flags() & FL_ONGROUND) && current_pitch >= 178.36304f) {
        return -89.0f;
    }
    
    // Normal pitch
    if (std::abs(current_pitch) <= 89.0f) {
        return current_pitch;
    }
    
    // Default to down
    return 89.0f;
}

// ============================================================================
// MAIN RESOLVER FUNCTION
// ============================================================================

void resolve_player(Entity* entity, Animations::Players& player, Animations::Players& prev_player) noexcept {
    if (!entity || !entity->isAlive() || entity->isDormant())
        return;
    
    int idx = entity->index();
    auto& data = player_data[idx];
    
    // Update jitter detection
    data.jitter.update(entity->eyeAngles().y);
    
    // Detect current state
    data.is_low_delta = detect_low_delta(entity);
    data.is_extended_desync = detect_extended_desync(entity);
    data.is_faking = detect_fake_angles(entity);
    
    // Determine mode
    float velocity_2d = entity->velocity().length2D();
    if (!(entity->flags() & FL_ONGROUND)) {
        data.mode = ResolverMode::AIR;
    } else if (velocity_2d > 5.0f) {
        data.mode = ResolverMode::WALK;
    } else {
        data.mode = ResolverMode::STAND;
    }
    
    // Advanced side detection
    detect_side_advanced(entity, data);
    
    // Calculate velocity confidence
    analyze_velocity_direction(entity, data);
    
    // Calculate history confidence based on hit rate
    if (data.hit_shots + data.missed_shots > 0) {
        float hit_rate = (float)data.hit_shots / (float)(data.hit_shots + data.missed_shots);
        data.confidence.history_confidence = hit_rate;
    }
    
    // Calculate combined confidence
    data.confidence.calculate();
    
    // Calculate optimal desync angle
    data.desync_angle = calculate_optimal_desync_angle(entity, data);
    
    // Resolve pitch
    data.resolved_pitch = resolve_pitch(entity, data);
    
    // Apply to entity
    auto animstate = entity->getAnimstate();
    if (animstate) {
        data.resolved_yaw = Helpers::normalizeYaw(entity->eyeAngles().y + data.desync_angle);
        animstate->footYaw = data.resolved_yaw;
    }
    
    // Store for history
    data.last_side = data.side;
    data.last_desync_angle = data.desync_angle;
    data.last_resolve_angle = data.resolved_yaw;
    
    // Update global stats
    Resolver::desync = data.desync_angle;
}

// ============================================================================
// SHOT TRACKING
// ============================================================================

void process_shot_result(int player_index, bool hit, float angle_used) noexcept {
    if (player_index < 0 || player_index >= 65)
        return;
    
    auto& data = player_data[player_index];
    
    if (hit) {
        data.hit_shots++;
        data.bruteforce.register_hit(angle_used);
        data.missed_shots = 0; // Reset miss counter
        
        total_hits++;
    } else {
        data.missed_shots++;
        data.bruteforce.register_miss(angle_used);
        
        total_misses++;
    }
    
    // Update global hit rate
    if (total_hits + total_misses > 0) {
        overall_hit_rate = (float)total_hits / (float)(total_hits + total_misses) * 100.0f;
    }
    
    // Update legacy compatibility
    Resolver::hits = total_hits;
    Resolver::misses = total_misses;
    Resolver::hit_rate = overall_hit_rate;
    Resolver::missed_shots[player_index] = data.missed_shots;
}

void update_shot_tracking(GameEvent* event) noexcept {
    // This will be called from event system
    // Implementation depends on your event system
}

// ============================================================================
// RESET FUNCTIONS
// ============================================================================

void reset() noexcept {
    for (auto& data : player_data) {
        data.reset();
    }
    total_hits = 0;
    total_misses = 0;
    overall_hit_rate = 0.0f;
}

void reset_player(int index) noexcept {
    if (index >= 0 && index < 65) {
        player_data[index].reset();
    }
}

} // namespace ResolverEnhanced
