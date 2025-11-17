

#include "../Memory.h"
#include "../Interfaces.h"
#include <intrin.h>
#include "Animations.h"
#include "Backtrack.h"
#include "EnginePrediction.h"
#include "Resolver.h"
#include "AntiAim.h"
#include "../SDK/LocalPlayer.h"
#include "../SDK/Cvar.h"
#include "../SDK/GlobalVars.h"
#include "../SDK/Entity.h"
#include "../SDK/UserCmd.h"
#include "../SDK/ConVar.h"
#include "../SDK/MemAlloc.h"
#include "../SDK/Input.h"
#include "../SDK/Vector.h"
#include "../Hooks.h"
#include "../xor.h"

// Глобальные переменные
static bool lockAngles = false;
static Vector holdAimAngles;
static Vector anglesToAnimate;
static std::array<Animations::Players, 65> players{};
static std::array<matrix3x4, MAXSTUDIOBONES> fakematrix{};
static std::array<matrix3x4, MAXSTUDIOBONES> fakelagmatrix{};
static std::array<matrix3x4, MAXSTUDIOBONES> realmatrix{};
static Vector localAngle{};
static Vector sentViewangles{};
static bool updatingLocal{ true };
static bool updatingEntity{ false };
static bool updatingFake{ false };
static bool sendPacket{ true };
static bool gotMatrix{ false };
static bool gotMatrixFakelag{ false };
static bool gotMatrixReal{ false };
static Vector viewangles{};
static Vector correctAngle{};
static int buildTransformsIndex = -1;
static std::array<AnimationLayer, 13> staticLayers{};
static std::array<AnimationLayer, 13> layers{};
static float primaryCycle{ 0.0f };
static float moveWeight{ 0.0f };
static float footYaw{};
static std::array<float, 24> poseParameters{};
static std::array<AnimationLayer, 13> sendPacketLayers{};

enum ADVANCED_ACTIVITY : int
{
    ACTIVITY_NONE = 0,
    ACTIVITY_JUMP,
    ACTIVITY_LAND,
    ACTIVE_LAND_LIGHT,
    ACTIVE_LAND_HEAVY
};

void Animations::init() noexcept
{
    static auto threadedBoneSetup = interfaces->cvar->findVar("cl_threaded_bone_setup");
    threadedBoneSetup->setValue(1);

    static auto extrapolate = interfaces->cvar->findVar("cl_extrapolate");
    extrapolate->setValue(0);
}

void Animations::reset() noexcept
{
    for (auto& record : players)
        record.reset();
    fakematrix = {};
    fakelagmatrix = {};
    realmatrix = {};
    localAngle = Vector{};
    updatingLocal = true;
    updatingEntity = false;
    sendPacket = true;
    gotMatrix = false;
    gotMatrixFakelag = false;
    gotMatrixReal = false;
    viewangles = Vector{};
    correctAngle = Vector{};
    buildTransformsIndex = -1;
    staticLayers = {};
    layers = {};
    primaryCycle = 0.0f;
    moveWeight = 0.0f;
    footYaw = {};
    poseParameters = {};
    sendPacketLayers = {};
}

// УЛУЧШЕННАЯ ФУНКЦИЯ: правильное обновление velocity
static Vector calculateVelocity(const Vector& origin, const Vector& prevOrigin, float simTime, float prevSimTime) noexcept
{
    if (prevOrigin.notNull() && simTime != prevSimTime)
    {
        const float timeDelta = simTime - prevSimTime;
        if (timeDelta > 0.0f && timeDelta < 1.0f) // Санитарная проверка
        {
            return (origin - prevOrigin) * (1.0f / timeDelta);
        }
    }
    return Vector{ 0.0f, 0.0f, 0.0f };
}

