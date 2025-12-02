#include "Animations.h"
#include "Resolver.h"
#include "Backtrack.h"
#include "EnginePrediction.h"
#include "AntiAim.h"
#include "../Memory.h"
#include "../Interfaces.h"
#include "../Helpers.h"
#include "../SDK/LocalPlayer.h"
#include "../SDK/NetworkChannel.h"
#include "../SDK/Cvar.h"
#include "../SDK/GlobalVars.h"
#include "../SDK/ConVar.h"
#include "../SDK/MemAlloc.h"
#include "../SDK/Input.h"
#include "../Config.h"
#include "../xor.h"
#include <algorithm>
#include <cmath>

// ============================================
// GLOBAL STATE (anonymous namespace)
// ============================================

namespace {
    std::array<Animations::Players, 65> players{};

    // Local player matrices
    std::array<matrix3x4, MAXSTUDIOBONES> fakematrix{};
    std::array<matrix3x4, MAXSTUDIOBONES> fakelagmatrix{};
    std::array<matrix3x4, MAXSTUDIOBONES> realmatrix{};

    // Local player state
    Vector localAngle{};
    Vector sentViewangles{};
    Vector viewangles{};
    Vector correctAngle{};

    // State flags
    bool updatingLocal = true;
    bool updatingEntity = false;
    bool updatingFake = false;
    bool sendPacket = true;
    bool gotMatrix = false;
    bool gotMatrixFakelag = false;
    bool gotMatrixReal = false;

    // Build transforms
    int buildTransformsIndex = -1;

    // Animation state backups
    std::array<AnimationLayer, ANIMATION_LAYER_COUNT> staticLayers{};
    std::array<AnimationLayer, ANIMATION_LAYER_COUNT> layers{};
    std::array<AnimationLayer, ANIMATION_LAYER_COUNT> sendPacketLayers{};
    std::array<float, 24> poseParameters{};

    float footYaw = 0.f;
    float primaryCycle = 0.f;
    float moveWeight = 0.f;

    // Fake animation state
    AnimState* fakeAnimState = nullptr;
    bool fakeAnimInitialized = false;
    float fakeSpawnTime = 0.f;

    // ============================================
    // HELPER STRUCTURES
    // ============================================

    struct GlobalVarsBackup {
        float currenttime;
        float frametime;
        float realtime;
        float absoluteFrameTime;
        int framecount;
        int tickCount;
        float interpolationAmount;

        void save() noexcept {
            currenttime = memory->globalVars->currenttime;
            frametime = memory->globalVars->frametime;
            realtime = memory->globalVars->realtime;
            absoluteFrameTime = memory->globalVars->absoluteFrameTime;
            framecount = memory->globalVars->framecount;
            tickCount = memory->globalVars->tickCount;
            interpolationAmount = memory->globalVars->interpolationAmount;
        }

        void restore() noexcept {
            memory->globalVars->currenttime = currenttime;
            memory->globalVars->frametime = frametime;
            memory->globalVars->realtime = realtime;
            memory->globalVars->absoluteFrameTime = absoluteFrameTime;
            memory->globalVars->framecount = framecount;
            memory->globalVars->tickCount = tickCount;
            memory->globalVars->interpolationAmount = interpolationAmount;
        }
    };

    struct EntityStateBackup {
        std::array<AnimationLayer, ANIMATION_LAYER_COUNT> layers{};
        std::array<float, 24> poseParams{};
        Vector absAngle{};
        float lby = 0.f;
        float duckAmount = 0.f;
        int flags = 0;
        uintptr_t effects = 0;

        void save(Entity* entity) noexcept {
            if (!entity) return;
            std::memcpy(layers.data(), entity->animOverlays(),
                sizeof(AnimationLayer) * ANIMATION_LAYER_COUNT);
            poseParams = entity->poseParameters();
            absAngle = entity->getAbsAngle();
            lby = entity->lby();
            duckAmount = entity->duckAmount();
            flags = entity->flags();
            effects = entity->getEffects();
        }

        void restore(Entity* entity) noexcept {
            if (!entity) return;
            std::memcpy(entity->animOverlays(), layers.data(),
                sizeof(AnimationLayer) * ANIMATION_LAYER_COUNT);
            entity->poseParameters() = poseParams;
            entity->lby() = lby;
            entity->duckAmount() = duckAmount;
            entity->flags() = flags;
            entity->getEffects() = effects;
        }
    };

    // ============================================
    // HELPER FUNCTIONS
    // ============================================

