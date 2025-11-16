#pragma once

#include "../SDK/Entity.h"
#include "../SDK/matrix3x4.h"
#include <array>
#include <deque>

// ============================================================================
// OPTIMIZED ANIMATION SYSTEM - ACCURATE & FAST
// ============================================================================

namespace OptimizedAnimations
{
    constexpr int MAX_PLAYERS = 65;
    constexpr int MAX_BACKTRACK_RECORDS = 12;
    
    // ========================================
    // DATA STRUCTURES
    // ========================================
    
    struct SimulationData
    {
        Vector origin{};
        Vector velocity{};
        Vector abs_angles{};
        
        float duck_amount = 0.0f;
        float simulation_time = 0.0f;
        int flags = 0;
        
        std::array<AnimationLayer, 13> layers{};
        std::array<matrix3x4, 128> bones{};
        bool valid_bones = false;
    };
    
    struct PlayerRecord
    {
        // Identity
        int index = 0;
        float spawn_time = 0.0f;
        
        // Current state
        SimulationData current{};
        SimulationData previous{};
        
        // Backtrack history
        std::deque<SimulationData> history{};
        
        // Animation data
        std::array<AnimationLayer, 13> layers{};
        std::array<AnimationLayer, 13> old_layers{};
        
        // Velocity tracking
        Vector velocity{};
        Vector old_velocity{};
        
        // State tracking  
        int choked_packets = 0;
        bool shot_this_tick = false;
        bool dormant = false;
        
        // Statistics
        int misses = 0;
        
        void reset()
        {
            history.clear();
            velocity = Vector{};
            old_velocity = Vector{};
            choked_packets = 0;
            shot_this_tick = false;
            misses = 0;
        }
    };
    
    // ========================================
    // ANIMATION MANAGER CLASS
    // ========================================
    
    class AnimationManager
    {
    public:
        // Initialization
        void initialize() noexcept;
        void reset() noexcept;
        
        // Update cycle
        void update_local_player(UserCmd* cmd, bool sendPacket) noexcept;
        void update_players(FrameStage stage) noexcept;
        
        // Matrix management
        const std::array<matrix3x4, 128>& get_fake_matrix() const noexcept { return m_fake_matrix; }
        const std::array<matrix3x4, 128>& get_real_matrix() const noexcept { return m_real_matrix; }
        bool has_fake_matrix() const noexcept { return m_has_fake_matrix; }
        bool has_real_matrix() const noexcept { return m_has_real_matrix; }
        
        // Player data access
        PlayerRecord& get_player(int index) noexcept { return m_players[index]; }
        const PlayerRecord& get_player_const(int index) const noexcept { return m_players[index]; }
        
    private:
        // Local player animation
        void update_local_animations(UserCmd* cmd) noexcept;
        void build_local_bones(bool sendPacket) noexcept;
        
        // Enemy animation
        void update_player_animations(int index, Entity* entity) noexcept;
        void simulate_player_ticks(int index, Entity* entity, int ticks) noexcept;
        
        // Velocity calculation
        Vector calculate_velocity(
            const Vector& current_origin,
            const Vector& previous_origin,
            float time_delta
        ) noexcept;
        
        Vector fix_velocity(
            Entity* entity,
            PlayerRecord& record,
            const Vector& velocity
        ) noexcept;
        
        // Air velocity correction
        void fix_air_velocity(
            Entity* entity,
            PlayerRecord& record
        ) noexcept;
        
        // Ground velocity correction
        void fix_ground_velocity(
            Entity* entity,
            PlayerRecord& record
        ) noexcept;
        
        // Pose parameters
        void fix_jump_fall_pose(Entity* entity, PlayerRecord& record) noexcept;
        void apply_ground_friction(Vector& velocity, float friction) noexcept;
        
        // Activity detection
        struct ActivityInfo
        {
            int activity = 0;
            int tick = 0;
        };
        
        ActivityInfo detect_activity(
            const AnimationLayer& current,
            const AnimationLayer& previous,
            float simulation_time
        ) noexcept;
        
        // Storage
        std::array<PlayerRecord, MAX_PLAYERS> m_players{};
        
        // Local player matrices
        std::array<matrix3x4, 128> m_fake_matrix{};
        std::array<matrix3x4, 128> m_real_matrix{};
        bool m_has_fake_matrix = false;
        bool m_has_real_matrix = false;
        
        // Local player layers
        std::array<AnimationLayer, 13> m_sent_layers{};
        float m_sent_foot_yaw = 0.0f;
    };
    
    // ========================================
    // CONSTANTS
    // ========================================
    
    constexpr float CS_PLAYER_SPEED_RUN = 260.0f;
    constexpr float CS_PLAYER_SPEED_WALK_MODIFIER = 0.52f;
    constexpr float CS_PLAYER_SPEED_DUCK_MODIFIER = 0.34f;
    
    constexpr float GROUND_FRICTION = 5.5f;
    constexpr float STOP_EPSILON = 0.1f;
    
    // ========================================
    // UTILITY FUNCTIONS
    // ========================================
    
    inline int time_to_ticks(float time) noexcept
    {
        return static_cast<int>(0.5f + time / memory->globalVars->intervalPerTick);
    }
    
    inline float ticks_to_time(int ticks) noexcept
    {
        return memory->globalVars->intervalPerTick * static_cast<float>(ticks);
    }
    
    // Global instance
    inline AnimationManager g_animation_manager;
}
