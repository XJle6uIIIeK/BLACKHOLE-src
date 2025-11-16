#include "ResolverImproved.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "../SDK/EngineTrace.h"
#include "../Helpers.h"
#include <algorithm>
#include <cmath>

namespace ImprovedResolver
{
    // ========================================
    // MAIN RESOLVER LOGIC
    // ========================================
    
    void Resolver::resolve_player(int index, Entity* entity, Animations::Players& player_data) noexcept
    {
        if (!entity || !entity->isAlive() || index < 1 || index > 64)
            return;
            
        auto& data = m_player_data[index];
        
        // Detect player state first
        data.state = get_player_state(entity);
        
        // Update jitter & low delta detection
        data.is_jittering = detect_jitter(index, entity);
        data.is_low_delta = detect_low_delta(entity);
        data.has_extended_desync = detect_extended_desync(index, entity, player_data);
        data.is_faking = detect_fake_angles(entity);
        
        // Priority-based resolution
        // 1. Try shot detection (most reliable)
        if (try_resolve_by_shot(index, entity, player_data))
        {
            data.mode = ResolverMode::SHOT_DETECTION;
            return;
        }
        
        // 2. Try animation layers (primary method)
        if (try_resolve_by_layers(index, entity, player_data))
        {
            data.mode = ResolverMode::ANIMATION_LAYERS;
            return;
        }
        
        // 3. Try low delta detection
        if (data.is_low_delta && try_resolve_low_delta(index, entity))
        {
            data.mode = ResolverMode::LOW_DELTA;
            return;
        }
        
        // 4. Try freestanding
        if (try_resolve_freestanding(index, entity))
        {
            data.mode = ResolverMode::FREESTANDING;
            return;
        }
        
        // 5. Last resort - bruteforce
        apply_bruteforce(index, entity);
        data.mode = ResolverMode::BRUTEFORCE;
    }
    
    // ========================================
    // SHOT-BASED RESOLUTION
    // ========================================
    
    bool Resolver::try_resolve_by_shot(int index, Entity* entity, Animations::Players& player) noexcept
    {
        if (!player.shot)
            return false;
            
        auto& data = m_player_data[index];
        
        // Calculate yaw from shot matrix
        const auto& shot_matrix = player.matrix[8]; // Head bone
        const Vector shot_origin = shot_matrix.origin();
        
        const Vector eye_pos = localPlayer->getEyePosition();
        const Vector delta = shot_origin - eye_pos;
        
        // Calculate angle to shot position
        const float yaw = atan2f(delta.y, delta.x) * (180.0f / 3.14159265f);
        
        data.resolved_yaw = normalize_yaw(yaw);
        data.side = (data.resolved_yaw > entity->eyeAngles().y) ? ResolverSide::LEFT : ResolverSide::RIGHT;
        
        // Update statistics
        update_statistics(index, true);
        
        return true;
    }
    
    // ========================================
    // ANIMATION LAYER RESOLUTION
    // ========================================
    
    bool Resolver::try_resolve_by_layers(int index, Entity* entity, Animations::Players& player) noexcept
    {
        if (player.layers[ANIMATION_LAYER_MOVEMENT_MOVE].weight <= 0.0f)
            return false; // Not enough layer data
            
        auto& data = m_player_data[index];
        
        // Compare current layers with previous
        const auto& current_layer = player.layers[ANIMATION_LAYER_MOVEMENT_MOVE];
        const auto& previous_layer = player.oldlayers[ANIMATION_LAYER_MOVEMENT_MOVE];
        
        // Calculate deltas for each possible side
        const float delta_left = std::fabsf(
            current_layer.playbackRate - player_data.resolver_layers[ROTATE_LEFT][ANIMATION_LAYER_MOVEMENT_MOVE].playbackRate
        );
        
        const float delta_right = std::fabsf(
            current_layer.playbackRate - player_data.resolver_layers[ROTATE_RIGHT][ANIMATION_LAYER_MOVEMENT_MOVE].playbackRate
        );
        
        const float delta_center = std::fabsf(
            current_layer.playbackRate - player_data.resolver_layers[ROTATE_SERVER][ANIMATION_LAYER_MOVEMENT_MOVE].playbackRate
        );
        
        // Find minimum delta (closest match)
        const float min_delta = std::min({delta_left, delta_right, delta_center});
        
        // Determine side based on closest match
        if (min_delta == delta_left && delta_left * 1000.0f < 1.0f)
        {
            data.side = ResolverSide::LEFT;
            data.desync_delta = data.has_extended_desync ? 60.0f : 58.0f;
        }
        else if (min_delta == delta_right && delta_right * 1000.0f < 1.0f)
        {
            data.side = ResolverSide::RIGHT;
            data.desync_delta = data.has_extended_desync ? -60.0f : -58.0f;
        }
        else if (min_delta == delta_center && delta_center * 1000.0f < 1.0f)
        {
            data.side = ResolverSide::CENTER;
            data.desync_delta = 0.0f;
        }
        else
        {
            return false; // No clear match
        }
        
        // Calculate resolved yaw
        data.resolved_yaw = normalize_yaw(entity->eyeAngles().y + data.desync_delta);
        
        return true;
    }
    
