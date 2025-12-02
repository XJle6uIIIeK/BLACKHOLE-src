#pragma once

#include "../SDK/Vector.h"
#include "../SDK/FrameStage.h"
#include <array>

struct UserCmd;
class Entity;

namespace EnginePrediction {

    // ============================================
    // CONSTANTS
    // ============================================

    constexpr int MULTIPLAYER_BACKUP = 150;
    constexpr float PREDICTION_TOLERANCE = 0.03125f;
    constexpr float VELOCITY_TOLERANCE = 0.5f;

    // ============================================
    // STRUCTURES
    // ============================================

    struct NetvarData {
        int tickbase = 0;

        // Punch angles
        Vector aimPunchAngle{};
        Vector aimPunchAngleVelocity{};
        Vector viewPunchAngle{};

        // Movement
        Vector velocity{};
        Vector origin{};
        Vector baseVelocity{};
        Vector viewOffset{};
        Vector networkOrigin{};

        // Duck
        float duckAmount = 0.f;
        float duckSpeed = 0.f;

        // Misc
        float fallVelocity = 0.f;
        float velocityModifier = 1.f;
        float thirdPersonRecoil = 0.f;

        // Flags
        int flags = 0;

        // Weapon
        float nextPrimaryAttack = 0.f;
        float nextSecondaryAttack = 0.f;
        float recoilIndex = 0.f;

        bool valid = false;

        // Helper functions
        static float checkDifference(float predicted, float original) noexcept {
            if (std::abs(predicted - original) < PREDICTION_TOLERANCE)
                return original;
            return predicted;
        }

        static Vector checkDifference(const Vector& predicted, const Vector& original) noexcept {
            Vector result = predicted;
            if (std::abs(predicted.x - original.x) < PREDICTION_TOLERANCE)
                result.x = original.x;
            if (std::abs(predicted.y - original.y) < PREDICTION_TOLERANCE)
                result.y = original.y;
            if (std::abs(predicted.z - original.z) < PREDICTION_TOLERANCE)
                result.z = original.z;
            return result;
        }
    };

    struct StoredData {
        NetvarData netvars{};
        bool hasData = false;
    };

    struct PredictionBackup {
        // Global vars
        float currentTime = 0.f;
        float frameTime = 0.f;
        int tickCount = 0;
        bool isFirstTimePredicted = false;
        bool inPrediction = false;

        // Player state
        int flags = 0;
        Vector velocity{};
        Vector origin{};
        Vector viewOffset{};
        Vector aimPunch{};
        Vector aimPunchVel{};
        Vector viewPunch{};
        Vector baseVelocity{};
        float duckAmount = 0.f;
        float duckSpeed = 0.f;
        float fallVelocity = 0.f;
        float velocityModifier = 1.f;
        float thirdPersonRecoil = 0.f;
        int tickBase = 0;

        // Weapon state
        bool hasWeapon = false;
        float nextPrimaryAttack = 0.f;
        float nextSecondaryAttack = 0.f;
        float recoilIndex = 0.f;
        float accuracy = 0.f;
        float spread = 0.f;

        // Animation state
        float footYaw = 0.f;
        float duckAdditional = 0.f;

        bool valid = false;
    };

    // ============================================
    // MAIN FUNCTIONS
    // ============================================

    // Initialize/Reset
    void reset() noexcept;

    // Main prediction
    void run(UserCmd* cmd) noexcept;

    // Update prediction state
    void update() noexcept;

    // Store/Restore netvar data
    void store() noexcept;
    void restore() noexcept;

    // Apply prediction corrections
    void apply(FrameStage stage) noexcept;

    // Pre/Post frame handlers
    void onPreFrame() noexcept;
    void onPostFrame() noexcept;

    // ============================================
    // GETTERS
    // ============================================

    int getFlags() noexcept;
    Vector getVelocity() noexcept;
    Vector getOrigin() noexcept;
    bool isInPrediction() noexcept;
    float getServerTime() noexcept;
    const NetvarData& getNetvarData(int tickbase) noexcept;

    // ============================================
    // INTERNAL DATA
    // ============================================

    inline std::array<StoredData, MULTIPLAYER_BACKUP> storedData{};
    inline NetvarData currentNetvars{};
    inline PredictionBackup backup{};
    inline UserCmd* lastCmd = nullptr;
    inline int savedFlags = 0;
    inline Vector savedVelocity{};
    inline Vector savedOrigin{};
    inline bool inPrediction = false;
}