// EnginePrediction.cpp - ИСПРАВЛЕННАЯ ВЕРСИЯ

#include "../Interfaces.h"
#include "../Memory.h"
#include "EnginePrediction.h"
#include "../SDK/ClientState.h"
#include "../SDK/Engine.h"
#include "../SDK/Entity.h"
#include "../SDK/EntityList.h"
#include "../SDK/FrameStage.h"
#include "../SDK/GameMovement.h"
#include "../SDK/GlobalVars.h"
#include "../SDK/MoveHelper.h"
#include "../SDK/Prediction.h"
#include "../SDK/PredictionCopy.h"
#include "../xor.h"

// Backup структура для всех важных данных
struct PredictionBackup
{
    // Player data
    int flags;
    Vector velocity;
    Vector origin;
    Vector aimPunch;
    Vector aimPunchVel;
    Vector viewPunch;
    Vector viewOffset;
    Vector baseVelocity;
    float duckAmount;
    float duckSpeed;
    float fallVelocity;
    float velocityModifier;
    float thirdPersonRecoil;

    // Weapon data (используем указатель на weapon)
    Entity* weapon;
    float nextPrimaryAttack;
    float nextSecondaryAttack;
    float recoilIndex;
    int itemDefinitionIndex;

    // Global vars
    float currentTime;
    float frameTime;
    int tickCount;

    // Animation state
    float footYaw;
    float duckAdditional;
    float lastUpdateIncrement;
};

static PredictionBackup g_predictionBackup;
static int localPlayerFlags;
static Vector localPlayerVelocity;
static bool inPrediction{ false };
static std::array<EnginePrediction::NetvarData, 150> netvarData;
static void* storedData = nullptr;
EnginePrediction::NetvarData netvars{ };

void EnginePrediction::reset() noexcept
{
    localPlayerFlags = {};
    localPlayerVelocity = Vector{};
    netvarData = {};
    storedData = nullptr;
    inPrediction = false;
    g_predictionBackup = {};

    m_data.fill(StoredData_t());
}

void EnginePrediction::update() noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return;

    const auto deltaTick = memory->clientState->deltaTick;
    const auto start = memory->clientState->lastCommandAck;
    const auto stop = memory->clientState->lastOutgoingCommand + memory->clientState->chokedCommands;
    bool valid{ memory->clientState->deltaTick > 0 };

    if (netvars.velocityModifier < 1.f)
        interfaces->prediction->inPrediction = true;

    if (deltaTick > 0)
    {
        interfaces->prediction->update(deltaTick, valid, start, stop);
    }
}

// Backup всех данных перед предикшеном
static void backupData() noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return;

    auto activeWeapon = localPlayer->getActiveWeapon();

    // Player backup
    g_predictionBackup.flags = localPlayer->flags();
    g_predictionBackup.velocity = localPlayer->velocity();
    g_predictionBackup.origin = localPlayer->origin();
    g_predictionBackup.aimPunch = localPlayer->aimPunchAngle();
    g_predictionBackup.aimPunchVel = localPlayer->aimPunchAngleVelocity();
    g_predictionBackup.viewPunch = localPlayer->viewPunchAngle();
    g_predictionBackup.viewOffset = localPlayer->viewOffset();
    g_predictionBackup.baseVelocity = localPlayer->baseVelocity();
    g_predictionBackup.duckAmount = localPlayer->duckAmount();
    g_predictionBackup.duckSpeed = localPlayer->duckSpeed();
    g_predictionBackup.fallVelocity = localPlayer->fallVelocity();
    g_predictionBackup.velocityModifier = localPlayer->velocityModifier();
    g_predictionBackup.thirdPersonRecoil = localPlayer->thirdPersonRecoil();

    // Weapon backup
    if (activeWeapon)
    {
        g_predictionBackup.weapon = activeWeapon;
        g_predictionBackup.nextPrimaryAttack = activeWeapon->nextPrimaryAttack();
        g_predictionBackup.nextSecondaryAttack = activeWeapon->nextSecondaryAttack();
        g_predictionBackup.recoilIndex = activeWeapon->recoilIndex();
        g_predictionBackup.itemDefinitionIndex = activeWeapon->itemDefinitionIndex();
    }
    else
    {
        g_predictionBackup.weapon = nullptr;
    }

    // Global vars backup
    g_predictionBackup.currentTime = memory->globalVars->currenttime;
    g_predictionBackup.frameTime = memory->globalVars->frametime;
    g_predictionBackup.tickCount = memory->globalVars->tickCount;

    // Animation state backup
    if (localPlayer->getAnimstate())
    {
        g_predictionBackup.footYaw = localPlayer->getAnimstate()->footYaw;
        g_predictionBackup.duckAdditional = localPlayer->getAnimstate()->duckAdditional;
        g_predictionBackup.lastUpdateIncrement = localPlayer->getAnimstate()->lastUpdateIncrement;
    }
}