    // ========================================
    // LOW DELTA RESOLUTION
    // ========================================
    
    bool Resolver::try_resolve_low_delta(int index, Entity* entity) noexcept
    {
        if (!detect_low_delta(entity))
            return false;
            
        auto& data = m_player_data[index];
        
        const float eye_yaw = entity->eyeAngles().y;
        const float foot_yaw = entity->getAnimstate()->footYaw;
        const float delta = std::fabsf(angle_diff(eye_yaw, foot_yaw));
        
        // Low delta means they're using legit AA or minimal desync
        if (delta < 30.0f)
        {
            // Use smart angles based on statistics
            const float predicted = get_predicted_angle(index, entity);
            
            if (predicted != 0.0f)
            {
                data.resolved_yaw = predicted;
                data.desync_delta = angle_diff(predicted, eye_yaw);
            }
            else
            {
                // Fallback to small offsets
                data.desync_delta = (data.bruteforce_step % 2 == 0) ? 29.0f : -29.0f;
                data.resolved_yaw = normalize_yaw(eye_yaw + data.desync_delta);
            }
            
            data.side = (data.desync_delta > 0) ? ResolverSide::LEFT : ResolverSide::RIGHT;
            return true;
        }
        
        return false;
    }
    
    // ========================================
    // FREESTANDING RESOLUTION
    // ========================================
    
    bool Resolver::try_resolve_freestanding(int index, Entity* entity) noexcept
    {
        auto& data = m_player_data[index];
        
        const ResolverSide optimal_side = trace_optimal_side(entity);
        
        if (optimal_side == ResolverSide::UNKNOWN)
            return false;
            
        data.side = optimal_side;
        
        // Calculate angle based on freestanding side
        const float base_yaw = Helpers::calculate_angle(
            localPlayer->origin(), 
            entity->origin()
        ).y;
        
        switch (optimal_side)
        {
            case ResolverSide::LEFT:
                data.desync_delta = 58.0f;
                break;
            case ResolverSide::RIGHT:
                data.desync_delta = -58.0f;
                break;
            default:
                data.desync_delta = 0.0f;
                break;
        }
        
        data.resolved_yaw = normalize_yaw(entity->eyeAngles().y + data.desync_delta);
        
        return true;
    }
    
    // ========================================
    // BRUTEFORCE (LAST RESORT)
    // ========================================
    
    void Resolver::apply_bruteforce(int index, Entity* entity) noexcept
    {
        auto& data = m_player_data[index];
        
        // Adaptive bruteforce based on player data
        const bool is_jittering = data.is_jittering;
        const int step = data.bruteforce_step;
        
        float angle_offset = 0.0f;
        
        if (is_jittering)
        {
            // Jitter bruteforce (3 steps)
            switch (step % 3)
            {
                case 0: angle_offset = data.has_extended_desync ? 60.0f : 58.0f; break;
                case 1: angle_offset = 0.0f; break;
                case 2: angle_offset = data.has_extended_desync ? -60.0f : -58.0f; break;
            }
        }
        else
        {
            // Static bruteforce (5 steps with low delta)
            switch (step % 5)
            {
                case 0: angle_offset = 58.0f; break;
                case 1: angle_offset = -58.0f; break;
                case 2: angle_offset = 29.0f; break;
                case 3: angle_offset = -29.0f; break;
                case 4: angle_offset = 0.0f; break;
            }
        }
        
        data.desync_delta = angle_offset;
        data.resolved_yaw = normalize_yaw(entity->eyeAngles().y + angle_offset);
        data.side = (angle_offset > 0) ? ResolverSide::LEFT : 
                    (angle_offset < 0) ? ResolverSide::RIGHT : ResolverSide::CENTER;
    }
    
