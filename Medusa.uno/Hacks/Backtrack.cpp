// Backtrack.cpp - УЛУЧШЕННАЯ ВЕРСИЯ

#include "../Config.h"
#include "AimbotFunctions.h"
#include "Animations.h"
#include "Backtrack.h"
#include "Tickbase.h"
#include "../xor.h"
#include "../SDK/ConVar.h"
#include "../SDK/Entity.h"
#include "../SDK/FrameStage.h"
#include "../SDK/LocalPlayer.h"
#include "../SDK/NetworkChannel.h"
#include "../SDK/UserCmd.h"
#include <algorithm>

static std::deque<Backtrack::incomingSequence> sequences;

struct Cvars
{
    ConVar* updateRate;
    ConVar* maxUpdateRate;
    ConVar* interp;
    ConVar* interpRatio;
    ConVar* minInterpRatio;
    ConVar* maxInterpRatio;
    ConVar* maxUnlag;
};

static Cvars cvars;

// НОВАЯ СТРУКТУРА: расширенные данные о backtrack записи
struct BacktrackRecordInfo
{
    int playerIndex;
    int recordIndex;
    float simulationTime;
    float priority;
    Vector bestPosition;
    bool isValid;

    bool operator<(const BacktrackRecordInfo& other) const noexcept
    {
        return priority > other.priority; // Descending
    }
};

// УЛУЧШЕНИЕ: более точный расчет lerp с учетом всех факторов
float Backtrack::getLerp() noexcept
{
    if (!cvars.interpRatio || !cvars.minInterpRatio || !cvars.maxInterpRatio ||
        !cvars.interp || !cvars.updateRate)
        return 0.0f;

    const float ratio = std::clamp(
        cvars.interpRatio->getFloat(),
        cvars.minInterpRatio->getFloat(),
        cvars.maxInterpRatio->getFloat()
    );

    const float updateRate = cvars.maxUpdateRate ?
        cvars.maxUpdateRate->getFloat() : cvars.updateRate->getFloat();

    // Защита от деления на ноль
    if (updateRate <= 0.0f)
        return cvars.interp->getFloat();

    const float calculatedLerp = ratio / updateRate;

    return (std::max)(cvars.interp->getFloat(), calculatedLerp);
}

// НОВАЯ ФУНКЦИЯ: расчет приоритета backtrack записи
static float calculateRecordPriority(
    const Animations::Players::Record& record,
    Entity* entity,
    const Vector& localEyePos,
    const Vector& aimAngles) noexcept
{
    if (!entity)
        return 0.0f;

    float priority = 0.0f;

    // Приоритет от времени (более свежие записи = лучше)
    const float timeDelta = memory->globalVars->serverTime() - record.simulationTime;
    const float maxUnlag = cvars.maxUnlag ? cvars.maxUnlag->getFloat() : 0.2f;
    const float timeScore = 1.0f - (timeDelta / maxUnlag);
    priority += timeScore * 50.0f;

    // Приоритет от расстояния до позиций костей
    float closestDistance = FLT_MAX;
    for (const auto& pos : record.positions)
    {
        const float distance = (pos - localEyePos).length();
        closestDistance = (std::min)(closestDistance, distance);
    }

    // Ближе = лучше (в пределах разумного)
    if (closestDistance < 4096.0f)
        priority += (1.0f - (closestDistance / 4096.0f)) * 30.0f;

    // Приоритет от origin изменения (большее движение = лучше для backtrack)
    static std::unordered_map<int, Vector> lastOrigins;
    const int entityIndex = entity->index();

    if (lastOrigins.find(entityIndex) != lastOrigins.end())
    {
        const float originDelta = (record.origin - lastOrigins[entityIndex]).length();
        // Если игрок движется быстро - backtrack более эффективен
        if (originDelta > 10.0f)
            priority += (std::min)(originDelta * 0.5f, 20.0f);
    }
    lastOrigins[entityIndex] = record.origin;

    // Приоритет от velocity (движущиеся цели = лучше для backtrack)
    const float velocity = entity->velocity().length2D();
    if (velocity > 50.0f)
        priority += (std::min)(velocity * 0.1f, 15.0f);

    return priority;
}

