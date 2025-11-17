// Tickbase.cpp - УЛУЧШЕННАЯ ВЕРСИЯ ДЛЯ TOP-1 HVH

#include "../Config.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "../includes.hpp"
#include "Tickbase.h"
#include "AntiAim.h"
#include "../SDK/ClientState.h"
#include "../SDK/Entity.h"
#include "../SDK/UserCmd.h"
#include "../SDK/NetworkChannel.h"
#include "../Hacks/EnginePrediction.h"
#include "../SDK/Input.h"
#include "../SDK/Prediction.h"

UserCmd* command;
static bool doDefensive{ false };
EnginePrediction::NetvarData netvars{ };

// НОВАЯ КОНСТАНТА: оптимальное время перезарядки
constexpr float OPTIMAL_RECHARGE_TIME = 0.24825f;
constexpr int MAX_SHIFT_AMOUNT_VALVE = 6;
constexpr int MAX_SHIFT_AMOUNT_COMMUNITY = 13;

void Tickbase::getCmd(UserCmd* cmd)
{
    command = cmd;
}

// УЛУЧШЕНИЕ: более точная логика start
void Tickbase::start(UserCmd* cmd) noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
    {
        hasHadTickbaseActive = false;
        ticksAllowedForProcessing = 0;
        return;
    }

    // Track choked packets от сервера
    if (const auto netChannel = interfaces->engine->getNetworkChannel(); netChannel)
    {
        if (netChannel->chokedPackets > chokedPackets)
            chokedPackets = netChannel->chokedPackets;
    }

    auto activeWeapon = localPlayer->getActiveWeapon();
    if (!activeWeapon)
    {
        hasHadTickbaseActive = false;
        return;
    }

    // УЛУЧШЕНИЕ: запрещаем DT для определенных оружий
    const auto weaponId = activeWeapon->itemDefinitionIndex2();
    if (activeWeapon->isGrenade() ||
        weaponId == WeaponId::Revolver ||
        weaponId == WeaponId::C4 ||
        weaponId == WeaponId::Healthshot)
    {
        return;
    }

    auto weaponData = activeWeapon->getWeaponData();
    if (!weaponData)
        return;

    auto netChannel = interfaces->engine->getNetworkChannel();
    if (!netChannel)
        return;

    // Если DT/HS выключены - force discharge если был активен
    if (!config->tickbase.doubletap.isActive() && !config->tickbase.hideshots.isActive())
    {
        if (hasHadTickbaseActive)
        {
            // Force discharge accumulated ticks
            shiftOffensive(cmd, ticksAllowedForProcessing, true);
        }
        hasHadTickbaseActive = false;
        targetTickShift = 0;
        return;
    }

    // УЛУЧШЕНИЕ: правильное определение shift amount
    if (config->tickbase.doubletap.isActive())
    {
        // DT: 12-13 тиков для community, 6 для Valve
        targetTickShift = (*memory->gameRules)->isValveDS() ?
            MAX_SHIFT_AMOUNT_VALVE : MAX_SHIFT_AMOUNT_COMMUNITY;

        // Break lag comp если включено
        if (config->tickbase.defensive_dt)
            breakLagComp(cmd, targetTickShift);
    }
    else if (config->tickbase.hideshots.isActive())
    {
        // Hideshots: 6-9 тиков
        targetTickShift = (*memory->gameRules)->isValveDS() ? 6 : 9;
    }

    // УЛУЧШЕНИЕ: clamp с учетом server limits
    static ConVar* sv_maxusrcmdprocessticks = interfaces->cvar->findVar(skCrypt("sv_maxusrcmdprocessticks"));
    const int serverMaxTicks = sv_maxusrcmdprocessticks ? sv_maxusrcmdprocessticks->getInt() - 1 : maxUserCmdProcessTicks - 1;

    targetTickShift = std::clamp(targetTickShift, 0, (std::min)(serverMaxTicks, maxUserCmdProcessTicks - 1));
    hasHadTickbaseActive = true;
}