    // ========================================
    // DETECTION METHODS
    // ========================================
    
    bool Resolver::detect_jitter(int index, Entity* entity) noexcept
    {
        auto& data = m_player_data[index];
        
        const float current_yaw = entity->eyeAngles().y;
        
        // Store in circular buffer
        data.yaw_history[data.history_index % 16] = current_yaw;
        data.history_index++;
        
        // Need at least 4 samples
        if (data.history_index < 4)
            return false;
            
        // Analyze variance in yaw over last 16 ticks
        int jitter_count = 0;
        int static_count = 0;
        
        for (int i = 0; i < 15; i++)
        {
            const float diff = std::fabsf(
                angle_diff(data.yaw_history[i], data.yaw_history[i + 1])
            );
            
            if (diff < 1.0f)
                static_count++;
            else if (diff > 15.0f)
                jitter_count++;
        }
        
        return jitter_count > static_count;
    }
    
    bool Resolver::detect_low_delta(Entity* entity) noexcept
    {
        if (!entity->getAnimstate())
            return false;
            
        const float eye_yaw = entity->eyeAngles().y;
        const float foot_yaw = entity->getAnimstate()->footYaw;
        const float delta = std::fabsf(angle_diff(eye_yaw, foot_yaw));
        
        return delta < 30.0f;
    }
    
    bool Resolver::detect_extended_desync(int index, Entity* entity, Animations::Players& player) noexcept
    {
        // Check layer 3 for extended desync indicator
        if (player.layers[3].cycle == 0.0f && player.layers[3].weight == 0.0f)
            return true;
            
        // Check if max desync angle exceeds standard
        const float max_desync = entity->getMaxDesyncAngle();
        return max_desync > 58.5f;
    }
    
    bool Resolver::detect_fake_angles(Entity* entity) noexcept
    {
        const float pitch = entity->eyeAngles().x;
        
        // Check for extreme pitch values indicating fake AA
        if (std::fabsf(pitch) > 85.0f)
            return true;
            
        // Check for invalid pitch ranges
        if (pitch > 0.0f && pitch < 85.0f)
            return false;
        if (pitch < 0.0f && pitch > -85.0f)
            return false;
            
        return true;
    }
    
    // ========================================
    // FREESTANDING HELPERS
    // ========================================
    
    ResolverSide Resolver::trace_optimal_side(Entity* entity) noexcept
    {
        if (!entity)
            return ResolverSide::UNKNOWN;
            
        const Vector eye_pos = entity->getEyePosition();
        
        // Calculate base angle to local player
        const Vector to_local = localPlayer->origin() - entity->origin();
        const float base_yaw = atan2f(to_local.y, to_local.x) * (180.0f / 3.14159265f);
        
        // Trace three directions
        const float left_dist = trace_wall_distance(entity, base_yaw + 90.0f);
        const float right_dist = trace_wall_distance(entity, base_yaw - 90.0f);
        const float back_dist = trace_wall_distance(entity, base_yaw + 180.0f);
        
        // Determine optimal hiding side
        if (left_dist > right_dist + 50.0f) // Significant difference
            return ResolverSide::LEFT;
        else if (right_dist > left_dist + 50.0f)
            return ResolverSide::RIGHT;
            
        return ResolverSide::UNKNOWN;
    }
    
    float Resolver::trace_wall_distance(Entity* entity, float yaw_offset) noexcept
    {
        Vector forward, right, up;
        Helpers::AngleVectors(Vector(0, yaw_offset, 0), &forward, &right, &up);
        
        const Vector start = entity->getEyePosition();
        const Vector end = start + forward * 8192.0f;
        
        Trace trace{};
        interfaces->engineTrace->traceRay({start, end}, 0x4600400B, {entity}, trace);
        
        return (trace.endpos - start).length();
    }
    
    // ========================================
    // HELPER CALCULATIONS
    // ========================================
    
