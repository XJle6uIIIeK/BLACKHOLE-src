#pragma once

#include "../SDK/Vector.h"
#include <array>

struct UserCmd;
class Entity;

namespace Fakelag {

    // ============================================
    // CONSTANTS
    // ============================================

    constexpr int MAX_CHOKE = 14;
    constexpr int MIN_CHOKE = 1;
    constexpr float BREAK_LC_DISTANCE = 64.f;
    constexpr float MIN_SPEED_FOR_FAKELAG = 2.f;
    constexpr float ADAPTIVE_BASE_DISTANCE = 64.f;

    // ============================================
    // ENUMS
    // ============================================

    enum class Mode {
        OFF = 0,
        STATIC,
        ADAPTIVE,
        RANDOM,
        BREAK_LC,
        PEEK,
        LEGIT,
        COUNT
    };

    enum class Trigger {
        ALWAYS = 0,
        MOVING,
        IN_AIR,
        PEEKING,
        STANDING
    };

    // ============================================
    // STRUCTURES
    // ============================================

    struct FakelagData {
        int chokedPackets = 0;
        int targetChoke = 0;
        int lastChoke = 0;

        float lastOriginUpdateTime = 0.f;
        Vector lastOrigin{};
        Vector lastSentOrigin{};

        bool isPeeking = false;
        bool shouldBreakLC = false;
        bool isActive = false;

        // Break LC tracking
        int breakLCCounter = 0;
        float lastBreakLCTime = 0.f;

        // Random seed for consistent randomness
        unsigned int randomSeed = 0;

        void reset() noexcept {
            chokedPackets = 0;
            targetChoke = 0;
            lastChoke = 0;
            lastOriginUpdateTime = 0.f;
            lastOrigin = Vector{};
            lastSentOrigin = Vector{};
            isPeeking = false;
            shouldBreakLC = false;
            isActive = false;
            breakLCCounter = 0;
            lastBreakLCTime = 0.f;
            randomSeed = 0;
        }
    };

    struct PeekData {
        Vector startPosition{};
        Vector peekDirection{};
        float peekStartTime = 0.f;
        bool isPeeking = false;
        bool wasVisible = false;
        int targetIndex = -1;
    };

    // ============================================
    // MAIN FUNCTIONS
    // ============================================

    void run(bool& sendPacket) noexcept;
    void update() noexcept;
    void reset() noexcept;

    // ============================================
    // MODE CALCULATIONS
    // ============================================

    int calculateStaticChoke() noexcept;
    int calculateAdaptiveChoke(float speed) noexcept;
    int calculateRandomChoke() noexcept;
    int calculateBreakLCChoke() noexcept;
    int calculatePeekChoke() noexcept;
    int calculateLegitChoke(float speed) noexcept;

    // ============================================
    // HELPERS
    // ============================================

    bool shouldChoke() noexcept;
    bool canChoke() noexcept;
    bool isBreakingLC() noexcept;
    float getDistanceMoved() noexcept;
    int getMaxChoke() noexcept;

    // ============================================
    // PEEK DETECTION
    // ============================================

    void updatePeekDetection() noexcept;
    bool detectPeek() noexcept;
    bool isCurrentlyPeeking() noexcept;

    // ============================================
    // GETTERS
    // ============================================

    int getChokedPackets() noexcept;
    int getTargetChoke() noexcept;
    const FakelagData& getData() noexcept;
    const PeekData& getPeekData() noexcept;

    // ============================================
    // GLOBAL DATA
    // ============================================

    inline FakelagData data{};
    inline PeekData peekData{};
}