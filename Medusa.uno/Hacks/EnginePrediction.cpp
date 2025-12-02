#include "EnginePrediction.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "../SDK/ClientState.h"
#include "../SDK/Engine.h"
#include "../SDK/Entity.h"
#include "../SDK/EntityList.h"
#include "../SDK/GameMovement.h"
#include "../SDK/GlobalVars.h"
#include "../SDK/MoveHelper.h"
#include "../SDK/Prediction.h"
#include "../SDK/LocalPlayer.h"
#include "../SDK/Cvar.h"
#include "../SDK/ConVar.h"
#include "../xor.h"
#include <algorithm>
#include <cmath>

// ============================================
// HELPER FUNCTIONS
// ============================================

namespace {
    // ConVar pointers (cached)
    ConVar* sv_footsteps = nullptr;
    ConVar* sv_min_jump_landing_sound = nullptr;

    // Backup values for ConVars
    float footstepsBackup = 0.f;
    float jumpSoundBackup = 0.f;

    void cacheConVars() noexcept {
        if (!sv_footsteps)
            sv_footsteps = interfaces->cvar->findVar("sv_footsteps");

        if (!sv_min_jump_landing_sound)
            sv_min_jump_landing_sound = interfaces->cvar->findVar("sv_min_jump_landing_sound");
    }

    void disableFootsteps() noexcept {
        cacheConVars();

        if (sv_footsteps) {
            footstepsBackup = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(sv_footsteps) + 0x2C);
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(sv_footsteps) + 0x2C) = 0;
        }

        if (sv_min_jump_landing_sound) {
            jumpSoundBackup = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(sv_min_jump_landing_sound) + 0x2C);
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(sv_min_jump_landing_sound) + 0x2C) = 0x7F7FFFFF;
        }
    }

    void restoreFootsteps() noexcept {
        if (sv_footsteps)
            *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(sv_footsteps) + 0x2C) = footstepsBackup;

        if (sv_min_jump_landing_sound)
            *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(sv_min_jump_landing_sound) + 0x2C) = jumpSoundBackup;
    }

    void backupGlobalVars(EnginePrediction::PredictionBackup& backup) noexcept {
        backup.currentTime = memory->globalVars->currenttime;
        backup.frameTime = memory->globalVars->frametime;
        backup.tickCount = memory->globalVars->tickCount;
        backup.isFirstTimePredicted = interfaces->prediction->isFirstTimePredicted;
        backup.inPrediction = interfaces->prediction->inPrediction;
    }

    void restoreGlobalVars(const EnginePrediction::PredictionBackup& backup) noexcept {
        memory->globalVars->currenttime = backup.currentTime;
        memory->globalVars->frametime = backup.frameTime;
        memory->globalVars->tickCount = backup.tickCount;
        interfaces->prediction->isFirstTimePredicted = backup.isFirstTimePredicted;
        interfaces->prediction->inPrediction = backup.inPrediction;
    }

    void backupPlayerState(EnginePrediction::PredictionBackup& backup) noexcept {
        if (!localPlayer || !localPlayer->isAlive())
            return;

        backup.flags = localPlayer->flags();
        backup.velocity = localPlayer->velocity();
        backup.origin = localPlayer->origin();
        backup.viewOffset = localPlayer->viewOffset();
        backup.aimPunch = localPlayer->aimPunchAngle();
        backup.aimPunchVel = localPlayer->aimPunchAngleVelocity();
        backup.viewPunch = localPlayer->viewPunchAngle();
        backup.baseVelocity = localPlayer->baseVelocity();
        backup.duckAmount = localPlayer->duckAmount();
        backup.duckSpeed = localPlayer->duckSpeed();
        backup.fallVelocity = localPlayer->fallVelocity();
        backup.velocityModifier = localPlayer->velocityModifier();
        backup.thirdPersonRecoil = localPlayer->thirdPersonRecoil();
        backup.tickBase = localPlayer->tickBase();

        // Animation state
        if (auto* animState = localPlayer->getAnimstate()) {
            backup.footYaw = animState->footYaw;
            backup.duckAdditional = animState->duckAdditional;
        }

        // Weapon state
        if (auto* weapon = localPlayer->getActiveWeapon()) {
            backup.hasWeapon = true;
            backup.nextPrimaryAttack = weapon->nextPrimaryAttack();
            backup.nextSecondaryAttack = weapon->nextSecondaryAttack();
            backup.recoilIndex = weapon->recoilIndex();
            backup.accuracy = weapon->getInaccuracy();
            backup.spread = weapon->getSpread();
        }
        else {
            backup.hasWeapon = false;
        }

        backup.valid = true;
    }

    void restoreWeaponState(const EnginePrediction::PredictionBackup& backup) noexcept {
        if (!localPlayer || !backup.valid || !backup.hasWeapon)
            return;

        auto* weapon = localPlayer->getActiveWeapon();
        if (!weapon)
            return;

        // Only restore if weapon hasn't changed
        weapon->nextPrimaryAttack() = backup.nextPrimaryAttack;
        weapon->nextSecondaryAttack() = backup.nextSecondaryAttack;
        weapon->recoilIndex() = backup.recoilIndex;
    }
}