// УЛУЧШЕНИЕ: более умная логика end
void Tickbase::end(UserCmd* cmd, bool sendPacket) noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return;

    auto weapon = localPlayer->getActiveWeapon();
    if (!weapon)
        return;

    // Сброс если выключено
    if (!config->tickbase.doubletap.isActive() && !config->tickbase.hideshots.isActive())
    {
        targetTickShift = 0;
        return;
    }

    // УЛУЧШЕНИЕ: проверяем готовность к шифту ПЕРЕД попыткой
    const bool readyToShift = canShiftDT(targetTickShift, false);

    // Doubletap logic
    if (config->tickbase.doubletap.isActive())
    {
        // Knife backstab
        if (weapon->isKnife() && (cmd->buttons & UserCmd::IN_ATTACK2))
        {
            if (readyToShift)
                shiftOffensive(cmd, targetTickShift);
        }
        // Normal attack
        else if (cmd->buttons & UserCmd::IN_ATTACK)
        {
            if (readyToShift)
                shiftOffensive(cmd, targetTickShift);
        }
    }
    // Hideshots logic
    else if (config->tickbase.hideshots.isActive())
    {
        if ((cmd->buttons & UserCmd::IN_ATTACK) && canShiftHS(targetTickShift, false))
        {
            shiftOffensive(cmd, targetTickShift);
        }
    }
}

// УЛУЧШЕНИЕ: оптимизированный shiftOffensive
bool Tickbase::shiftOffensive(UserCmd* cmd, int shiftAmount, bool forceShift) noexcept
{
    if (!canShiftDT(shiftAmount, forceShift))
        return false;

    auto weapon = localPlayer->getActiveWeapon();
    if (!weapon || weapon->itemDefinitionIndex2() == WeaponId::Revolver)
        return false;

    // КРИТИЧЕСКИ ВАЖНО: сохраняем время шифта
    realTime = memory->globalVars->realtime;
    shiftedTickbase = shiftAmount;
    shiftCommand = cmd->commandNumber;
    tickShift = shiftAmount;

    // НОВОЕ: помечаем что мы в процессе шифта
    shifting = true;

    return true;
}

// УЛУЧШЕНИЕ: более надежный breakLagComp для defensive AA
void Tickbase::breakLagComp(UserCmd* cmd, int amount)
{
    static int defensive_tick = 0;
    static bool can_defensive = false;

    if (!memory->clientState)
        return;

    const int currentServerTick = memory->clientState->m_clock_drift_mgr.m_server_tick;

    // УЛУЧШЕНИЕ: проверяем разницу тиков
    if (std::abs(currentServerTick - defensive_tick) > 1)
    {
        defensive_tick = currentServerTick;

        // Defensive exploit условия
        if (isPeeking)
        {
            if (!can_defensive)
            {
                can_defensive = true;
                sendPacket = true;
                targetTickShift = amount;
            }
        }
        else
        {
            can_defensive = false;
            isPeeking = false;
            targetTickShift = amount;
        }
    }
    else
    {
        targetTickShift = amount;
    }
}

// НОВАЯ ФУНКЦИЯ: defensive shift (для defensive AA)
bool Tickbase::shiftDefensive(UserCmd* cmd, int shiftAmount, bool forceShift) noexcept
{
    if (!canShiftDT(shiftAmount, forceShift))
        return false;

    auto weapon = localPlayer->getActiveWeapon();
    if (!weapon || weapon->itemDefinitionIndex2() == WeaponId::Revolver)
        return false;

    realTime = memory->globalVars->realtime;
    shiftedTickbase = shiftAmount;
    shiftCommand = cmd->commandNumber;
    tickShift = shiftAmount;

    // DEFENSIVE: manually увеличиваем choked packets
    if (memory->clientState && memory->clientState->netChannel)
    {
        for (int i = 0; i < shiftAmount; i++)
        {
            ++memory->clientState->netChannel->chokedPackets;
        }
    }

    return true;
}

