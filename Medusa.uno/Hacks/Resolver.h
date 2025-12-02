#pragma once

#include "../SDK/Entity.h"
#include "../SDK/GameEvent.h"
#include "../SDK/Vector.h"
#include "../SDK/matrix3x4.h"
#include "Animations.h"
#include <array>
#include <deque>
#include <string>
#include <unordered_map>

class Resolver {
public:
    // ============================================
    // ENUMS
    // ============================================

    enum class ResolveSide : int {
        ORIGINAL = 0,
        LEFT = 1,
        RIGHT = -1
    };

    enum class ResolveMode : int {
        NONE = 0,
        STANDING,
        MOVING,
        AIR,
        SLOW_WALK,
        FAKE_DUCK,
        LADDER
    };

    enum class ResolveMethod : int {
        LBY = 0,
        ANIMSTATE,
        POSE_PARAM,
        TRACE,
        BRUTEFORCE,
        FREESTANDING,
        LAST_MOVING,
        PREDICTION,
        COUNT
    };

    // === НОВЫЕ: Типы промахов ===
    enum class MissType : int {
        NONE = 0,
        SPREAD,                    // Разброс оружия
        RESOLVER_WRONG_SIDE,       // Резольвер выбрал неправильную сторону
        DESYNC_CORRECTION,         // Ошибка коррекции desync
        JITTER_CORRECTION,         // Ошибка коррекции jitter
        PREDICTION_ERROR,          // Ошибка предсказания
        MISPREDICTION,             // Неверное предсказание движения
        DAMAGE_REJECTION,          // Сервер отклонил урон
        SERVER_REJECTION,          // Сервер отклонил выстрел
        UNREGISTERED_SHOT,         // Незарегистрированный выстрел
        BACKTRACK_FAILURE,         // Ошибка backtrack
        OCCLUSION,                 // Препятствие на пути
        TICKBASE_MISMATCH,         // Рассинхронизация tickbase
        ANIMATION_DESYNC,          // Рассинхронизация анимации
        HITBOX_MISMATCH,           // Несоответствие hitbox
        NETWORK_LOSS,              // Потеря пакетов
        ANTI_AIM_EXPLOIT,          // Эксплойт анти-аима
        EXTENDED_DESYNC,           // Extended desync не обнаружен
        LBY_BREAK,                 // LBY break не предсказан
        FAKE_FLICK,                // Fake flick не обнаружен
        COUNT
    };

    // === НОВЫЕ: Типы попаданий ===
    enum class HitType : int {
        NONE = 0,
        RESOLVER_CORRECT,          // Резольвер правильно определил сторону
        LBY_PREDICTION,            // Предсказание LBY сработало
        JITTER_PREDICTION,         // Предсказание jitter сработало
        BRUTEFORCE_HIT,            // Bruteforce попал
        FREESTANDING_HIT,          // Freestanding попал
        BACKTRACK_HIT,             // Backtrack попал
        ANIMSTATE_CORRECT,         // AnimState правильно определил
        POSE_PARAM_CORRECT,        // Pose parameters правильно определили
        PREDICTION_CORRECT,        // Предсказание движения сработало
        TRACE_CORRECT,             // Trace resolver сработал
        COUNT
    };

    // ============================================
    // STRUCTURES
    // ============================================

    struct MethodVote {
        ResolveMethod method = ResolveMethod::LBY;
        ResolveSide side = ResolveSide::ORIGINAL;
        float suggested_yaw = 0.f;
        float confidence = 0.f;
        bool valid = false;
        std::string reason;
    };

    struct LBYData {
        float current_lby = 0.f;
        float last_lby = 0.f;
        float lby_delta = 0.f;
        float last_update_time = 0.f;
        float next_update_time = 0.f;
        float standing_time = 0.f;
        float last_moving_lby = 0.f;
        float predicted_lby = 0.f;
        int flick_count = 0;
        int history_index = 0;
        std::array<float, 32> lby_history{};
        bool updated_this_tick = false;
        bool is_flicking = false;
        bool break_predicted = false;
        float break_time = 0.f;
    };

    struct AnimStateData {
        float foot_yaw = 0.f;
        float last_foot_yaw = 0.f;
        float goal_feet_yaw = 0.f;
        float eye_yaw = 0.f;
        float duck_amount = 0.f;
        float velocity_length = 0.f;
        float move_yaw = 0.f;
        std::array<AnimationLayer, 13> layers{};
        std::array<AnimationLayer, 13> prev_layers{};

        struct LayerDelta {
            float weight_delta = 0.f;
            float cycle_delta = 0.f;
            float rate_delta = 0.f;
        };
        std::array<LayerDelta, 13> layer_deltas{};
    };