// НОВАЯ ФУНКЦИЯ: улучшенный расчет ground velocity
static void fixVelocity(Animations::Players& record, Entity* entity) noexcept
{
    if (!entity)
        return;

    Vector velocity = record.velocity;

    // Ground velocity fix
    if (entity->flags() & FL_ONGROUND)
    {
        velocity.z = 0.0f;

        // Правильный расчет скорости на основе animation layer
        if (record.layers[ANIMATION_LAYER_ALIVELOOP].weight > 0.0f &&
            record.layers[ANIMATION_LAYER_ALIVELOOP].weight < 1.0f &&
            record.layers[ANIMATION_LAYER_ALIVELOOP].cycle > record.oldlayers[ANIMATION_LAYER_ALIVELOOP].cycle)
        {
            const auto weapon = entity->getActiveWeapon();
            const float maxSpeed = weapon ? std::fmaxf(weapon->getMaxSpeed(), 0.001f) : CS_PLAYER_SPEED_RUN;

            const float weightDelta = 1.0f - record.layers[ANIMATION_LAYER_ALIVELOOP].weight;
            const float speedFraction = (weightDelta / 2.8571432f) + 0.55f;

            if (speedFraction > 0.0f)
            {
                const float speed = speedFraction * maxSpeed;
                const float currentSpeed = velocity.length2D();

                if (speed > 0.0f && currentSpeed > 0.0f)
                {
                    velocity.x = (velocity.x / currentSpeed) * speed;
                    velocity.y = (velocity.y / currentSpeed) * speed;
                }
            }
        }
    }
    else
    {
        // Gravity для воздуха
        static auto gravity = interfaces->cvar->findVar(skCrypt("sv_gravity"));
        if (gravity)
        {
            velocity.z -= gravity->getFloat() * 0.5f * memory->globalVars->intervalPerTick;
        }
    }

    record.velocity = velocity;
}

// УЛУЧШЕННАЯ ФУНКЦИЯ: определение активности (jump/land)
static int detectActivity(Animations::Players& record, Animations::Players& prevRecord, Entity* entity) noexcept
{
    if (!entity)
        return ACTIVITY_NONE;

    // Detect landing
    if ((record.layers[ANIMATION_LAYER_MOVEMENT_MOVE].playbackRate <= 0.f ||
        record.layers[ANIMATION_LAYER_MOVEMENT_MOVE].weight <= 0.f) &&
        (entity->flags() & FL_ONGROUND))
    {
        int activity = entity->sequenceDuration(record.layers[ANIMATION_LAYER_MOVEMENT_LAND_OR_CLIMB].sequence);

        if (activity == ACTIVE_LAND_LIGHT || activity == ACTIVE_LAND_HEAVY)
        {
            float landTime = record.layers[ANIMATION_LAYER_MOVEMENT_LAND_OR_CLIMB].cycle /
                record.layers[ANIMATION_LAYER_MOVEMENT_LAND_OR_CLIMB].playbackRate;

            if (landTime > 0.f)
                return ACTIVITY_LAND;
        }
    }

    // Detect jump
    if (record.layers[ANIMATION_LAYER_MOVEMENT_JUMP_OR_FALL].weight > 0.0f &&
        prevRecord.layers[ANIMATION_LAYER_MOVEMENT_JUMP_OR_FALL].weight <= 0.0f)
    {
        int activity = entity->sequenceDuration(record.layers[ANIMATION_LAYER_MOVEMENT_JUMP_OR_FALL].sequence);

        if (activity == ACTIVITY_JUMP)
        {
            float jumpTime = record.layers[ANIMATION_LAYER_MOVEMENT_JUMP_OR_FALL].cycle /
                record.layers[ANIMATION_LAYER_MOVEMENT_JUMP_OR_FALL].playbackRate;

            if (jumpTime > 0.f)
                return ACTIVITY_JUMP;
        }
    }

    return ACTIVITY_NONE;
}

float getExtraTicks() noexcept
{
    if (!config->backtrack.fakeLatency || config->backtrack.fakeLatencyAmount <= 0)
        return 0.f;
    return static_cast<float>(config->backtrack.fakeLatencyAmount) / 1000.f;
}