// УЛУЧШЕННАЯ ФУНКЦИЯ: валидация backtrack записи
bool Backtrack::valid(float simtime) noexcept
{
    if (!cvars.maxUnlag)
        return false;

    const auto network = interfaces->engine->getNetworkChannel();
    if (!network)
        return false;

    const float maxUnlag = cvars.maxUnlag->getFloat();
    const float serverTime = memory->globalVars->serverTime();

    // Проверка 1: не слишком старая запись
    const float deadTime = serverTime - maxUnlag;
    if (simtime < deadTime)
        return false;

    // Проверка 2: не из будущего
    if (simtime > serverTime)
        return false;

    // Проверка 3: учитываем latency и lerp
    const float latency = network->getLatency(0) + network->getLatency(1);
    const float lerp = getLerp();

    // Учитываем tickbase shift если доступен
    const float extraTickbaseDelta = Tickbase::canShiftDT(Tickbase::getTargetTickShift()) ?
        ticksToTime(Tickbase::getTargetTickShift()) : 0.0f;

    // Рассчитываем допустимое окно
    const float totalDelay = std::clamp(latency + lerp, 0.0f, maxUnlag);
    const float adjustedServerTime = serverTime - extraTickbaseDelta;
    const float delta = totalDelay - (adjustedServerTime - simtime);

    // Более строгая проверка для HvH
    constexpr float tolerance = 0.2f; // 200ms tolerance
    return std::abs(delta) <= tolerance;
}

// НОВАЯ ФУНКЦИЯ: проверка качества backtrack записи
static bool isRecordHighQuality(
    const Animations::Players::Record& record,
    Entity* entity,
    const Animations::Players& player) noexcept
{
    if (!entity)
        return false;

    // Проверка 1: запись не слишком старая
    const float age = memory->globalVars->serverTime() - record.simulationTime;
    if (age > 0.2f) // Старше 200ms = низкое качество
        return false;

    // Проверка 2: нет телепорта
    if (!player.backtrackRecords.empty())
    {
        const auto& latestRecord = player.backtrackRecords.front();
        const float distance = (record.origin - latestRecord.origin).length();
        const float timeDelta = latestRecord.simulationTime - record.simulationTime;

        if (timeDelta > 0.0f)
        {
            const float speed = distance / timeDelta;
            // Если скорость > 1000 units/sec = вероятно телепорт
            if (speed > 1000.0f)
                return false;
        }
    }

    // Проверка 3: валидная позиция
    if (record.origin.x == 0.0f && record.origin.y == 0.0f && record.origin.z == 0.0f)
        return false;

    return true;
}

// УЛУЧШЕННАЯ ФУНКЦИЯ: основной backtrack
void Backtrack::run(UserCmd* cmd) noexcept
{
    if (!config->backtrack.enabled)
        return;

    if (!(cmd->buttons & UserCmd::IN_ATTACK))
        return;

    if (!localPlayer || !localPlayer->isAlive())
        return;

    const auto activeWeapon = localPlayer->getActiveWeapon();
    if (!activeWeapon || activeWeapon->isKnife() || activeWeapon->isGrenade())
        return;

    const auto localPlayerEyePosition = localPlayer->getEyePosition();
    const auto aimPunch = activeWeapon->requiresRecoilControl() ?
        localPlayer->getAimPunch() : Vector{};

    std::vector<BacktrackRecordInfo> validRecords;

    // УЛУЧШЕНИЕ: собираем все валидные записи с приоритетами
    for (int i = 1; i <= interfaces->engine->getMaxClients(); i++)
    {
        auto entity = interfaces->entityList->getEntity(i);
        if (!entity || entity == localPlayer.get() || entity->isDormant() ||
            !entity->isAlive() || !entity->isOtherEnemy(localPlayer.get()))
            continue;

        const auto player = Animations::getPlayer(i);
        if (!player.gotMatrix || player.backtrackRecords.empty())
            continue;

        // Проверяем каждую запись
        for (size_t j = 0; j < player.backtrackRecords.size(); j++)
        {
            const auto& record = player.backtrackRecords[j];

            // Валидация записи
            if (!Backtrack::valid(record.simulationTime))
                continue;

            // Проверка качества
            if (!isRecordHighQuality(record, entity, player))
                continue;

            // Находим лучшую позицию кости в этой записи
            float bestFov = FLT_MAX;
            Vector bestPosition;

            for (const auto& position : record.positions)
            {
                const auto angle = AimbotFunction::calculateRelativeAngle(
                    localPlayerEyePosition,
                    position,
                    cmd->viewangles - aimPunch
                );

                const float fov = std::hypot(angle.x, angle.y);

                if (fov < bestFov)
                {
                    bestFov = fov;
                    bestPosition = position;
                }
            }

            // Проверяем FOV лимит
            if (bestFov > config->ragebot.fov)
                continue;

            // Рассчитываем приоритет
            const float priority = calculateRecordPriority(
                record,
                entity,
                localPlayerEyePosition,
                cmd->viewangles - aimPunch
            ) + (100.0f - bestFov); // Добавляем FOV бонус

            // Добавляем запись
            BacktrackRecordInfo info;
            info.playerIndex = i;
            info.recordIndex = static_cast<int>(j);
            info.simulationTime = record.simulationTime;
            info.priority = priority;
            info.bestPosition = bestPosition;
            info.isValid = true;

            validRecords.push_back(info);
        }
    }

    if (validRecords.empty())
        return;

    // Сортируем по приоритету
    std::sort(validRecords.begin(), validRecords.end());

    // Берем лучшую запись
    const auto& bestRecord = validRecords.front();

    // Устанавливаем tick count
    cmd->tickCount = timeToTicks(bestRecord.simulationTime + getLerp());
}

