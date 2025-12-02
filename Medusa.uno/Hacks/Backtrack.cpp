#include "Backtrack.h"
#include "AimbotFunctions.h"
#include "Animations.h"
#include "Tickbase.h"
#include "../Config.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "../SDK/ConVar.h"
#include "../SDK/Entity.h"
#include "../SDK/LocalPlayer.h"
#include "../SDK/NetworkChannel.h"
#include "../SDK/UserCmd.h"
#include "../SDK/GlobalVars.h"
#include "../SDK/Cvar.h"
#include "../xor.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

// ============================================
// CACHED CONVARS
// ============================================

namespace {
    ConVar* cv_updateRate = nullptr;
    ConVar* cv_maxUpdateRate = nullptr;
    ConVar* cv_interp = nullptr;
    ConVar* cv_interpRatio = nullptr;
    ConVar* cv_minInterpRatio = nullptr;
    ConVar* cv_maxInterpRatio = nullptr;
    ConVar* cv_maxUnlag = nullptr;

    // Tracking data for priority calculation
    std::unordered_map<int, Vector> lastOrigins{};
    std::unordered_map<int, float> lastSimTimes{};
}

// ============================================
// INITIALIZATION
// ============================================

void Backtrack::init() noexcept {
    cv_updateRate = interfaces->cvar->findVar("cl_updaterate");
    cv_maxUpdateRate = interfaces->cvar->findVar("sv_maxupdaterate");
    cv_interp = interfaces->cvar->findVar("cl_interp");
    cv_interpRatio = interfaces->cvar->findVar("cl_interp_ratio");
    cv_minInterpRatio = interfaces->cvar->findVar("sv_client_min_interp_ratio");
    cv_maxInterpRatio = interfaces->cvar->findVar("sv_client_max_interp_ratio");
    cv_maxUnlag = interfaces->cvar->findVar("sv_maxunlag");

    updateConVars();
}

void Backtrack::reset() noexcept {
    sequences.clear();
    lastSequenceNumber = 0;
    lastOrigins.clear();
    lastSimTimes.clear();
    conVars.initialized = false;
}

void Backtrack::updateConVars() noexcept {
    if (cv_updateRate)
        conVars.updateRate = cv_updateRate->getFloat();

    if (cv_maxUpdateRate)
        conVars.maxUpdateRate = cv_maxUpdateRate->getFloat();

    if (cv_interp)
        conVars.interp = cv_interp->getFloat();

    if (cv_interpRatio)
        conVars.interpRatio = cv_interpRatio->getFloat();

    if (cv_minInterpRatio)
        conVars.minInterpRatio = cv_minInterpRatio->getFloat();

    if (cv_maxInterpRatio)
        conVars.maxInterpRatio = cv_maxInterpRatio->getFloat();

    if (cv_maxUnlag)
        conVars.maxUnlag = cv_maxUnlag->getFloat();

    conVars.initialized = true;
}

// ============================================
// LERP CALCULATION
// ============================================

float Backtrack::getLerp() noexcept {
    if (!conVars.initialized)
        updateConVars();

    const float ratio = std::clamp(
        conVars.interpRatio,
        conVars.minInterpRatio,
        conVars.maxInterpRatio
    );

    const float updateRate = conVars.maxUpdateRate > 0.f ?
        conVars.maxUpdateRate : conVars.updateRate;

    if (updateRate <= 0.f)
        return conVars.interp;

    const float calculatedLerp = ratio / updateRate;

    return (std::max)(conVars.interp, calculatedLerp);
}

float Backtrack::getMaxUnlag() noexcept {
    if (!conVars.initialized)
        updateConVars();

    return conVars.maxUnlag > 0.f ? conVars.maxUnlag : MAX_UNLAG_DEFAULT;
}

// ============================================
// VALIDATION
// ============================================