    float getMaxSpeed(Entity* entity) noexcept {
        if (!entity) return CS_PLAYER_SPEED_RUN;

        float maxSpeed = CS_PLAYER_SPEED_RUN;

        if (auto weapon = entity->getActiveWeapon()) {
            if (auto weaponData = weapon->getWeaponData()) {
                maxSpeed = entity->isScoped() ?
                    weaponData->maxSpeedAlt : weaponData->maxSpeed;
                maxSpeed = std::fmaxf(maxSpeed, 0.001f);
            }
        }

        if (entity->duckAmount() >= 1.0f)
            maxSpeed *= CS_PLAYER_SPEED_DUCK_MODIFIER;
        else if (entity->is_walking())
            maxSpeed *= CS_PLAYER_SPEED_WALK_MODIFIER;

        return maxSpeed;
    }

    float getExtraTicks() noexcept {
        if (!config->backtrack.fakeLatency || config->backtrack.fakeLatencyAmount <= 0)
            return 0.f;
        return static_cast<float>(config->backtrack.fakeLatencyAmount) / 1000.f;
    }

    void setGlobalVarsForTick(float time) noexcept {
        memory->globalVars->currenttime = time;
        memory->globalVars->realtime = time;
        memory->globalVars->frametime = memory->globalVars->intervalPerTick;
        memory->globalVars->absoluteFrameTime = memory->globalVars->intervalPerTick;
        memory->globalVars->framecount = timeToTicks(time);
        memory->globalVars->tickCount = timeToTicks(time);
        memory->globalVars->interpolationAmount = 0.0f;
    }

    void prepareEntityForUpdate(Entity* entity) noexcept {
        if (!entity || !entity->getAnimstate()) return;

        entity->getEFlags() &= ~0x1000;
        entity->getEffects() |= 8;

        if (entity->getAnimstate()->lastUpdateFrame == memory->globalVars->framecount)
            entity->getAnimstate()->lastUpdateFrame -= 1;

        if (entity->getAnimstate()->lastUpdateTime == memory->globalVars->currenttime)
            entity->getAnimstate()->lastUpdateTime += ticksToTime(1);
    }

    void applyAnimationBreakers() noexcept {
        if (!localPlayer || !localPlayer->getAnimstate())
            return;

        const bool slowwalk = config->misc.slowwalk && config->misc.slowwalkKey.isActive();

        // Static breaks
        if (config->condAA.animBreakers & (1 << 0)) {
            localPlayer->setPoseParameter(1, 6);
        }
        else {
            localPlayer->setPoseParameter(0, 6);
        }

        // Landing break
        if (config->condAA.animBreakers & (1 << 1)) {
            static float endTime = 0.f;
            if (localPlayer->getAnimstate()->landing) {
                endTime = memory->globalVars->currenttime + 3.5f;
            }

            if (endTime > memory->globalVars->currenttime) {
                localPlayer->setPoseParameter(2, 12);
            }
        }

        // Zero pitch
        if (config->condAA.animBreakers & (1 << 2)) {
            localPlayer->poseParameters()[8] = 0;
            localPlayer->poseParameters()[9] = 0;
            localPlayer->poseParameters()[10] = 0;
        }

        // Movement weight
        if ((config->condAA.animBreakers & (1 << 3)) && !slowwalk) {
            if (localPlayer->velocity().length2D() > 2.5f &&
                !(localPlayer->flags() & FL_ONGROUND)) {
                auto* moveLayer = localPlayer->getAnimationLayer(ANIMATION_LAYER_MOVEMENT_MOVE);
                if (moveLayer) moveLayer->weight = 1.f;
            }
        }
    }
}

// ============================================
// INITIALIZATION
// ============================================

void Animations::init() noexcept {
    static auto threadedBoneSetup = interfaces->cvar->findVar("cl_threaded_bone_setup");
    if (threadedBoneSetup) threadedBoneSetup->setValue(1);

    static auto extrapolate = interfaces->cvar->findVar("cl_extrapolate");
    if (extrapolate) extrapolate->setValue(0);
}

void Animations::reset() noexcept {
    for (auto& record : players)
        record.reset();

    for (auto& m : fakematrix) m = matrix3x4{};
    for (auto& m : fakelagmatrix) m = matrix3x4{};
    for (auto& m : realmatrix) m = matrix3x4{};

    localAngle = Vector{};
    sentViewangles = Vector{};
    viewangles = Vector{};
    correctAngle = Vector{};

    updatingLocal = true;
    updatingEntity = false;
    updatingFake = false;
    sendPacket = true;
    gotMatrix = false;
    gotMatrixFakelag = false;
    gotMatrixReal = false;

    buildTransformsIndex = -1;

    for (auto& l : staticLayers) l = AnimationLayer{};
    for (auto& l : layers) l = AnimationLayer{};
    for (auto& l : sendPacketLayers) l = AnimationLayer{};
    poseParameters.fill(0.f);

    footYaw = 0.f;
    primaryCycle = 0.f;
    moveWeight = 0.f;

    fakeAnimState = nullptr;
    fakeAnimInitialized = false;
    fakeSpawnTime = 0.f;
}

