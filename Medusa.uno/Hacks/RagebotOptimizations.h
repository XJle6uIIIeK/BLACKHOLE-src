#pragma once

#include "../SDK/Entity.h"
#include "../SDK/UserCmd.h"
#include "../SDK/Vector.h"
#include <array>
#include <vector>

// ============================================================================
// RAGEBOT OPTIMIZATIONS - SPEED & ACCURACY
// ============================================================================

namespace RagebotOptimizations
{
    // ========================================
    // MULTIPOINT SYSTEM
    // ========================================
    
    struct ScanPoint
    {
        Vector position{};
        int hitbox_id = 0;
        float damage = 0.0f;
        float hitchance = 0.0f;
        bool safe = false;
    };
    
    struct HitboxConfig
    {
        int hitbox_id = 0;
        float priority = 1.0f;
        bool use_multipoint = true;
        float scale = 0.9f;  // Scale от центра хитбокса
        int point_count = 5; // Количество точек
    };
    
    // Preset configurations
    namespace Presets
    {
        // Head multipoint (5 точек)
        inline const std::array<Vector, 5> HEAD_POINTS = {
            Vector{0.0f, 0.0f, 0.0f},      // Center
            Vector{0.5f, 0.5f, 0.0f},      // Top-right
            Vector{-0.5f, 0.5f, 0.0f},     // Top-left
            Vector{0.5f, -0.5f, 0.0f},     // Bottom-right
            Vector{-0.5f, -0.5f, 0.0f}     // Bottom-left
        };
        
        // Body multipoint (4 точки)
        inline const std::array<Vector, 4> BODY_POINTS = {
            Vector{0.0f, 0.0f, 0.0f},      // Center
            Vector{0.5f, 0.0f, 0.0f},      // Right
            Vector{-0.5f, 0.0f, 0.0f},     // Left
            Vector{0.0f, 0.5f, 0.0f}       // Top
        };
    }
    
    // ========================================
    // MULTIPOINT GENERATOR
    // ========================================
    
    class MultipointGenerator
    {
    public:
        // Generate scan points for entity
        std::vector<ScanPoint> generate_points(
            Entity* entity,
            const matrix3x4* bones,
            const std::vector<HitboxConfig>& hitbox_configs
        ) noexcept;
        
        // Generate points for specific hitbox
        std::vector<Vector> generate_hitbox_points(
            Entity* entity,
            const matrix3x4* bones,
            int hitbox_id,
            float scale,
            int point_count
        ) noexcept;
        
    private:
        // Get hitbox bounds
        struct HitboxBounds
        {
            Vector min{};
            Vector max{};
            Vector center{};
            float radius = 0.0f;
        };
        
        HitboxBounds get_hitbox_bounds(
            Entity* entity,
            const matrix3x4* bones,
            int hitbox_id
        ) noexcept;
        
        // Point generation strategies
        std::vector<Vector> generate_capsule_points(
            const HitboxBounds& bounds,
            float scale,
            int count
        ) noexcept;
        
        std::vector<Vector> generate_sphere_points(
            const HitboxBounds& bounds,
            float scale,
            int count
        ) noexcept;
    };
    
    // ========================================
    // TARGET SELECTOR
    // ========================================
    
    struct TargetInfo
    {
        Entity* entity = nullptr;
        ScanPoint best_point{};
        float priority_score = 0.0f;
        bool valid = false;
    };
    
    class TargetSelector
    {
    public:
        // Find best target
        TargetInfo select_best_target(
            const Vector& eye_position,
            const Vector& view_angles,
            const std::vector<Entity*>& candidates
        ) noexcept;
        
        // Calculate priority score
        float calculate_priority(
            Entity* entity,
            const ScanPoint& point,
            const Vector& eye_position,
            const Vector& view_angles
        ) noexcept;
        
    private:
        // Visibility check
        bool is_visible(
            const Vector& start,
            const Vector& end,
            Entity* entity
        ) noexcept;
        
