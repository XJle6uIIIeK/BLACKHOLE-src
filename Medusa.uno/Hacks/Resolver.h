#pragma once
#include "../SDK/Entity.h"
#include "Animations.h"
#include <array>
#include <deque>
#include <vector>
#include <unordered_map>

class Resolver {
public:
    // ============================================
    // ENUMS & CONSTANTS
    // ============================================

    enum class ResolveMode {
        NONE,
        STANDING,
        MOVING,
        AIR,
        SLOW_WALK
    };

    enum class ResolveSide {
        ORIGINAL = 0,
        LEFT = -1,
        RIGHT = 1
    };

    enum class ResolveMethod {
        LBY,
        ANIMSTATE,
        POSE_PARAM,
        TRACE,
        BRUTEFORCE,
        COUNT
    };

    // Pose parameter indices (CS:GO specific)
    enum PoseParameters {
        LEAN_YAW = 0,
        SPEED = 1,
        LADDER_SPEED = 2,
        LADDER_YAW = 3,
        MOVE_YAW = 4,
        RUN = 5,
        BODY_YAW = 6,
        BODY_PITCH = 7,
        DEATH_YAW = 8,
        STAND = 9,
        JUMP_FALL = 10,
        AIM_BLEND_STAND_IDLE = 11,
        AIM_BLEND_CROUCH_IDLE = 12,
        STRAFE_DIR = 13,
        AIM_BLEND_STAND_WALK = 14,
        AIM_BLEND_STAND_RUN = 15,
        AIM_BLEND_CROUCH_WALK = 16,
        MOVE_BLEND_WALK = 17,
        MOVE_BLEND_RUN = 18,
        MOVE_BLEND_CROUCH_WALK = 19,
        AIM_MATRIX = 20,
        POSE_PARAM_COUNT = 24
    };

    // ============================================
    // STRUCTURES
    // ============================================

    struct MethodVote {
        ResolveMethod method;
        ResolveSide side;
        float confidence;      // 0.0 - 1.0
        float suggested_yaw;   // Suggested absolute yaw
        bool valid;

        MethodVote() : method(ResolveMethod::LBY), side(ResolveSide::ORIGINAL),
            confidence(0.f), suggested_yaw(0.f), valid(false) {
        }
    };

    struct LBYData {
        float current_lby = 0.f;
        float last_lby = 0.f;
        float last_moving_lby = 0.f;
        float lby_delta = 0.f;
        float last_update_time = 0.f;
        float next_update_time = 0.f;
        float standing_time = 0.f;
        int flick_count = 0;
        bool updated_this_tick = false;
        bool is_flicking = false;

        std::array<float, 32> lby_history{};
        int history_index = 0;

        void reset() {
            current_lby = 0.f;
            last_lby = 0.f;
            last_moving_lby = 0.f;
            lby_delta = 0.f;
            last_update_time = 0.f;
            next_update_time = 0.f;
            standing_time = 0.f;
            flick_count = 0;
            updated_this_tick = false;
            is_flicking = false;
            lby_history.fill(0.f);
            history_index = 0;
        }
    };

    struct AnimStateData {
        float foot_yaw = 0.f;
        float last_foot_yaw = 0.f;
        float goal_feet_yaw = 0.f;
        float eye_yaw = 0.f;
        float move_yaw = 0.f;
        float move_yaw_ideal = 0.f;
        float move_yaw_current = 0.f;
        float duck_amount = 0.f;
        float velocity_length = 0.f;

        std::array<AnimationLayer, 13> layers{};
        std::array<AnimationLayer, 13> prev_layers{};

        // Layer deltas for comparison
        struct LayerDelta {
            float weight_delta = 0.f;
            float cycle_delta = 0.f;
            float rate_delta = 0.f;
        };
        std::array<LayerDelta, 13> layer_deltas{};

        void reset() {
            foot_yaw = 0.f;
            last_foot_yaw = 0.f;
            goal_feet_yaw = 0.f;
            eye_yaw = 0.f;
            move_yaw = 0.f;
            move_yaw_ideal = 0.f;
            move_yaw_current = 0.f;
            duck_amount = 0.f;
            velocity_length = 0.f;
        }
    };

    struct PoseParamData {
        std::array<float, POSE_PARAM_COUNT> current{};
        std::array<float, POSE_PARAM_COUNT> previous{};
        std::array<float, POSE_PARAM_COUNT> deltas{};