// ============================================
// VELOCITY CALCULATION
// ============================================

Vector Animations::calculateVelocity(Entity* entity, Players& record) noexcept {
    if (!entity)
        return Vector{};

    if (record.oldOrigin.notNull() && record.simulationTime > 0.f) {
        const float timeDelta = entity->simulationTime() - record.simulationTime;

        if (timeDelta > 0.0f && timeDelta < 1.0f) {
            return (entity->origin() - record.oldOrigin) * (1.0f / timeDelta);
        }
    }

    return entity->velocity();
}

void Animations::fixVelocity(Entity* entity, Players& record) noexcept {
    if (!entity)
        return;

    Vector velocity = record.velocity;

    if (entity->flags() & FL_ONGROUND) {
        velocity.z = 0.0f;

        const auto& aliveLoop = record.layers[ANIMATION_LAYER_ALIVELOOP];
        const auto& oldAliveLoop = record.oldlayers[ANIMATION_LAYER_ALIVELOOP];

        if (aliveLoop.weight > 0.0f && aliveLoop.weight < 1.0f) {
            if (aliveLoop.cycle > oldAliveLoop.cycle ||
                (aliveLoop.cycle < 0.1f && oldAliveLoop.cycle > 0.9f)) {

                const float maxSpeed = getMaxSpeed(entity);
                const float weightDelta = 1.0f - aliveLoop.weight;
                float speedFraction = (weightDelta / 2.8571432f) + 0.55f;

                speedFraction = std::clamp(speedFraction, 0.0f, 1.0f);

                if (speedFraction > 0.0f) {
                    const float targetSpeed = speedFraction * maxSpeed;
                    const float currentSpeed = velocity.length2D();

                    if (currentSpeed > 0.1f && targetSpeed > 0.1f) {
                        const float speedRatio = targetSpeed / currentSpeed;
                        velocity.x *= speedRatio;
                        velocity.y *= speedRatio;
                    }
                }
            }
        }

        const float maxSpeed = getMaxSpeed(entity);
        const float speed = velocity.length2D();
        if (speed > maxSpeed) {
            const float ratio = maxSpeed / speed;
            velocity.x *= ratio;
            velocity.y *= ratio;
        }
    }
    else {
        static auto gravity = interfaces->cvar->findVar("sv_gravity");
        if (gravity) {
            velocity.z -= gravity->getFloat() * 0.5f * memory->globalVars->intervalPerTick;
        }
    }

    record.velocity = velocity;
}

// ============================================
// ACTIVITY DETECTION
// ============================================

ActivityType Animations::detectActivity(Entity* entity, Players& record) noexcept {
    if (!entity)
        return ACTIVITY_NONE;

    const auto& jumpLayer = record.layers[ANIMATION_LAYER_MOVEMENT_JUMP_OR_FALL];
    const auto& oldJumpLayer = record.oldlayers[ANIMATION_LAYER_MOVEMENT_JUMP_OR_FALL];
    const auto& landLayer = record.layers[ANIMATION_LAYER_MOVEMENT_LAND_OR_CLIMB];

    if (jumpLayer.weight > 0.0f && oldJumpLayer.weight <= 0.0f) {
        if (jumpLayer.cycle < 0.5f) {
            return ACTIVITY_JUMP;
        }
    }

    if (entity->flags() & FL_ONGROUND) {
        if (landLayer.weight > 0.0f && landLayer.cycle < 0.5f) {
            if (landLayer.playbackRate > 0.5f) {
                return ACTIVITY_LAND_HEAVY;
            }
            return ACTIVITY_LAND_LIGHT;
        }
    }

    return ACTIVITY_NONE;
}

// ============================================
// LAYER COMPARISON
// ============================================

float Animations::compareLayerDelta(const AnimationLayer& a, const AnimationLayer& b) noexcept {
    float delta = 0.0f;

    delta += std::fabsf(a.weight - b.weight) * 2.0f;
    delta += std::fabsf(a.cycle - b.cycle);
    delta += std::fabsf(a.playbackRate - b.playbackRate) * 0.5f;

    return delta;
}

