#include "AntiAimImproved.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "../Config.h"
#include "AimbotFunctions.h"
#include "Tickbase.h"
#include "../SDK/EntityList.h"
#include <algorithm>

namespace ImprovedAntiAim
{
    // ========================================
    // ENEMY TRACKER IMPLEMENTATION
    // ========================================
    
    void EnemyTracker::update(Entity* enemy, Entity* local) noexcept
    {
        if (!enemy || !local)
            return;
            
        entity_index = enemy->index();
        last_eye_pos = enemy->getEyePosition();
        
        // Calculate aim direction
        const Vector eye_angles = enemy->eyeAngles();
        aim_direction = Vector::fromAngle(eye_angles);
        
        // Calculate FOV to local player
        const Vector to_local = local->getEyePosition() - last_eye_pos;
        const float distance = to_local.length();
        
        if (distance > 0.0f)
        {
            const Vector normalized = to_local / distance;
            const float dot = aim_direction.dotProduct(normalized);
            fov_to_local = acosf(clamp(dot, -1.0f, 1.0f)) * (180.0f / 3.14159265f);
        }
        
        // Check if aiming at us (within 20° FOV)
        const bool currently_aiming = fov_to_local < 20.0f;
        
        if (currently_aiming)
        {
            ticks_aimed++;
            is_aiming_at_me = ticks_aimed > 3; // Sustained aim for 3+ ticks
        }
        else
        {
            ticks_aimed = 0;
            is_aiming_at_me = false;
        }
    }
    
    // ========================================
    // MAIN ANTI-AIM LOGIC
    // ========================================
    
    void AntiAim::run(UserCmd* cmd, const Vector& viewAngles, bool& sendPacket) noexcept
    {
        if (!can_run(cmd))
            return;
            
        // Update enemy tracking
        update_enemies();
        
        // Get movement state
        const auto movement_flag = get_movement_flag(cmd, localPlayer.get());
        
        // Check for backstab threat
        float backstab_yaw = 0.0f;
        if (detect_backstab_threat(backstab_yaw))
        {
            cmd->viewangles.y = backstab_yaw;
            cmd->viewangles.x = 0.0f;
            return;
        }
        
        // Apply pitch
        apply_pitch(cmd, PitchMode::DOWN); // Can be configured
        
        // Apply yaw modifications
        float yaw_modifier = 0.0f;
        
        // Handle manual AA
        handle_manual_aa(cmd, yaw_modifier);
        
        // Apply freestanding if enabled
        if (config->rageAntiAim[static_cast<int>(movement_flag)].freestand)
        {
            if (apply_freestanding(yaw_modifier))
                cmd->viewangles.y += yaw_modifier;
        }
        
        // Apply base yaw
        apply_yaw(cmd, yaw_modifier, movement_flag);
        cmd->viewangles.y += yaw_modifier;
        
        // Apply desync on choked packets
        if (!sendPacket)
            apply_desync(cmd, yaw_modifier, movement_flag, sendPacket);
        
        // Movement jitter
        jitter_move(cmd);
        
        // Track state
        m_last_tick = memory->globalVars->tickCount;
        m_last_send_packet = sendPacket;
    }
    
    // ========================================
    // PITCH MODIFICATION
    // ========================================
    
    void AntiAim::apply_pitch(UserCmd* cmd, PitchMode mode) noexcept
    {
        if (Tickbase::isShifting())
            return;
            
        switch (mode)
        {
            case PitchMode::DOWN:
                cmd->viewangles.x = 89.0f;
                break;
            case PitchMode::UP:
                cmd->viewangles.x = -89.0f;
                break;
            case PitchMode::ZERO:
                cmd->viewangles.x = 0.0f;
                break;
            case PitchMode::FAKE:
                cmd->viewangles.x = (memory->globalVars->tickCount % 20 == 0) ? -89.0f : 89.0f;
                break;
            case PitchMode::RANDOM:
                cmd->viewangles.x = random_float(-89.0f, 89.0f);
                break;
            default:
                break;
        }
    }
    
    // ========================================
    // YAW MODIFICATION
    // ========================================
    