// Restore данных после предикшена
static void restoreData() noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return;

    // Проверяем что weapon не изменился
    if (g_predictionBackup.weapon && g_predictionBackup.weapon == localPlayer->getActiveWeapon())
    {
        auto activeWeapon = g_predictionBackup.weapon;

        // Restore weapon data
        activeWeapon->nextPrimaryAttack() = g_predictionBackup.nextPrimaryAttack;
        activeWeapon->nextSecondaryAttack() = g_predictionBackup.nextSecondaryAttack;
        activeWeapon->recoilIndex() = g_predictionBackup.recoilIndex;
    }

    // Restore global vars
    memory->globalVars->currenttime = g_predictionBackup.currentTime;
    memory->globalVars->frametime = g_predictionBackup.frameTime;
    memory->globalVars->tickCount = g_predictionBackup.tickCount;
}

void EnginePrediction::run(UserCmd* cmd) noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return;

    inPrediction = true;

    // Backup всех данных
    backupData();

    float sv_footsteps_backup = 0.0f;
    float sv_min_jump_landing_sound_backup = 0.0f;
    ConVar* sv_footsteps = nullptr;
    ConVar* sv_min_jump_landing_sound = nullptr;

    localPlayerFlags = localPlayer->flags();
    localPlayerVelocity = localPlayer->velocity();

    *memory->predictionRandomSeed = 0;
    *memory->predictionPlayer = reinterpret_cast<int>(localPlayer.get());

    const auto oldCurrenttime = memory->globalVars->currenttime;
    const auto oldFrametime = memory->globalVars->frametime;
    const auto oldIsFirstTimePredicted = interfaces->prediction->isFirstTimePredicted;
    const auto oldInPrediction = interfaces->prediction->inPrediction;

    // Setup prediction time
    memory->globalVars->currenttime = memory->globalVars->serverTime();
    memory->globalVars->frametime = interfaces->prediction->enginePaused ? 0 : memory->globalVars->intervalPerTick;
    interfaces->prediction->isFirstTimePredicted = false;
    interfaces->prediction->inPrediction = true;

    pLastCmd = cmd;

    const int iOldTickBase = localPlayer->tickBase();

    // Update button state
    const int iButtons = cmd->buttons;
    const int nLocalButtons = localPlayer->getButtons();
    const int nButtonsChanged = iButtons ^ nLocalButtons;

    localPlayer->getButtonLast() = nLocalButtons;
    localPlayer->getButtons() = iButtons;
    localPlayer->getButtonPressed() = nButtonsChanged & iButtons;
    localPlayer->getButtonReleased() = nButtonsChanged & (~iButtons);

    // Run prethink
    if (localPlayer->physicsRunThink(THINK_FIRE_ALL_FUNCTIONS))
        localPlayer->preThink();

    // Disable footsteps sounds
    if (!sv_footsteps)
        sv_footsteps = interfaces->cvar->findVar(skCrypt("sv_footsteps"));

    if (!sv_min_jump_landing_sound)
        sv_min_jump_landing_sound = interfaces->cvar->findVar(skCrypt("sv_min_jump_landing_sound"));

    if (sv_footsteps)
    {
        sv_footsteps_backup = *(float*)(uintptr_t(sv_footsteps) + 0x2C);
        *(uint32_t*)(uintptr_t(sv_footsteps) + 0x2C) = (uint32_t)sv_footsteps ^ uint32_t(0.0f);
    }

    if (sv_min_jump_landing_sound)
    {
        sv_min_jump_landing_sound_backup = *(float*)(uintptr_t(sv_min_jump_landing_sound) + 0x2C);
        *(uint32_t*)(uintptr_t(sv_min_jump_landing_sound) + 0x2C) = (uint32_t)sv_min_jump_landing_sound ^ 0x7F7FFFFF;
    }

    // Handle impulse
    if (cmd->impulse)
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(localPlayer.get()) + 0x320C) = cmd->impulse;

    // Update buttons
    cmd->buttons |= *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(localPlayer.get()) + 0x3344);
    cmd->buttons &= ~(*reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(localPlayer.get()) + 0x3340));

    localPlayer->updateButtonState(cmd->buttons);

    // Start prediction
    interfaces->gameMovement->startTrackPredictionErrors(localPlayer.get());
    interfaces->prediction->checkMovingGround(localPlayer.get(), memory->globalVars->frametime);

    localPlayer->runPreThink();
    localPlayer->runThink();

    // Setup move
    memory->moveHelper->setHost(localPlayer.get());
    interfaces->prediction->setupMove(localPlayer.get(), cmd, memory->moveHelper, memory->moveData);

    // Process movement
    interfaces->gameMovement->processMovement(localPlayer.get(), memory->moveData);

    // Finish move
    interfaces->prediction->finishMove(localPlayer.get(), cmd, memory->moveData);
    memory->moveHelper->processImpacts();

    localPlayer->runPostThink();

    // Restore footsteps
    if (sv_footsteps)
        *(float*)(uintptr_t(sv_footsteps) + 0x2C) = sv_footsteps_backup;

    if (sv_min_jump_landing_sound)
        *(float*)(uintptr_t(sv_min_jump_landing_sound) + 0x2C) = sv_min_jump_landing_sound_backup;

    // Finish tracking
    interfaces->gameMovement->finishTrackPredictionErrors(localPlayer.get());
    memory->moveHelper->setHost(nullptr);
    interfaces->gameMovement->reset();

    *memory->predictionRandomSeed = -1;
    *memory->predictionPlayer = 0;

    // Restore global vars
    memory->globalVars->currenttime = oldCurrenttime;
    memory->globalVars->frametime = oldFrametime;
    interfaces->prediction->isFirstTimePredicted = oldIsFirstTimePredicted;
    interfaces->prediction->inPrediction = oldInPrediction;

    // Update weapon accuracy
    const auto activeWeapon = localPlayer->getActiveWeapon();
    if (activeWeapon && !activeWeapon->isGrenade() && !activeWeapon->isKnife())
    {
        // Restore tickbase
        localPlayer->tickBase() = iOldTickBase;

        // КРИТИЧЕСКИ ВАЖНО: обновляем accuracy penalty после предикшена
        activeWeapon->updateAccuracyPenalty();
    }

    // Restore weapon data
    restoreData();

    inPrediction = false;
}