bool Animations::isLayerActive(const AnimationLayer& layer) noexcept {
    return layer.weight > 0.0f && layer.playbackRate != 0.0f;
}

// ============================================
// ENTITY SIMULATION
// ============================================

void Animations::simulateEntity(Entity* entity, Players& record, int ticks) noexcept {
    if (!entity || !entity->getAnimstate() || ticks <= 0)
        return;

    record.simulatedTicks.clear();
    record.simulatedTicks.reserve(ticks);

    GlobalVarsBackup gvBackup;
    gvBackup.save();

    EntityStateBackup entityBackup;
    entityBackup.save(entity);

    const float startTime = record.simulationTime;
    const float endTime = entity->simulationTime();
    const float timeDelta = endTime - startTime;

    if (timeDelta <= 0.0f)
        return;

    for (int tick = 0; tick < ticks; tick++) {
        const float lerpFactor = static_cast<float>(tick + 1) / static_cast<float>(ticks);
        const float simulatedTime = startTime + (timeDelta * lerpFactor);

        SimulatedTick simTick;
        simTick.time = simulatedTime;

        simTick.origin = Helpers::lerp(lerpFactor, record.oldOrigin, entity->origin());
        simTick.velocity = Helpers::lerp(lerpFactor, record.oldVelocity, record.velocity);
        simTick.duckAmount = Helpers::lerp(lerpFactor, record.oldDuckAmount, entity->duckAmount());

        if (record.activity == ACTIVITY_JUMP && tick == 0) {
            simTick.flags = entity->flags() | FL_ONGROUND;
        }
        else if (record.activity == ACTIVITY_JUMP && tick == 1) {
            simTick.flags = entity->flags() & ~FL_ONGROUND;
        }
        else {
            simTick.flags = entity->flags();
        }

        setGlobalVarsForTick(simulatedTime);
        entity->getAbsVelocity() = simTick.velocity;
        entity->duckAmount() = simTick.duckAmount;
        entity->flags() = simTick.flags;

        if (record.activity == ACTIVITY_JUMP && tick == 1) {
            auto* jumpLayer = entity->getAnimationLayer(ANIMATION_LAYER_MOVEMENT_JUMP_OR_FALL);
            if (jumpLayer) jumpLayer->cycle = 0.0f;
        }

        prepareEntityForUpdate(entity);
        entity->updateClientSideAnimation();

        std::memcpy(simTick.layers.data(), entity->animOverlays(),
            sizeof(AnimationLayer) * ANIMATION_LAYER_COUNT);
        simTick.poseParameters = entity->poseParameters();

        record.simulatedTicks.push_back(simTick);
    }

    gvBackup.restore();
    entityBackup.restore(entity);
}

void Animations::simulateSide(Entity* entity, Players& record, float yaw,
    std::array<AnimationLayer, ANIMATION_LAYER_COUNT>& outLayers,
    std::array<matrix3x4, MAXSTUDIOBONES>& outMatrix) noexcept {
    if (!entity || !entity->getAnimstate())
        return;

    GlobalVarsBackup gvBackup;
    gvBackup.save();

    EntityStateBackup entityBackup;
    entityBackup.save(entity);

    memory->globalVars->currenttime = entity->simulationTime();
    memory->globalVars->frametime = memory->globalVars->intervalPerTick;

    entity->getEFlags() &= ~0x1000;
    entity->getAbsVelocity() = record.velocity;

    auto* animState = entity->getAnimstate();
    animState->footYaw = yaw;

    prepareEntityForUpdate(entity);
    entity->updateClientSideAnimation();

    std::memcpy(outLayers.data(), entity->animOverlays(),
        sizeof(AnimationLayer) * ANIMATION_LAYER_COUNT);

    memory->setAbsAngle(entity, Vector{ 0, yaw, 0 });
    entity->setupBones(outMatrix.data(), MAXSTUDIOBONES, 0x7FF00, memory->globalVars->currenttime);

    gvBackup.restore();
    entityBackup.restore(entity);
}

void Animations::rebuildAnimationState(Entity* entity, Players& record) noexcept {
    if (!entity || !entity->getAnimstate())
        return;

    auto* animState = entity->getAnimstate();

    const float speed = record.velocity.length2D();
    const float maxSpeed = getMaxSpeed(entity);

    animState->velocityLengthXY = speed;
    animState->velocityLengthZ = record.velocity.z;

    if (maxSpeed > 0.0f) {
        animState->speedAsPortionOfWalkTopSpeed = speed / (maxSpeed * CS_PLAYER_SPEED_WALK_MODIFIER);
        animState->speedAsPortionOfCrouchTopSpeed = speed / (maxSpeed * CS_PLAYER_SPEED_DUCK_MODIFIER);
        animState->speedAsPortionOfRunTopSpeed = speed / maxSpeed;
    }

    animState->animDuckAmount = record.duckAmount;
    animState->onGround = (record.flags & FL_ONGROUND) != 0;
}