bool Backtrack::valid(float simulationTime) noexcept {
    const auto network = interfaces->engine->getNetworkChannel();
    if (!network)
        return false;

    const float maxUnlag = getMaxUnlag();
    const float serverTime = memory->globalVars->serverTime();

    // Check 1: Not too old
    const float deadTime = serverTime - maxUnlag;
    if (simulationTime < deadTime)
        return false;

    // Check 2: Not in future
    if (simulationTime > serverTime)
        return false;

    // Check 3: Account for latency and lerp
    const float latency = network->getLatency(0) + network->getLatency(1);
    const float lerp = getLerp();

    // Account for tickbase shift if available
    float extraDelta = 0.f;
    if (Tickbase::isReady()) {
        extraDelta = ticksToTime(Tickbase::getTargetShift());
    }

    // Calculate valid window
    const float totalDelay = std::clamp(latency + lerp, 0.f, maxUnlag);
    const float adjustedServerTime = serverTime - extraDelta;
    const float delta = totalDelay - (adjustedServerTime - simulationTime);

    // Tolerance check
    return std::abs(delta) <= RECORD_TOLERANCE;
}

bool Backtrack::isRecordValid(const Record& record, float serverTime) noexcept {
    if (!record.isValid)
        return false;

    if (record.dormant)
        return false;

    return valid(record.simulationTime);
}

bool Backtrack::isRecordHighQuality(int playerIndex, const Record& record) noexcept {
    // Check 1: Not too old
    const float age = getRecordAge(record.simulationTime);
    if (age > MAX_UNLAG_DEFAULT)
        return false;

    // Check 2: Valid origin
    if (record.origin.null())
        return false;

    // Check 3: No teleport detection
    auto it = lastOrigins.find(playerIndex);
    if (it != lastOrigins.end()) {
        const float distance = (record.origin - it->second).length();

        auto timeIt = lastSimTimes.find(playerIndex);
        if (timeIt != lastSimTimes.end()) {
            const float timeDelta = record.simulationTime - timeIt->second;

            if (timeDelta > 0.f) {
                const float speed = distance / timeDelta;
                // Speed > 1000 u/s = likely teleport
                if (speed > 1000.f && distance > TELEPORT_THRESHOLD)
                    return false;
            }
        }
    }

    // Update tracking
    lastOrigins[playerIndex] = record.origin;
    lastSimTimes[playerIndex] = record.simulationTime;

    return true;
}

// ============================================
// PRIORITY CALCULATION
// ============================================

float Backtrack::calculatePriority(int playerIndex, const Record& record,
    const Vector& eyePos, const Vector& aimAngles) noexcept {
    float priority = 0.f;

    // Time priority (fresher = better)
    const float age = getRecordAge(record.simulationTime);
    const float maxUnlag = getMaxUnlag();
    const float timeScore = 1.f - std::clamp(age / maxUnlag, 0.f, 1.f);
    priority += timeScore * 50.f;

    // Distance priority
    float closestDist = FLT_MAX;
    for (const auto& pos : record.bonePositions) {
        const float dist = (pos - eyePos).length();
        closestDist = (std::min)(closestDist, dist);
    }

    if (closestDist < 4096.f) {
        priority += (1.f - (closestDist / 4096.f)) * 30.f;
    }

    // Movement priority (moving targets benefit more from backtrack)
    auto it = lastOrigins.find(playerIndex);
    if (it != lastOrigins.end()) {
        const float originDelta = (record.origin - it->second).length();
        if (originDelta > 10.f) {
            priority += (std::min)(originDelta * 0.5f, 20.f);
        }
    }

    // Velocity priority
    const float velocity = record.velocity.length2D();
    if (velocity > 50.f) {
        priority += (std::min)(velocity * 0.1f, 15.f);
    }

    return priority;
}

// ============================================
// RECORD GATHERING
// ============================================

