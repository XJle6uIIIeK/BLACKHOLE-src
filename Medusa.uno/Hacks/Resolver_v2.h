#pragma once
#include "../SDK/FrameStage.h"

namespace ResolverV2 {
    
    // Main processing function - call in FrameStageNotify
    void processPlayers(FrameStage stage) noexcept;
    
    // Event callbacks
    void onPlayerHurt(int attackerId, int victimId, int hitgroup) noexcept;
    void onPlayerMiss(int entityIndex) noexcept;
    
    // Lifecycle
    void reset() noexcept;
}