// ============================================
// MAIN PLAYER HANDLING
// ============================================

void Animations::handlePlayers(FrameStage stage) noexcept {
    if (stage != FrameStage::NET_UPDATE_END)
        return;

    if (!localPlayer) {
        for (auto& record : players)
            record.clear();
        return;
    }

    float latency = 0.0f;
    if (const auto networkChannel = interfaces->engine->getNetworkChannel()) {
        if (networkChannel->getLatency(0) > 0.0f && !config->backtrack.fakeLatency) {
            latency = networkChannel->getLatency(0);
        }
    }

    const float timeLimit = static_cast<float>(config->backtrack.timeLimit) / 1000.f +
        getExtraTicks() + latency;

    for (int i = 1; i <= interfaces->engine->getMaxClients(); i++) {
        const auto entity = interfaces->entityList->getEntity(i);
        auto& record = players.at(i);

        if (!entity || entity == localPlayer.get() || entity->isDormant() || !entity->isAlive()) {
            record.clear();
            continue;
        }

        if (entity->spawnTime() != record.spawnTime) {
            record.spawnTime = entity->spawnTime();
            record.reset();
        }

        EntityStateBackup entityBackup;
        entityBackup.save(entity);

        GlobalVarsBackup gvBackup;
        gvBackup.save();

        const float newSimTime = entity->simulationTime();
        const bool simTimeUpdated = record.simulationTime != newSimTime &&
            record.simulationTime < newSimTime;

        if (simTimeUpdated) {
            // Store old values
            record.oldSimulationTime = record.simulationTime;
            record.oldOrigin = record.origin;
            record.oldVelocity = record.velocity;
            record.oldDuckAmount = record.duckAmount;
            record.oldFlags = record.flags;
            record.oldLby = record.lby;
            std::memcpy(record.oldlayers.data(), record.layers.data(),
                sizeof(AnimationLayer) * ANIMATION_LAYER_COUNT);
            record.oldPoseParameters = record.poseParameters;
            record.lastChokedPackets = record.chokedPackets;
            record.lastSide = record.side;

            // Update current values
            record.simulationTime = newSimTime;
            record.origin = entity->origin();
            record.lby = entity->lby();
            record.duckAmount = entity->duckAmount();
            record.flags = entity->flags();
            std::memcpy(record.layers.data(), entity->animOverlays(),
                sizeof(AnimationLayer) * ANIMATION_LAYER_COUNT);
            record.poseParameters = entity->poseParameters();

            // Calculate choked packets
            const float simDelta = newSimTime - record.oldSimulationTime;
            if (simDelta > 0.0f && record.oldSimulationTime > 0.0f) {
                record.chokedPackets = static_cast<int>(
                    std::round(simDelta / memory->globalVars->intervalPerTick)) - 1;
                record.chokedPackets = std::clamp(record.chokedPackets, 0, maxUserCmdProcessTicks);
            }
            else {
                record.chokedPackets = 0;
            }

            // Calculate velocity
            record.velocity = calculateVelocity(entity, record);
            fixVelocity(entity, record);

            // Detect activity
            record.activity = detectActivity(entity, record);
            if (record.activity != ACTIVITY_NONE) {
                record.activityStartTime = memory->globalVars->currenttime;
            }

            // Setup for animation update
            memory->globalVars->currenttime = newSimTime;
            memory->globalVars->frametime = memory->globalVars->intervalPerTick;
            entity->getEffects() |= 8;

            // Run resolver pre-update
            Resolver::update(entity, record);

            // Simulate animation
            updatingEntity = true;

            if (record.chokedPackets <= 0) {
                entity->getAbsVelocity() = record.velocity;
                prepareEntityForUpdate(entity);
                entity->updateClientSideAnimation();
            }
            else {
                simulateEntity(entity, record, record.chokedPackets + 1);

                entity->getAbsVelocity() = record.velocity;
                entity->duckAmount() = record.duckAmount;
                prepareEntityForUpdate(entity);
                entity->updateClientSideAnimation();
            }

            updatingEntity = false;

            rebuildAnimationState(entity, record);

            // Simulate different sides for resolver
            const float eyeYaw = entity->eyeAngles().y;
            const float maxDesync = entity->getMaxDesyncAngle();

            simulateSide(entity, record, eyeYaw, record.centerLayer, record.centerMatrix);
            simulateSide(entity, record, eyeYaw + maxDesync, record.leftLayer, record.leftMatrix);
            simulateSide(entity, record, eyeYaw - maxDesync, record.rightLayer, record.rightMatrix);

            // Store resolver layers
            std::memcpy(record.resolver_layers[0].data(), record.leftLayer.data(),
                sizeof(AnimationLayer) * ANIMATION_LAYER_COUNT);
            std::memcpy(record.resolver_layers[1].data(), record.rightLayer.data(),
                sizeof(AnimationLayer) * ANIMATION_LAYER_COUNT);
            std::memcpy(record.resolver_layers[2].data(), record.centerLayer.data(),
                sizeof(AnimationLayer) * ANIMATION_LAYER_COUNT);

            // Run resolver post-update
            Resolver::update(entity, record);

            // Setup final bones
            record.absAngle = entity->getAbsAngle();
            record.mins = entity->getCollideable()->obbMins();
            record.maxs = entity->getCollideable()->obbMaxs();
            record.moveWeight = entity->getAnimstate() ? entity->getAnimstate()->moveWeight : 0.f;

            const auto& resolverData = Resolver::getData(i);
            float resolvedYaw = entity->getAnimstate() ? entity->getAnimstate()->footYaw : eyeYaw;

            memory->setAbsAngle(entity, Vector{ 0, resolvedYaw, 0 });
            record.gotMatrix = entity->setupBones(record.matrix.data(), MAXSTUDIOBONES, 0x7FF00,
                memory->globalVars->currenttime);
        }

        gvBackup.restore();
        entityBackup.restore(entity);

        // Handle backtrack records
        if (config->backtrack.enabled && entity->isOtherEnemy(localPlayer.get())) {
            if (simTimeUpdated && record.gotMatrix) {
                if (!record.backtrackRecords.empty() &&
                    record.backtrackRecords.front().simulationTime == record.simulationTime) {
                    continue;
                }

                Players::Record btRecord{};
                btRecord.origin = record.origin;
                btRecord.absAngle = record.absAngle;
                btRecord.simulationTime = record.simulationTime;
                btRecord.mins = record.mins;
                btRecord.maxs = record.maxs;
                btRecord.side = record.side;
                btRecord.footYaw = entity->getAnimstate() ? entity->getAnimstate()->footYaw : 0.f;

                std::copy(record.matrix.begin(), record.matrix.end(), btRecord.matrix);
                std::copy(record.layers.begin(), record.layers.end(), btRecord.layers.begin());

                for (auto bone : { 8, 4, 3, 7, 6, 5 }) {
                    btRecord.positions.push_back(btRecord.matrix[bone].origin());
                }

                record.backtrackRecords.push_front(btRecord);

                while (record.backtrackRecords.size() > 3 &&
                    record.backtrackRecords.size() > static_cast<size_t>(timeToTicks(timeLimit))) {
                    record.backtrackRecords.pop_back();
                }
            }
        }
        else {
            record.backtrackRecords.clear();
        }
    }
}