std::vector<Backtrack::RecordPriority> Backtrack::gatherValidRecords(UserCmd* cmd, float maxFov) noexcept {
    std::vector<RecordPriority> validRecords;

    if (!localPlayer || !localPlayer->isAlive())
        return validRecords;

    const auto weapon = localPlayer->getActiveWeapon();
    if (!weapon)
        return validRecords;

    const Vector eyePos = localPlayer->getEyePosition();
    const Vector aimPunch = weapon->requiresRecoilControl() ? localPlayer->getAimPunch() : Vector{};
    const Vector aimAngles = cmd->viewangles - aimPunch;

    for (int i = 1; i <= interfaces->engine->getMaxClients(); i++) {
        const auto entity = interfaces->entityList->getEntity(i);
        if (!entity || entity == localPlayer.get())
            continue;

        if (entity->isDormant() || !entity->isAlive())
            continue;

        if (!entity->isOtherEnemy(localPlayer.get()))
            continue;

        const auto& player = Animations::getPlayer(i);
        if (!player.gotMatrix || player.backtrackRecords.empty())
            continue;

        for (size_t j = 0; j < player.backtrackRecords.size(); j++) {
            const auto& btRecord = player.backtrackRecords[j];

            // Validate
            if (!valid(btRecord.simulationTime))
                continue;

            // Create Record from Animations::Players::Record
            Record record;
            record.simulationTime = btRecord.simulationTime;
            record.origin = btRecord.origin;
            record.absAngle = btRecord.absAngle;
            record.mins = btRecord.mins;
            record.maxs = btRecord.maxs;
            record.isValid = true;

            for (const auto& pos : btRecord.positions) {
                record.bonePositions.push_back(pos);
            }

            // Quality check
            if (!isRecordHighQuality(i, record))
                continue;

            // Find best bone position
            float bestFov = FLT_MAX;
            Vector bestPosition;

            for (const auto& position : btRecord.positions) {
                const auto angle = AimbotFunction::calculateRelativeAngle(
                    eyePos, position, aimAngles
                );

                const float fov = std::hypot(angle.x, angle.y);

                if (fov < bestFov) {
                    bestFov = fov;
                    bestPosition = position;
                }
            }

            // FOV check
            if (bestFov > maxFov)
                continue;

            // Calculate priority
            float priority = calculatePriority(i, record, eyePos, aimAngles);
            priority += (100.f - bestFov); // FOV bonus

            // Add to list
            RecordPriority rp;
            rp.playerIndex = i;
            rp.recordIndex = static_cast<int>(j);
            rp.simulationTime = btRecord.simulationTime;
            rp.priority = priority;
            rp.fov = bestFov;
            rp.bestPosition = bestPosition;
            rp.visible = AimbotFunction::isVisible(entity, bestPosition);

            // Visibility bonus
            if (rp.visible) {
                rp.priority += 25.f;
            }

            validRecords.push_back(rp);
        }
    }

    // Sort by priority
    std::sort(validRecords.begin(), validRecords.end());

    return validRecords;
}

int Backtrack::findBestRecord(int playerIndex, const Vector& eyePos,
    const Vector& aimAngles, float maxFov) noexcept {
    const auto& player = Animations::getPlayer(playerIndex);

    if (!player.gotMatrix || player.backtrackRecords.empty())
        return -1;

    int bestIndex = -1;
    float bestPriority = -FLT_MAX;

    for (size_t i = 0; i < player.backtrackRecords.size(); i++) {
        const auto& btRecord = player.backtrackRecords[i];

        if (!valid(btRecord.simulationTime))
            continue;

        // Create temp record for quality check
        Record record;
        record.simulationTime = btRecord.simulationTime;
        record.origin = btRecord.origin;
        record.isValid = true;

        if (!isRecordHighQuality(playerIndex, record))
            continue;

        // Check FOV
        float bestFov = FLT_MAX;
        for (const auto& pos : btRecord.positions) {
            const auto angle = AimbotFunction::calculateRelativeAngle(
                eyePos, pos, aimAngles
            );
            bestFov = (std::min)(bestFov, std::hypot(angle.x, angle.y));
        }

        if (bestFov > maxFov)
            continue;

        // Calculate priority
        for (const auto& pos : btRecord.positions) {
            record.bonePositions.push_back(pos);
        }

        float priority = calculatePriority(playerIndex, record, eyePos, aimAngles);
        priority += (100.f - bestFov);

        if (priority > bestPriority) {
            bestPriority = priority;
            bestIndex = static_cast<int>(i);
        }
    }

    return bestIndex;
}