// hideshots shift
bool Tickbase::shiftHideShots(UserCmd* cmd, int shiftAmount, bool forceShift) noexcept
{
    if (!canShiftHS(shiftAmount, forceShift))
        return false;

    shiftedTickbase = shiftAmount;
    return true;
}

// УЛУЧШЕННАЯ ФУНКЦИЯ: canRun с лучшей логикой recharge
bool Tickbase::canRun() noexcept
{
    static float spawnTime = 0.f;

    // Сброс в неактивных состояниях
    if (!interfaces->engine->isInGame() || !interfaces->engine->isConnected())
    {
        ticksAllowedForProcessing = 0;
        chokedPackets = 0;
        pauseTicks = 0;
        bShouldRecharge = false;
        return true;
    }

    if (!localPlayer || !localPlayer->isAlive() || !targetTickShift)
    {
        ticksAllowedForProcessing = 0;
        bShouldRecharge = false;
        return true;
    }

    // Freeze period - останавливаем recharge
    if ((*memory->gameRules)->freezePeriod())
    {
        realTime = memory->globalVars->realtime;
        return true;
    }

    // Spawn reset
    if (spawnTime != localPlayer->spawnTime())
    {
        spawnTime = localPlayer->spawnTime();
        ticksAllowedForProcessing = 0;
        pauseTicks = 0;
        bShouldRecharge = false;
    }

    // Fakeduck - полностью отключаем tickbase
    if (config->misc.fakeduck && config->misc.fakeduckKey.isActive())
    {
        realTime = memory->globalVars->realtime;
        shiftedTickbase = 0;
        shiftCommand = 0;
        tickShift = 0;
        bShouldRecharge = false;
        return true;
    }

    // УЛУЧШЕНИЕ: правильная логика recharge
    static ConVar* sv_maxusrcmdprocessticks = interfaces->cvar->findVar(skCrypt("sv_maxusrcmdprocessticks"));
    int maxShiftAmount = sv_maxusrcmdprocessticks ? sv_maxusrcmdprocessticks->getInt() - 1 : maxUserCmdProcessTicks - 1;

    // Valve servers - лимит 6
    if ((*memory->gameRules)->isValveDS())
        maxShiftAmount = (std::min)(MAX_SHIFT_AMOUNT_VALVE, maxShiftAmount);

    const float timeSinceLastShift = memory->globalVars->realtime - realTime;
    const float timeSinceLastShot = memory->globalVars->realtime - AntiAim::getLastShotTime();

    // КРИТИЧЕСКОЕ УСЛОВИЕ RECHARGE:
    // 1. Недостаточно тиков ИЛИ слишком много choked packets
    // 2. Прошло достаточно времени с последнего шифта (cooldown)
    // 3. Прошло достаточно времени с последнего выстрела ИЛИ мы только включили DT
    const bool needsRecharge = (ticksAllowedForProcessing < targetTickShift ||
        chokedPackets > maxUserCmdProcessTicks - targetTickShift);

    const bool cooldownPassed = timeSinceLastShift > OPTIMAL_RECHARGE_TIME;
    const bool shotCooldownPassed = (timeSinceLastShot > 1.0f || !hasHadTickbaseActive);

    if (needsRecharge && cooldownPassed && shotCooldownPassed)
    {
        // УЛУЧШЕНИЕ: постепенно заряжаем тики
        ticksAllowedForProcessing = (std::min)(ticksAllowedForProcessing + 1, maxUserCmdProcessTicks);
        chokedPackets = (std::max)(chokedPackets - 1, 0);
        pauseTicks++;
        bShouldRecharge = true;
        isRecharging = true;
        return false; // Не разрешаем действия во время recharge
    }
    else if (chokedPackets >= maxShiftAmount)
    {
        bShouldRecharge = false;
        isRecharging = false;
    }
    else
    {
        isRecharging = false;
    }

    return true;
}