        // Simulated pose params for left/right/center
        std::array<float, POSE_PARAM_COUNT> simulated_left{};
        std::array<float, POSE_PARAM_COUNT> simulated_right{};
        std::array<float, POSE_PARAM_COUNT> simulated_center{};

        float body_yaw = 0.f;
        float lean_yaw = 0.f;
        float move_yaw = 0.f;

        void reset() {
            current.fill(0.f);
            previous.fill(0.f);
            deltas.fill(0.f);
            simulated_left.fill(0.f);
            simulated_right.fill(0.f);
            simulated_center.fill(0.f);
            body_yaw = 0.f;
            lean_yaw = 0.f;
            move_yaw = 0.f;
        }
    };

    struct JitterData {
        std::array<float, 32> yaw_history{};
        int history_index = 0;
        int jitter_ticks = 0;
        int static_ticks = 0;
        float average_yaw = 0.f;
        float yaw_variance = 0.f;
        bool is_jittering = false;
        int jitter_side = 0; // For jitter pattern tracking

        void reset() {
            yaw_history.fill(0.f);
            history_index = 0;
            jitter_ticks = 0;
            static_ticks = 0;
            average_yaw = 0.f;
            yaw_variance = 0.f;
            is_jittering = false;
            jitter_side = 0;
        }
    };

    struct ResolverData {
        // Final resolved data
        ResolveSide side = ResolveSide::ORIGINAL;
        ResolveMode mode = ResolveMode::NONE;
        float resolved_yaw = 0.f;
        float confidence = 0.f;

        // Method-specific data
        LBYData lby;
        AnimStateData animstate;
        PoseParamData pose;
        JitterData jitter;

        // Voting results
        std::array<MethodVote, static_cast<int>(ResolveMethod::COUNT)> votes{};
        ResolveMethod winning_method = ResolveMethod::LBY;

        // Statistics
        int misses = 0;
        int hits = 0;
        int total_shots = 0;

        // State flags
        bool is_low_delta = false;
        bool is_faking = false;
        bool is_moving = false;
        bool is_on_ground = false;
        bool extended_desync = false;

        // Timing
        float last_resolve_time = 0.f;
        float last_moving_time = 0.f;

        // Eye angles history
        std::array<Vector, 16> eye_angles_history{};
        int eye_angles_index = 0;

        void reset() {
            side = ResolveSide::ORIGINAL;
            mode = ResolveMode::NONE;
            resolved_yaw = 0.f;
            confidence = 0.f;
            lby.reset();
            animstate.reset();
            pose.reset();
            jitter.reset();
            for (auto& v : votes) v = MethodVote();
            winning_method = ResolveMethod::LBY;
            misses = 0;
            hits = 0;
            total_shots = 0;
            is_low_delta = false;
            is_faking = false;
            is_moving = false;
            is_on_ground = false;
            extended_desync = false;
            last_resolve_time = 0.f;
            last_moving_time = 0.f;
            eye_angles_history.fill(Vector{});
            eye_angles_index = 0;
        }
    };

    struct ShotSnapshot {
        int player_index = -1;
        int backtrack_tick = -1;
        Vector eye_position{};
        Vector impact_position{};
        float time = -1.f;
        bool got_impact = false;
        matrix3x4 matrix[256]{};
        const Model* model = nullptr;
        ResolverData resolver_state; // Store resolver state at shot time
    };

    // ============================================
    // PUBLIC METHODS
    // ============================================

    static void initialize() noexcept;
    static void reset() noexcept;
    static void update(Entity* entity, Animations::Players& player) noexcept;

    // Event handling
    static void processEvents(GameEvent* event) noexcept;

    // Shot tracking
    static void saveShot(int playerIndex, float simTime, int backtrackTick = -1) noexcept;
    static void processMissedShots() noexcept;

    // Event listeners
    static void updateEventListeners(bool forceRemove = false) noexcept;

    // Getters
    static const ResolverData& getData(int index) noexcept { return data[index]; }
    static bool isEnabled() noexcept { return enabled; }
    static void setEnabled(bool value) noexcept { enabled = value; }

    // Debug info
    static std::string getDebugInfo(int index) noexcept;
    static std::string getMethodName(ResolveMethod method) noexcept;

private:
    // ============================================
    // CORE RESOLVE METHODS
    // ============================================

    // Main voting system
    static void collectVotes(Entity* entity, ResolverData& info) noexcept;
    static void processVotes(Entity* entity, ResolverData& info) noexcept;
    static float calculateFinalYaw(Entity* entity, ResolverData& info) noexcept;