    void AntiAim::apply_yaw(UserCmd* cmd, float& yaw, MovementFlag flag) noexcept
    {
        const auto& aa_config = config->rageAntiAim[static_cast<int>(flag)];
        
        // Base yaw direction
        switch (aa_config.yawBase)
        {
            case Yaw::backward:
                yaw += 180.0f;
                break;
            case Yaw::left:
                yaw += 90.0f;
                break;
            case Yaw::right:
                yaw += -90.0f;
                break;
            case Yaw::forward:
                // No change
                break;
            default:
                break;
        }
        
        // At targets mode
        if (aa_config.atTargets)
        {
            float best_fov = 255.0f;
            float target_yaw = 0.0f;
            
            const Vector local_eye = localPlayer->getEyePosition();
            const Vector aim_punch = localPlayer->getAimPunch();
            
            for (int i = 1; i <= interfaces->engine->getMaxClients(); ++i)
            {
                const auto enemy = interfaces->entityList->getEntity(i);
                if (!enemy || enemy == localPlayer.get() || !enemy->isAlive())
                    continue;
                if (!enemy->isOtherEnemy(localPlayer.get()))
                    continue;
                    
                const auto angle = AimbotFunction::calculateRelativeAngle(
                    local_eye, enemy->getAbsOrigin(), cmd->viewangles + aim_punch
                );
                
                const float fov = angle.length2D();
                if (fov < best_fov)
                {
                    target_yaw = angle.y;
                    best_fov = fov;
                }
            }
            
            if (best_fov < 255.0f)
                yaw = target_yaw;
        }
    }
    
    // ========================================
    // DYNAMIC DESYNC
    // ========================================
    
    void AntiAim::apply_desync(UserCmd* cmd, float& yaw, MovementFlag flag, bool sendPacket) noexcept
    {
        const auto& aa_config = config->rageAntiAim[static_cast<int>(flag)];
        
        if (!aa_config.desync)
            return;
            
        // Update invert state
        if (sendPacket)
        {
            switch (aa_config.peekMode)
            {
                case 1: // Peek real
                    m_desync.invert = !apply_freestanding(yaw);
                    break;
                case 2: // Peek fake  
                    m_desync.invert = apply_freestanding(yaw);
                    break;
                case 3: // Jitter
                    m_desync.invert = !m_desync.invert;
                    break;
                case 4: // On movement
                    if (localPlayer->velocity().length2D() > 5.0f)
                        m_desync.invert = !m_desync.invert;
                    break;
            }
        }
        
        // Calculate dynamic desync angle
        const float desync_angle = calculate_dynamic_desync(flag);
        
        // Apply desync
        const float final_desync = m_desync.invert ? desync_angle : -desync_angle;
        yaw += final_desync;
        
        m_desync.current_delta = final_desync;
        
        // Handle LBY
        handle_lby_break(cmd, flag);
    }
    
    float AntiAim::calculate_dynamic_desync(MovementFlag flag) noexcept
    {
        const auto& aa_config = config->rageAntiAim[static_cast<int>(flag)];
        
        // Check if enemies are aiming at us
        bool under_threat = false;
        for (const auto& enemy : m_enemies)
        {
            if (enemy.is_aiming_at_me)
            {
                under_threat = true;
                break;
            }
        }
        
        // Adaptive desync based on threat level
        if (under_threat)
        {
            // High jitter when being aimed at
            const float base = m_desync.invert ? aa_config.leftLimit : aa_config.rightLimit;
            const float variation = random_float(-5.0f, 5.0f);
            return base + variation;
        }
        
        // Standard desync
        const float base = m_desync.invert ? 
            random_float(aa_config.leftMin, aa_config.leftLimit) :
            random_float(aa_config.rightMin, aa_config.rightLimit);
            
        return base;
    }
    
    // ========================================
    // LBY MANIPULATION
    // ========================================
    
    void AntiAim::handle_lby_break(UserCmd* cmd, MovementFlag flag) noexcept
    {
        const auto& aa_config = config->rageAntiAim[static_cast<int>(flag)];
        
        if (aa_config.lbyMode == 0) // Normal sidemove
        {
            if (std::fabsf(cmd->sidemove) < 5.0f)
            {
                cmd->sidemove = (cmd->tickCount & 1) ? 
                    (cmd->buttons & UserCmd::IN_DUCK ? 3.25f : 1.1f) :
                    (cmd->buttons & UserCmd::IN_DUCK ? -3.25f : -1.1f);
            }
        }
        else if (should_update_lby())
        {
            // Force LBY update
            const float opposite_delta = m_desync.invert ? 
                -aa_config.leftLimit : aa_config.rightLimit;
                
            cmd->viewangles.y += opposite_delta;
        }
    }
    
