#pragma once

#include "Animations.h"
#include "../SDK/GameEvent.h"
#include "../SDK/Entity.h"
#include <array>
#include <vector>
#include <deque>

// ============================================================================
// ENHANCED RESOLVER SYSTEM FOR BLACKHOLE
// Version: 2.0
// Purpose: Advanced resolver with machine learning capabilities
// ============================================================================

enum class ResolverMode : int {
    NONE = 0,
    STAND,
    WALK,
    AIR,
    MANUAL,
    BRUTEFORCE
};

enum class ResolverSide : int {
    LEFT = -1,
    CENTER = 0,
    RIGHT = 1,
    UNKNOWN = 2
};

// Confidence system for resolver decisions
struct ResolverConfidence {
    float animation_confidence = 0.0f;  // Confidence from animation layers
    float trace_confidence = 0.0f;      // Confidence from freestanding
    float history_confidence = 0.0f;    // Confidence from shot history
    float velocity_confidence = 0.0f;   // Confidence from velocity analysis
    float combined_confidence = 0.0f;   // Final combined confidence
    
    void calculate() noexcept {
        // Weighted average of all confidence sources
        combined_confidence = (
            animation_confidence * 0.35f +
            trace_confidence * 0.25f +
            history_confidence * 0.25f +
            velocity_confidence * 0.15f
        );
    }
    
    void reset() noexcept {
        animation_confidence = 0.0f;
        trace_confidence = 0.0f;
        history_confidence = 0.0f;
        velocity_confidence = 0.0f;
        combined_confidence = 0.0f;
    }
};

// Smart bruteforce system with learning
struct SmartBruteforce {
    // Predefined angles to try
    static constexpr std::array<float, 12> ANGLES = {
        58.0f, -58.0f,      // Max desync
        45.0f, -45.0f,      // Safe angles
        29.0f, -29.0f,      // Low delta
        35.0f, -35.0f,      // Medium delta
        90.0f, -90.0f,      // Sideways
        15.0f, -15.0f       // Micro adjustments
    };
    
    int current_index = 0;
    
    // Track success rate for each angle
    std::array<int, 12> hit_count = {};
    std::array<int, 12> miss_count = {};
    std::array<float, 12> success_rate = {};
    
    // Get next angle based on success rates
    float get_next_angle() noexcept {
        // Update success rates
        for (int i = 0; i < 12; ++i) {
            int total = hit_count[i] + miss_count[i];
            success_rate[i] = total > 0 ? (float)hit_count[i] / total : 0.5f;
        }
        
        // Find angle with highest success rate that hasn't been tried recently
        float best_angle = ANGLES[current_index % 12];
        float best_rate = -1.0f;
        
        for (int i = 0; i < 12; ++i) {
            if (success_rate[i] > best_rate) {
                best_rate = success_rate[i];
                best_angle = ANGLES[i];
            }
        }
        
        current_index++;
        return best_angle;
    }
    
    void register_hit(float angle) noexcept {
        for (int i = 0; i < 12; ++i) {
            if (std::abs(ANGLES[i] - angle) < 1.0f) {
                hit_count[i]++;
                break;
            }
        }
    }
    
    void register_miss(float angle) noexcept {
        for (int i = 0; i < 12; ++i) {
            if (std::abs(ANGLES[i] - angle) < 1.0f) {
                miss_count[i]++;
                break;
            }
        }
    }
    
    void reset() noexcept {
        current_index = 0;
        hit_count.fill(0);
        miss_count.fill(0);
        success_rate.fill(0.5f);
    }
};

// Jitter detection system
struct JitterDetection {
    float yaw_cache[20] = {};
    int cache_offset = 0;
    
    float last_yaw = 0.0f;
    int jitter_count = 0;
    int static_count = 0;
    
    bool is_jittering = false;
    float jitter_frequency = 0.0f;
    float jitter_magnitude = 0.0f;
    