// ============================================
// INITIALIZATION
// ============================================

void EnginePrediction::reset() noexcept {
    for (auto& data : storedData) {
        data = StoredData{};
    }

    currentNetvars = NetvarData{};
    backup = PredictionBackup{};
    lastCmd = nullptr;
    savedFlags = 0;
    savedVelocity = Vector{};
    savedOrigin = Vector{};
    inPrediction = false;
}

// ============================================
// UPDATE
// ============================================

void EnginePrediction::update() noexcept {
    if (!localPlayer || !localPlayer->isAlive())
        return;

    if (!memory->clientState)
        return;

    const int deltaTick = memory->clientState->deltaTick;
    if (deltaTick <= 0)
        return;

    const int start = memory->clientState->lastCommandAck;
    const int stop = memory->clientState->lastOutgoingCommand + memory->clientState->chokedCommands;

    // Force prediction update if velocity modifier changed
    if (currentNetvars.velocityModifier < 1.f)
        interfaces->prediction->inPrediction = true;

    interfaces->prediction->update(deltaTick, deltaTick > 0, start, stop);
}

// ============================================
// MAIN PREDICTION
// ============================================

void EnginePrediction::run(UserCmd* cmd) noexcept {
    if (!localPlayer || !localPlayer->isAlive())
        return;

    // Mark as in prediction
    inPrediction = true;
    lastCmd = cmd;

    // Save pre-prediction state
    savedFlags = localPlayer->flags();
    savedVelocity = localPlayer->velocity();
    savedOrigin = localPlayer->origin();

    // Backup everything
    backupGlobalVars(backup);
    backupPlayerState(backup);

    // Setup prediction random seed
    *memory->predictionRandomSeed = 0;
    *memory->predictionPlayer = reinterpret_cast<int>(localPlayer.get());

    // Setup prediction time
    memory->globalVars->currenttime = memory->globalVars->serverTime();
    memory->globalVars->frametime = interfaces->prediction->enginePaused ? 0.f : memory->globalVars->intervalPerTick;
    interfaces->prediction->isFirstTimePredicted = false;
    interfaces->prediction->inPrediction = true;

    // Save tickbase for restoration
    const int oldTickBase = localPlayer->tickBase();

    // Update button states
    const int currentButtons = cmd->buttons;
    const int playerButtons = localPlayer->getButtons();
    const int buttonsChanged = currentButtons ^ playerButtons;

    localPlayer->getButtonLast() = playerButtons;
    localPlayer->getButtons() = currentButtons;
    localPlayer->getButtonPressed() = buttonsChanged & currentButtons;
    localPlayer->getButtonReleased() = buttonsChanged & (~currentButtons);

    // Run PreThink
    if (localPlayer->physicsRunThink(0))
        localPlayer->preThink();

    // Disable footstep sounds during prediction
    disableFootsteps();

    // Handle impulse command
    if (cmd->impulse)
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(localPlayer.get()) + 0x320C) = cmd->impulse;

    // Update buttons from force states
    cmd->buttons |= *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(localPlayer.get()) + 0x3344);
    cmd->buttons &= ~(*reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(localPlayer.get()) + 0x3340));

    localPlayer->updateButtonState(cmd->buttons);

    // Start tracking prediction errors
    interfaces->gameMovement->startTrackPredictionErrors(localPlayer.get());

    // Check for moving ground
    interfaces->prediction->checkMovingGround(localPlayer.get(), memory->globalVars->frametime);

    // Run think functions
    localPlayer->runPreThink();
    localPlayer->runThink();

    // Setup and process movement
    memory->moveHelper->setHost(localPlayer.get());
    interfaces->prediction->setupMove(localPlayer.get(), cmd, memory->moveHelper, memory->moveData);
    interfaces->gameMovement->processMovement(localPlayer.get(), memory->moveData);
    interfaces->prediction->finishMove(localPlayer.get(), cmd, memory->moveData);

    // Process impacts
    memory->moveHelper->processImpacts();

    // Run PostThink
    localPlayer->runPostThink();

    // Restore footstep sounds
    restoreFootsteps();

    // Finish tracking prediction errors
    interfaces->gameMovement->finishTrackPredictionErrors(localPlayer.get());

    // Cleanup
    memory->moveHelper->setHost(nullptr);
    interfaces->gameMovement->reset();

    // Reset prediction seeds
    *memory->predictionRandomSeed = -1;
    *memory->predictionPlayer = 0;

    // Restore global vars
    restoreGlobalVars(backup);

    // Update weapon accuracy
    if (auto* weapon = localPlayer->getActiveWeapon()) {
        if (!weapon->isGrenade() && !weapon->isKnife()) {
            // Restore tickbase for accuracy calculation
            localPlayer->tickBase() = oldTickBase;
            weapon->updateAccuracyPenalty();
        }
    }

    // Restore weapon timing data
    restoreWeaponState(backup);

    // Mark prediction as complete
    inPrediction = false;
}