// УЛУЧШЕНИЕ: добавление latency с проверками
void Backtrack::addLatencyToNetwork(NetworkChannel* network, float latency) noexcept
{
    if (!network || sequences.empty())
        return;

    // Ограничиваем latency
    latency = std::clamp(latency, 0.0f, 0.2f); // Максимум 200ms

    for (auto& sequence : sequences)
    {
        const float age = memory->globalVars->serverTime() - sequence.servertime;

        if (age >= latency)
        {
            network->inReliableState = sequence.inreliablestate;
            network->inSequenceNr = sequence.sequencenr;
            break;
        }
    }
}

// УЛУЧШЕНИЕ: обновление sequence с защитой
void Backtrack::updateIncomingSequences() noexcept
{
    static int lastIncomingSequenceNumber = 0;

    if (!localPlayer)
        return;

    auto network = interfaces->engine->getNetworkChannel();
    if (!network)
        return;

    // Проверяем что sequence изменился
    if (network->inSequenceNr != lastIncomingSequenceNumber)
    {
        lastIncomingSequenceNumber = network->inSequenceNr;

        incomingSequence sequence{};
        sequence.inreliablestate = network->inReliableState;
        sequence.sequencenr = network->inSequenceNr;
        sequence.servertime = memory->globalVars->serverTime();

        sequences.push_front(sequence);
    }

    // УЛУЧШЕНИЕ: динамический размер буфера на основе tickrate
    const int maxSequences = (std::max)(timeToTicks(1.0f), 64);
    while (sequences.size() > static_cast<size_t>(maxSequences))
        sequences.pop_back();
}

void Backtrack::init() noexcept
{
    cvars.updateRate = interfaces->cvar->findVar(skCrypt("cl_updaterate"));
    cvars.maxUpdateRate = interfaces->cvar->findVar(skCrypt("sv_maxupdaterate"));
    cvars.interp = interfaces->cvar->findVar(skCrypt("cl_interp"));
    cvars.interpRatio = interfaces->cvar->findVar(skCrypt("cl_interp_ratio"));
    cvars.minInterpRatio = interfaces->cvar->findVar(skCrypt("sv_client_min_interp_ratio"));
    cvars.maxInterpRatio = interfaces->cvar->findVar(skCrypt("sv_client_max_interp_ratio"));
    cvars.maxUnlag = interfaces->cvar->findVar(skCrypt("sv_maxunlag"));

    // Проверяем что все cvars найдены
    if (!cvars.updateRate || !cvars.interp || !cvars.interpRatio ||
        !cvars.minInterpRatio || !cvars.maxInterpRatio || !cvars.maxUnlag)
    {
        // Log error или fallback значения
    }
}

float Backtrack::getMaxUnlag() noexcept
{
    return cvars.maxUnlag ? cvars.maxUnlag->getFloat() : 0.2f;
}