    struct PoseParamData {
        static constexpr int POSE_PARAM_COUNT = 24;
        std::array<float, POSE_PARAM_COUNT> current{};
        std::array<float, POSE_PARAM_COUNT> previous{};
        std::array<float, POSE_PARAM_COUNT> deltas{};
        std::array<float, POSE_PARAM_COUNT> simulated_left{};
        std::array<float, POSE_PARAM_COUNT> simulated_right{};
        std::array<float, POSE_PARAM_COUNT> simulated_center{};
        float body_yaw = 0.f;
        float lean_yaw = 0.f;
        float move_yaw = 0.f;
    };

    struct JitterData {
        bool is_jittering = false;
        int jitter_ticks = 0;
        int static_ticks = 0;
        int history_index = 0;
        float yaw_variance = 0.f;
        float average_yaw = 0.f;
        int jitter_side = 0;
        float jitter_range = 0.f;
        int jitter_pattern = 0;
        std::array<float, 32> yaw_history{};
        std::array<float, 16> delta_history{};
        int pattern_length = 0;
        bool pattern_detected = false;
    };

    // === НОВЫЕ: Статистика для детального анализа ===
    struct ShotStatistics {
        int total_shots = 0;
        int hits = 0;
        int misses = 0;

        // Детализация попаданий
        std::array<int, static_cast<int>(HitType::COUNT)> hit_types{};

        // Детализация промахов
        std::array<int, static_cast<int>(MissType::COUNT)> miss_types{};

        // Статистика по методам
        std::array<int, static_cast<int>(ResolveMethod::COUNT)> method_attempts{};
        std::array<int, static_cast<int>(ResolveMethod::COUNT)> method_successes{};

        // Статистика по сторонам
        int left_hits = 0;
        int left_misses = 0;
        int right_hits = 0;
        int right_misses = 0;
        int center_hits = 0;
        int center_misses = 0;

        // Backtrack статистика
        int backtrack_attempts = 0;
        int backtrack_successes = 0;
        int backtrack_failures = 0;

        // Последовательности
        int consecutive_misses = 0;
        int max_consecutive_misses = 0;
        int consecutive_hits = 0;
        int max_consecutive_hits = 0;

        // Временные метки
        float last_hit_time = 0.f;
        float last_miss_time = 0.f;
        float last_shot_time = 0.f;

        void reset() {
            total_shots = 0;
            hits = 0;
            misses = 0;
            hit_types.fill(0);
            miss_types.fill(0);
            method_attempts.fill(0);
            method_successes.fill(0);
            left_hits = left_misses = 0;
            right_hits = right_misses = 0;
            center_hits = center_misses = 0;
            backtrack_attempts = backtrack_successes = backtrack_failures = 0;
            consecutive_misses = max_consecutive_misses = 0;
            consecutive_hits = max_consecutive_hits = 0;
            last_hit_time = last_miss_time = last_shot_time = 0.f;
        }
    };

    struct ResolverData {
        // Core data
        ResolveMode mode = ResolveMode::NONE;
        ResolveSide side = ResolveSide::ORIGINAL;
        ResolveSide last_side = ResolveSide::ORIGINAL;
        ResolveMethod winning_method = ResolveMethod::LBY;
        float confidence = 0.f;
        float resolved_yaw = 0.f;
        float last_resolve_time = 0.f;

        // State flags
        bool is_on_ground = false;
        bool is_moving = false;
        bool is_low_delta = false;
        bool is_faking = false;
        bool extended_desync = false;
        bool is_fake_walking = false;
        bool is_on_ladder = false;
        bool is_fake_ducking = false;

        // Sub-data
        LBYData lby;
        AnimStateData animstate;
        PoseParamData pose;
        JitterData jitter;

        // Voting
        std::array<MethodVote, static_cast<int>(ResolveMethod::COUNT)> votes{};

        // Eye angles history
        int eye_angles_index = 0;
        std::array<Vector, 32> eye_angles_history{};

        // Movement data
        float last_moving_time = 0.f;
        Vector last_velocity{};
        Vector last_origin{};

        // === НОВОЕ: Расширенная статистика ===
        ShotStatistics stats;

        // Legacy (для совместимости)
        int misses = 0;
        int hits = 0;
        int total_shots = 0;

        void reset() {
            mode = ResolveMode::NONE;
            side = ResolveSide::ORIGINAL;
            last_side = ResolveSide::ORIGINAL;
            winning_method = ResolveMethod::LBY;
            confidence = 0.f;
            resolved_yaw = 0.f;
            last_resolve_time = 0.f;
            is_on_ground = is_moving = is_low_delta = false;
            is_faking = extended_desync = is_fake_walking = false;
            is_on_ladder = is_fake_ducking = false;
            lby = LBYData{};
            animstate = AnimStateData{};
            pose = PoseParamData{};
            jitter = JitterData{};
            eye_angles_index = 0;
            eye_angles_history.fill(Vector{});
            last_moving_time = 0.f;
            last_velocity = Vector{};
            last_origin = Vector{};
            stats.reset();
            misses = hits = total_shots = 0;
        }
    };