void EnginePrediction::save() noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return;

    if (!storedData)
    {
        const auto allocSize = localPlayer->getIntermidateDataSize();
        storedData = new byte[allocSize];
    }

    if (!storedData)
        return;

    PredictionCopy helper(PC_EVERYTHING, (byte*)storedData, true, (byte*)localPlayer.get(), false, PredictionCopy::TRANSFERDATA_COPYONLY, NULL);
    helper.transferData("EnginePrediction::save", localPlayer->index(), localPlayer->getPredDescMap());
}

void EnginePrediction::store() noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return;

    // Fix view offset clamping
    if (localPlayer->viewOffset().z <= 46.05f)
        localPlayer->viewOffset().z = 46.0f;
    else if (localPlayer->viewOffset().z > 64.0f)
        localPlayer->viewOffset().z = 64.0f;

    const int tickbase = localPlayer->tickBase();

    NetvarData netvars{ };
    StoredData_t* data;

    netvars.tickbase = tickbase;
    data = &m_data[tickbase % MULTIPLAYER_BACKUP];

    // Store all netvar data
    netvars.aimPunchAngle = localPlayer->aimPunchAngle();
    netvars.aimPunchAngleVelocity = localPlayer->aimPunchAngleVelocity();
    netvars.baseVelocity = localPlayer->baseVelocity();
    netvars.duckAmount = localPlayer->duckAmount();
    netvars.duckSpeed = localPlayer->duckSpeed();
    netvars.fallVelocity = localPlayer->fallVelocity();
    netvars.thirdPersonRecoil = localPlayer->thirdPersonRecoil();
    netvars.velocity = localPlayer->velocity();
    netvars.velocityModifier = localPlayer->velocityModifier();
    netvars.viewPunchAngle = localPlayer->viewPunchAngle();
    netvars.viewOffset = localPlayer->viewOffset();
    netvars.network_origin = localPlayer->network_origin();

    netvarData.at(tickbase % 150) = netvars;
}