// ============================================
// LOCAL PLAYER UPDATE
// ============================================

void Animations::update(UserCmd* cmd, bool& _sendPacket) noexcept {
    static float spawnTime = 0.f;

    if (!localPlayer || !localPlayer->isAlive())
        return;

    if (interfaces->engine->isHLTV())
        return;

    if (spawnTime != localPlayer->spawnTime()) {
        spawnTime = localPlayer->spawnTime();

        for (int i = 0; i < ANIMATION_LAYER_COUNT; i++) {
            if (i == ANIMATION_LAYER_FLINCH || i == ANIMATION_LAYER_FLASHED ||
                i == ANIMATION_LAYER_WHOLE_BODY || i == ANIMATION_LAYER_WEAPON_ACTION ||
                i == ANIMATION_LAYER_WEAPON_ACTION_RECROUCH)
                continue;

            auto* layer = localPlayer->getAnimationLayer(i);
            if (layer) layer->reset();
        }
    }

    if (!localPlayer->getAnimstate())
        return;

    viewangles = cmd->viewangles;
    sendPacket = _sendPacket;
    localPlayer->getAnimstate()->buttons = cmd->buttons;

    if (sendPacket)
        sentViewangles = cmd->viewangles;

    updatingLocal = true;

    prepareEntityForUpdate(localPlayer.get());
    localPlayer->getAbsVelocity() = EnginePrediction::getVelocity();

    localPlayer->updateState(localPlayer->getAnimstate(), viewangles);
    localPlayer->updateClientSideAnimation();

    std::memcpy(layers.data(), localPlayer->animOverlays(),
        sizeof(AnimationLayer) * ANIMATION_LAYER_COUNT);

    if (sendPacket) {
        applyAnimationBreakers();

        std::memcpy(sendPacketLayers.data(), localPlayer->animOverlays(),
            sizeof(AnimationLayer) * ANIMATION_LAYER_COUNT);

        footYaw = localPlayer->getAnimstate()->footYaw;
        poseParameters = localPlayer->poseParameters();

        gotMatrixReal = localPlayer->setupBones(realmatrix.data(),
            localPlayer->getBoneCache().size,
            0x7FF00, memory->globalVars->currenttime);

        if (gotMatrixReal) {
            const auto origin = localPlayer->getRenderOrigin();
            for (auto& mat : realmatrix) {
                mat[0][3] -= origin.x;
                mat[1][3] -= origin.y;
                mat[2][3] -= origin.z;
            }
        }

        localAngle = cmd->viewangles;
    }

    updatingLocal = false;
}