    // === УЛУЧШЕННЫЙ: ShotSnapshot ===
    struct ShotSnapshot {
        int player_index = 0;
        int backtrack_tick = 0;
        int command_number = 0;
        float time = -1.f;
        float simulation_time = 0.f;

        Vector eye_position{};
        Vector aim_point{};
        Vector impact_position{};
        Vector predicted_position{};

        const Model* model = nullptr;
        matrix3x4 matrix[256]{};

        ResolverData resolver_state{};

        // Детали выстрела
        int hitbox_targeted = 0;
        int hitgroup_targeted = 0;
        float hitchance_predicted = 0.f;
        float damage_predicted = 0.f;

        // Backtrack данные
        bool used_backtrack = false;
        int backtrack_ticks = 0;
        float backtrack_simtime = 0.f;

        // Результат
        bool got_impact = false;
        bool got_hurt_event = false;
        int actual_damage = 0;
        int actual_hitgroup = 0;

        // Анализ
        MissType miss_type = MissType::NONE;
        HitType hit_type = HitType::NONE;
        std::string analysis_result;

        // Timing
        float client_time = 0.f;
        float server_time = 0.f;
        int client_tick = 0;
        int server_tick = 0;
    };

    // ============================================
    // CONSTANTS
    // ============================================

    static constexpr float LBY_UPDATE_TIME = 1.1f;
    static constexpr float MOVING_SPEED_THRESHOLD = 0.1f;
    static constexpr float FAKE_WALK_THRESHOLD = 34.f;
    static constexpr float LOW_DELTA_THRESHOLD = 35.f;
    static constexpr float JITTER_THRESHOLD = 25.f;
    static constexpr int HISTORY_SIZE = 32;
    static constexpr size_t MAX_SNAPSHOTS = 16;

    // Weights
    static constexpr float WEIGHT_LBY = 1.0f;
    static constexpr float WEIGHT_ANIMSTATE = 1.2f;
    static constexpr float WEIGHT_POSE = 0.9f;
    static constexpr float WEIGHT_TRACE = 0.8f;
    static constexpr float WEIGHT_BRUTE = 0.6f;
    static constexpr float WEIGHT_FREESTANDING = 0.85f;
    static constexpr float WEIGHT_LAST_MOVING = 0.7f;
    static constexpr float WEIGHT_PREDICTION = 0.75f;

    // Pose parameter indices
    static constexpr int LEAN_YAW = 0;
    static constexpr int SPEED = 1;
    static constexpr int MOVE_YAW = 4;
    static constexpr int BODY_YAW = 6;
    static constexpr int BODY_PITCH = 7;
    static constexpr int STRAFE_DIR = 13;
    static constexpr int POSE_PARAM_COUNT = 24;

    // Animation layer indices
    static constexpr int ANIMATION_LAYER_ADJUST = 3;
    static constexpr int ANIMATION_LAYER_MOVEMENT_MOVE = 6;
    static constexpr int ANIMATION_LAYER_LEAN = 12;

    // ============================================
    // MAIN INTERFACE
    // ============================================

    static void initialize() noexcept;
    static void reset() noexcept;
    static void update(Entity* entity, Animations::Players& player) noexcept;

    // ============================================
    // VOTING SYSTEM
    // ============================================

    static void collectVotes(Entity* entity, ResolverData& info) noexcept;
    static void processVotes(Entity* entity, ResolverData& info) noexcept;
    static float calculateFinalYaw(Entity* entity, ResolverData& info) noexcept;
    static float getMethodWeight(ResolveMethod method, ResolverData& info) noexcept;

    // ============================================
    // RESOLVE METHODS
    // ============================================

    static MethodVote resolveLBY(Entity* entity, ResolverData& info) noexcept;
    static MethodVote resolveAnimState(Entity* entity, ResolverData& info) noexcept;
    static MethodVote resolvePoseParams(Entity* entity, ResolverData& info) noexcept;
    static MethodVote resolveTrace(Entity* entity, ResolverData& info) noexcept;
    static MethodVote resolveBruteforce(Entity* entity, ResolverData& info) noexcept;
    static MethodVote resolveFreestanding(Entity* entity, ResolverData& info) noexcept;
    static MethodVote resolveLastMoving(Entity* entity, ResolverData& info) noexcept;
    static MethodVote resolvePrediction(Entity* entity, ResolverData& info) noexcept;

    // ============================================
    // DATA UPDATES
    // ============================================