void EnginePrediction::restore() noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return;

    const int tickbase = localPlayer->tickBase();
    netvars.tickbase = tickbase;

    // Restore all netvars
    localPlayer->aimPunchAngle() = netvars.aimPunchAngle;
    localPlayer->aimPunchAngleVelocity() = netvars.aimPunchAngleVelocity;
    localPlayer->baseVelocity() = netvars.baseVelocity;
    localPlayer->duckAmount() = netvars.duckAmount;
    localPlayer->duckSpeed() = netvars.duckSpeed;
    localPlayer->fallVelocity() = netvars.fallVelocity;
    localPlayer->thirdPersonRecoil() = netvars.thirdPersonRecoil;
    localPlayer->velocity() = netvars.velocity;
    localPlayer->velocityModifier() = netvars.velocityModifier;
    localPlayer->viewPunchAngle() = netvars.viewPunchAngle;
    localPlayer->viewOffset() = netvars.viewOffset;
    localPlayer->network_origin() = netvars.network_origin;

    netvarData.at(tickbase % 150) = netvars;
}

void EnginePrediction::apply(FrameStage stage) noexcept
{
    if (stage != FrameStage::NET_UPDATE_END)
        return;

    if (!localPlayer || !localPlayer->isAlive())
        return;

    if (netvarData.empty())
        return;

    const int tickbase = localPlayer->tickBase();
    const auto& netvars = netvarData.at(tickbase % MULTIPLAYER_BACKUP);

    if (!&netvars || netvars.tickbase != tickbase)
        return;

    // Calculate deltas
    Vector punch_delta = localPlayer->aimPunchAngle() - netvars.aimPunchAngle;
    Vector punch_vel_delta = localPlayer->aimPunchAngleVelocity() - netvars.aimPunchAngleVelocity;
    Vector velocity_diff = localPlayer->m_vecVelocity() - netvars.m_vecVelocity;
    Vector view_delta = localPlayer->viewOffset() - netvars.viewOffset;
    float modifier_delta = localPlayer->velocityModifier() - netvars.velocityModifier;
    Vector view_punch_angle_delta = localPlayer->viewPunchAngle() - netvars.viewPunchAngle;
    Vector view_offset_delta = localPlayer->m_vecViewOffset() - netvars.m_vecViewOffset;
    float thirdperson_recoil_delta = localPlayer->m_flThirdpersonRecoil() - netvars.m_flThirdpersonRecoil;
    float duck_amount_delta = localPlayer->duckAmount() - netvars.duckAmount;
    Vector viewoffset_diff = netvars.viewOffset - localPlayer->viewOffset();

    // Apply corrections with tolerance checks
    constexpr float tolerance = 0.03125f;

    if (std::abs(punch_delta.x) <= tolerance && std::abs(punch_delta.y) <= tolerance && std::abs(punch_delta.z) <= tolerance)
        localPlayer->aimPunchAngle() = netvars.aimPunchAngle;

    if (std::abs(punch_vel_delta.x) <= tolerance && std::abs(punch_vel_delta.y) <= tolerance && std::abs(punch_vel_delta.z) <= tolerance)
        localPlayer->aimPunchAngleVelocity() = netvars.aimPunchAngleVelocity;

    if (std::abs(viewoffset_diff.x) < tolerance && std::abs(viewoffset_diff.y) < tolerance && std::abs(viewoffset_diff.z) < tolerance)
        localPlayer->viewOffset() = netvars.viewOffset;

    if (std::abs(modifier_delta) <= tolerance)
        localPlayer->velocityModifier() = netvars.velocityModifier;

    if (std::fabs(view_punch_angle_delta.x) < tolerance && std::fabs(view_punch_angle_delta.y) < tolerance && std::fabs(view_punch_angle_delta.z) < tolerance)
        localPlayer->viewPunchAngle() = netvars.viewPunchAngle;

    if (std::abs(velocity_diff.x) <= 0.5f && std::abs(velocity_diff.y) <= 0.5f && std::abs(velocity_diff.z) <= 0.5f)
        localPlayer->m_vecVelocity() = netvars.m_vecVelocity;

    if (std::abs(duck_amount_delta) <= tolerance)
        localPlayer->duckAmount() = netvars.duckAmount;

    if (std::abs(localPlayer->duckSpeed() - netvars.duckSpeed) <= tolerance)
        localPlayer->duckSpeed() = netvars.duckSpeed;

    if (std::abs(localPlayer->fallVelocity() - netvars.fallVelocity) <= 0.5f)
        localPlayer->fallVelocity() = netvars.fallVelocity;

    if (std::fabs(view_offset_delta.x) <= tolerance && std::fabs(view_offset_delta.y) <= tolerance && std::fabs(view_offset_delta.z) <= tolerance)
        localPlayer->m_vecViewOffset() = netvars.m_vecViewOffset;

    // Use checkDifference for additional safety
    localPlayer->aimPunchAngle() = NetvarData::checkDifference(localPlayer->aimPunchAngle(), netvars.aimPunchAngle);
    localPlayer->aimPunchAngleVelocity() = NetvarData::checkDifference(localPlayer->aimPunchAngleVelocity(), netvars.aimPunchAngleVelocity);
    localPlayer->baseVelocity() = NetvarData::checkDifference(localPlayer->baseVelocity(), netvars.baseVelocity);
    localPlayer->duckAmount() = std::clamp(NetvarData::checkDifference(localPlayer->duckAmount(), netvars.duckAmount), 0.0f, 1.0f);
    localPlayer->duckSpeed() = NetvarData::checkDifference(localPlayer->duckSpeed(), netvars.duckSpeed);
    localPlayer->fallVelocity() = NetvarData::checkDifference(localPlayer->fallVelocity(), netvars.fallVelocity);
    localPlayer->thirdPersonRecoil() = NetvarData::checkDifference(localPlayer->thirdPersonRecoil(), netvars.thirdPersonRecoil);
    localPlayer->velocity() = NetvarData::checkDifference(localPlayer->velocity(), netvars.velocity);
    localPlayer->velocityModifier() = NetvarData::checkDifference(localPlayer->velocityModifier(), netvars.velocityModifier);
    localPlayer->viewPunchAngle() = NetvarData::checkDifference(localPlayer->viewPunchAngle(), netvars.viewPunchAngle);
    localPlayer->viewOffset() = NetvarData::checkDifference(localPlayer->viewOffset(), netvars.viewOffset);
    localPlayer->tickBase() = static_cast<int>(NetvarData::checkDifference(static_cast<float>(localPlayer->tickBase()), static_cast<float>(netvars.tickbase)));
}

int EnginePrediction::getFlags() noexcept
{
    return localPlayerFlags;
}

Vector EnginePrediction::getVelocity() noexcept
{
    return localPlayerVelocity;
}

bool EnginePrediction::isInPrediction() noexcept
{
    return inPrediction;
}

static float checkDifference(float predicted, float original) noexcept
{
    float delta = predicted - original;
    if (std::abs(delta) < 0.03125)
        return original;
    return predicted;
}

static Vector checkDifference(Vector predicted, Vector original) noexcept
{
    Vector delta = predicted - original;
    if (std::abs(delta.x) < 0.03125 && std::abs(delta.y) < 0.03125f && std::abs(delta.z) < 0.031254)
    {
        return original;
    }
    return predicted;
}