void Animations::handlePlayers(FrameStage stage) noexcept
{
    if (stage != FrameStage::NET_UPDATE_END)
        return;

    if (!localPlayer)
    {
        for (auto& record : players)
            record.clear();
        return;
    }

    float latency = 0.0f;
    if (auto networkChannel = interfaces->engine->getNetworkChannel();
        networkChannel && networkChannel->getLatency(0) > 0.0f && !config->backtrack.fakeLatency)
    {
        latency = networkChannel->getLatency(0);
    }

    static auto gravity = interfaces->cvar->findVar(skCrypt("sv_gravity"));
    const float timeLimit = static_cast<float>(config->backtrack.timeLimit) / 1000.f + getExtraTicks() + latency;

    for (int i = 1; i <= interfaces->engine->getMaxClients(); i++)
    {
        const auto entity = interfaces->entityList->getEntity(i);
        auto& records = players.at(i);
        auto& prev_records = players.at(i);

        if (!entity || entity == localPlayer.get() || entity->isDormant() || !entity->isAlive())
        {
            records.clear();
            prev_records.clear();
            continue;
        }

        if (entity->spawnTime() != records.spawnTime)
        {
            records.spawnTime = entity->spawnTime();
            records.reset();
        }

        // Backup layers
        std::array<AnimationLayer, 13> layersBackup;
        std::memcpy(&layersBackup, entity->animOverlays(), sizeof(AnimationLayer) * entity->getAnimationLayersCount());
        std::memcpy(&records.centerLayer, entity->animOverlays(), sizeof(AnimationLayer) * entity->getAnimationLayersCount());
        std::memcpy(&records.leftLayer, entity->animOverlays(), sizeof(AnimationLayer) * entity->getAnimationLayersCount());
        std::memcpy(&records.rightLayer, entity->animOverlays(), sizeof(AnimationLayer) * entity->getAnimationLayersCount());

        const auto lowerBodyYawTarget = entity->lby();
        const auto duckAmount = entity->duckAmount();
        const auto flags = entity->flags();

        const auto currentTime = memory->globalVars->currenttime;
        const auto frameTime = memory->globalVars->frametime;
        const auto realTime = memory->globalVars->realtime;
        const auto frameCount = memory->globalVars->framecount;
        const auto absoluteFT = memory->globalVars->absoluteFrameTime;
        const auto tickCount = memory->globalVars->tickCount;
        const auto interpolationAmount = memory->globalVars->interpolationAmount;

        memory->globalVars->currenttime = entity->simulationTime();
        memory->globalVars->frametime = memory->globalVars->intervalPerTick;

        const uintptr_t backupEffects = entity->getEffects();
        entity->getEffects() |= 8;

        bool runPostUpdate = false;

        if (records.simulationTime != entity->simulationTime() && records.simulationTime < entity->simulationTime())
        {
            runPostUpdate = true;

            if (records.simulationTime == -1.0f)
                records.simulationTime = entity->simulationTime();

            // Backup old layers
            if (!records.layers.empty())
                std::memcpy(&records.oldlayers, &records.layers, sizeof(AnimationLayer) * entity->getAnimationLayersCount());

            std::memcpy(&records.layers, entity->animOverlays(), sizeof(AnimationLayer) * entity->getAnimationLayersCount());

            // УЛУЧШЕНИЕ: правильный расчет choked packets
            const float simDifference = entity->simulationTime() - records.simulationTime;
            records.chokedPackets = simDifference > 0.0f ?
                static_cast<int>(std::round(simDifference / memory->globalVars->intervalPerTick)) - 1 : 0;
            records.chokedPackets = std::clamp(records.chokedPackets, 0, maxUserCmdProcessTicks);

            // УЛУЧШЕНИЕ: правильный расчет velocity
            records.velocity = calculateVelocity(
                entity->origin(),
                records.origin,
                entity->simulationTime(),
                records.simulationTime
            );

            // Fix velocity
            fixVelocity(records, entity);

            // Update other data
            records.moveWeight = entity->getAnimstate()->moveWeight;
            records.flags = entity->flags();
            records.oldDuckAmount = records.duckAmount;
            records.duckAmount = entity->duckAmount();
            records.oldOrigin = records.origin;
            records.origin = entity->origin();

            // Detect activity
            int activity = detectActivity(records, prev_records, entity);

            // Run resolver pre-update
            Resolver::runPreUpdate(records, prev_records, entity);

            // УЛУЧШЕНИЕ: правильная симуляция missing ticks
            updatingEntity = true;

            if (records.chokedPackets <= 0)
            {
                // No choke - simple update
                if (entity->getAnimstate()->lastUpdateFrame == memory->globalVars->framecount)
                    entity->getAnimstate()->lastUpdateFrame -= 1;

                if (entity->getAnimstate()->lastUpdateTime == memory->globalVars->currenttime)
                    entity->getAnimstate()->lastUpdateTime += ticksToTime(1);

                entity->getEFlags() &= ~0x1000;
                entity->getAbsVelocity() = records.velocity;
                entity->updateClientSideAnimation();
            }
            else
            {
                // КРИТИЧЕСКОЕ УЛУЧШЕНИЕ: правильная симуляция каждого choked tick
                for (int tick = 0; tick <= records.chokedPackets; tick++)
                {
                    const float simulatedTime = records.simulationTime + (memory->globalVars->intervalPerTick * (tick + 1));
                    const float lerpValue = static_cast<float>(tick + 1) / static_cast<float>(records.chokedPackets + 1);

                    // Backup current time
                    const float currentTimeBackup = memory->globalVars->currenttime;

                    // Setup simulated time
                    memory->globalVars->currenttime = simulatedTime;
                    memory->globalVars->realtime = simulatedTime;
                    memory->globalVars->frametime = memory->globalVars->intervalPerTick;
                    memory->globalVars->absoluteFrameTime = memory->globalVars->intervalPerTick;
                    memory->globalVars->framecount = timeToTicks(simulatedTime);
                    memory->globalVars->tickCount = timeToTicks(simulatedTime);
                    memory->globalVars->interpolationAmount = 0.0f;

                    // Handle activity (jump/land)
                    if (activity == ACTIVITY_JUMP || activity == ACTIVITY_LAND)
                    {
                        // Set proper flags for jump/land
                        if (tick == 0 && activity == ACTIVITY_JUMP)
                        {
                            entity->flags() |= FL_ONGROUND;
                        }
                        else if (tick == 1 && activity == ACTIVITY_JUMP)
                        {
                            entity->flags() &= ~FL_ONGROUND;
                            entity->getAnimationLayer(ANIMATION_LAYER_MOVEMENT_JUMP_OR_FALL)->cycle = 0.f;
                        }
                    }

                    // Interpolate values
                    entity->getEFlags() &= ~0x1000;
                    entity->getAbsVelocity() = Helpers::lerp(lerpValue, records.oldVelocity, records.velocity);
                    entity->duckAmount() = Helpers::lerp(lerpValue, records.oldDuckAmount, records.duckAmount);

                    // Update animation state
                    if (entity->getAnimstate()->lastUpdateFrame == memory->globalVars->framecount)
                        entity->getAnimstate()->lastUpdateFrame -= 1;

                    if (entity->getAnimstate()->lastUpdateTime == memory->globalVars->currenttime)
                        entity->getAnimstate()->lastUpdateTime += ticksToTime(1);

                    entity->updateClientSideAnimation();

                    // Restore time
                    memory->globalVars->currenttime = currentTimeBackup;
                }

                // Final update with actual values
                entity->getEFlags() &= ~0x1000;
                entity->getAbsVelocity() = records.velocity;
                entity->duckAmount() = records.duckAmount;
                entity->updateClientSideAnimation();
            }

            updatingEntity = false;

            // Run resolver post-update
            Resolver::runPostUpdate(records, prev_records, entity);

            // УЛУЧШЕНИЕ: правильный расчет animated speed
            float max_speed = CS_PLAYER_SPEED_RUN;
            if (Entity* weapon = entity->getActiveWeapon(); weapon && weapon->getWeaponData())
                max_speed = std::fmaxf(entity->isScoped() ? weapon->getWeaponData()->maxSpeedAlt : weapon->getWeaponData()->maxSpeed, 0.001f);

            if (entity->is_walking())
                max_speed *= CS_PLAYER_SPEED_WALK_MODIFIER;

            if (entity->duckAmount() >= 1.f)
                max_speed *= CS_PLAYER_SPEED_DUCK_MODIFIER;

            // Apply animated speed correction
            if (entity->flags() & FL_ONGROUND)
            {
                auto& layer_aliveloop = records.layers[ANIMATION_LAYER_ALIVELOOP];
                float base_value = 1.f - layer_aliveloop.weight;
                float fake_speed_portion = base_value / 2.85f;

                if (fake_speed_portion > 0.f)
                    fake_speed_portion += 0.55f;

                float anim_velocity_length = (std::min)(records.velocity.length(), CS_PLAYER_SPEED_RUN);

                if (fake_speed_portion > 0.f && anim_velocity_length > 0.f)
                    records.velocity = records.velocity * ((fake_speed_portion * max_speed) / anim_velocity_length);
            }
        }

        // Setup bones
        if (runPostUpdate)
        {
            records.simulationTime = entity->simulationTime();
            records.mins = entity->getCollideable()->obbMins();
            records.maxs = entity->getCollideable()->obbMaxs();

            // Setup matrices для всех поз
            std::memcpy(entity->animOverlays(), &records.centerLayer, sizeof(AnimationLayer) * entity->getAnimationLayersCount());
            entity->updateClientSideAnimation();

            std::memcpy(entity->animOverlays(), &records.leftLayer, sizeof(AnimationLayer) * entity->getAnimationLayersCount());
            entity->updateClientSideAnimation();

            std::memcpy(entity->animOverlays(), &records.rightLayer, sizeof(AnimationLayer) * entity->getAnimationLayersCount());
            entity->updateClientSideAnimation();

            records.gotMatrix = entity->setupBones(records.matrix.data(), entity->getBoneCache().size, 0x7FF00, memory->globalVars->currenttime);
        }

        // Restore everything
        memory->globalVars->frametime = frameTime;
        memory->globalVars->currenttime = currentTime;
        memory->globalVars->realtime = realTime;
        memory->globalVars->absoluteFrameTime = absoluteFT;
        memory->globalVars->framecount = frameCount;
        memory->globalVars->tickCount = tickCount;
        memory->globalVars->interpolationAmount = interpolationAmount;

        entity->getEffects() = backupEffects;
        entity->lby() = lowerBodyYawTarget;
        entity->duckAmount() = duckAmount;
        entity->flags() = flags;

        std::memcpy(entity->animOverlays(), &layersBackup, sizeof(AnimationLayer) * entity->getAnimationLayersCount());

        // Backtrack records
        if (config->backtrack.enabled && entity->isOtherEnemy(localPlayer.get()))
        {
            if (runPostUpdate)
            {
                if (!records.backtrackRecords.empty() && records.backtrackRecords.front().simulationTime == entity->simulationTime())
                    continue;

                Players::Record record{};
                record.origin = records.origin;
                record.absAngle = records.absAngle;
                record.simulationTime = records.simulationTime;
                record.mins = records.mins;
                record.maxs = records.maxs;
                std::copy(records.matrix.begin(), records.matrix.end(), record.matrix);

                for (auto bone : { 8, 4, 3, 7, 6, 5 })
                {
                    record.positions.push_back(record.matrix[bone].origin());
                }

                records.backtrackRecords.push_front(record);

                while (records.backtrackRecords.size() > 3 &&
                    records.backtrackRecords.size() > static_cast<size_t>(timeToTicks(timeLimit)))
                {
                    records.backtrackRecords.pop_back();
                }
            }
        }
        else
        {
            records.backtrackRecords.clear();
        }
    }
}