    static void updateLBYData(Entity* entity, ResolverData& info) noexcept;
    static void updateAnimStateData(Entity* entity, ResolverData& info) noexcept;
    static void updatePoseParamData(Entity* entity, ResolverData& info) noexcept;

    // ============================================
    // LBY METHODS
    // ============================================

    static bool predictLBYUpdate(Entity* entity, ResolverData& info) noexcept;
    static bool detectLBYFlick(Entity* entity, ResolverData& info) noexcept;
    static bool predictLBYBreak(Entity* entity, ResolverData& info) noexcept;
    static ResolveSide getLBYSide(Entity* entity, ResolverData& info) noexcept;

    // ============================================
    // ANIMSTATE METHODS
    // ============================================

    static float getMoveYaw(Entity* entity, ResolverData& info) noexcept;
    static bool detectExtendedDesync(Entity* entity, ResolverData& info) noexcept;
    static float compareAnimLayers(Entity* entity, ResolverData& info) noexcept;
    static ResolveSide getAnimStateSide(Entity* entity, ResolverData& info) noexcept;

    // ============================================
    // POSE PARAM METHODS
    // ============================================

    static void simulatePoseParams(Entity* entity, ResolverData& info, float yaw,
        std::array<float, POSE_PARAM_COUNT>& out) noexcept;
    static float comparePoseParams(const std::array<float, POSE_PARAM_COUNT>& a,
        const std::array<float, POSE_PARAM_COUNT>& b) noexcept;
    static float extractBodyYawFromPose(Entity* entity, ResolverData& info) noexcept;
    static ResolveSide getPoseParamSide(Entity* entity, ResolverData& info) noexcept;

    // ============================================
    // DETECTION METHODS
    // ============================================

    static ResolveMode detectMode(Entity* entity, ResolverData& info) noexcept;
    static bool detectJitter(Entity* entity, ResolverData& info) noexcept;
    static bool detectJitterPattern(Entity* entity, ResolverData& info) noexcept;
    static bool detectLowDelta(Entity* entity, ResolverData& info) noexcept;
    static bool detectFakeAngles(Entity* entity) noexcept;
    static bool detectFakeWalk(Entity* entity, ResolverData& info) noexcept;
    static bool detectFakeDuck(Entity* entity, ResolverData& info) noexcept;

    // ============================================
    // UTILITY METHODS
    // ============================================

    static float getMaxDesync(Entity* entity) noexcept;
    static float buildServerAbsYaw(Entity* entity, float yaw) noexcept;
    static float getAtTargetYaw(Entity* entity) noexcept;
    static float normalizeYaw(float yaw) noexcept;
    static float angleDiff(float a, float b) noexcept;
    static float approachAngle(float target, float value, float speed) noexcept;

    // ============================================
    // EVENT HANDLING & LOGGING
    // ============================================

    static void processEvents(GameEvent* event) noexcept;
    static void updateEventListeners(bool forceRemove = false) noexcept;
    static void saveShot(int playerIndex, float simTime, int backtrackTick,
    const Vector& aimPoint, int hitbox, float hitchance, float damage) noexcept;
    static void processMissedShots() noexcept;

    // === НОВЫЕ: Расширенное логирование ===
    static void logHit(int playerIndex, HitType type, const std::string& details) noexcept;
    static void logMiss(int playerIndex, MissType type, const std::string& details) noexcept;
    static void analyzeShot(ShotSnapshot& snapshot) noexcept;
    static MissType determineMissType(ShotSnapshot& snapshot) noexcept;
    static HitType determineHitType(ShotSnapshot& snapshot) noexcept;
    static std::string getMissTypeName(MissType type) noexcept;
    static std::string getHitTypeName(HitType type) noexcept;

    // ============================================
    // DEBUG & INFO
    // ============================================

    static std::string getMethodName(ResolveMethod method) noexcept;
    static std::string getModeName(ResolveMode mode) noexcept;
    static std::string getSideName(ResolveSide side) noexcept;
    static std::string getDebugInfo(int index) noexcept;
    static std::string getStatisticsInfo(int index) noexcept;

    void resetStatistics() noexcept;
    void resetPlayerStatistics(int index) noexcept;


    // ============================================
    // DATA ACCESS
    // ============================================

    static ResolverData& getData(int index) noexcept { return data[index]; }
    static const ShotStatistics& getStatistics(int index) noexcept { return data[index].stats; }

    // ============================================
    // MEMBER VARIABLES
    // ============================================

    inline static bool enabled = true;
    inline static std::array<ResolverData, 65> data{};
    inline static std::deque<ShotSnapshot> snapshots{};

    // === НОВЫЕ: Глобальная статистика ===
    inline static ShotStatistics globalStats{};
    inline static std::deque<std::string> recentLogs{};
    inline static constexpr size_t MAX_LOGS = 50;
};