        // Damage prediction
        float estimate_damage(
            const Vector& start,
            const Vector& end,
            Entity* entity,
            int hitbox_id
        ) noexcept;
    };
    
    // ========================================
    // HITCHANCE CALCULATOR
    // ========================================
    
    class HitchanceCalculator
    {
    public:
        // Calculate hitchance for point
        float calculate(
            Entity* shooter,
            Entity* target,
            const Vector& point,
            int hitbox_id,
            int sample_count = 256
        ) noexcept;
        
    private:
        // Spread simulation
        struct SpreadInfo
        {
            float inaccuracy = 0.0f;
            float spread = 0.0f;
            float range = 0.0f;
        };
        
        SpreadInfo get_weapon_spread(Entity* shooter) noexcept;
        
        // Ray intersection
        bool ray_intersects_hitbox(
            const Vector& start,
            const Vector& direction,
            Entity* target,
            const matrix3x4* bones,
            int hitbox_id
        ) noexcept;
    };
    
    // ========================================
    // PERFORMANCE OPTIMIZATIONS
    // ========================================
    
    class PerformanceOptimizer
    {
    public:
        // Occlusion culling
        std::vector<Entity*> get_visible_enemies() noexcept;
        
        // FOV culling
        std::vector<Entity*> get_enemies_in_fov(
            const Vector& view_angles,
            float max_fov = 180.0f
        ) noexcept;
        
        // Early rejection
        bool should_skip_entity(
            Entity* entity,
            const Vector& eye_position,
            const Vector& view_angles
        ) noexcept;
        
    private:
        // Frustum check
        bool in_frustum(
            const Vector& point,
            const Vector& view_angles,
            float fov
        ) noexcept;
    };
    
    // ========================================
    // AUTO-STOP
    // ========================================
    
    class AutoStop
    {
    public:
        // Calculate stop movement
        void calculate_stop(
            UserCmd* cmd,
            bool between_shots = false
        ) noexcept;
        
        // Check if we can shoot accurately
        bool can_shoot_accurately() noexcept;
        
    private:
        // Get minimum speed for accuracy
        float get_accuracy_speed(Entity* weapon) noexcept;
        
        // Calculate brake direction
        Vector calculate_brake_velocity(const Vector& velocity) noexcept;
    };
    
    // ========================================
    // AUTO-SCOPE
    // ========================================
    
    class AutoScope
    {
    public:
        // Handle scoping
        void handle_scope(UserCmd* cmd) noexcept;
        
        // Check if should scope
        bool should_scope() noexcept;
        
    private:
        bool m_should_unscope = false;
        int m_scope_tick = 0;
    };
    
    // ========================================
    // UTILITY FUNCTIONS
    // ========================================
    
    // Scale factor based on hitbox
    inline float get_hitbox_scale(int hitbox_id) noexcept
    {
        switch (hitbox_id)
        {
            case 0: // Head
                return 0.85f;
            case 1: // Neck
                return 0.75f;
            case 2: // Pelvis
            case 3: // Stomach
            case 4: // Lower chest
            case 5: // Chest
            case 6: // Upper chest
                return 0.90f;
            default:
                return 0.80f;
        }
    }
    
    // Priority based on hitbox
    inline float get_hitbox_priority(int hitbox_id) noexcept
    {
        switch (hitbox_id)
        {
            case 0: // Head
                return 10.0f;
            case 5: // Chest
            case 6: // Upper chest
                return 8.0f;
            case 2: // Pelvis
            case 3: // Stomach
                return 7.0f;
            default:
                return 5.0f;
        }
    }
    
    // Global instances
    inline MultipointGenerator g_multipoint;
    inline TargetSelector g_target_selector;
    inline HitchanceCalculator g_hitchance;
    inline PerformanceOptimizer g_optimizer;
    inline AutoStop g_autostop;
    inline AutoScope g_autoscope;
}