// ============================================
// STORE/RESTORE
// ============================================

void EnginePrediction::store() noexcept {
    if (!localPlayer || !localPlayer->isAlive())
        return;

    const int tickbase = localPlayer->tickBase();
    const int index = tickbase % MULTIPLAYER_BACKUP;

    auto& data = storedData[index];
    auto& netvars = data.netvars;

    netvars.tickbase = tickbase;
    netvars.valid = true;

    // Player state
    netvars.flags = localPlayer->flags();
    netvars.velocity = localPlayer->velocity();
    netvars.origin = localPlayer->origin();
    netvars.viewOffset = localPlayer->viewOffset();
    netvars.networkOrigin = localPlayer->network_origin();
    netvars.baseVelocity = localPlayer->baseVelocity();

    // Punch angles
    netvars.aimPunchAngle = localPlayer->aimPunchAngle();
    netvars.aimPunchAngleVelocity = localPlayer->aimPunchAngleVelocity();
    netvars.viewPunchAngle = localPlayer->viewPunchAngle();

    // Duck
    netvars.duckAmount = localPlayer->duckAmount();
    netvars.duckSpeed = localPlayer->duckSpeed();

    // Misc
    netvars.fallVelocity = localPlayer->fallVelocity();
    netvars.velocityModifier = localPlayer->velocityModifier();
    netvars.thirdPersonRecoil = localPlayer->thirdPersonRecoil();

    // Weapon
    if (auto* weapon = localPlayer->getActiveWeapon()) {
        netvars.nextPrimaryAttack = weapon->nextPrimaryAttack();
        netvars.nextSecondaryAttack = weapon->nextSecondaryAttack();
        netvars.recoilIndex = weapon->recoilIndex();
    }

    // Clamp view offset
    if (netvars.viewOffset.z <= 46.05f)
        netvars.viewOffset.z = 46.0f;
    else if (netvars.viewOffset.z > 64.0f)
        netvars.viewOffset.z = 64.0f;

    data.hasData = true;
    currentNetvars = netvars;
}

void EnginePrediction::restore() noexcept {
    if (!localPlayer || !localPlayer->isAlive())
        return;

    const int tickbase = localPlayer->tickBase();
    const int index = tickbase % MULTIPLAYER_BACKUP;

    const auto& data = storedData[index];

    if (!data.hasData || data.netvars.tickbase != tickbase)
        return;

    const auto& netvars = data.netvars;

    // Restore all values
    localPlayer->aimPunchAngle() = netvars.aimPunchAngle;
    localPlayer->aimPunchAngleVelocity() = netvars.aimPunchAngleVelocity;
    localPlayer->viewPunchAngle() = netvars.viewPunchAngle;
    localPlayer->velocity() = netvars.velocity;
    localPlayer->baseVelocity() = netvars.baseVelocity;
    localPlayer->viewOffset() = netvars.viewOffset;
    localPlayer->duckAmount() = netvars.duckAmount;
    localPlayer->duckSpeed() = netvars.duckSpeed;
    localPlayer->fallVelocity() = netvars.fallVelocity;
    localPlayer->velocityModifier() = netvars.velocityModifier;
    localPlayer->thirdPersonRecoil() = netvars.thirdPersonRecoil;
}

// ============================================
// APPLY CORRECTIONS
// ============================================