// УЛУЧШЕННАЯ ФУНКЦИЯ: canShiftDT с лучшими проверками
bool Tickbase::canShiftDT(int shiftAmount, bool forceShift) noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return false;

    // УЛУЧШЕНИЕ: проверка на 0 shift
    if (shiftAmount <= 0)
        return false;

    // УЛУЧШЕНИЕ: проверка recharge cooldown
    const float timeSinceLastShift = memory->globalVars->realtime - realTime;
    if (timeSinceLastShift <= OPTIMAL_RECHARGE_TIME)
        return false;

    // УЛУЧШЕНИЕ: проверка накопленных тиков
    if (shiftAmount > ticksAllowedForProcessing)
        return false;

    // Force shift bypass
    if (forceShift)
        return true;

    // Fakeduck блокирует
    if (config->misc.fakeduck && config->misc.fakeduckKey.isActive())
        return false;

    const auto activeWeapon = localPlayer->getActiveWeapon();
    if (!activeWeapon || !activeWeapon->clip())
        return false;

    // Запрещенные оружия
    const auto weaponId = activeWeapon->itemDefinitionIndex2();
    if (activeWeapon->isGrenade() ||
        activeWeapon->isBomb() ||
        weaponId == WeaponId::Healthshot ||
        weaponId == WeaponId::Revolver)
    {
        return false;
    }

    // КРИТИЧЕСКИ ВАЖНО: проверяем timing на shifted tickbase
    const float shiftTime = (localPlayer->tickBase() - shiftAmount) * memory->globalVars->intervalPerTick;

    // Проверка global attack cooldown
    if (localPlayer->nextAttack() > shiftTime)
        return false;

    // Проверка weapon attack cooldown
    if (activeWeapon->nextPrimaryAttack() > shiftTime)
        return false;

    // Проверка на semi-auto weapons
    if (localPlayer->shotsFired() > 0 && !activeWeapon->isFullAuto())
        return false;

    return true;
}

// canShiftHS для hideshots
bool Tickbase::canShiftHS(int shiftAmount, bool forceShift) noexcept
{
    // Hideshots не работает одновременно с DT
    if (config->tickbase.doubletap.isActive())
        return false;

    if (!localPlayer || !localPlayer->isAlive())
        return false;

    if (shiftAmount <= 0 || shiftAmount > ticksAllowedForProcessing)
        return false;

    if (forceShift)
        return true;

    if (config->misc.fakeduck && config->misc.fakeduckKey.isActive())
        return false;

    const auto activeWeapon = localPlayer->getActiveWeapon();
    if (!activeWeapon || !activeWeapon->clip())
        return false;

    // Запрещаем для ножей и гранат
    if (activeWeapon->isKnife() ||
        activeWeapon->isGrenade() ||
        activeWeapon->isBomb() ||
        activeWeapon->itemDefinitionIndex2() == WeaponId::Healthshot)
    {
        return false;
    }

    const float shiftTime = (localPlayer->tickBase() - shiftAmount) * memory->globalVars->intervalPerTick;

    if (localPlayer->nextAttack() > shiftTime)
        return false;

    if (activeWeapon->nextPrimaryAttack() > shiftTime)
        return false;

    if (localPlayer->shotsFired() > 0 && !activeWeapon->isFullAuto())
        return false;

    return true;
}

// Calculation helpers
int calc_correction_ticks()
{
    auto clock = interfaces->cvar->findVar(skCrypt("sv_clockcorrection_msecs"));

    if (!clock || !memory->globalVars || memory->globalVars->maxClients <= 1)
        return -1;

    const float correction_ms = clock->getFloat();
    const float correction_seconds = std::clamp(correction_ms / 1000.0f, 0.0f, 1.0f);

    return static_cast<int>((correction_seconds / memory->globalVars->intervalPerTick) + 0.5f);
}

