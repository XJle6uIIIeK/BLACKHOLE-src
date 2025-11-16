#pragma once

#include "../SDK/UserCmd.h"
#include "../SDK/Vector.h"
#include <array>
#include <random>

// ============================================================================
// IMPROVED ANTI-AIM SYSTEM - UNPREDICTABLE & ADAPTIVE
// ============================================================================

namespace ImprovedAntiAim
{
    // ========================================
    // ENUMS & CONSTANTS
    // ========================================
    
    enum class DesyncMode : uint8_t
    {
        STATIC,              // Fixed angle
        JITTER,              // Alternating sides
        RANDOM,              // Randomized angles
        ADAPTIVE,            // Based on enemy actions
        AVOID_OVERLAP        // Counter-resolver
    };
    
    enum class YawBase : uint8_t
    {
        OFF,
        FORWARD,
        BACKWARD,
        LEFT,
        RIGHT,
        AT_TARGETS
    };
    
    enum class PitchMode : uint8_t
    {
        NONE,
        DOWN,
        UP,
        ZERO,
        FAKE,
        RANDOM
    };
    
    enum class MovementFlag : uint8_t
    {
        STANDING,
        MOVING,
        AIR,
        CROUCHING
    };
    
    // ========================================
    // DATA STRUCTURES
    // ========================================
    
    struct DesyncData
    {
        float left_limit = 58.0f;
        float right_limit = 58.0f;
        float current_delta = 0.0f;
        bool invert = false;
        
        // Random seed for unpredictability
        uint32_t seed = 0;
        
        // Adaptive parameters
        float last_shot_angle = 0.0f;
        int ticks_since_shot = 0;
        
        void reset()
        {
            current_delta = 0.0f;
            last_shot_angle = 0.0f;
            ticks_since_shot = 0;
        }
    };
    
    struct EnemyTracker
    {
        int entity_index = 0;
        Vector last_eye_pos{};
        Vector aim_direction{};
        float fov_to_local = 180.0f;
        bool is_aiming_at_me = false;
        int ticks_aimed = 0;
        
        void update(Entity* enemy, Entity* local) noexcept;
    };
    
    // ========================================
    // ANTI-AIM CLASS
    // ========================================
    
    class AntiAim
    {
    public:
        // Main interface
        void run(UserCmd* cmd, const Vector& viewAngles, bool& sendPacket) noexcept;
        bool can_run(UserCmd* cmd) noexcept;
        
        // Manual override
        void handle_manual_aa(UserCmd* cmd, float& yaw) noexcept;
        
        // Movement correction
        void jitter_move(UserCmd* cmd) noexcept;
        
        // State management
        void reset() noexcept;
        void update_enemies() noexcept;
        
        // Getters
        bool is_inverter_active() const noexcept { return m_desync.invert; }
        float get_current_desync() const noexcept { return m_desync.current_delta; }
        
    private:
        // Core AA logic
        void apply_pitch(UserCmd* cmd, PitchMode mode) noexcept;
        void apply_yaw(UserCmd* cmd, float& yaw, MovementFlag flag) noexcept;
        void apply_desync(UserCmd* cmd, float& yaw, MovementFlag flag, bool sendPacket) noexcept;
        
        // Dynamic desync
        float calculate_dynamic_desync(MovementFlag flag) noexcept;
        float get_adaptive_angle(MovementFlag flag) noexcept;
        bool should_use_defensive_aa() noexcept;
        
        // LBY manipulation
        void handle_lby_break(UserCmd* cmd, MovementFlag flag) noexcept;
        bool should_update_lby() noexcept;
        
        // Distortion
        void apply_distortion(UserCmd* cmd, MovementFlag flag) noexcept;
        
        // Anti-backstab
        bool detect_backstab_threat(float& override_yaw) noexcept;
        
        // Freestanding
        bool apply_freestanding(float& yaw) noexcept;
        
        // Random utilities
        float random_float(float min, float max) noexcept;
        float random_float_seeded(float min, float max, uint32_t seed) noexcept;
        
        // State tracking
        DesyncData m_desync{};
        std::array<EnemyTracker, 64> m_enemies{};
        
        // LBY state
        float m_lby_update_time = 0.0f;
        bool m_lby_needs_update = false;
        
        // Random engine
        std::mt19937 m_rng{std::random_device{}()};
        
        // Frame tracking
        int m_last_tick = 0;
        bool m_last_send_packet = true;
    };
    
    // ========================================
    // UTILITY FUNCTIONS
    // ========================================
    
    // Get movement flag based on player state
    inline MovementFlag get_movement_flag(UserCmd* cmd, Entity* local) noexcept
    {
        if (!local)
            return MovementFlag::STANDING;
            
        if (!(local->flags() & FL_ONGROUND))
            return MovementFlag::AIR;
            
        if (cmd->buttons & UserCmd::IN_DUCK)
            return MovementFlag::CROUCHING;
            
        const float velocity = local->velocity().length2D();
        if (velocity > 5.0f)
            return MovementFlag::MOVING;
            
        return MovementFlag::STANDING;
    }
    
    // Check if player can shoot
    inline bool can_shoot(Entity* local) noexcept
    {
        if (!local || !local->isAlive())
            return false;
            
        const auto weapon = local->getActiveWeapon();
        if (!weapon || !weapon->clip())
            return false;
            
        if (weapon->nextPrimaryAttack() > memory->globalVars->serverTime())
            return false;
            
        if (local->nextAttack() > memory->globalVars->serverTime())
            return false;
            
        return true;
    }
    
    // Global instance
    inline AntiAim g_antiaim;
}
