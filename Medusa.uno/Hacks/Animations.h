#pragma once

#include "../SDK/matrix3x4.h"
#include "../SDK/Vector.h"
#include "../SDK/AnimState.h"
#include "../SDK/FrameStage.h"
#include "../SDK/UserCmd.h"
#include "../SDK/Entity.h"
#include <array>
#include <deque>

// ============================================
// CONSTANTS (глобальные, вне namespace)
// ============================================

constexpr int MAX_ANIMATION_LAYERS = 13;

// ============================================
// ENUMS (глобальные, вне namespace)
// ============================================


// Activity types
enum ActivityType : int {
    ACTIVITY_NONE = 0,
    ACTIVITY_JUMP,
    ACTIVITY_LAND_LIGHT,
    ACTIVITY_LAND_HEAVY
};


// ============================================
// NAMESPACE ANIMATIONS
// ============================================

namespace Animations {

    // ============================================
    // STRUCTURES
    // ============================================

    struct AnimationData {
        std::array<AnimationLayer, ANIMATION_LAYER_COUNT> layers{};
        std::array<float, 24> poseParameters{};
        float footYaw = 0.f;
        float moveWeight = 0.f;
        float primaryCycle = 0.f;
        float duckAmount = 0.f;
        Vector velocity{};
        Vector origin{};
        int flags = 0;
    };

    struct SimulatedTick {
        float time = 0.f;
        Vector origin{};
        Vector velocity{};
        float duckAmount = 0.f;
        int flags = 0;
        std::array<AnimationLayer, ANIMATION_LAYER_COUNT> layers{};
        std::array<float, 24> poseParameters{};
    };

    struct Players {
        // Basic info
        float simulationTime = -1.f;
        float oldSimulationTime = -1.f;
        float spawnTime = 0.f;

        // Position & movement
        Vector origin{};
        Vector oldOrigin{};
        Vector absAngle{};
        Vector velocity{};
        Vector oldVelocity{};
        Vector mins{};
        Vector maxs{};

        // Animation state
        float moveWeight = 0.f;
        float duckAmount = 0.f;
        float oldDuckAmount = 0.f;
        float lby = 0.f;
        float oldLby = 0.f;
        int flags = 0;
        int oldFlags = 0;

        // Choke detection
        int chokedPackets = 0;
        int lastChokedPackets = 0;

        // Animation layers
        std::array<AnimationLayer, ANIMATION_LAYER_COUNT> layers{};
        std::array<AnimationLayer, ANIMATION_LAYER_COUNT> oldlayers{};
        std::array<float, 24> poseParameters{};
        std::array<float, 24> oldPoseParameters{};

        // Simulated layers for resolver
        std::array<AnimationLayer, ANIMATION_LAYER_COUNT> centerLayer{};
        std::array<AnimationLayer, ANIMATION_LAYER_COUNT> leftLayer{};
        std::array<AnimationLayer, ANIMATION_LAYER_COUNT> rightLayer{};

        // Resolver data
        int side = 0;
        int lastSide = 0;
        int misses = 0;
        bool shot = false;
        bool gotMatrix = false;
        bool extended = false;
        int rotation_side = 0;
        int rotation_mode = 0;
        int last_side = 0;
        bool anim_resolved = false;

        // Activity detection
        ActivityType activity = ACTIVITY_NONE;
        float activityStartTime = 0.f;

        // Bone matrices
        std::array<matrix3x4, MAXSTUDIOBONES> matrix{};
        std::array<matrix3x4, MAXSTUDIOBONES> leftMatrix{};
        std::array<matrix3x4, MAXSTUDIOBONES> rightMatrix{};
        std::array<matrix3x4, MAXSTUDIOBONES> centerMatrix{};

        // Backtrack records
        struct Record {
            float simulationTime = 0.f;
            Vector origin{};
            Vector absAngle{};
            Vector mins{};
            Vector maxs{};
            matrix3x4 matrix[MAXSTUDIOBONES]{};
            std::vector<Vector> positions{};

            // Extended data for better resolver integration
            int side = 0;
            float footYaw = 0.f;
            std::array<AnimationLayer, ANIMATION_LAYER_COUNT> layers{};
        };
        std::deque<Record> backtrackRecords{};

        // Simulated ticks for choke reconstruction
        std::vector<SimulatedTick> simulatedTicks{};

        // Resolver layers for comparison
        std::array<AnimationLayer, ANIMATION_LAYER_COUNT> resolver_layers[3]{}; // LEFT, RIGHT, CENTER
        float Left = 0.f;
        float Right = 0.f;