// УЛУЧШЕННАЯ ФУНКЦИЯ: adjust_tick_base
int Tickbase::adjust_tick_base(const int old_new_cmds, const int total_new_cmds, const int delta) noexcept
{
    const auto correction_ticks = calc_correction_ticks();

    if (correction_ticks == -1)
        return localPlayer->tickBase();

    int ret = 0;

    if (netvars.spawn_time == localPlayer->spawnTime())
    {
        ret = netvars.tickbase + 1;

        const int tick_count = ret + old_new_cmds - targetTickShift;
        const int ideal_final_tick = tick_count + correction_ticks;
        const int too_fast_limit = ideal_final_tick + correction_ticks;
        const int too_slow_limit = ideal_final_tick - correction_ticks;
        const int adjusted_final_tick = ret + total_new_cmds;

        // Корректируем если вышли за пределы
        if (adjusted_final_tick > too_fast_limit || adjusted_final_tick < too_slow_limit)
        {
            ret = ideal_final_tick - total_new_cmds;
        }
    }

    if (ret != 0)
        return ret;

    // Fallback
    const int baseTickbase = (netvars.spawn_time != localPlayer->spawnTime()) ?
        netvars.tickbase : localPlayer->tickBase();

    return baseTickbase - delta;
}

// УЛУЧШЕННАЯ ФУНКЦИЯ: getCorrectTickbase
int Tickbase::getCorrectTickbase(int commandNumber) noexcept
{
    const int tickBase = localPlayer->tickBase();

    // Во время recharge добавляем накопленные тики
    if (isRecharging)
        return tickBase + ticksAllowedForProcessing;

    // ВАЖНО для prediction
    memory->globalVars->currenttime = ticksToTime(tickBase);

    // Tick в момент шифта
    if (commandNumber == shiftCommand)
    {
        return tickBase - shiftedTickbase + memory->globalVars->m_simticksthisframe + 1;
    }
    // Tick сразу после шифта
    else if (commandNumber == shiftCommand + 1)
    {
        if (!config->tickbase.teleport)
        {
            // Non-teleport mode: плавное восстановление
            return tickBase + shiftedTickbase - memory->globalVars->m_simticksthisframe + 1;
        }
        // Teleport mode: резкий сброс
        return tickBase;
    }

    // Во время pause
    if (pauseTicks > 0)
        return tickBase + pauseTicks;

    return tickBase;
}

// Getters
int& Tickbase::pausedTicks() noexcept
{
    return pauseTicks;
}

int Tickbase::getTargetTickShift() noexcept
{
    if (!bShouldRecharge)
        return 1;
    return targetTickShift;
}

int Tickbase::getTickshift() noexcept
{
    return tickShift;
}

// УЛУЧШЕНИЕ: правильный reset
void Tickbase::resetTickshift() noexcept
{
    shiftedTickbase = tickShift;

    // Teleport mode: вычитаем использованные тики
    if (config->tickbase.teleport && config->tickbase.doubletap.isActive())
    {
        ticksAllowedForProcessing = (std::max)(ticksAllowedForProcessing - tickShift, 0);
    }

    tickShift = 0;
    shifting = false;
}

bool& Tickbase::isFinalTick() noexcept
{
    return finalTick;
}

bool& Tickbase::isShifting() noexcept
{
    return shifting;
}

void Tickbase::updateInput() noexcept
{
    config->tickbase.doubletap.handleToggle();
    config->tickbase.hideshots.handleToggle();
}

// УЛУЧШЕНИЕ: полный reset всех переменных
void Tickbase::reset() noexcept
{
    isRecharging = false;
    shifting = false;
    finalTick = false;
    hasHadTickbaseActive = false;
    sendPacket = false;
    isPeeking = false;
    bShouldRecharge = false;

    pauseTicks = 0;
    chokedPackets = 0;
    tickShift = 0;
    shiftCommand = 0;
    shiftedTickbase = 0;
    ticksAllowedForProcessing = 0;
    targetTickShift = 0;
    realTime = 0.0f;
}