    void update(float current_yaw) noexcept {
        yaw_cache[cache_offset % 20] = current_yaw;
        cache_offset++;
        
        // Analyze last 14 samples
        if (cache_offset >= 15) {
            jitter_count = 0;
            static_count = 0;
            float total_delta = 0.0f;
            
            for (int i = 0; i < 14; ++i) {
                float diff = std::abs(yaw_cache[i] - yaw_cache[i + 1]);
                total_delta += diff;
                
                if (diff <= 1.0f) {
                    static_count++;
                } else if (diff >= 15.0f) {
                    jitter_count++;
                }
            }
            
            jitter_magnitude = total_delta / 14.0f;
            jitter_frequency = (float)jitter_count / 14.0f;
            
            // Jittering if more jitter ticks than static
            is_jittering = jitter_count > static_count && jitter_magnitude > 20.0f;
        }
        
        last_yaw = current_yaw;
    }
    
    void reset() noexcept {
        std::memset(yaw_cache, 0, sizeof(yaw_cache));
        cache_offset = 0;
        jitter_count = 0;
        static_count = 0;
        is_jittering = false;
        jitter_frequency = 0.0f;
        jitter_magnitude = 0.0f;
    }
};

// Main resolver data per player
struct ResolverData {
    // Current state
    ResolverMode mode = ResolverMode::NONE;
    ResolverSide side = ResolverSide::UNKNOWN;
    ResolverSide last_side = ResolverSide::UNKNOWN;
    
    // Angles
    float desync_angle = 0.0f;
    float last_desync_angle = 0.0f;
    float resolved_yaw = 0.0f;
    float resolved_pitch = 0.0f;
    
    // State flags
    bool is_faking = false;
    bool is_low_delta = false;
    bool is_extended_desync = false;
    bool is_roll_aa = false;
    
    // Confidence & bruteforce
    ResolverConfidence confidence;
    SmartBruteforce bruteforce;
    JitterDetection jitter;
    
    // Shot tracking
    int missed_shots = 0;
    int hit_shots = 0;
    float last_resolve_angle = 0.0f;
    
    void reset() noexcept {
        mode = ResolverMode::NONE;
        side = ResolverSide::UNKNOWN;
        last_side = ResolverSide::UNKNOWN;
        desync_angle = 0.0f;
        last_desync_angle = 0.0f;
        is_faking = false;
        is_low_delta = false;
        is_extended_desync = false;
        is_roll_aa = false;
        confidence.reset();
        bruteforce.reset();
        jitter.reset();
        missed_shots = 0;
        hit_shots = 0;
    }
};

namespace ResolverEnhanced {
    // Global data
    inline static std::array<ResolverData, 65> player_data{};
    inline static int total_hits = 0;
    inline static int total_misses = 0;
    inline static float overall_hit_rate = 0.0f;
    
    // Core resolver functions
    void resolve_player(Entity* entity, Animations::Players& player, Animations::Players& prev_player) noexcept;
    void process_shot_result(int player_index, bool hit, float angle_used) noexcept;
    void update_shot_tracking(GameEvent* event) noexcept;
    
    // Advanced analysis functions
    bool analyze_animation_layers(Entity* entity, ResolverData& data) noexcept;
    float analyze_velocity_direction(Entity* entity, ResolverData& data) noexcept;
    bool detect_low_delta(Entity* entity) noexcept;
    bool detect_extended_desync(Entity* entity) noexcept;
    bool detect_fake_angles(Entity* entity) noexcept;
    
    // Side detection
    void detect_side_advanced(Entity* entity, ResolverData& data) noexcept;
    int detect_freestanding(Entity* entity) noexcept;
    
    // Angle calculation
    float calculate_optimal_desync_angle(Entity* entity, ResolverData& data) noexcept;
    float resolve_pitch(Entity* entity, ResolverData& data) noexcept;
    
    // Utility functions
    float get_max_desync_delta(Entity* entity) noexcept;
    float get_left_yaw(Entity* entity) noexcept;
    float get_right_yaw(Entity* entity) noexcept;
    float get_backward_yaw(Entity* entity) noexcept;
    
    // Reset & cleanup
    void reset() noexcept;
    void reset_player(int index) noexcept;
}

// Legacy compatibility - keep old resolver namespace
namespace Resolver {
    // Re-export enhanced functions with old names
    using ResolverEnhanced::resolve_player;
    using ResolverEnhanced::detect_freestanding;
    using ResolverEnhanced::get_max_desync_delta;
    
    // Legacy data structures
    inline static int missed_shots[65] = {};
    inline static float desync = 0.0f;
    inline static int hits = 0;
    inline static int misses = 0;
    inline static float hit_rate = 0.0f;
    inline static bool updating_animation = false;
    inline static bool should_force_safepoint = false;
}