    float Resolver::calculate_desync_delta(Entity* entity, Animations::Players& player) noexcept
    {
        if (!entity->getAnimstate())
            return 0.0f;
            
        const float eye_yaw = entity->getAnimstate()->eyeYaw;
        const float foot_yaw = entity->getAnimstate()->footYaw;
        
        float delta = angle_diff(eye_yaw, foot_yaw);
        
        // Clamp to max possible desync
        const float max_desync = entity->getMaxDesyncAngle();
        delta = clamp(delta, -max_desync, max_desync);
        
        return delta;
    }
    
    PlayerState Resolver::get_player_state(Entity* entity) noexcept
    {
        if (!(entity->flags() & FL_ONGROUND))
            return PlayerState::AIR;
            
        if (entity->duckAmount() > 0.5f)
            return PlayerState::CROUCHING;
            
        if (entity->velocity().length2D() > 5.0f)
            return PlayerState::MOVING;
            
        return PlayerState::STANDING;
    }
    
    // ========================================
    // STATISTICS & ML
    // ========================================
    
    void Resolver::update_statistics(int index, bool hit) noexcept
    {
        if (index < 1 || index > 64)
            return;
            
        auto& data = m_player_data[index];
        
        data.total_shots++;
        
        if (hit)
        {
            data.hits++;
            m_total_hits++;
            
            // Reset bruteforce on hit
            data.bruteforce_step = 0;
        }
        else
        {
            data.misses++;
            m_total_misses++;
            
            // Increment bruteforce
            data.bruteforce_step++;
            
            // Prevent overflow
            if (data.bruteforce_step > 10)
                data.bruteforce_step = 0;
        }
        
        // Update hit rate
        if (data.total_shots > 0)
            data.hit_rate = static_cast<float>(data.hits) / static_cast<float>(data.total_shots) * 100.0f;
    }
    
    float Resolver::get_predicted_angle(int index, Entity* entity) noexcept
    {
        if (index < 1 || index > 64)
            return 0.0f;
            
        auto& data = m_player_data[index];
        
        // If we have good hit rate, use last successful angle
        if (data.hit_rate > 50.0f && data.hits > 2)
            return data.resolved_yaw;
            
        // Otherwise return 0 to try other methods
        return 0.0f;
    }
    
    float Resolver::get_global_hit_rate() const noexcept
    {
        const int total = m_total_hits + m_total_misses;
        if (total == 0)
            return 0.0f;
            
        return static_cast<float>(m_total_hits) / static_cast<float>(total) * 100.0f;
    }
    
    // ========================================
    // EVENT HANDLERS
    // ========================================
    
    void Resolver::on_player_hurt(int attacker_id, int victim_id, int hitgroup) noexcept
    {
        if (attacker_id != localPlayer->getUserId())
            return;
            
        const int index = interfaces->engine->getPlayerFromUserID(victim_id);
        if (index < 1 || index > 64)
            return;
            
        update_statistics(index, true);
    }
    
    void Resolver::on_weapon_fire(int user_id) noexcept
    {
        const int index = interfaces->engine->getPlayerFromUserID(user_id);
        if (index < 1 || index > 64 || index == localPlayer->index())
            return;
            
        // Mark that this player fired
        auto& shot_data = m_shot_history[index];
        ShotData data;
        data.server_time = memory->globalVars->serverTime();
        data.tick_count = memory->globalVars->tickCount;
        
        shot_data.push_front(data);
        
        // Keep only recent shots
        while (shot_data.size() > 10)
            shot_data.pop_back();
    }
    
    void Resolver::on_bullet_impact(int user_id, const Vector& impact) noexcept
    {
        if (user_id != localPlayer->getUserId())
            return;
            
        // Store impact for miss detection
        // Implementation depends on your shot tracking system
    }
    
    // ========================================
    // RESET FUNCTIONS
    // ========================================
    
    void Resolver::reset_player(int index) noexcept
    {
        if (index < 1 || index > 64)
            return;
            
        m_player_data[index].reset();
        m_shot_history[index].clear();
    }
    
    void Resolver::reset_all() noexcept
    {
        for (int i = 1; i <= 64; i++)
            reset_player(i);
            
        m_total_hits = 0;
        m_total_misses = 0;
    }
}