        void reset() noexcept {
            simulationTime = -1.f;
            oldSimulationTime = -1.f;
            origin = Vector{};
            oldOrigin = Vector{};
            absAngle = Vector{};
            velocity = Vector{};
            oldVelocity = Vector{};
            mins = Vector{};
            maxs = Vector{};
            moveWeight = 0.f;
            duckAmount = 0.f;
            oldDuckAmount = 0.f;
            lby = 0.f;
            oldLby = 0.f;
            flags = 0;
            oldFlags = 0;
            chokedPackets = 0;
            lastChokedPackets = 0;

            for (auto& l : layers) l = AnimationLayer{};
            for (auto& l : oldlayers) l = AnimationLayer{};
            poseParameters.fill(0.f);
            oldPoseParameters.fill(0.f);
            for (auto& l : centerLayer) l = AnimationLayer{};
            for (auto& l : leftLayer) l = AnimationLayer{};
            for (auto& l : rightLayer) l = AnimationLayer{};

            side = 0;
            lastSide = 0;
            misses = 0;
            shot = false;
            gotMatrix = false;
            extended = false;
            rotation_side = 0;
            rotation_mode = 0;
            last_side = 0;
            anim_resolved = false;
            activity = ACTIVITY_NONE;
            activityStartTime = 0.f;

            for (auto& m : matrix) m = matrix3x4{};
            for (auto& m : leftMatrix) m = matrix3x4{};
            for (auto& m : rightMatrix) m = matrix3x4{};
            for (auto& m : centerMatrix) m = matrix3x4{};

            backtrackRecords.clear();
            simulatedTicks.clear();

            for (auto& rl : resolver_layers) {
                for (auto& l : rl) l = AnimationLayer{};
            }
            Left = 0.f;
            Right = 0.f;
        }

        void clear() noexcept {
            backtrackRecords.clear();
            simulatedTicks.clear();
            shot = false;
        }
    };

    // ============================================
    // CORE FUNCTIONS
    // ============================================

    void init() noexcept;
    void reset() noexcept;

    // Main update functions
    void handlePlayers(FrameStage stage) noexcept;
    void update(UserCmd* cmd, bool& sendPacket) noexcept;
    void fake() noexcept;

    // Frame stage handlers
    void renderStart(FrameStage stage) noexcept;
    void packetStart() noexcept;
    void postDataUpdate() noexcept;

    // ============================================
    // SIMULATION FUNCTIONS
    // ============================================

    // Simulate entity animation for given ticks
    void simulateEntity(Entity* entity, Players& record, int ticks) noexcept;

    // Simulate specific side (for resolver)
    void simulateSide(Entity* entity, Players& record, float yaw,
        std::array<AnimationLayer, ANIMATION_LAYER_COUNT>& outLayers,
        std::array<matrix3x4, MAXSTUDIOBONES>& outMatrix) noexcept;

    // Rebuild animation state
    void rebuildAnimationState(Entity* entity, Players& record) noexcept;

    // ============================================
    // UTILITY FUNCTIONS
    // ============================================

    // Velocity calculation
    Vector calculateVelocity(Entity* entity, Players& record) noexcept;
    void fixVelocity(Entity* entity, Players& record) noexcept;

    // Activity detection
    ActivityType detectActivity(Entity* entity, Players& record) noexcept;

    // Layer comparison
    float compareLayerDelta(const AnimationLayer& a, const AnimationLayer& b) noexcept;
    bool isLayerActive(const AnimationLayer& layer) noexcept;

    // ============================================
    // GETTERS
    // ============================================

    void saveCorrectAngle(int entityIndex, Vector correctAng) noexcept;

    int& buildTransformationsIndex() noexcept;
    Vector* getCorrectAngle() noexcept;
    Vector* getViewAngles() noexcept;
    Vector* getLocalAngle() noexcept;

    bool isLocalUpdating() noexcept;
    bool isEntityUpdating() noexcept;
    bool isFakeUpdating() noexcept;

    bool gotFakeMatrix() noexcept;
    std::array<matrix3x4, MAXSTUDIOBONES> getFakeMatrix() noexcept;

    bool gotFakelagMatrix() noexcept;
    std::array<matrix3x4, MAXSTUDIOBONES> getFakelagMatrix() noexcept;

    bool gotRealMatrix() noexcept;
    std::array<matrix3x4, MAXSTUDIOBONES> getRealMatrix() noexcept;

    float getFootYaw() noexcept;
    std::array<float, 24> getPoseParameters() noexcept;
    std::array<AnimationLayer, ANIMATION_LAYER_COUNT> getAnimLayers() noexcept;

    Players getPlayer(int index) noexcept;
    Players* setPlayer(int index) noexcept;
    std::array<Players, 65> getPlayers() noexcept;
    std::array<Players, 65>* setPlayers() noexcept;
    const std::deque<Players::Record>* getBacktrackRecords(int index) noexcept;
}