// Остальные функции остаются без изменений но добавлю их для полноты

void Animations::update(UserCmd* cmd, bool& _sendPacket) noexcept
{
    static float spawnTime = 0.f;

    if (!localPlayer || !localPlayer->isAlive())
        return;

    if (interfaces->engine->isHLTV())
        return;

    if (spawnTime != localPlayer->spawnTime())
    {
        spawnTime = localPlayer->spawnTime();

        for (int i = 0; i < 13; i++)
        {
            if (i == ANIMATION_LAYER_FLINCH || i == ANIMATION_LAYER_FLASHED ||
                i == ANIMATION_LAYER_WHOLE_BODY || i == ANIMATION_LAYER_WEAPON_ACTION ||
                i == ANIMATION_LAYER_WEAPON_ACTION_RECROUCH)
                continue;

            auto& animLayers = *localPlayer->getAnimationLayer(i);
            if (!&animLayers)
                continue;

            animLayers.reset();
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

    if (localPlayer->getAnimstate()->lastUpdateFrame == memory->globalVars->framecount)
        localPlayer->getAnimstate()->lastUpdateFrame -= 1;

    if (localPlayer->getAnimstate()->lastUpdateTime == memory->globalVars->currenttime)
        localPlayer->getAnimstate()->lastUpdateTime += ticksToTime(1);

    localPlayer->getEFlags() &= ~0x1000;
    localPlayer->getAbsVelocity() = EnginePrediction::getVelocity();

    localPlayer->updateState(localPlayer->getAnimstate(), viewangles);
    localPlayer->updateClientSideAnimation();

    // Animation breakers
    if (config->misc.moonwalk_style == 2)
    {
        static auto alpha = 1.0f;
        static auto switch_alpha = false;

        if (alpha <= 0.0f || alpha >= 1.0f)
            switch_alpha = !switch_alpha;

        alpha += switch_alpha ? 1.75f * memory->globalVars->frametime : -1.75f * memory->globalVars->frametime;
        alpha = std::clamp(alpha, 0.0f, 1.0f);

        if (localPlayer->flags() & FL_ONGROUND)
        {
            if (switch_alpha)
            {
                localPlayer->setPoseParameter(-1, 0);
                localPlayer->setPoseParameter(1, 1);
            }
            else
            {
                localPlayer->setPoseParameter(1, 0);
                localPlayer->setPoseParameter(-1, 1);
            }
        }
    }

    std::memcpy(&layers, localPlayer->animOverlays(), sizeof(AnimationLayer) * localPlayer->getAnimationLayersCount());

    if (sendPacket)
    {
        bool slowwalk = config->misc.slowwalk && config->misc.slowwalkKey.isActive();

        if (config->misc.moonwalk_style == 4)
            localPlayer->setPoseParameter(0, 7);

        localPlayer->poseParameters().data()[2] = std::rand() % 4 * static_cast<float>(config->condAA.moveBreakers);

        // Animation breakers
        if ((config->condAA.animBreakers & (1 << 0)))
            localPlayer->setPoseParameter(1, 6);
        else
            localPlayer->setPoseParameter(0, 6);

        if ((config->condAA.animBreakers & (1 << 1)))
        {
            static float endTime = memory->globalVars->currenttime;
            if (localPlayer->getAnimstate()->landing)
                endTime = memory->globalVars->currenttime + 3.5f;
            else
                endTime = memory->globalVars->currenttime;

            if (endTime > memory->globalVars->currenttime)
                localPlayer->setPoseParameter(2, 12);
        }

        if ((config->condAA.animBreakers & (1 << 2)))
        {
            localPlayer->poseParameters().data()[8] = 0;
            localPlayer->poseParameters().data()[9] = 0;
            localPlayer->poseParameters().data()[10] = 0;
        }

        if ((config->condAA.animBreakers & (1 << 3)) && !slowwalk)
        {
            if (localPlayer->velocity().length2D() > 2.5f && !(localPlayer->flags() & FL_ONGROUND))
                localPlayer->getAnimationLayer(ANIMATION_LAYER_MOVEMENT_MOVE)->weight = 1.f;
        }

        std::memcpy(&sendPacketLayers, localPlayer->animOverlays(), sizeof(AnimationLayer) * localPlayer->getAnimationLayersCount());
        footYaw = localPlayer->getAnimstate()->footYaw;
        poseParameters = localPlayer->poseParameters();

        gotMatrixReal = localPlayer->setupBones(realmatrix.data(), localPlayer->getBoneCache().size, 0x7FF00, memory->globalVars->currenttime);

        if (gotMatrixReal)
        {
            const auto origin = localPlayer->getRenderOrigin();
            for (auto& i : realmatrix)
            {
                i[0][3] -= origin.x;
                i[1][3] -= origin.y;
                i[2][3] -= origin.z;
            }
        }

        localAngle = cmd->viewangles;
    }

    updatingLocal = false;
}

void Animations::fake() noexcept
{
    static AnimState* fakeAnimState = nullptr;
    static bool updateFakeAnim = true;
    static bool initFakeAnim = true;
    static float spawnTime = 0.f;

    if (!localPlayer || !localPlayer->isAlive() || !localPlayer->getAnimstate())
        return;

    if (interfaces->engine->isHLTV())
        return;

    if (spawnTime != localPlayer->spawnTime() || updateFakeAnim)
    {
        spawnTime = localPlayer->spawnTime();
        initFakeAnim = false;
        updateFakeAnim = false;
    }

    if (!initFakeAnim)
    {
        fakeAnimState = static_cast<AnimState*>(memory->memalloc->Alloc(sizeof(AnimState)));

        if (fakeAnimState != nullptr)
            localPlayer->createState(fakeAnimState);

        initFakeAnim = true;
    }

    if (!fakeAnimState)
        return;

    if (sendPacket)
    {
        updatingFake = true;

        std::array<AnimationLayer, 13> layersBackup;
        std::memcpy(&layersBackup, localPlayer->animOverlays(), sizeof(AnimationLayer) * localPlayer->getAnimationLayersCount());

        const auto backupAbs = localPlayer->getAbsAngle();
        const auto backupPoses = localPlayer->poseParameters();

        localPlayer->updateState(fakeAnimState, viewangles);

        if (fabsf(fakeAnimState->footYaw - footYaw) <= 5.f)
        {
            gotMatrix = false;
            updatingFake = false;

            memory->setAbsAngle(localPlayer.get(), Vector{ 0, fakeAnimState->footYaw, 0 });
            std::memcpy(localPlayer->animOverlays(), &layersBackup, sizeof(AnimationLayer) * localPlayer->getAnimationLayersCount());
            localPlayer->getAnimationLayer(ANIMATION_LAYER_LEAN)->weight = std::numeric_limits<float>::epsilon();

            gotMatrixFakelag = localPlayer->setupBones(fakelagmatrix.data(), localPlayer->getBoneCache().size, 0x7FF00, memory->globalVars->currenttime);

            std::memcpy(localPlayer->animOverlays(), &layersBackup, sizeof(AnimationLayer) * localPlayer->getAnimationLayersCount());
            localPlayer->poseParameters() = backupPoses;
            memory->setAbsAngle(localPlayer.get(), Vector{ 0, backupAbs.y, 0 });
            return;
        }

        memory->setAbsAngle(localPlayer.get(), Vector{ 0, fakeAnimState->footYaw, 0 });
        std::memcpy(localPlayer->animOverlays(), &layersBackup, sizeof(AnimationLayer) * localPlayer->getAnimationLayersCount());
        localPlayer->getAnimationLayer(ANIMATION_LAYER_LEAN)->weight = std::numeric_limits<float>::epsilon();

        gotMatrix = localPlayer->setupBones(fakematrix.data(), localPlayer->getBoneCache().size, 0x7FF00, memory->globalVars->currenttime);
        gotMatrixFakelag = gotMatrix;

        if (gotMatrix)
        {
            std::copy(fakematrix.begin(), fakematrix.end(), fakelagmatrix.data());
            const auto origin = localPlayer->getRenderOrigin();
            for (auto& i : fakematrix)
            {
                i[0][3] -= origin.x;
                i[1][3] -= origin.y;
                i[2][3] -= origin.z;
            }
        }

        std::memcpy(localPlayer->animOverlays(), &layersBackup, sizeof(AnimationLayer) * localPlayer->getAnimationLayersCount());
        localPlayer->poseParameters() = backupPoses;
        memory->setAbsAngle(localPlayer.get(), Vector{ 0, backupAbs.y, 0 });

        updatingFake = false;
    }
}

void Animations::renderStart(FrameStage stage) noexcept
{
    if (stage != FrameStage::RENDER_START)
        return;

    if (!localPlayer)
        return;

    if (interfaces->engine->isHLTV())
        return;

    for (int i = 0; i < 13; i++)
    {
        if (i == ANIMATION_LAYER_FLINCH || i == ANIMATION_LAYER_FLASHED ||
            i == ANIMATION_LAYER_WHOLE_BODY || i == ANIMATION_LAYER_WEAPON_ACTION ||
            i == ANIMATION_LAYER_WEAPON_ACTION_RECROUCH)
            continue;

        auto& animLayers = *localPlayer->getAnimationLayer(i);
        if (!&animLayers)
            continue;

        animLayers = layers.at(i);
    }
}

void Animations::packetStart() noexcept
{
    if (!localPlayer || !localPlayer->animOverlays())
        return;

    if (interfaces->engine->isHLTV())
        return;

    std::memcpy(&staticLayers, localPlayer->animOverlays(), sizeof(AnimationLayer) * localPlayer->getAnimationLayersCount());

    if (!localPlayer->getAnimstate())
        return;

    primaryCycle = localPlayer->getAnimstate()->primaryCycle;
    moveWeight = localPlayer->getAnimstate()->moveWeight;
}

static void verifyLayer(int32_t layer) noexcept
{
    AnimationLayer currentlayer = *localPlayer->getAnimationLayer(layer);
    AnimationLayer previousLayer = staticLayers.at(layer);

    auto& l = *localPlayer->getAnimationLayer(layer);
    if (!&l)
        return;

    if (currentlayer.order != previousLayer.order)
        l.order = previousLayer.order;

    if (currentlayer.sequence != previousLayer.sequence)
        l.sequence = previousLayer.sequence;

    if (currentlayer.prevCycle != previousLayer.prevCycle)
        l.prevCycle = previousLayer.prevCycle;

    if (currentlayer.weight != previousLayer.weight)
        l.weight = previousLayer.weight;

    if (currentlayer.weightDeltaRate != previousLayer.weightDeltaRate)
        l.weightDeltaRate = previousLayer.weightDeltaRate;

    if (currentlayer.playbackRate != previousLayer.playbackRate)
        l.playbackRate = previousLayer.playbackRate;

    if (currentlayer.cycle != previousLayer.cycle)
        l.cycle = previousLayer.cycle;
}

void Animations::postDataUpdate() noexcept
{
    if (!localPlayer || !localPlayer->animOverlays())
        return;

    if (interfaces->engine->isHLTV())
        return;

    for (int i = 0; i < 13; i++)
    {
        if (i == ANIMATION_LAYER_FLINCH || i == ANIMATION_LAYER_FLASHED ||
            i == ANIMATION_LAYER_WHOLE_BODY || i == ANIMATION_LAYER_WEAPON_ACTION ||
            i == ANIMATION_LAYER_WEAPON_ACTION_RECROUCH)
            continue;

        verifyLayer(i);
    }

    if (!localPlayer->getAnimstate())
        return;

    localPlayer->getAnimstate()->primaryCycle = primaryCycle;
    localPlayer->getAnimstate()->moveWeight = moveWeight;
}

// Getters остаются без изменений
void Animations::saveCorrectAngle(int entityIndex, Vector correctAng) noexcept
{
    buildTransformsIndex = entityIndex;
    correctAngle = correctAng;
}

int& Animations::buildTransformationsIndex() noexcept
{
    return buildTransformsIndex;
}

Vector* Animations::getCorrectAngle() noexcept
{
    return &sentViewangles;
}

Vector* Animations::getViewAngles() noexcept
{
    return &viewangles;
}

Vector* Animations::getLocalAngle() noexcept
{
    return &localAngle;
}

bool Animations::isLocalUpdating() noexcept
{
    return updatingLocal;
}

bool Animations::isEntityUpdating() noexcept
{
    return updatingEntity;
}

bool Animations::isFakeUpdating() noexcept
{
    return updatingFake;
}

bool Animations::gotFakeMatrix() noexcept
{
    return gotMatrix;
}

std::array<matrix3x4, MAXSTUDIOBONES> Animations::getFakeMatrix() noexcept
{
    return fakematrix;
}

bool Animations::gotFakelagMatrix() noexcept
{
    return gotMatrixFakelag;
}

std::array<matrix3x4, MAXSTUDIOBONES> Animations::getFakelagMatrix() noexcept
{
    return fakelagmatrix;
}

bool Animations::gotRealMatrix() noexcept
{
    return gotMatrixReal;
}

std::array<matrix3x4, MAXSTUDIOBONES> Animations::getRealMatrix() noexcept
{
    return realmatrix;
}

float Animations::getFootYaw() noexcept
{
    return footYaw;
}

std::array<float, 24> Animations::getPoseParameters() noexcept
{
    return poseParameters;
}

std::array<AnimationLayer, 13> Animations::getAnimLayers() noexcept
{
    return sendPacketLayers;
}

Animations::Players Animations::getPlayer(int index) noexcept
{
    return players.at(index);
}

Animations::Players* Animations::setPlayer(int index) noexcept
{
    return &players.at(index);
}

std::array<Animations::Players, 65> Animations::getPlayers() noexcept
{
    return players;
}

std::array<Animations::Players, 65>* Animations::setPlayers() noexcept
{
    return &players;
}

const std::deque<Animations::Players::Record>* Animations::getBacktrackRecords(int index) noexcept
{
    return &players.at(index).backtrackRecords;
}