    // Individual resolvers
    static MethodVote resolveLBY(Entity* entity, ResolverData& info) noexcept;
    static MethodVote resolveAnimState(Entity* entity, ResolverData& info) noexcept;
    static MethodVote resolvePoseParams(Entity* entity, ResolverData& info) noexcept;
    static MethodVote resolveTrace(Entity* entity, ResolverData& info) noexcept;
    static MethodVote resolveBruteforce(Entity* entity, ResolverData& info) noexcept;

    // ============================================
    // LBY RESOLVER
    // ============================================

    static void updateLBYData(Entity* entity, ResolverData& info) noexcept;
    static bool predictLBYUpdate(Entity* entity, ResolverData& info) noexcept;
    static float getLBYUpdateTime(Entity* entity, ResolverData& info) noexcept;
    static bool detectLBYFlick(Entity* entity, ResolverData& info) noexcept;
    static ResolveSide getLBYSide(Entity* entity, ResolverData& info) noexcept;

    // ============================================
    // ANIMSTATE RESOLVER
    // ============================================

    static void updateAnimStateData(Entity* entity, ResolverData& info) noexcept;
    static void simulateAnimState(Entity* entity, ResolverData& info, float yaw) noexcept;
    static float compareAnimLayers(Entity* entity, ResolverData& info) noexcept;
    static ResolveSide getAnimStateSide(Entity* entity, ResolverData& info) noexcept;
    static float getMoveYaw(Entity* entity, ResolverData& info) noexcept;
    static bool detectExtendedDesync(Entity* entity, ResolverData& info) noexcept;

    // ============================================
    // POSE PARAMETER RESOLVER
    // ============================================

    static void updatePoseParamData(Entity* entity, ResolverData& info) noexcept;
    static void simulatePoseParams(Entity* entity, ResolverData& info, float yaw,
        std::array<float, POSE_PARAM_COUNT>& out) noexcept;
    static float comparePoseParams(const std::array<float, POSE_PARAM_COUNT>& a,
        const std::array<float, POSE_PARAM_COUNT>& b) noexcept;
    static ResolveSide getPoseParamSide(Entity* entity, ResolverData& info) noexcept;
    static float extractBodyYawFromPose(Entity* entity, ResolverData& info) noexcept;

    // ============================================
    // DETECTION METHODS
    // ============================================

    static ResolveMode detectMode(Entity* entity, ResolverData& info) noexcept;
    static bool detectJitter(Entity* entity, ResolverData& info) noexcept;
    static bool detectLowDelta(Entity* entity, ResolverData& info) noexcept;
    static bool detectFakeAngles(Entity* entity) noexcept;
    static bool detectFakeWalk(Entity* entity, ResolverData& info) noexcept;

    // ============================================
    // UTILITY METHODS
    // ============================================

    static float getMaxDesync(Entity* entity) noexcept;
    static float buildServerAbsYaw(Entity* entity, float yaw) noexcept;
    static float getAtTargetYaw(Entity* entity) noexcept;
    static float normalizeYaw(float yaw) noexcept;
    static float angleDiff(float a, float b) noexcept;
    static float approachAngle(float target, float value, float speed) noexcept;

    // Weight calculations for voting
    static float getMethodWeight(ResolveMethod method, ResolverData& info) noexcept;
    static float calculateConfidence(Entity* entity, ResolverData& info, MethodVote& vote) noexcept;

    // ============================================
    // DATA
    // ============================================

    static inline std::array<ResolverData, 65> data{};
    static inline std::deque<ShotSnapshot> snapshots{};
    static inline bool enabled = true;

    // ============================================
    // CONSTANTS
    // ============================================

    static constexpr float LBY_UPDATE_TIME = 1.1f;
    static constexpr float JITTER_THRESHOLD = 15.f;
    static constexpr float LOW_DELTA_THRESHOLD = 35.f;
    static constexpr float MOVING_SPEED_THRESHOLD = 0.1f;
    static constexpr float FAKE_WALK_THRESHOLD = 52.f;
    static constexpr int MAX_SNAPSHOTS = 32;
    static constexpr int HISTORY_SIZE = 32;

    // Method weights (can be adjusted)
    static constexpr float WEIGHT_LBY = 1.0f;
    static constexpr float WEIGHT_ANIMSTATE = 1.2f;
    static constexpr float WEIGHT_POSE = 1.1f;
    static constexpr float WEIGHT_TRACE = 0.8f;
    static constexpr float WEIGHT_BRUTE = 0.5f;
};