// ============================================
// MAIN BACKTRACK
// ============================================

void Backtrack::run(UserCmd* cmd) noexcept {
    if (!config->backtrack.enabled)
        return;

    if (!(cmd->buttons & UserCmd::IN_ATTACK))
        return;

    if (!localPlayer || !localPlayer->isAlive())
        return;

    const auto weapon = localPlayer->getActiveWeapon();
    if (!weapon || weapon->isKnife() || weapon->isGrenade())
        return;

    // Gather all valid records with priorities
    auto validRecords = gatherValidRecords(cmd, config->ragebot.fov);

    if (validRecords.empty())
        return;

    // Get best record (already sorted by priority)
    const auto& bestRecord = validRecords.front();

    // Set tick count
    cmd->tickCount = getTickCount(bestRecord.simulationTime);
}

void Backtrack::update() noexcept {
    // Update convars periodically
    static float lastUpdate = 0.f;
    const float currentTime = memory->globalVars->realtime;

    if (currentTime - lastUpdate > 1.f) {
        updateConVars();
        lastUpdate = currentTime;
    }

    // Update sequences
    updateIncomingSequences();
}

// ============================================
// FAKE LATENCY
// ============================================

void Backtrack::addLatencyToNetwork(NetworkChannel* network, float latency) noexcept {
    if (!network || sequences.empty())
        return;

    // Clamp latency
    latency = std::clamp(latency, 0.f, MAX_UNLAG_DEFAULT);

    const float serverTime = memory->globalVars->serverTime();

    for (const auto& sequence : sequences) {
        const float age = serverTime - sequence.serverTime;

        if (age >= latency) {
            network->inReliableState = sequence.inReliableState;
            network->inSequenceNr = sequence.sequenceNr;
            break;
        }
    }
}

void Backtrack::updateIncomingSequences() noexcept {
    if (!localPlayer)
        return;

    auto* network = interfaces->engine->getNetworkChannel();
    if (!network)
        return;

    // Check if sequence changed
    if (network->inSequenceNr == lastSequenceNumber)
        return;

    lastSequenceNumber = network->inSequenceNr;

    // Add new sequence
    IncomingSequence sequence;
    sequence.inReliableState = network->inReliableState;
    sequence.sequenceNr = network->inSequenceNr;
    sequence.serverTime = memory->globalVars->serverTime();

    sequences.push_front(sequence);

    // Limit buffer size
    const int maxSequences = (std::max)(timeToTicks(1.f), 64);
    while (sequences.size() > static_cast<size_t>(maxSequences)) {
        sequences.pop_back();
    }
}

// ============================================
// UTILITY
// ============================================

float Backtrack::getRecordAge(float simulationTime) noexcept {
    return memory->globalVars->serverTime() - simulationTime;
}

int Backtrack::getTickCount(float simulationTime) noexcept {
    return timeToTicks(simulationTime + getLerp());
}

// ============================================
// GETTERS
// ============================================

const std::deque<Backtrack::IncomingSequence>& Backtrack::getSequences() noexcept {
    return sequences;
}

const Backtrack::ConVarCache& Backtrack::getConVars() noexcept {
    if (!conVars.initialized)
        updateConVars();
    return conVars;
}

bool Backtrack::isEnabled() noexcept {
    return config->backtrack.enabled;
}

float Backtrack::getFakeLatency() noexcept {
    if (!config->backtrack.fakeLatency)
        return 0.f;

    return static_cast<float>(config->backtrack.fakeLatencyAmount) / 1000.f;
}