    bool AntiAim::should_update_lby() noexcept
    {
        if (!localPlayer || !(localPlayer->flags() & FL_ONGROUND))
            return false;
            
        const float velocity = localPlayer->velocity().length2D();
        
        // Update timer
        if (velocity > 0.1f || std::fabsf(localPlayer->velocity().z) > 100.0f)
        {
            m_lby_update_time = memory->globalVars->serverTime() + 0.22f;
            return false;
        }
        
        // Check if it's time to update
        if (m_lby_update_time < memory->globalVars->serverTime())
        {
            m_lby_update_time = memory->globalVars->serverTime() + 1.1f;
            return true;
        }
        
        return false;
    }
    
    // ========================================
    // SPECIAL FEATURES
    // ========================================
    
    void AntiAim::handle_manual_aa(UserCmd* cmd, float& yaw) noexcept
    {
        const bool forward = config->manualForward.isActive();
        const bool back = config->manualBackward.isActive();
        const bool right = config->manualRight.isActive();
        const bool left = config->manualLeft.isActive();
        
        if (!forward && !back && !right && !left)
            return;
            
        // Priority: corners > sides > single direction
        if (back && left)
            yaw = -180.0f - 45.0f;
        else if (back && right)
            yaw = -180.0f + 45.0f;
        else if (forward && left)
            yaw = 0.0f - 45.0f;
        else if (forward && right)
            yaw = 0.0f + 45.0f;
        else if (back)
            yaw = -180.0f;
        else if (left)
            yaw = 90.0f;
        else if (right)
            yaw = -90.0f;
        else if (forward)
            yaw = 0.0f;
    }
    
    bool AntiAim::detect_backstab_threat(float& override_yaw) noexcept
    {
        const Vector local_pos = localPlayer->getAbsOrigin();
        const Vector local_eye = localPlayer->getEyePosition();
        
        for (int i = 1; i <= interfaces->engine->getMaxClients(); ++i)
        {
            const auto enemy = interfaces->entityList->getEntity(i);
            if (!enemy || enemy == localPlayer.get() || !enemy->isAlive())
                continue;
            if (!enemy->isOtherEnemy(localPlayer.get()))
                continue;
                
            // Check distance
            const float distance = enemy->getAbsOrigin().distTo(local_pos);
            if (distance > 200.0f) // Knife range
                continue;
                
            // Check if they have knife
            const auto weapon = enemy->getActiveWeapon();
            if (!weapon || !weapon->isKnife())
                continue;
                
            // Calculate angle to face threat
            const Vector to_enemy = enemy->getAbsOrigin() - local_pos;
            override_yaw = atan2f(to_enemy.y, to_enemy.x) * (180.0f / 3.14159265f);
            
            return true; // Threat detected!
        }
        
        return false;
    }
    
    bool AntiAim::apply_freestanding(float& yaw) noexcept
    {
        if (!localPlayer)
            return false;
            
        Vector forward, right, up;
        Helpers::AngleVectors(Vector(0, yaw, 0), &forward, &right, &up);
        
        const Vector src = localPlayer->getEyePosition();
        const Vector dst = src + forward * 100.0f;
        
        // Trace three positions
        std::array<bool, 3> hit{false, false, false};
        const std::array<float, 3> offsets{-30.0f, 0.0f, 30.0f};
        
        for (size_t i = 0; i < 3; i++)
        {
            const Vector start = src + right * offsets[i];
            const Vector end = dst + right * offsets[i];
            
            Trace trace{};
            interfaces->engineTrace->traceRay({start, end}, 0x1 | 0x2, nullptr, trace);
            
            if (trace.fraction != 1.0f)
                hit[i] = true;
        }
        
        // Determine optimal side
        if (hit[0] && hit[1] && !hit[2])
        {
            yaw += 90.0f; // Go right
            return true;
        }
        else if (!hit[0] && hit[1] && hit[2])
        {
            yaw += -90.0f; // Go left
            return true;
        }
        
        return false;
    }
    
