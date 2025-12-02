#pragma once

#include "../SDK/Vector.h"
#include "../SDK/matrix3x4.h"
#include <deque>
#include <vector>
#include <array>

struct UserCmd;
class Entity;
class NetworkChannel;

namespace Backtrack {

    // ============================================
    // CONSTANTS
    // ============================================

    constexpr float MAX_UNLAG_DEFAULT = 0.2f;
    constexpr float LERP_MIN = 0.0f;
    constexpr float LERP_MAX = 0.1f;
    constexpr int MAX_SEQUENCE_BUFFER = 2048;
    constexpr float TELEPORT_THRESHOLD = 64.0f;
    constexpr float RECORD_TOLERANCE = 0.2f;

    // ============================================
    // STRUCTURES
    // ============================================

    struct IncomingSequence {
        int inReliableState = 0;
        int sequenceNr = 0;
        float serverTime = 0.f;
    };

    struct Record {
        float simulationTime = 0.f;
        float serverTime = 0.f;

        Vector origin{};
        Vector absAngle{};
        Vector mins{};
        Vector maxs{};
        Vector velocity{};
        Vector eyeAngles{};

        matrix3x4 matrix[256]{};
        std::vector<Vector> bonePositions{};

        int flags = 0;
        int tickCount = 0;

        bool isValid = false;
        bool dormant = false;

        float GetAge(float currentTime) const noexcept {
            return currentTime - simulationTime;
        }
    };

    struct RecordPriority {
        int playerIndex = -1;
        int recordIndex = -1;
        float simulationTime = 0.f;
        float priority = 0.f;
        float fov = 0.f;
        float damage = 0.f;
        Vector bestPosition{};
        bool visible = false;

        bool operator<(const RecordPriority& other) const noexcept {
            return priority > other.priority;
        }
    };

    struct ConVarCache {
        float updateRate = 128.f;
        float maxUpdateRate = 128.f;
        float interp = 0.f;
        float interpRatio = 2.f;
        float minInterpRatio = 1.f;
        float maxInterpRatio = 2.f;
        float maxUnlag = 0.2f;

        bool initialized = false;
    };

    // ============================================
    // MAIN FUNCTIONS
    // ============================================

    // Initialization
    void init() noexcept;
    void reset() noexcept;

    // Main backtrack
    void run(UserCmd* cmd) noexcept;
    void update() noexcept;

    // Validation
    bool valid(float simulationTime) noexcept;
    bool isRecordValid(const Record& record, float serverTime) noexcept;
    bool isRecordHighQuality(int playerIndex, const Record& record) noexcept;

    // Calculations
    float getLerp() noexcept;
    float getMaxUnlag() noexcept;
    float calculatePriority(int playerIndex, const Record& record, const Vector& eyePos, const Vector& aimAngles) noexcept;

    // Fake latency
    void addLatencyToNetwork(NetworkChannel* network, float latency) noexcept;
    void updateIncomingSequences() noexcept;

    // Record management
    int findBestRecord(int playerIndex, const Vector& eyePos, const Vector& aimAngles, float maxFov) noexcept;
    std::vector<RecordPriority> gatherValidRecords(UserCmd* cmd, float maxFov) noexcept;

    // Utility
    float getRecordAge(float simulationTime) noexcept;
    int getTickCount(float simulationTime) noexcept;
    void updateConVars() noexcept;

    // ============================================
    // GETTERS
    // ============================================

    const std::deque<IncomingSequence>& getSequences() noexcept;
    const ConVarCache& getConVars() noexcept;
    bool isEnabled() noexcept;
    float getFakeLatency() noexcept;

    // ============================================
    // GLOBAL DATA
    // ============================================

    inline std::deque<IncomingSequence> sequences{};
    inline ConVarCache conVars{};
    inline int lastSequenceNumber = 0;
}