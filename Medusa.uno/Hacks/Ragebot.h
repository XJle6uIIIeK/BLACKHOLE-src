#pragma once

#include "../SDK/Vector.h"
#include "../SDK/UserCmd.h"
#include "../SDK/Entity.h"
#include "../SDK/matrix3x4.h"
#include <array>
#include <vector>
#include <deque>
#include <unordered_map>

class Ragebot {
public:
    // ============================================
    // STRUCTURES
    // ============================================

    struct TargetMovement {
        Vector velocity;
        float simulationTime;
    };

    struct Enemies {
        int id;
        int health;
        float distance;
        float fov;
    };

    struct MovementData {
        Vector velocity;
        float speed;
        float maxSpeed;
        float acceleration;
        float friction;
    };

    // === НОВОЕ: Данные о точке прицеливания ===
    struct AimPoint {
        Vector position;
        int hitbox;
        int hitgroup;
        float damage;
        float hitchance;
        bool isSafe;           // Safe point (center of hitbox)
        bool isMultipoint;     // Multipoint position
        float priority;        // Calculated priority score
        float distanceFromCenter; // Distance from hitbox center

        bool operator<(const AimPoint& other) const {
            return priority > other.priority; // Higher priority first
        }
    };

    // === НОВОЕ: Данные о цели ===
    struct TargetData {
        Entity* entity = nullptr;
        int index = -1;
        float simulationTime = 0.f;
        int health = 0;
        float distance = 0.f;
        float fov = 0.f;
        bool isBacktrack = false;
        int backtrackTick = 0;

        std::vector<AimPoint> aimPoints;
        AimPoint bestPoint;
        bool hasValidPoint = false;
    };

    // === НОВОЕ: Delay Shot данные ===
    struct DelayShotData {
        bool isDelaying = false;
        int delayStartTick = 0;
        int delayTicks = 0;
        int maxDelayTicks = 0;

        TargetData bestTarget;
        float bestDamage = 0.f;
        float bestHitchance = 0.f;

        // History for comparison
        std::deque<std::pair<int, float>> damageHistory; // tick, damage
        std::deque<std::pair<int, float>> hitchanceHistory; // tick, hitchance

        void reset() {
            isDelaying = false;
            delayStartTick = 0;
            delayTicks = 0;
            bestTarget = TargetData{};
            bestDamage = 0.f;
            bestHitchance = 0.f;
            damageHistory.clear();
            hitchanceHistory.clear();
        }
    };

    // === НОВОЕ: Hitbox Priority данные ===
    struct HitboxPriority {
        int hitbox;
        float basePriority;
        float damageMultiplier;
        bool isLethal;         // Can kill with one shot
        bool isPreferred;      // Preferred by user config
    };

    // === НОВОЕ: Safe Point данные ===
    struct SafePointData {
        bool onlyHeadSafe = false;
        bool onlyBodySafe = false;
        bool preferSafe = false;
        int safePointsRequired = 1; // Minimum safe points to shoot
    };

    // ============================================
    // ENUMS
    // ============================================

    enum class ShotType {
        NORMAL,
        DELAY_SHOT,
        FORCE_SHOT
    };

    enum class HitboxGroup {
        HEAD,
        CHEST,
        STOMACH,
        ARMS,
        LEGS,
        FEET
    };

    // ============================================
    // CONSTANTS
    // ============================================

    static constexpr int MAX_DELAY_TICKS = 16;
    static constexpr int MIN_DELAY_TICKS = 2;
    static constexpr float SAFE_POINT_THRESHOLD = 0.9f; // 90% of hitbox radius
    static constexpr float LETHAL_DAMAGE_THRESHOLD = 100.f;

    // ============================================
    // MAIN INTERFACE
    // ============================================

    static void run(UserCmd* cmd) noexcept;
    static void updateInput() noexcept;
    static void reset() noexcept;

    // ============================================
    // DELAY SHOT
    // ============================================

    static bool shouldDelayShot(UserCmd* cmd, const TargetData& target, int weaponIndex) noexcept;
    static void updateDelayShot(UserCmd* cmd, const TargetData& target, int weaponIndex) noexcept;
    static bool isDelayShotReady() noexcept;
    static void executeDelayShot(UserCmd* cmd) noexcept;

    // ============================================
    // FORCE SHOT
    // ============================================

    static bool isForceShotActive() noexcept;
    static void handleForceShot(UserCmd* cmd, const TargetData& target) noexcept;

    // ============================================
    // HITBOX PRIORITIZATION
    // ============================================

    static std::vector<HitboxPriority> calculateHitboxPriorities(
        Entity* entity, Entity* weapon, int minDamage, int weaponIndex) noexcept;
    static float getHitboxBasePriority(int hitbox, int weaponIndex) noexcept;
    static bool isLethalHitbox(int hitbox, Entity* entity, Entity* weapon) noexcept;

    // ============================================
    // SAFE POINTS & UNSAFE HITBOX
    // ============================================

    static bool isSafePoint(Entity* entity, const Vector& point, int hitbox,
        matrix3x4* matrix, StudioHitboxSet* set) noexcept;
    static std::vector<Vector> getSafePoints(Entity* entity, int hitbox,
        matrix3x4* matrix, StudioHitboxSet* set, const Vector& eyePos) noexcept;
    static std::vector<Vector> getUnsafePoints(Entity* entity, int hitbox,
        matrix3x4* matrix, StudioHitboxSet* set, const Vector& eyePos) noexcept;
    static bool hasMinimumSafePoints(Entity* entity, int hitbox,
        matrix3x4* matrix, StudioHitboxSet* set, const Vector& eyePos, int required) noexcept;

    // ============================================
    // AIM POINT GENERATION
    // ============================================

    static std::vector<AimPoint> generateAimPoints(
        Entity* entity, matrix3x4* matrix, StudioHitboxSet* set,
        const Vector& eyePos, Entity* weapon, int weaponIndex,
        const std::array<bool, Hitboxes::Max>& enabledHitboxes,
        int minDamage, int multiPointHead, int multiPointBody) noexcept;

    static AimPoint selectBestAimPoint(
        const std::vector<AimPoint>& points,
        int weaponIndex, bool preferSafe, bool forceSafe) noexcept;

    // ============================================
    // TARGET SELECTION
    // ============================================

    static std::vector<TargetData> collectTargets(UserCmd* cmd, Entity* weapon,
        int weaponIndex, const Vector& eyePos, const Vector& aimPunch) noexcept;
    static TargetData selectBestTarget(const std::vector<TargetData>& targets,
        int priorityMode) noexcept;

    // ============================================
    // UTILITY
    // ============================================

    static int getWeaponIndex(WeaponId weaponId) noexcept;
    static void resetMatrix(Entity* entity, matrix3x4* backup, const Vector& origin,
        const Vector& absAngle, const Vector& mins, const Vector& maxs) noexcept;
    static float calculatePointPriority(const AimPoint& point, int weaponIndex,
        int targetHealth, bool preferSafe) noexcept;

    // ============================================
    // SORTING
    // ============================================

    static bool healthSort(const Enemies& a, const Enemies& b) noexcept {
        return a.health < b.health;
    }
    static bool distanceSort(const Enemies& a, const Enemies& b) noexcept {
        return a.distance < b.distance;
    }
    static bool fovSort(const Enemies& a, const Enemies& b) noexcept {
        return a.fov < b.fov;
    }

    // ============================================
    // DATA
    // ============================================

    inline static std::string latest_player;
    inline static DelayShotData delayShotData;
    inline static bool forceShotActive = false;
    inline static int currentTick = 0;
};