    void AntiAim::apply_distortion(UserCmd* cmd, MovementFlag flag) noexcept
    {
        const auto& aa_config = config->rageAntiAim[static_cast<int>(flag)];
        
        float speed = aa_config.distortionSpeed;
        float amount = aa_config.distortionAmount;
        
        if (speed == 0.0f)
            speed = random_float(30.0f, 100.0f);
        if (amount == 0.0f)
            amount = random_float(90.0f, 360.0f);
            
        const float sine = (sinf(memory->globalVars->currenttime * (speed / 10.0f)) + 1.0f) / 2.0f * amount;
        cmd->viewangles.y += sine - (amount / 2.0f);
    }
    
    // ========================================
    // MOVEMENT CORRECTION
    // ========================================
    
    void AntiAim::jitter_move(UserCmd* cmd) noexcept
    {
        if (!localPlayer || !(localPlayer->flags() & FL_ONGROUND))
            return;
            
        if (localPlayer->getActiveWeapon() && localPlayer->getActiveWeapon()->isGrenade())
            return;
            
        const float total_move = std::fabsf(cmd->sidemove) + std::fabsf(cmd->forwardmove);
        if (total_move < 10.0f)
            return;
            
        const float velocity_sqr = localPlayer->m_vecVelocity().length2DSqr();
        if (velocity_sqr < 140.0f * 140.0f)
            return;
            
        // Dynamic factor based on time
        const float factor = 0.95f + fmodf(memory->globalVars->currenttime, 0.2f) * 0.25f;
        
        cmd->sidemove = clamp(cmd->sidemove, -250.0f, 250.0f) * factor;
        cmd->forwardmove = clamp(cmd->forwardmove, -250.0f, 250.0f) * factor;
    }
    
    // ========================================
    // ENEMY TRACKING
    // ========================================
    
    void AntiAim::update_enemies() noexcept
    {
        if (!localPlayer)
            return;
            
        for (int i = 1; i <= interfaces->engine->getMaxClients(); ++i)
        {
            const auto enemy = interfaces->entityList->getEntity(i);
            if (!enemy || enemy == localPlayer.get() || !enemy->isAlive())
                continue;
            if (!enemy->isOtherEnemy(localPlayer.get()))
                continue;
                
            m_enemies[i].update(enemy, localPlayer.get());
        }
    }
    
    // ========================================
    // VALIDATION
    // ========================================
    
    bool AntiAim::can_run(UserCmd* cmd) noexcept
    {
        if (!localPlayer || !localPlayer->isAlive())
            return false;
            
        // Don't run during freeze time
        if ((*memory->gameRules)->freezePeriod())
            return false;
            
        // Don't run when using objects
        if (cmd->buttons & UserCmd::IN_USE)
            return false;
            
        // Don't run on ladder/noclip
        const auto move_type = localPlayer->moveType();
        if (move_type == MoveType::LADDER || move_type == MoveType::NOCLIP)
            return false;
            
        // Check weapon state
        const auto weapon = localPlayer->getActiveWeapon();
        if (!weapon)
            return true;
            
        // Allow AA while holding grenades
        if (weapon->isGrenade() && !weapon->isThrowing())
            return true;
            
        // Don't run while shooting
        if (weapon->nextPrimaryAttack() <= memory->globalVars->serverTime() && 
            (cmd->buttons & UserCmd::IN_ATTACK))
            return false;
            
        if (localPlayer->nextAttack() <= memory->globalVars->serverTime() && 
            (cmd->buttons & UserCmd::IN_ATTACK))
            return false;
            
        return true;
    }
    
    // ========================================
    // RANDOM UTILITIES
    // ========================================
    
    float AntiAim::random_float(float min, float max) noexcept
    {
        std::uniform_real_distribution<float> dist(min, max);
        return dist(m_rng);
    }
    
    float AntiAim::random_float_seeded(float min, float max, uint32_t seed) noexcept
    {
        std::mt19937 seeded_rng(seed);
        std::uniform_real_distribution<float> dist(min, max);
        return dist(seeded_rng);
    }
    
    // ========================================
    // RESET
    // ========================================
    
    void AntiAim::reset() noexcept
    {
        m_desync.reset();
        m_enemies.fill(EnemyTracker{});
        m_lby_update_time = 0.0f;
        m_lby_needs_update = false;
        m_last_tick = 0;
        m_last_send_packet = true;
    }
}