// ============================================
// FAKE ANIMATION
// ============================================

void Animations::fake() noexcept {
    if (!localPlayer || !localPlayer->isAlive() || !localPlayer->getAnimstate())
        return;

    if (interfaces->engine->isHLTV())
        return;

    if (fakeSpawnTime != localPlayer->spawnTime()) {
        fakeSpawnTime = localPlayer->spawnTime();
        fakeAnimInitialized = false;
    }

    if (!fakeAnimInitialized) {
        fakeAnimState = static_cast<AnimState*>(memory->memalloc->Alloc(sizeof(AnimState)));
        if (fakeAnimState) {
            localPlayer->createState(fakeAnimState);
        }
        fakeAnimInitialized = true;
    }

    if (!fakeAnimState)
        return;

    if (!sendPacket)
        return;

    updatingFake = true;

    EntityStateBackup backup;
    backup.save(localPlayer.get());

    localPlayer->updateState(fakeAnimState, viewangles);

    const float yawDiff = std::fabsf(fakeAnimState->footYaw - footYaw);

    if (yawDiff <= 5.f) {
        gotMatrix = false;

        memory->setAbsAngle(localPlayer.get(), Vector{ 0, fakeAnimState->footYaw, 0 });
        backup.restore(localPlayer.get());

        auto* leanLayer = localPlayer->getAnimationLayer(ANIMATION_LAYER_LEAN);
        if (leanLayer) leanLayer->weight = std::numeric_limits<float>::epsilon();

        gotMatrixFakelag = localPlayer->setupBones(fakelagmatrix.data(),
            localPlayer->getBoneCache().size,
            0x7FF00, memory->globalVars->currenttime);

        backup.restore(localPlayer.get());
        updatingFake = false;
        return;
    }

    memory->setAbsAngle(localPlayer.get(), Vector{ 0, fakeAnimState->footYaw, 0 });
    backup.restore(localPlayer.get());

    auto* leanLayer = localPlayer->getAnimationLayer(ANIMATION_LAYER_LEAN);
    if (leanLayer) leanLayer->weight = std::numeric_limits<float>::epsilon();

    gotMatrix = localPlayer->setupBones(fakematrix.data(),
        localPlayer->getBoneCache().size,
        0x7FF00, memory->globalVars->currenttime);
    gotMatrixFakelag = gotMatrix;

    if (gotMatrix) {
        std::copy(fakematrix.begin(), fakematrix.end(), fakelagmatrix.data());

        const auto origin = localPlayer->getRenderOrigin();
        for (auto& mat : fakematrix) {
            mat[0][3] -= origin.x;
            mat[1][3] -= origin.y;
            mat[2][3] -= origin.z;
        }
    }

    backup.restore(localPlayer.get());
    updatingFake = false;
}

// ============================================
// FRAME HANDLERS
// ============================================

void Animations::renderStart(FrameStage stage) noexcept {
    if (stage != FrameStage::RENDER_START)
        return;

    if (!localPlayer || interfaces->engine->isHLTV())
        return;

    for (int i = 0; i < ANIMATION_LAYER_COUNT; i++) {
        if (i == ANIMATION_LAYER_FLINCH || i == ANIMATION_LAYER_FLASHED ||
            i == ANIMATION_LAYER_WHOLE_BODY || i == ANIMATION_LAYER_WEAPON_ACTION ||
            i == ANIMATION_LAYER_WEAPON_ACTION_RECROUCH)
            continue;

        auto* layer = localPlayer->getAnimationLayer(i);
        if (layer) *layer = layers[i];
    }
}

