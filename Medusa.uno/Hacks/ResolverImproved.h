#pragma once

#include "Animations.h"
#include "../SDK/Entity.h"
#include <array>
#include <deque>

// ============================================================================
// IMPROVED RESOLVER SYSTEM - CLEAN ARCHITECTURE
// ============================================================================

namespace ImprovedResolver
{
    // ========================================
    // ENUMS & CONSTANTS
    // ========================================
    
    enum class ResolverMode : uint8_t
    {
        NONE = 0,
        SHOT_DETECTION,      // Highest priority - analyzing shot matrix
        ANIMATION_LAYERS,    // Primary method - animation layer analysis
        LOW_DELTA,          // For legit AA users
        FREESTANDING,       // Geometric trace-based
        BRUTEFORCE          // Last resort
    };
    
    enum class ResolverSide : int8_t
    {
        LEFT = -1,
        CENTER = 0,
        RIGHT = 1,
        UNKNOWN = 2
    };
    
    enum class PlayerState : uint8_t
    {
        STANDING,
        MOVING,
        AIR,
        CROUCHING
    };
    
    // ========================================
    // DATA STRUCTURES
    // ========================================
    
    struct ResolverData
    {
        // Core data
        ResolverMode mode = ResolverMode::NONE;
        ResolverSide side = ResolverSide::UNKNOWN;
        PlayerState state = PlayerState::STANDING;
        
        // Angles
        float resolved_yaw = 0.0f;
        float desync_delta = 0.0f;
        float max_desync = 58.0f;
        
        // Detection flags
        bool is_faking = false;
        bool is_jittering = false;
        bool is_low_delta = false;
        bool has_extended_desync = false;
        
        // Statistics (for ML approach)
        int total_shots = 0;
        int hits = 0;
        int misses = 0;
        float hit_rate = 0.0f;
        
        // History for adaptive resolving
        std::array<float, 16> yaw_history{};
        int history_index = 0;
        
        // Bruteforce state
        int bruteforce_step = 0;
        
        void reset()
        {
            mode = ResolverMode::NONE;
            side = ResolverSide::UNKNOWN;
            resolved_yaw = 0.0f;
            desync_delta = 0.0f;
            is_faking = false;
            is_jittering = false;
            is_low_delta = false;
            yaw_history.fill(0.0f);
            history_index = 0;
        }
    };
    
    struct ShotData
    {
        Vector impact_pos{};
        Vector eye_pos{};
        float server_time = 0.0f;
        int tick_count = 0;
        bool processed = false;
    };
    
    // ========================================
    // RESOLVER CLASS
    // ========================================
    
    class Resolver
    {
    public:
        // Main interface
        void resolve_player(int index, Entity* entity, Animations::Players& player_data) noexcept;
        void on_player_hurt(int attacker_id, int victim_id, int hitgroup) noexcept;
        void on_bullet_impact(int user_id, const Vector& impact) noexcept;
        void on_weapon_fire(int user_id) noexcept;
        
        // Getters
        ResolverData& get_player_data(int index) noexcept { return m_player_data[index]; }
        float get_resolved_yaw(int index) const noexcept { return m_player_data[index].resolved_yaw; }
        ResolverSide get_resolved_side(int index) const noexcept { return m_player_data[index].side; }
        
        // Stats
        float get_global_hit_rate() const noexcept;
        void reset_player(int index) noexcept;
        void reset_all() noexcept;
        
    private:
        // Resolver methods (priority order)
        bool try_resolve_by_shot(int index, Entity* entity, Animations::Players& player) noexcept;
        bool try_resolve_by_layers(int index, Entity* entity, Animations::Players& player) noexcept;
        bool try_resolve_low_delta(int index, Entity* entity) noexcept;
        bool try_resolve_freestanding(int index, Entity* entity) noexcept;
        void apply_bruteforce(int index, Entity* entity) noexcept;
        
        // Detection helpers
        bool detect_jitter(int index, Entity* entity) noexcept;
        bool detect_low_delta(Entity* entity) noexcept;
        bool detect_extended_desync(int index, Entity* entity, Animations::Players& player) noexcept;
        bool detect_fake_angles(Entity* entity) noexcept;
        
        // Calculation helpers
        float calculate_desync_delta(Entity* entity, Animations::Players& player) noexcept;
        float calculate_optimal_yaw(int index, Entity* entity, ResolverSide side) noexcept;
        PlayerState get_player_state(Entity* entity) noexcept;
        
        // Freestanding
        ResolverSide trace_optimal_side(Entity* entity) noexcept;
        float trace_wall_distance(Entity* entity, float yaw_offset) noexcept;
        
        // Animation layer analysis
        struct LayerDelta
        {
            float playback_delta = 0.0f;
            float weight_delta = 0.0f;
            float cycle_delta = 0.0f;
        };
        
        LayerDelta calculate_layer_delta(
            const AnimationLayer& current, 
            const AnimationLayer& previous,
            const AnimationLayer& resolver
        ) noexcept;
        
        ResolverSide determine_side_from_layers(
            int index,
            Entity* entity,
            Animations::Players& player
        ) noexcept;
        
        // Statistics & ML
        void update_statistics(int index, bool hit) noexcept;
        float get_predicted_angle(int index, Entity* entity) noexcept;
        
        // Data storage
        std::array<ResolverData, 65> m_player_data{};
        std::array<std::deque<ShotData>, 65> m_shot_history{};
        
        // Global stats
        int m_total_hits = 0;
        int m_total_misses = 0;
    };
    
    // ========================================
    // UTILITY FUNCTIONS
    // ========================================
    
    // Angle utilities
    inline float normalize_yaw(float yaw) noexcept
    {
        while (yaw > 180.0f) yaw -= 360.0f;
        while (yaw < -180.0f) yaw += 360.0f;
        return yaw;
    }
    
    inline float angle_diff(float a, float b) noexcept
    {
        float diff = normalize_yaw(a - b);
        return diff;
    }
    
    inline float approach_angle(float target, float current, float speed) noexcept
    {
        float diff = angle_diff(target, current);
        
        if (diff > speed) return current + speed;
        if (diff < -speed) return current - speed;
        
        return target;
    }
    
    // Math utilities
    template<typename T>
    inline T clamp(T value, T min, T max) noexcept
    {
        if (value < min) return min;
        if (value > max) return max;
        return value;
    }
    
    template<typename T>
    inline T lerp(T a, T b, float t) noexcept
    {
        return a + (b - a) * t;
    }
    
    // Global instance
    inline Resolver g_resolver;
}
