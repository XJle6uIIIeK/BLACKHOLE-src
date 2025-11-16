#pragma once
#include "../SDK/Vector.h"
#include "../SDK/Entity.h"
#include "../SDK/StudioHdr.h"
#include <vector>

namespace AimbotFunctionV2 {
    
    struct ScanResult {
        float damage;
        int hitgroup;
        Vector impactPoint;
        bool canHit;
    };
    
    // Advanced damage scanning with penetration
    ScanResult advancedDamageScan(
        Entity* entity,
        const Vector& destination,
        const WeaponInfo* weaponData,
        int minDamage,
        bool allowFriendlyFire) noexcept;
    
    // SIMD-optimized multipoint generation
    std::vector<Vector> advancedMultiPoint(
        Entity* entity,
        const matrix3x4 matrix[MAXSTUDIOBONES],
        StudioBbox* hitbox,
        Vector localEyePos,
        int hitboxId,
        float headScale,
        float bodyScale) noexcept;
    
    // Enhanced hitchance with early exit optimization
    bool enhancedHitChance(
        Entity* localPlayer,
        Entity* entity,
        StudioHitboxSet* set,
        const matrix3x4 matrix[MAXSTUDIOBONES],
        Entity* activeWeapon,
        const Vector& targetPos,
        const UserCmd* cmd,
        int requiredHitChance) noexcept;
}
