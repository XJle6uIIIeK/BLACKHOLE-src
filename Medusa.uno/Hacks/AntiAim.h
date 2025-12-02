#pragma once

#include "../SDK/Vector.h"
#include "../SDK/UserCmd.h"
#include "../SDK/Entity.h"
#include <array>
#include <deque>

class AntiAim {
public:
    // ============================================
    // STRUCTURES
    // ============================================

    struct DesyncState {
        float realYaw = 0.f;
        float fakeYaw = 0.f;
        float desyncDelta = 0.f;
        bool isInverted = false;
        int side = 0;
    };

    struct LBYData {
        float lastLBY = 0.f;
        float nextUpdateTime = 0.f;
        bool willUpdate = false;
        bool justUpdated = false;
        float standingTime = 0.f;
    };

    struct FreestandData {
        bool isActive = false;
        int side = 0;
        float leftDamage = 0.f;
        float rightDamage = 0.f;
        float lastCheckTime = 0.f;
    };

    struct EdgeData {
        bool isNearEdge = false;
        Vector edgeNormal;
        float distanceToEdge = 0.f;
    };

    struct AntiAimState {
        DesyncState desync;
        LBYData lby;
        FreestandData freestand;
        EdgeData edge;

        float currentYaw = 0.f;
        float targetYaw = 0.f;
        float currentPitch = 0.f;

        bool isManualOverride = false;
        int manualSide = 0;

        bool willGetStabbed = false;
        Entity* knifeAttacker = nullptr;

        int jitterStage = 0;
        bool jitterFlip = false;

        int ticksSinceShot = 0;
    };

    // ============================================
    // MAIN INTERFACE
    // ============================================

    static void run(UserCmd* cmd, const Vector& previousViewAngles,
        const Vector& currentViewAngles, bool& sendPacket) noexcept;
    static void updateInput() noexcept;
    static bool canRun(UserCmd* cmd) noexcept;
    static void reset() noexcept;

    // ============================================
    // ANTI-AIM MODES
    // ============================================

    static void rage(UserCmd* cmd, const Vector& previousViewAngles,
        const Vector& currentViewAngles, bool& sendPacket) noexcept;

    // ============================================
    // PITCH
    // ============================================

    static float calculatePitch(UserCmd* cmd, int movingFlag) noexcept;

    // ============================================
    // YAW
    // ============================================

    static float calculateBaseYaw(UserCmd* cmd, int movingFlag) noexcept;
    static float applyYawModifier(float yaw, UserCmd* cmd, int movingFlag) noexcept;
    static float applyManualYaw(float yaw) noexcept;
    static float applyAtTargets(float yaw, UserCmd* cmd) noexcept;

    // ============================================
    // JITTER
    // ============================================

    static float applyJitterCentered(float yaw, int movingFlag) noexcept;
    static float applyJitterOffset(float yaw, int movingFlag) noexcept;
    static float applyJitterRandom(float yaw, int movingFlag) noexcept;
    static float applyJitter3Way(float yaw, int movingFlag) noexcept;
    static float applyJitter5Way(float yaw, int movingFlag) noexcept;
    static float applySpin(float yaw, UserCmd* cmd, int movingFlag) noexcept;

    // ============================================
    // DESYNC
    // ============================================

    static void applyDesync(UserCmd* cmd, bool& sendPacket, int movingFlag) noexcept;
    static float getDesyncDelta(bool inverted, int movingFlag) noexcept;
    static void handleLBYBreak(UserCmd* cmd, bool& sendPacket, int movingFlag) noexcept;
    static bool updateLBY(bool update = false) noexcept;
    static bool predictLBYUpdate() noexcept;

    // ============================================
    // SPECIAL FEATURES
    // ============================================

    static void applyFreestand(UserCmd* cmd, int movingFlag) noexcept;
    static void applyEdgeYaw(UserCmd* cmd) noexcept;
    static bool detectKnifeThreat(UserCmd* cmd) noexcept;
    static void handleKnifeThreat(UserCmd* cmd, float& yaw, float& pitch) noexcept;

    // ============================================
    // DISTORTION
    // ============================================

    static void applyDistortion(UserCmd* cmd, int movingFlag) noexcept;

    // ============================================
    // ROLL
    // ============================================

    static float calculateRoll(int movingFlag) noexcept;

    // ============================================
    // ANIM BREAKERS
    // ============================================

    static void applyAnimBreakers(UserCmd* cmd) noexcept;
    static void JitterMove(UserCmd* cmd) noexcept;
    static void microMovement(UserCmd* cmd) noexcept;

    // ============================================
    // FAKE FLICK
    // ============================================

    static void applyFakeFlick(float& yaw, int movingFlag) noexcept;

    // ============================================
    // AUTO DIRECTION
    // ============================================

    static bool autoDirection(const Vector& eyeAngle) noexcept;
    static int getAutoDirectionSide(const Vector& eyeAngle) noexcept;

    // ============================================
    // UTILITY
    // ============================================

    static int getMovingFlag(UserCmd* cmd) noexcept;
    static float normalizeYaw(float yaw) noexcept;
    static float randomFloat(float min, float max) noexcept;

    // ============================================
    // GETTERS/SETTERS
    // ============================================

    static float getLastShotTime() noexcept;
    static bool getIsShooting() noexcept;
    static bool getDidShoot() noexcept;
    static void setLastShotTime(float shotTime) noexcept;
    static void setIsShooting(bool shooting) noexcept;
    static void setDidShoot(bool shot) noexcept;

    static const AntiAimState& getState() noexcept { return state; }
    static int getAutoDirectionYaw() noexcept { return auto_direction_yaw; }

    // ============================================
    // DATA
    // ============================================

    inline static AntiAimState state;
    inline static int auto_direction_yaw = 0;
    inline static bool r8Working = false;  // <-- ÄÎÁÀÂËÅÍÎ
    inline static bool invert = true;       // <-- ÏÅÐÅÌÅÙÅÍÎ â public

private:
    inline static bool isShooting = false;
    inline static bool didShoot = false;
    inline static float lastShotTime = 0.f;
};

// Helper function for external use - âîçâðàùàåò int
inline int get_moving_flag(UserCmd* cmd) {
    return AntiAim::getMovingFlag(cmd);
}