void Animations::packetStart() noexcept {
    if (!localPlayer || !localPlayer->animOverlays() || interfaces->engine->isHLTV())
        return;

    std::memcpy(staticLayers.data(), localPlayer->animOverlays(),
        sizeof(AnimationLayer) * ANIMATION_LAYER_COUNT);

    if (localPlayer->getAnimstate()) {
        primaryCycle = localPlayer->getAnimstate()->primaryCycle;
        moveWeight = localPlayer->getAnimstate()->moveWeight;
    }
}

void Animations::postDataUpdate() noexcept {
    if (!localPlayer || !localPlayer->animOverlays() || interfaces->engine->isHLTV())
        return;

    for (int i = 0; i < ANIMATION_LAYER_COUNT; i++) {
        if (i == ANIMATION_LAYER_FLINCH || i == ANIMATION_LAYER_FLASHED ||
            i == ANIMATION_LAYER_WHOLE_BODY || i == ANIMATION_LAYER_WEAPON_ACTION ||
            i == ANIMATION_LAYER_WEAPON_ACTION_RECROUCH)
            continue;

        auto* layer = localPlayer->getAnimationLayer(i);
        if (!layer) continue;

        const auto& staticLayer = staticLayers[i];

        if (layer->order != staticLayer.order) layer->order = staticLayer.order;
        if (layer->sequence != staticLayer.sequence) layer->sequence = staticLayer.sequence;
        if (layer->prevCycle != staticLayer.prevCycle) layer->prevCycle = staticLayer.prevCycle;
        if (layer->weight != staticLayer.weight) layer->weight = staticLayer.weight;
        if (layer->weightDeltaRate != staticLayer.weightDeltaRate)
            layer->weightDeltaRate = staticLayer.weightDeltaRate;
        if (layer->playbackRate != staticLayer.playbackRate)
            layer->playbackRate = staticLayer.playbackRate;
        if (layer->cycle != staticLayer.cycle) layer->cycle = staticLayer.cycle;
    }

    if (localPlayer->getAnimstate()) {
        localPlayer->getAnimstate()->primaryCycle = primaryCycle;
        localPlayer->getAnimstate()->moveWeight = moveWeight;
    }
}

// ============================================
// GETTERS
// ============================================

void Animations::saveCorrectAngle(int entityIndex, Vector correctAng) noexcept {
    buildTransformsIndex = entityIndex;
    correctAngle = correctAng;
}

int& Animations::buildTransformationsIndex() noexcept { return buildTransformsIndex; }
Vector* Animations::getCorrectAngle() noexcept { return &sentViewangles; }
Vector* Animations::getViewAngles() noexcept { return &viewangles; }
Vector* Animations::getLocalAngle() noexcept { return &localAngle; }

bool Animations::isLocalUpdating() noexcept { return updatingLocal; }
bool Animations::isEntityUpdating() noexcept { return updatingEntity; }
bool Animations::isFakeUpdating() noexcept { return updatingFake; }

bool Animations::gotFakeMatrix() noexcept { return gotMatrix; }
std::array<matrix3x4, MAXSTUDIOBONES> Animations::getFakeMatrix() noexcept { return fakematrix; }

bool Animations::gotFakelagMatrix() noexcept { return gotMatrixFakelag; }
std::array<matrix3x4, MAXSTUDIOBONES> Animations::getFakelagMatrix() noexcept { return fakelagmatrix; }

bool Animations::gotRealMatrix() noexcept { return gotMatrixReal; }
std::array<matrix3x4, MAXSTUDIOBONES> Animations::getRealMatrix() noexcept { return realmatrix; }

float Animations::getFootYaw() noexcept { return footYaw; }
std::array<float, 24> Animations::getPoseParameters() noexcept { return poseParameters; }
std::array<AnimationLayer, ANIMATION_LAYER_COUNT> Animations::getAnimLayers() noexcept {
    return sendPacketLayers;
}

Animations::Players Animations::getPlayer(int index) noexcept { return players.at(index); }
Animations::Players* Animations::setPlayer(int index) noexcept { return &players.at(index); }
std::array<Animations::Players, 65> Animations::getPlayers() noexcept { return players; }
std::array<Animations::Players, 65>* Animations::setPlayers() noexcept { return &players; }

const std::deque<Animations::Players::Record>* Animations::getBacktrackRecords(int index) noexcept {
    return &players.at(index).backtrackRecords;
}