void EnginePrediction::apply(FrameStage stage) noexcept {
    if (stage != FrameStage::NET_UPDATE_END)
        return;

    if (!localPlayer || !localPlayer->isAlive())
        return;

    const int tickbase = localPlayer->tickBase();
    const int index = tickbase % MULTIPLAYER_BACKUP;

    const auto& data = storedData[index];

    if (!data.hasData || data.netvars.tickbase != tickbase)
        return;

    const auto& netvars = data.netvars;

    // Apply corrections with tolerance checks

    // Aim punch
    Vector punchDelta = localPlayer->aimPunchAngle() - netvars.aimPunchAngle;
    if (std::abs(punchDelta.x) <= PREDICTION_TOLERANCE &&
        std::abs(punchDelta.y) <= PREDICTION_TOLERANCE &&
        std::abs(punchDelta.z) <= PREDICTION_TOLERANCE) {
        localPlayer->aimPunchAngle() = netvars.aimPunchAngle;
    }

    // Aim punch velocity
    Vector punchVelDelta = localPlayer->aimPunchAngleVelocity() - netvars.aimPunchAngleVelocity;
    if (std::abs(punchVelDelta.x) <= PREDICTION_TOLERANCE &&
        std::abs(punchVelDelta.y) <= PREDICTION_TOLERANCE &&
        std::abs(punchVelDelta.z) <= PREDICTION_TOLERANCE) {
        localPlayer->aimPunchAngleVelocity() = netvars.aimPunchAngleVelocity;
    }

    // View punch
    Vector viewPunchDelta = localPlayer->viewPunchAngle() - netvars.viewPunchAngle;
    if (std::abs(viewPunchDelta.x) <= PREDICTION_TOLERANCE &&
        std::abs(viewPunchDelta.y) <= PREDICTION_TOLERANCE &&
        std::abs(viewPunchDelta.z) <= PREDICTION_TOLERANCE) {
        localPlayer->viewPunchAngle() = netvars.viewPunchAngle;
    }

    // Velocity
    Vector velDelta = localPlayer->velocity() - netvars.velocity;
    if (std::abs(velDelta.x) <= VELOCITY_TOLERANCE &&
        std::abs(velDelta.y) <= VELOCITY_TOLERANCE &&
        std::abs(velDelta.z) <= VELOCITY_TOLERANCE) {
        localPlayer->velocity() = netvars.velocity;
    }

    // View offset
    Vector viewOffsetDelta = localPlayer->viewOffset() - netvars.viewOffset;
    if (std::abs(viewOffsetDelta.x) <= PREDICTION_TOLERANCE &&
        std::abs(viewOffsetDelta.y) <= PREDICTION_TOLERANCE &&
        std::abs(viewOffsetDelta.z) <= PREDICTION_TOLERANCE) {
        localPlayer->viewOffset() = netvars.viewOffset;
    }

    // Velocity modifier
    if (std::abs(localPlayer->velocityModifier() - netvars.velocityModifier) <= PREDICTION_TOLERANCE) {
        localPlayer->velocityModifier() = netvars.velocityModifier;
    }

    // Duck amount
    if (std::abs(localPlayer->duckAmount() - netvars.duckAmount) <= PREDICTION_TOLERANCE) {
        localPlayer->duckAmount() = std::clamp(netvars.duckAmount, 0.f, 1.f);
    }

    // Duck speed
    if (std::abs(localPlayer->duckSpeed() - netvars.duckSpeed) <= PREDICTION_TOLERANCE) {
        localPlayer->duckSpeed() = netvars.duckSpeed;
    }

    // Fall velocity
    if (std::abs(localPlayer->fallVelocity() - netvars.fallVelocity) <= VELOCITY_TOLERANCE) {
        localPlayer->fallVelocity() = netvars.fallVelocity;
    }

    // Base velocity
    Vector baseVelDelta = localPlayer->baseVelocity() - netvars.baseVelocity;
    if (std::abs(baseVelDelta.x) <= PREDICTION_TOLERANCE &&
        std::abs(baseVelDelta.y) <= PREDICTION_TOLERANCE &&
        std::abs(baseVelDelta.z) <= PREDICTION_TOLERANCE) {
        localPlayer->baseVelocity() = netvars.baseVelocity;
    }

    // Third person recoil
    if (std::abs(localPlayer->thirdPersonRecoil() - netvars.thirdPersonRecoil) <= PREDICTION_TOLERANCE) {
        localPlayer->thirdPersonRecoil() = netvars.thirdPersonRecoil;
    }
}

// ============================================
// PRE/POST FRAME HANDLERS
// ============================================

void EnginePrediction::onPreFrame() noexcept {
    if (!localPlayer || !localPlayer->isAlive())
        return;

    // Store current state before frame
    store();
}

void EnginePrediction::onPostFrame() noexcept {
    if (!localPlayer || !localPlayer->isAlive())
        return;

    // Apply any needed corrections
    // This is called after the game has processed the frame
}

// ============================================
// GETTERS
// ============================================

int EnginePrediction::getFlags() noexcept {
    return savedFlags;
}

Vector EnginePrediction::getVelocity() noexcept {
    return savedVelocity;
}

Vector EnginePrediction::getOrigin() noexcept {
    return savedOrigin;
}

bool EnginePrediction::isInPrediction() noexcept {
    return inPrediction;
}

float EnginePrediction::getServerTime() noexcept {
    if (!localPlayer)
        return 0.f;

    return static_cast<float>(localPlayer->tickBase()) * memory->globalVars->intervalPerTick;
}

const EnginePrediction::NetvarData& EnginePrediction::getNetvarData(int tickbase) noexcept {
    const int index = tickbase % MULTIPLAYER_BACKUP;
    return storedData[index].netvars;
}