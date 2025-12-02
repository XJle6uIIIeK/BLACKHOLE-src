#include "Resolver.h"
#include "AimbotFunctions.h"
#include "Backtrack.h"
#include "Tickbase.h"
#include "../Logger.h"
#include "../SDK/GameEvent.h"
#include "../SDK/NetworkChannel.h"
#include "../GameData.h"
#include "../Helpers.h"
#include "../Memory.h"
#include "../Interfaces.h"
#include "../Config.h"
#include "../ThreadPool.h"
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>

// ============================================
// LOGGING HELPERS
// ============================================

namespace {
    std::string formatFloat(float value, int precision = 1) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(precision) << value;
        return ss.str();
    }

    std::string formatTime() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::ostringstream ss;
        ss << std::put_time(std::localtime(&time), "%H:%M:%S");
        ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }

    std::string getPlayerName(int index) {
        PlayerInfo info;
        if (interfaces->engine->getPlayerInfo(index, info)) {
            return std::string(info.name);
        }
        return "Unknown";
    }

    float getCurrentTime() noexcept {
        return memory->globalVars->realtime;
    }

    float getServerTime() noexcept {
        return memory->globalVars->serverTime();
    }
}

// ============================================
// TYPE NAME FUNCTIONS
// ============================================

std::string Resolver::getMissTypeName(MissType type) noexcept {
    switch (type) {
    case MissType::SPREAD: return "Spread";
    case MissType::RESOLVER_WRONG_SIDE: return "Resolver (Wrong Side)";
    case MissType::DESYNC_CORRECTION: return "Desync Correction";
    case MissType::JITTER_CORRECTION: return "Jitter Correction";
    case MissType::PREDICTION_ERROR: return "Prediction Error";
    case MissType::MISPREDICTION: return "Misprediction";
    case MissType::DAMAGE_REJECTION: return "Damage Rejection";
    case MissType::SERVER_REJECTION: return "Server Rejection";
    case MissType::UNREGISTERED_SHOT: return "Unregistered Shot";
    case MissType::BACKTRACK_FAILURE: return "Backtrack Failure";
    case MissType::OCCLUSION: return "Occlusion";
    case MissType::TICKBASE_MISMATCH: return "Tickbase Mismatch";
    case MissType::ANIMATION_DESYNC: return "Animation Desync";
    case MissType::HITBOX_MISMATCH: return "Hitbox Mismatch";
    case MissType::NETWORK_LOSS: return "Network Loss";
    case MissType::ANTI_AIM_EXPLOIT: return "Anti-Aim Exploit";
    case MissType::EXTENDED_DESYNC: return "Extended Desync";
    case MissType::LBY_BREAK: return "LBY Break";
    case MissType::FAKE_FLICK: return "Fake Flick";
    default: return "Unknown";
    }
}

std::string Resolver::getHitTypeName(HitType type) noexcept {
    switch (type) {
    case HitType::RESOLVER_CORRECT: return "Resolver Correct";
    case HitType::LBY_PREDICTION: return "LBY Prediction";
    case HitType::JITTER_PREDICTION: return "Jitter Prediction";
    case HitType::BRUTEFORCE_HIT: return "Bruteforce";
    case HitType::FREESTANDING_HIT: return "Freestanding";
    case HitType::BACKTRACK_HIT: return "Backtrack";
    case HitType::ANIMSTATE_CORRECT: return "AnimState";
    case HitType::POSE_PARAM_CORRECT: return "Pose Param";
    case HitType::PREDICTION_CORRECT: return "Prediction";
    case HitType::TRACE_CORRECT: return "Trace";
    default: return "Unknown";
    }
}

std::string Resolver::getMethodName(ResolveMethod method) noexcept {
    switch (method) {
    case ResolveMethod::LBY: return "LBY";
    case ResolveMethod::ANIMSTATE: return "AnimState";
    case ResolveMethod::POSE_PARAM: return "PoseParam";
    case ResolveMethod::TRACE: return "Trace";
    case ResolveMethod::BRUTEFORCE: return "Bruteforce";
    case ResolveMethod::FREESTANDING: return "Freestanding";
    case ResolveMethod::LAST_MOVING: return "LastMoving";
    case ResolveMethod::PREDICTION: return "Prediction";
    default: return "Unknown";
    }
}

std::string Resolver::getModeName(ResolveMode mode) noexcept {
    switch (mode) {
    case ResolveMode::STANDING: return "Standing";
    case ResolveMode::MOVING: return "Moving";
    case ResolveMode::AIR: return "Air";
    case ResolveMode::SLOW_WALK: return "SlowWalk";
    case ResolveMode::FAKE_DUCK: return "FakeDuck";
    case ResolveMode::LADDER: return "Ladder";
    default: return "None";
    }
}

std::string Resolver::getSideName(ResolveSide side) noexcept {
    switch (side) {
    case ResolveSide::LEFT: return "Left";
    case ResolveSide::RIGHT: return "Right";
    default: return "Center";
    }
}

// ============================================
// INITIALIZATION
// ============================================

void Resolver::initialize() noexcept {
    for (auto& d : data) {
        d.reset();
    }
    snapshots.clear();
    recentLogs.clear();
    globalStats.reset();
}

void Resolver::reset() noexcept {
    initialize();
}

// ============================================
// EXTENDED LOGGING
// ============================================

void Resolver::logHit(int playerIndex, HitType type, const std::string& details) noexcept {
    if (playerIndex < 1 || playerIndex > 64)
        return;

    auto& info = data[playerIndex];
    auto& stats = info.stats;

    // Update statistics
    stats.hits++;
    stats.hit_types[static_cast<int>(type)]++;
    stats.consecutive_hits++;
    stats.consecutive_misses = 0;
    stats.max_consecutive_hits = (std::max)(stats.max_consecutive_hits, stats.consecutive_hits);
    stats.last_hit_time = memory->globalVars->currenttime;

    // Update side statistics
    switch (info.side) {
    case ResolveSide::LEFT:
        stats.left_hits++;
        break;
    case ResolveSide::RIGHT:
        stats.right_hits++;
        break;
    default:
        stats.center_hits++;
        break;
    }

    // Update method statistics
    stats.method_successes[static_cast<int>(info.winning_method)]++;

    // Update global stats
    globalStats.hits++;
    globalStats.hit_types[static_cast<int>(type)]++;

    // Format log message
    std::ostringstream log;;
    log << "[HIT] ";
    log << getPlayerName(playerIndex) << " | ";
    log << getHitTypeName(type) << " | ";
    log << "Method: " << getMethodName(info.winning_method) << " | ";
    log << "Side: " << getSideName(info.side) << " | ";
    log << "Conf: " << static_cast<int>(info.confidence * 100) << "% | ";
    log << details;

    std::string logStr = log.str();
    Logger::addLog(logStr);

    // Store recent log
    recentLogs.push_back(logStr);
    while (recentLogs.size() > MAX_LOGS)
        recentLogs.pop_front();
}

void Resolver::logMiss(int playerIndex, MissType type, const std::string& details) noexcept {
    if (playerIndex < 1 || playerIndex > 64)
        return;

    auto& info = data[playerIndex];
    auto& stats = info.stats;

    // Update statistics
    stats.misses++;
    stats.miss_types[static_cast<int>(type)]++;
    stats.consecutive_misses++;
    stats.consecutive_hits = 0;
    stats.max_consecutive_misses = (std::max)(stats.max_consecutive_misses, stats.consecutive_misses);
    stats.last_miss_time = memory->globalVars->currenttime;

    // Update legacy counter
    info.misses++;

    // Update side statistics
    switch (info.side) {
    case ResolveSide::LEFT:
        stats.left_misses++;
        break;
    case ResolveSide::RIGHT:
        stats.right_misses++;
        break;
    default:
        stats.center_misses++;
        break;
    }

    // Update global stats
    globalStats.misses++;
    globalStats.miss_types[static_cast<int>(type)]++;

    // Format log message
    std::ostringstream log;
    log << "[MISS] ";
    log << getPlayerName(playerIndex) << " | ";
    log << getMissTypeName(type) << " | ";
    log << "Method: " << getMethodName(info.winning_method) << " | ";
    log << "Side: " << getSideName(info.side) << " | ";
    log << "Conf: " << static_cast<int>(info.confidence * 100) << "% | ";
    log << "Misses: " << info.misses << " | ";
    log << details;

    std::string logStr = log.str();
    Logger::addLog(logStr);

    // Store recent log
    recentLogs.push_back(logStr);
    while (recentLogs.size() > MAX_LOGS)
        recentLogs.pop_front();
}

// ============================================
// SHOT ANALYSIS
// ============================================

Resolver::MissType Resolver::determineMissType(ShotSnapshot& snapshot) noexcept {
    const auto& state = snapshot.resolver_state;

    // Check for network issues
    if (auto* netChannel = interfaces->engine->getNetworkChannel()) {
        float latency = netChannel->getLatency(0);
        int choked = netChannel->chokedPackets;

        if (latency > 0.2f || choked > 10) {
            return MissType::NETWORK_LOSS;
        }
    }

    // Check for backtrack failure
    if (snapshot.used_backtrack) {
        float timeDelta = std::fabsf(snapshot.simulation_time - snapshot.backtrack_simtime);
        if (timeDelta > 0.2f) {
            return MissType::BACKTRACK_FAILURE;
        }
    }

    // Check for tickbase issues
    if (Tickbase::isShifting() || Tickbase::isRecharging()) {
        return MissType::TICKBASE_MISMATCH;
    }

    // Check for server rejection
    if (!snapshot.got_impact && !snapshot.got_hurt_event) {
        float timeSinceShot = memory->globalVars->serverTime() - snapshot.time;
        if (timeSinceShot > 0.5f) {
            return MissType::SERVER_REJECTION;
        }
        return MissType::UNREGISTERED_SHOT;
    }

    // Check if impact was far from target
    if (snapshot.got_impact) {
        float impactDistance = snapshot.impact_position.distTo(snapshot.aim_point);

        // Large spread miss
        if (impactDistance > 50.f) {
            return MissType::SPREAD;
        }

        // Occlusion check
        const auto entity = interfaces->entityList->getEntity(snapshot.player_index);
        if (entity) {
            Trace trace;
            interfaces->engineTrace->traceRay(
                { snapshot.eye_position, snapshot.impact_position },
                MASK_SHOT, { localPlayer.get() }, trace
            );

            if (trace.entity != entity && trace.fraction < 0.9f) {
                return MissType::OCCLUSION;
            }
        }
    }

    // Check jitter correction
    if (state.jitter.is_jittering) {
        return MissType::JITTER_CORRECTION;
    }

    // Check extended desync
    if (state.extended_desync) {
        return MissType::EXTENDED_DESYNC;
    }

    // Check LBY break
    if (state.lby.break_predicted && !state.lby.updated_this_tick) {
        return MissType::LBY_BREAK;
    }

    // Check if we predicted movement wrong
    if (state.is_moving) {
        Vector predictedPos = state.last_origin + state.last_velocity * memory->globalVars->intervalPerTick;
        const auto entity = interfaces->entityList->getEntity(snapshot.player_index);
        if (entity) {
            float posDelta = predictedPos.distTo(entity->getAbsOrigin());
            if (posDelta > 20.f) {
                return MissType::MISPREDICTION;
            }
        }
    }

    // Check for anti-aim exploit
    const auto entity = interfaces->entityList->getEntity(snapshot.player_index);
    if (entity) {
        auto* animState = entity->getAnimstate();
        if (animState) {
            float footYaw = animState->footYaw;
            float eyeYaw = entity->eyeAngles().y;
            float desync = std::fabsf(angleDiff(footYaw, eyeYaw));

            if (desync > 58.f) {
                return MissType::ANTI_AIM_EXPLOIT;
            }
        }
    }

    // Check animation desync
    if (state.animstate.layer_deltas[6].weight_delta > 0.3f) {
        return MissType::ANIMATION_DESYNC;
    }

    // Check for fake flick
    if (state.lby.is_flicking) {
        return MissType::FAKE_FLICK;
    }

    // Check desync correction
    if (state.is_low_delta) {
        return MissType::DESYNC_CORRECTION;
    }

    // Default to wrong side
    return MissType::RESOLVER_WRONG_SIDE;
}

Resolver::HitType Resolver::determineHitType(ShotSnapshot& snapshot) noexcept {
    const auto& state = snapshot.resolver_state;

    // Check backtrack hit
    if (snapshot.used_backtrack && snapshot.backtrack_ticks > 2) {
        return HitType::BACKTRACK_HIT;
    }

    // Check which method was used
    switch (state.winning_method) {
    case ResolveMethod::LBY:
        if (state.lby.updated_this_tick || state.lby.break_predicted) {
            return HitType::LBY_PREDICTION;
        }
        return HitType::RESOLVER_CORRECT;

    case ResolveMethod::ANIMSTATE:
        return HitType::ANIMSTATE_CORRECT;

    case ResolveMethod::POSE_PARAM:
        return HitType::POSE_PARAM_CORRECT;

    case ResolveMethod::TRACE:
        return HitType::TRACE_CORRECT;

    case ResolveMethod::BRUTEFORCE:
        return HitType::BRUTEFORCE_HIT;

    case ResolveMethod::FREESTANDING:
        return HitType::FREESTANDING_HIT;

    case ResolveMethod::PREDICTION:
        return HitType::PREDICTION_CORRECT;

    default:
        break;
    }

    // Check jitter prediction
    if (state.jitter.is_jittering && state.jitter.pattern_detected) {
        return HitType::JITTER_PREDICTION;
    }

    return HitType::RESOLVER_CORRECT;
}

void Resolver::analyzeShot(ShotSnapshot& snapshot) noexcept {
    if (!snapshot.got_impact && !snapshot.got_hurt_event) {
        return;
    }

    const auto entity = interfaces->entityList->getEntity(snapshot.player_index);
    if (!entity) {
        snapshot.analysis_result = "Target invalid";
        return;
    }

    std::ostringstream analysis;
    // Backtrack info
    if (snapshot.used_backtrack) {
        analysis << " | BT: " << snapshot.backtrack_ticks << " ticks";
    }

    // Result
    if (snapshot.got_hurt_event) {
        snapshot.hit_type = determineHitType(snapshot);
        analysis << " | Actual DMG: " << snapshot.actual_damage;
        logHit(snapshot.player_index, snapshot.hit_type, analysis.str());
    }
    else {
        snapshot.miss_type = determineMissType(snapshot);

        // Add specific miss details
        switch (snapshot.miss_type) {
        case MissType::SPREAD:
            analysis << " (Impact dist: " << formatFloat(
                snapshot.impact_position.distTo(snapshot.aim_point)) << ")";
            break;
        case MissType::BACKTRACK_FAILURE:
            analysis << " (Simtime delta: " << formatFloat(
                std::fabsf(snapshot.simulation_time - snapshot.backtrack_simtime) * 1000) << "ms)";
            break;
        case MissType::NETWORK_LOSS:
            if (auto* netChannel = interfaces->engine->getNetworkChannel()) {
                analysis << " (Latency: " << formatFloat(netChannel->getLatency(0) * 1000) << "ms)";
            }
            break;
        case MissType::TICKBASE_MISMATCH:
            analysis << " (Tickbase shifting/recharging)";
            break;
        case MissType::JITTER_CORRECTION:
            analysis << " (Jitter range: " << formatFloat(snapshot.resolver_state.jitter.jitter_range) << ")";
            break;
        case MissType::MISPREDICTION:
            analysis << " (Movement prediction failed)";
            break;
        default:
            break;
        }

        logMiss(snapshot.player_index, snapshot.miss_type, analysis.str());
    }

    snapshot.analysis_result = analysis.str();
}

// ============================================
// SAVE SHOT
// ============================================

void Resolver::saveShot(int playerIndex, float simTime, int backtrackTick,
    const Vector& aimPoint, int hitbox, float hitchance, float damage) noexcept {

    if (!localPlayer || playerIndex < 1 || playerIndex > 64)
        return;

    const auto entity = interfaces->entityList->getEntity(playerIndex);
    const auto& player = Animations::getPlayer(playerIndex);

    if (!entity || !player.gotMatrix)
        return;

    ShotSnapshot snapshot;
    snapshot.player_index = playerIndex;
    snapshot.backtrack_tick = backtrackTick;
    snapshot.simulation_time = simTime;
    snapshot.eye_position = localPlayer->getEyePosition();
    snapshot.aim_point = aimPoint;
    snapshot.model = entity->getModel();
    snapshot.time = -1.f;
    snapshot.got_impact = false;
    snapshot.got_hurt_event = false;

    // Shot details
    snapshot.hitbox_targeted = hitbox;
    snapshot.hitchance_predicted = hitchance;
    snapshot.damage_predicted = damage;

    // Backtrack data
    snapshot.used_backtrack = backtrackTick > 0;
    snapshot.backtrack_ticks = backtrackTick;

    snapshot.backtrack_simtime = player.backtrackRecords[playerIndex].simulationTime;
    

    // Timing
    snapshot.client_time = memory->globalVars->currenttime;
    snapshot.server_time = memory->globalVars->serverTime();
    snapshot.client_tick = memory->globalVars->tickCount;

    // Copy resolver state
    snapshot.resolver_state = data[playerIndex];

    // Copy matrix
    std::memcpy(snapshot.matrix, player.matrix.data(), sizeof(snapshot.matrix));

    // Add to queue
    snapshots.push_back(std::move(snapshot));

    while (snapshots.size() > MAX_SNAPSHOTS)
        snapshots.pop_front();

    // Update statistics
    data[playerIndex].stats.total_shots++;
    data[playerIndex].stats.last_shot_time = memory->globalVars->currenttime;
    data[playerIndex].stats.method_attempts[static_cast<int>(data[playerIndex].winning_method)]++;
    data[playerIndex].total_shots++;

    if (snapshot.used_backtrack) {
        data[playerIndex].stats.backtrack_attempts++;
    }

    globalStats.total_shots++;
    globalStats.method_attempts[static_cast<int>(data[playerIndex].winning_method)]++;
}

// ============================================
// PROCESS MISSED SHOTS
// ============================================

void Resolver::processMissedShots() noexcept {
    if (!enabled || snapshots.empty())
        return;

    if (!localPlayer || !localPlayer->isAlive()) {
        snapshots.clear();
        return;
    }

    auto& snapshot = snapshots.front();

    // Wait for time to be set
    if (snapshot.time < 0.f)
        return;

    // Timeout check
    float timeSinceShot = memory->globalVars->serverTime() - snapshot.time;
    if (timeSinceShot > 1.f) {
        // Timeout - treat as unregistered
        if (!snapshot.got_impact && !snapshot.got_hurt_event) {
            snapshot.miss_type = MissType::UNREGISTERED_SHOT;

            std::ostringstream details;
            details << "Shot timed out after " << formatFloat(timeSinceShot * 1000) << "ms";
            details << " | No impact/hurt event received";
            details << " | Command: " << snapshot.command_number;

            logMiss(snapshot.player_index, MissType::UNREGISTERED_SHOT, details.str());

            // Update backtrack stats if applicable
            if (snapshot.used_backtrack) {
                data[snapshot.player_index].stats.backtrack_failures++;
            }
        }
        snapshots.pop_front();
        return;
    }

    // Wait for impact
    if (!snapshot.got_impact)
        return;

    // Analyze the shot
    const auto entity = interfaces->entityList->getEntity(snapshot.player_index);
    if (!entity) {
        snapshots.pop_front();
        return;
    }

    const Model* model = snapshot.model;
    if (!model) {
        snapshots.pop_front();
        return;
    }

    StudioHdr* hdr = interfaces->modelInfo->getStudioModel(model);
    if (!hdr) {
        snapshots.pop_front();
        return;
    }

    StudioHitboxSet* set = hdr->getHitboxSet(0);
    if (!set) {
        snapshots.pop_front();
        return;
    }

    // Calculate trace from impact
    const auto angle = AimbotFunction::calculateRelativeAngle(
        snapshot.eye_position,
        snapshot.impact_position,
        Vector{}
    );
    const auto end = snapshot.impact_position + Vector::fromAngle(angle) * 2000.f;

    // Check if we would have hit any hitbox with stored matrix
    bool wouldHit = false;
    int hitHitbox = -1;

    for (int hitbox = 0; hitbox < Hitboxes::Max; hitbox++) {
        if (AimbotFunction::hitboxIntersection(snapshot.matrix, hitbox, set,
            snapshot.eye_position, end)) {
            wouldHit = true;
            hitHitbox = hitbox;
            break;
        }
    }

    if (wouldHit && !snapshot.got_hurt_event) {
        // Resolver miss - bullet passed through where hitbox should be
        analyzeShot(snapshot);

        // Update backtrack statistics
        if (snapshot.used_backtrack) {
            data[snapshot.player_index].stats.backtrack_failures++;
        }
    }

    snapshots.pop_front();
}

// ============================================
// EVENT HANDLING
// ============================================

void Resolver::processEvents(GameEvent* event) noexcept {
    if (!event || !localPlayer || interfaces->engine->isHLTV())
        return;

    switch (fnv::hashRuntime(event->getName())) {
    case fnv::hash("round_start"): {
        // Reset all statistics
        for (auto& d : data) {
            d.stats.reset();
            d.misses = 0;
            d.hits = 0;
            d.total_shots = 0;
        }
        globalStats.reset();
        snapshots.clear();
        recentLogs.clear();

        Logger::addLog("[Resolver] Round started - statistics reset");
        break;
    }

    case fnv::hash("player_hurt"): {
        const int attackerId = event->getInt("attacker");
        const int victimId = event->getInt("userid");

        if (attackerId != localPlayer->getUserId())
            break;

        const int victimIndex = interfaces->engine->getPlayerFromUserID(victimId);
        if (victimIndex < 1 || victimIndex > 64)
            break;

        const int damage = event->getInt("dmg_health");
        const int hitgroup = event->getInt("hitgroup");
        const int health = event->getInt("health");
        const int armor = event->getInt("dmg_armor");

        // Find matching snapshot
        bool foundSnapshot = false;
        for (auto& snapshot : snapshots) {
            if (snapshot.player_index == victimIndex && !snapshot.got_hurt_event) {
                snapshot.got_hurt_event = true;
                snapshot.actual_damage = damage;
                snapshot.actual_hitgroup = hitgroup;

                // Analyze and log
                analyzeShot(snapshot);

                // Update backtrack statistics
                if (snapshot.used_backtrack) {
                    data[victimIndex].stats.backtrack_successes++;
                }

                foundSnapshot = true;
                break;
            }
        }

        // Update basic statistics
        data[victimIndex].hits++;
        data[victimIndex].stats.hits++;

        // If no snapshot found, log basic hit
        if (!foundSnapshot) {
            std::ostringstream log;
            log << "[HIT] " << getPlayerName(victimIndex);
            log << " | DMG: " << damage;
            log << " | Armor DMG: " << armor;
            log << " | Hitgroup: " << hitgroup;
            log << " | Remaining HP: " << health;
            log << " | Method: " << getMethodName(data[victimIndex].winning_method);
            log << " | Side: " << getSideName(data[victimIndex].side);
            log << " | Conf: " << static_cast<int>(data[victimIndex].confidence * 100) << "%";

            Logger::addLog(log.str());

            // Update hit statistics
            HitType hitType = HitType::RESOLVER_CORRECT;
            data[victimIndex].stats.hit_types[static_cast<int>(hitType)]++;
            data[victimIndex].stats.method_successes[static_cast<int>(data[victimIndex].winning_method)]++;
            globalStats.hits++;
        }
        break;
    }

    case fnv::hash("bullet_impact"): {
        if (snapshots.empty())
            break;

        if (event->getInt("userid") != localPlayer->getUserId())
            break;

        const Vector impactPos{
            event->getFloat("x"),
            event->getFloat("y"),
            event->getFloat("z")
        };

        // Find first snapshot without impact
        for (auto& snapshot : snapshots) {
            if (!snapshot.got_impact) {
                snapshot.time = memory->globalVars->serverTime();
                snapshot.impact_position = impactPos;
                snapshot.got_impact = true;
                break;
            }
        }
        break;
    }

    case fnv::hash("player_death"): {
        const int attackerId = event->getInt("attacker");
        const int victimId = event->getInt("userid");

        if (attackerId == localPlayer->getUserId()) {
            const int victimIndex = interfaces->engine->getPlayerFromUserID(victimId);
            if (victimIndex > 0 && victimIndex < 65) {
                // Reset player data after kill
                data[victimIndex].reset();
            }
        }

        // If we died, clear snapshots
        if (victimId == localPlayer->getUserId()) {
            snapshots.clear();
            Logger::addLog("[Resolver] Local player died - clearing shot queue");
        }
        break;
    }

    case fnv::hash("weapon_fire"): {
        const int playerId = event->getInt("userid");
        if (playerId == localPlayer->getUserId())
            break;

        const int index = interfaces->engine->getPlayerFromUserID(playerId);
        if (index > 0 && index < 65) {
            auto* player = Animations::setPlayer(index);
            if (player) {
                player->shot = true;
            }
        }
        break;
    }

    case fnv::hash("player_spawn"): {
        const int playerId = event->getInt("userid");
        const int index = interfaces->engine->getPlayerFromUserID(playerId);

        if (index > 0 && index < 65) {
            // Reset resolver data for respawned player
            data[index].reset();
        }
        break;
    }


    case fnv::hash("cs_win_panel_match"): {
        // Log match summary
        std::ostringstream log;
        log << "[Match Summary] ";
        log << "Total shots: " << globalStats.total_shots;
        log << " | Hits: " << globalStats.hits;
        log << " | Misses: " << globalStats.misses;

        float accuracy = globalStats.total_shots > 0
            ? static_cast<float>(globalStats.hits) / globalStats.total_shots * 100.f
            : 0.f;
        log << " | Overall accuracy: " << formatFloat(accuracy) << "%";

        // Log miss breakdown
        log << std::endl << "Miss breakdown: ";
        for (int i = 1; i < static_cast<int>(MissType::COUNT); i++) {
            if (globalStats.miss_types[i] > 0) {
                log << getMissTypeName(static_cast<MissType>(i)) << ": " << globalStats.miss_types[i] << " ";
            }
        }

        Logger::addLog(log.str());
        break;
    }
    }
}

void Resolver::updateEventListeners(bool forceRemove) noexcept {
    class ResolverEventListener : public GameEventListener {
    public:
        void fireGameEvent(GameEvent* event) override {
            Resolver::processEvents(event);
        }
    };

    static ResolverEventListener listener;
    static bool registered = false;

    if (enabled && !registered && !forceRemove) {
        interfaces->gameEventManager->addListener(&listener, "bullet_impact");
        interfaces->gameEventManager->addListener(&listener, "player_hurt");
        interfaces->gameEventManager->addListener(&listener, "round_start");
        interfaces->gameEventManager->addListener(&listener, "round_end");
        interfaces->gameEventManager->addListener(&listener, "weapon_fire");
        interfaces->gameEventManager->addListener(&listener, "player_death");
        interfaces->gameEventManager->addListener(&listener, "player_spawn");
        interfaces->gameEventManager->addListener(&listener, "cs_win_panel_match");
        registered = true;
    }
    else if ((!enabled || forceRemove) && registered) {
        interfaces->gameEventManager->removeListener(&listener);
        registered = false;
    }
}

// ============================================
// MAIN UPDATE FUNCTION
// ============================================

void Resolver::update(Entity* entity, Animations::Players& player) noexcept {
    if (!enabled || !entity || !entity->isAlive())
        return;

    if (entity->isDormant() || entity->isBot())
        return;

    if (!localPlayer || entity->team() == localPlayer->team())
        return;

    const int index = entity->index();
    if (index < 1 || index > 64)
        return;

    auto& info = data[index];
    auto* animState = entity->getAnimstate();

    if (!animState)
        return;

    // Store previous side
    info.last_side = info.side;

    // Update mode
    info.mode = detectMode(entity, info);
    info.is_on_ground = (entity->flags() & FL_ONGROUND) != 0;
    info.is_moving = entity->velocity().length2D() > MOVING_SPEED_THRESHOLD;

    // Store eye angles history
    info.eye_angles_history[info.eye_angles_index % 32] = entity->eyeAngles();
    info.eye_angles_index++;

    // Store movement data
    info.last_velocity = entity->velocity();
    info.last_origin = entity->getAbsOrigin();

    // Update all resolver data
    updateLBYData(entity, info);
    updateAnimStateData(entity, info);
    updatePoseParamData(entity, info);


    // Detect states
    info.jitter.is_jittering = detectJitter(entity, info);
    detectJitterPattern(entity, info);
    info.is_low_delta = detectLowDelta(entity, info);
    info.is_faking = detectFakeAngles(entity);
    info.extended_desync = detectExtendedDesync(entity, info);
    info.is_fake_walking = detectFakeWalk(entity, info);
    info.is_fake_ducking = detectFakeDuck(entity, info);
    predictLBYBreak(entity, info);

    // Collect votes from all methods
    collectVotes(entity, info);

    // Process votes and determine final angle
    processVotes(entity, info);

    // Calculate and apply final yaw
    float finalYaw = calculateFinalYaw(entity, info);
    animState->footYaw = buildServerAbsYaw(entity, finalYaw);

    // Update player data
    player.side = static_cast<int>(info.side);
    info.last_resolve_time = memory->globalVars->currenttime;
}

// ============================================
// VOTING SYSTEM
// ============================================

void Resolver::collectVotes(Entity* entity, ResolverData& info) noexcept {
    // Clear previous votes
    for (auto& vote : info.votes) {
        vote = MethodVote();
    }

    // Collect vote from each method
    info.votes[static_cast<int>(ResolveMethod::LBY)] = resolveLBY(entity, info);
    info.votes[static_cast<int>(ResolveMethod::ANIMSTATE)] = resolveAnimState(entity, info);
    info.votes[static_cast<int>(ResolveMethod::POSE_PARAM)] = resolvePoseParams(entity, info);
    info.votes[static_cast<int>(ResolveMethod::TRACE)] = resolveTrace(entity, info);
    info.votes[static_cast<int>(ResolveMethod::BRUTEFORCE)] = resolveBruteforce(entity, info);
    info.votes[static_cast<int>(ResolveMethod::FREESTANDING)] = resolveFreestanding(entity, info);
    info.votes[static_cast<int>(ResolveMethod::LAST_MOVING)] = resolveLastMoving(entity, info);
    info.votes[static_cast<int>(ResolveMethod::PREDICTION)] = resolvePrediction(entity, info);
}

void Resolver::processVotes(Entity* entity, ResolverData& info) noexcept {
    // Calculate weighted scores for each side
    float leftScore = 0.f;
    float rightScore = 0.f;
    float centerScore = 0.f;

    float totalWeight = 0.f;
    float bestConfidence = 0.f;
    ResolveMethod bestMethod = ResolveMethod::LBY;
    float bestYaw = entity->eyeAngles().y;

    for (int i = 0; i < static_cast<int>(ResolveMethod::COUNT); i++) {
        const auto& vote = info.votes[i];
        if (!vote.valid)
            continue;

        float weight = getMethodWeight(static_cast<ResolveMethod>(i), info);
        float weightedConfidence = vote.confidence * weight;

        switch (vote.side) {
        case ResolveSide::LEFT:
            leftScore += weightedConfidence;
            break;
        case ResolveSide::RIGHT:
            rightScore += weightedConfidence;
            break;
        case ResolveSide::ORIGINAL:
            centerScore += weightedConfidence;
            break;
        }

        totalWeight += weight;

        // Track best individual method
        if (weightedConfidence > bestConfidence) {
            bestConfidence = weightedConfidence;
            bestMethod = static_cast<ResolveMethod>(i);
            bestYaw = vote.suggested_yaw;
        }
    }

    // Normalize scores
    if (totalWeight > 0.f) {
        leftScore /= totalWeight;
        rightScore /= totalWeight;
        centerScore /= totalWeight;
    }

    // Determine winning side
    if (leftScore > rightScore && leftScore > centerScore) {
        info.side = ResolveSide::LEFT;
        info.confidence = leftScore;
    }
    else if (rightScore > leftScore && rightScore > centerScore) {
        info.side = ResolveSide::RIGHT;
        info.confidence = rightScore;
    }
    else {
        info.side = ResolveSide::ORIGINAL;
        info.confidence = centerScore;
    }

    info.winning_method = bestMethod;

    // Calculate weighted average yaw from all valid votes
    float sinSum = 0.f;
    float cosSum = 0.f;
    float yawWeight = 0.f;

    for (int i = 0; i < static_cast<int>(ResolveMethod::COUNT); i++) {
        const auto& vote = info.votes[i];
        if (!vote.valid)
            continue;

        float weight = getMethodWeight(static_cast<ResolveMethod>(i), info) * vote.confidence;
        float rad = vote.suggested_yaw * (3.14159265f / 180.f);

        sinSum += std::sin(rad) * weight;
        cosSum += std::cos(rad) * weight;
        yawWeight += weight;
    }

    if (yawWeight > 0.f) {
        info.resolved_yaw = normalizeYaw(std::atan2(sinSum, cosSum) * (180.f / 3.14159265f));
    }
    else {
        info.resolved_yaw = entity->eyeAngles().y;
    }
}

float Resolver::calculateFinalYaw(Entity* entity, ResolverData& info) noexcept {
    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);

    // If high confidence, use resolved yaw directly
    if (info.confidence > 0.7f) {
        return info.resolved_yaw;
    }

    // Otherwise, use side-based calculation
    float desyncAngle = maxDesync;

    if (info.is_low_delta) {
        desyncAngle *= 0.5f;
    }

    if (info.extended_desync) {
        desyncAngle = maxDesync;
    }

    // Reduce desync if fake walking
    if (info.is_fake_walking) {
        desyncAngle *= 0.7f;
    }

    return eyeYaw + desyncAngle * static_cast<float>(info.side);
}

float Resolver::getMethodWeight(ResolveMethod method, ResolverData& info) noexcept {
    float weight = 1.f;

    switch (method) {
    case ResolveMethod::LBY:
        weight = WEIGHT_LBY;
        if (!info.is_moving && info.lby.updated_this_tick)
            weight *= 1.5f;
        if (info.jitter.is_jittering)
            weight *= 0.6f;
        if (info.lby.is_flicking)
            weight *= 0.5f;
        break;

    case ResolveMethod::ANIMSTATE:
        weight = WEIGHT_ANIMSTATE;
        if (info.is_moving)
            weight *= 1.4f;
        if (info.is_fake_walking)
            weight *= 0.8f;
        break;

    case ResolveMethod::POSE_PARAM:
        weight = WEIGHT_POSE;
        if (info.extended_desync)
            weight *= 1.3f;
        break;

    case ResolveMethod::TRACE:
        weight = WEIGHT_TRACE;
        break;

    case ResolveMethod::BRUTEFORCE:
        weight = WEIGHT_BRUTE;
        weight += info.misses * 0.15f;
        weight = (std::min)(weight, 1.5f);
        break;

    case ResolveMethod::FREESTANDING:
        weight = WEIGHT_FREESTANDING;
        break;

    case ResolveMethod::LAST_MOVING:
        weight = WEIGHT_LAST_MOVING;
        // Decay over time
        {
            float timeSinceMoving = memory->globalVars->currenttime - info.last_moving_time;
            weight *= (std::max)(0.3f, 1.f - timeSinceMoving / 5.f);
        }
        break;

    case ResolveMethod::PREDICTION:
        weight = WEIGHT_PREDICTION;
        if (info.jitter.pattern_detected)
            weight *= 1.2f;
        if (info.lby.break_predicted)
            weight *= 1.3f;
        break;

    default:
        break;
    }

    // Adjust weight based on method success rate
    const auto& stats = info.stats;
    int attempts = stats.method_attempts[static_cast<int>(method)];
    int successes = stats.method_successes[static_cast<int>(method)];

    if (attempts >= 3) {
        float successRate = static_cast<float>(successes) / attempts;
        weight *= (0.5f + successRate);
    }

    return weight;
}

// ============================================
// LBY RESOLVER
// ============================================

void Resolver::updateLBYData(Entity* entity, ResolverData& info) noexcept {
    auto& lby = info.lby;

    lby.last_lby = lby.current_lby;
    lby.current_lby = entity->lby();
    lby.lby_delta = angleDiff(lby.current_lby, lby.last_lby);

    // Store LBY history
    lby.lby_history[lby.history_index % HISTORY_SIZE] = lby.current_lby;
    lby.history_index++;

    // Detect LBY update
    lby.updated_this_tick = std::fabsf(lby.lby_delta) > 1.f;

    if (lby.updated_this_tick) {
        lby.last_update_time = memory->globalVars->currenttime;
        lby.flick_count++;
    }

    // Track standing time for LBY prediction
    if (!info.is_moving && info.is_on_ground) {
        lby.standing_time += memory->globalVars->intervalPerTick;
    }
    else {
        lby.standing_time = 0.f;
        lby.last_moving_lby = lby.current_lby;
        info.last_moving_time = memory->globalVars->currenttime;
    }

    // Predict next LBY update
    lby.next_update_time = lby.last_update_time + LBY_UPDATE_TIME;

    // Detect LBY flicking
    lby.is_flicking = detectLBYFlick(entity, info);
}

bool Resolver::predictLBYUpdate(Entity* entity, ResolverData& info) noexcept {
    if (info.is_moving)
        return false;

    const float currentTime = memory->globalVars->currenttime;
    const float timeSinceUpdate = currentTime - info.lby.last_update_time;

    return timeSinceUpdate >= LBY_UPDATE_TIME - memory->globalVars->intervalPerTick;
}

bool Resolver::detectLBYFlick(Entity* entity, ResolverData& info) noexcept {
    auto& lby = info.lby;

    int flickCount = 0;
    for (int i = 0; i < 8; i++) {
        int idx1 = (lby.history_index - i - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        int idx2 = (lby.history_index - i - 2 + HISTORY_SIZE) % HISTORY_SIZE;

        float delta = std::fabsf(angleDiff(lby.lby_history[idx1], lby.lby_history[idx2]));
        if (delta > 30.f) {
            flickCount++;
        }
    }

    return flickCount >= 3;
}

bool Resolver::predictLBYBreak(Entity* entity, ResolverData& info) noexcept {
    auto& lby = info.lby;

    if (info.is_moving) {
        lby.break_predicted = false;
        return false;
    }

    float timeSinceUpdate = memory->globalVars->currenttime - lby.last_update_time;
    float timeUntilBreak = LBY_UPDATE_TIME - timeSinceUpdate;

    if (timeUntilBreak > 0.f && timeUntilBreak < memory->globalVars->intervalPerTick * 3.f) {
        lby.break_predicted = true;
        lby.break_time = memory->globalVars->currenttime + timeUntilBreak;
        return true;
    }

    lby.break_predicted = false;
    return false;
}

Resolver::ResolveSide Resolver::getLBYSide(Entity* entity, ResolverData& info) noexcept {
    const float eyeYaw = entity->eyeAngles().y;
    const float lbyDelta = angleDiff(info.lby.current_lby, eyeYaw);

    if (std::fabsf(lbyDelta) < 10.f)
        return ResolveSide::ORIGINAL;

    return lbyDelta > 0.f ? ResolveSide::LEFT : ResolveSide::RIGHT;
}

Resolver::MethodVote Resolver::resolveLBY(Entity* entity, ResolverData& info) noexcept {
    MethodVote vote;
    vote.method = ResolveMethod::LBY;
    vote.valid = true;

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);

    // When moving, LBY is reliable
    if (info.is_moving) {
        vote.side = ResolveSide::ORIGINAL;
        vote.suggested_yaw = info.lby.current_lby;
        vote.confidence = 0.9f;
        vote.reason = "Moving - LBY reliable";
        return vote;
    }

    // Check if LBY just updated
    if (info.lby.updated_this_tick && !info.lby.is_flicking) {
        vote.side = getLBYSide(entity, info);
        vote.suggested_yaw = info.lby.current_lby;
        vote.confidence = 0.85f;
        vote.reason = "LBY just updated";
        return vote;
    }

    // Predict LBY update
    if (predictLBYUpdate(entity, info)) {
        vote.side = getLBYSide(entity, info);
        vote.suggested_yaw = eyeYaw + maxDesync * static_cast<float>(vote.side);
        vote.confidence = 0.7f;
        vote.reason = "LBY update predicted";
        return vote;
    }

    // LBY flicking - less reliable
    if (info.lby.is_flicking) {
        vote.side = getLBYSide(entity, info);
        vote.confidence = 0.4f;
        vote.reason = "LBY flicking detected";
    }
    else {
        const float lbyDelta = angleDiff(info.lby.last_moving_lby, eyeYaw);
        vote.side = lbyDelta > 0.f ? ResolveSide::LEFT : ResolveSide::RIGHT;
        vote.confidence = 0.5f;
        vote.reason = "Using last known LBY";
    }

    vote.suggested_yaw = eyeYaw + maxDesync * static_cast<float>(vote.side);
    return vote;
}

// ============================================
// ANIMSTATE RESOLVER
// ============================================

void Resolver::updateAnimStateData(Entity* entity, ResolverData& info) noexcept {
    auto* animState = entity->getAnimstate();
    if (!animState)
        return;

    auto& as = info.animstate;

    // Store previous values
    as.last_foot_yaw = as.foot_yaw;
    std::memcpy(as.prev_layers.data(), as.layers.data(), sizeof(as.layers));

    // Update current values
    as.foot_yaw = animState->footYaw;
    as.goal_feet_yaw = animState->eyeYaw;
    as.eye_yaw = animState->eyeYaw;
    as.duck_amount = animState->animDuckAmount;
    as.velocity_length = entity->velocity().length2D();

    // Copy current layers
    auto* layers = entity->get_animlayers();
    if (layers) {
        std::memcpy(as.layers.data(), layers, sizeof(as.layers));
    }

    // Calculate layer deltas
    for (int i = 0; i < 13; i++) {
        as.layer_deltas[i].weight_delta = as.layers[i].weight - as.prev_layers[i].weight;
        as.layer_deltas[i].cycle_delta = as.layers[i].cycle - as.prev_layers[i].cycle;
        as.layer_deltas[i].rate_delta = as.layers[i].playbackRate - as.prev_layers[i].playbackRate;
    }

    // Calculate move yaw
    as.move_yaw = getMoveYaw(entity, info);
}

float Resolver::getMoveYaw(Entity* entity, ResolverData& info) noexcept {
    const Vector velocity = entity->velocity();
    const float speed = velocity.length2D();

    if (speed < 0.1f)
        return info.animstate.foot_yaw;

    float velYaw = std::atan2(velocity.y, velocity.x) * (180.f / 3.14159265f);
    return normalizeYaw(velYaw - info.animstate.foot_yaw);
}

bool Resolver::detectExtendedDesync(Entity* entity, ResolverData& info) noexcept {
    const auto& layer3 = info.animstate.layers[3];

    if (layer3.cycle == 0.f && layer3.weight == 0.f) {
        return true;
    }

    return false;
}

float Resolver::compareAnimLayers(Entity* entity, ResolverData& info) noexcept {
    const auto& moveLayer = info.animstate.layers[6];
    const auto& prevMoveLayer = info.animstate.prev_layers[6];

    if (moveLayer.weight < 0.01f)
        return 0.f;

    float leftDelta = std::fabsf(moveLayer.playbackRate - prevMoveLayer.playbackRate);
    float rightDelta = std::fabsf(moveLayer.playbackRate - prevMoveLayer.playbackRate);

    return leftDelta - rightDelta;
}

Resolver::ResolveSide Resolver::getAnimStateSide(Entity* entity, ResolverData& info) noexcept {
    const float eyeYaw = entity->eyeAngles().y;
    const float footYaw = info.animstate.foot_yaw;
    const float delta = angleDiff(footYaw, eyeYaw);

    if (std::fabsf(delta) < 10.f)
        return ResolveSide::ORIGINAL;

    return delta > 0.f ? ResolveSide::RIGHT : ResolveSide::LEFT;
}

Resolver::MethodVote Resolver::resolveAnimState(Entity* entity, ResolverData& info) noexcept {
    MethodVote vote;
    vote.method = ResolveMethod::ANIMSTATE;
    vote.valid = true;

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);

    const auto& moveLayer = info.animstate.layers[ANIMATION_LAYER_MOVEMENT_MOVE];
    const auto& prevMoveLayer = info.animstate.prev_layers[ANIMATION_LAYER_MOVEMENT_MOVE];

    if (info.is_moving && moveLayer.weight > 0.1f) {
        float rateDelta = moveLayer.playbackRate - prevMoveLayer.playbackRate;

        if (std::fabsf(rateDelta) > 0.001f) {
            vote.side = rateDelta > 0.f ? ResolveSide::RIGHT : ResolveSide::LEFT;
            vote.confidence = (std::min)(std::fabsf(rateDelta) * 10.f, 0.9f);
            vote.reason = "Movement layer rate delta";
        }
        else {
            vote.side = getAnimStateSide(entity, info);
            vote.confidence = 0.6f;
            vote.reason = "AnimState foot yaw";
        }
    }
    else {
        const auto& adjustLayer = info.animstate.layers[ANIMATION_LAYER_ADJUST];

        if (adjustLayer.weight > 0.f && adjustLayer.cycle > 0.f) {
            vote.side = adjustLayer.cycle > 0.5f ? ResolveSide::LEFT : ResolveSide::RIGHT;
            vote.confidence = 0.7f;
            vote.reason = "Adjust layer cycle";
        }
        else {
            vote.side = getAnimStateSide(entity, info);
            vote.confidence = 0.5f;
            vote.reason = "Default AnimState";
        }
    }

    vote.suggested_yaw = eyeYaw + maxDesync * static_cast<float>(vote.side);
    return vote;
}

// ============================================
// POSE PARAMETER RESOLVER
// ============================================

void Resolver::updatePoseParamData(Entity* entity, ResolverData& info) noexcept {
    auto& pose = info.pose;

    auto& poseParams = entity->poseParameters();
    // ✅ Swap вместо memcpy (мгновенно)
    std::swap(pose.previous, pose.current);

    // ✅ Сначала копируй, потом считай дельты (лучше кэш-локальность)
    constexpr int COUNT = (std::min)(POSE_PARAM_COUNT, 24);
    for (int i = 0; i < COUNT; i++) {
        pose.current[i] = poseParams[i];
    }

    for (int i = 0; i < COUNT; i++) {
        pose.deltas[i] = pose.current[i] - pose.previous[i];
    }
}


void Resolver::simulatePoseParams(Entity* entity, ResolverData& info, float yaw,
    std::array<float, POSE_PARAM_COUNT>& out) noexcept {

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);

    float bodyYaw = angleDiff(yaw, eyeYaw);
    bodyYaw = std::clamp(bodyYaw, -maxDesync, maxDesync);

    out[BODY_YAW] = (bodyYaw + 60.f) / 120.f;
    out[LEAN_YAW] = info.pose.current[LEAN_YAW];

    if (info.is_moving) {
        const Vector velocity = entity->velocity();
        float velYaw = std::atan2(velocity.y, velocity.x) * (180.f / 3.14159265f);
        float moveYaw = angleDiff(velYaw, yaw);
        out[MOVE_YAW] = (moveYaw + 180.f) / 360.f;
    }
    else {
        out[MOVE_YAW] = 0.5f;
    }
}

float Resolver::comparePoseParams(const std::array<float, POSE_PARAM_COUNT>& a,
    const std::array<float, POSE_PARAM_COUNT>& b) noexcept {

    float diff = 0.f;

    const float weights[14] = {
        1.5f, 0.5f, 0.0f, 0.0f, 1.2f, 0.5f, 2.0f,
        1.0f, 0.0f, 0.5f, 0.0f, 0.3f, 0.3f, 0.8f
    };

    for (int i = 0; i < 14; i++) {
        diff += std::fabsf(a[i] - b[i]) * weights[i];
    }

    return diff;
}

float Resolver::extractBodyYawFromPose(Entity* entity, ResolverData& info) noexcept {
    float bodyYawNorm = info.pose.current[BODY_YAW];
    float bodyYaw = (bodyYawNorm * 120.f) - 60.f;
    return bodyYaw;
}

Resolver::ResolveSide Resolver::getPoseParamSide(Entity* entity, ResolverData& info) noexcept {
    float bodyYaw = extractBodyYawFromPose(entity, info);

    if (std::fabsf(bodyYaw) < 10.f)
        return ResolveSide::ORIGINAL;

    return bodyYaw > 0.f ? ResolveSide::RIGHT : ResolveSide::LEFT;
}

Resolver::MethodVote Resolver::resolvePoseParams(Entity* entity, ResolverData& info) noexcept {
    MethodVote vote;
    vote.method = ResolveMethod::POSE_PARAM;
    vote.valid = true;

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);

    simulatePoseParams(entity, info, eyeYaw + maxDesync, info.pose.simulated_left);
    simulatePoseParams(entity, info, eyeYaw - maxDesync, info.pose.simulated_right);
    simulatePoseParams(entity, info, eyeYaw, info.pose.simulated_center);

    float leftDiff = comparePoseParams(info.pose.current, info.pose.simulated_left);
    float rightDiff = comparePoseParams(info.pose.current, info.pose.simulated_right);
    float centerDiff = comparePoseParams(info.pose.current, info.pose.simulated_center);

    float minDiff = (std::min)({ leftDiff, rightDiff, centerDiff });

    if (minDiff == leftDiff) {
        vote.side = ResolveSide::LEFT;
        vote.suggested_yaw = eyeYaw + maxDesync;
        vote.reason = "Pose params match left";
    }
    else if (minDiff == rightDiff) {
        vote.side = ResolveSide::RIGHT;
        vote.suggested_yaw = eyeYaw - maxDesync;
        vote.reason = "Pose params match right";
    }
    else {
        vote.side = ResolveSide::ORIGINAL;
        vote.suggested_yaw = eyeYaw;
        vote.reason = "Pose params match center";
    }

    float totalDiff = leftDiff + rightDiff + centerDiff;
    if (totalDiff > 0.f) {
        vote.confidence = 1.f - (minDiff / totalDiff);
    }
    else {
        vote.confidence = 0.3f;
    }

    ResolveSide poseBasedSide = getPoseParamSide(entity, info);
    if (poseBasedSide == vote.side) {
        vote.confidence = (std::min)(vote.confidence + 0.15f, 1.f);
    }

    return vote;
}

// ============================================
// TRACE RESOLVER
// ============================================

Resolver::MethodVote Resolver::resolveTrace(Entity* entity, ResolverData& info) noexcept {
    MethodVote vote;
    vote.method = ResolveMethod::TRACE;
    vote.valid = true;

    if (!localPlayer) {
        vote.valid = false;
        return vote;
    }

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);
    const float atTarget = getAtTargetYaw(entity);

    Vector forward, right, up;
    Helpers::AngleVectors(Vector(0, atTarget, 0), &forward, &right, &up);

    const Vector src = entity->getEyePosition();
    const Vector dst = src + forward * 384.f;

    Trace trLeft, trRight, trCenter;

    interfaces->engineTrace->traceRay(
        { src - right * 35.f, dst - right * 35.f },
        MASK_SHOT, { entity }, trLeft
    );
    float leftDist = (trLeft.endpos - trLeft.startpos).length();

    interfaces->engineTrace->traceRay(
        { src + right * 35.f, dst + right * 35.f },
        MASK_SHOT, { entity }, trRight
    );
    float rightDist = (trRight.endpos - trRight.startpos).length();

    interfaces->engineTrace->traceRay(
        { src, dst },
        MASK_SHOT, { entity }, trCenter
    );

    float distDiff = std::fabsf(leftDist - rightDist);

    if (distDiff < 20.f) {
        vote.side = ResolveSide::ORIGINAL;
        vote.confidence = 0.3f;
        vote.reason = "No clear freestanding";
    }
    else if (leftDist > rightDist) {
        vote.side = ResolveSide::LEFT;
        vote.confidence = (std::min)(distDiff / 100.f, 0.8f);
        vote.reason = "Left side more open";
    }
    else {
        vote.side = ResolveSide::RIGHT;
        vote.confidence = (std::min)(distDiff / 100.f, 0.8f);
        vote.reason = "Right side more open";
    }

    vote.suggested_yaw = eyeYaw + maxDesync * static_cast<float>(vote.side);
    return vote;
}

// ============================================
// BRUTEFORCE RESOLVER
// ============================================

Resolver::MethodVote Resolver::resolveBruteforce(Entity* entity, ResolverData& info) noexcept {
    MethodVote vote;
    vote.method = ResolveMethod::BRUTEFORCE;
    vote.valid = true;

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);
    const int misses = info.misses;

    float bruteYaw = 0.f;

    if (info.jitter.is_jittering) {
        switch (misses % 4) {
        case 0:
            bruteYaw = maxDesync;
            vote.side = ResolveSide::LEFT;
            vote.reason = "Jitter brute: full left";
            break;
        case 1:
            bruteYaw = -maxDesync;
            vote.side = ResolveSide::RIGHT;
            vote.reason = "Jitter brute: full right";
            break;
        case 2:
            bruteYaw = maxDesync * 0.5f;
            vote.side = ResolveSide::LEFT;
            vote.reason = "Jitter brute: half left";
            break;
        case 3:
            bruteYaw = -maxDesync * 0.5f;
            vote.side = ResolveSide::RIGHT;
            vote.reason = "Jitter brute: half right";
            break;
        }
    }
    else {
        switch (misses % 8) {
        case 0:
            bruteYaw = maxDesync * static_cast<float>(info.side);
            vote.side = info.side;
            vote.reason = "Brute: current side";
            break;
        case 1:
            bruteYaw = -maxDesync * static_cast<float>(info.side);
            vote.side = info.side == ResolveSide::LEFT ? ResolveSide::RIGHT : ResolveSide::LEFT;
            vote.reason = "Brute: opposite side";
            break;
        case 2:
            bruteYaw = 0.f;
            vote.side = ResolveSide::ORIGINAL;
            vote.reason = "Brute: center";
            break;
        case 3:
            bruteYaw = maxDesync * 0.5f;
            vote.side = ResolveSide::LEFT;
            vote.reason = "Brute: half left";
            break;
        case 4:
            bruteYaw = -maxDesync * 0.5f;
            vote.side = ResolveSide::RIGHT;
            vote.reason = "Brute: half right";
            break;
        case 5:
            bruteYaw = maxDesync * 0.7f;
            vote.side = ResolveSide::LEFT;
            vote.reason = "Brute: 70% left";
            break;
        case 6:
            bruteYaw = -maxDesync * 0.7f;
            vote.side = ResolveSide::RIGHT;
            vote.reason = "Brute: 70% right";
            break;
        case 7:
            bruteYaw = maxDesync * 0.3f;
            vote.side = ResolveSide::LEFT;
            vote.reason = "Brute: 30% left";
            break;
        }
    }

    vote.suggested_yaw = eyeYaw + bruteYaw;
    vote.confidence = 0.3f + (misses > 0 ? 0.08f * (std::min)(misses, 6) : 0.f);

    return vote;
}

// ============================================
// FREESTANDING RESOLVER
// ============================================

Resolver::MethodVote Resolver::resolveFreestanding(Entity* entity, ResolverData& info) noexcept {
    MethodVote vote;
    vote.method = ResolveMethod::FREESTANDING;
    vote.valid = true;

    if (!localPlayer) {
        vote.valid = false;
        return vote;
    }

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);
    const Vector entityPos = entity->getEyePosition();
    const Vector localPos = localPlayer->getEyePosition();

    float atTarget = getAtTargetYaw(entity);

    Vector forward, right, up;
    Helpers::AngleVectors(Vector(0, atTarget, 0), &forward, &right, &up);

    float leftOpenness = 0.f;
    float rightOpenness = 0.f;

    for (int i = 0; i < 3; i++) {
        float offset = 20.f + i * 15.f;

        Trace trLeft;
        Vector leftStart = entityPos - right * offset;
        Vector leftEnd = leftStart + forward * 300.f;
        interfaces->engineTrace->traceRay(
            { leftStart, leftEnd }, MASK_SHOT, { entity }, trLeft
        );
        leftOpenness += trLeft.fraction;

        Trace trRight;
        Vector rightStart = entityPos + right * offset;
        Vector rightEnd = rightStart + forward * 300.f;
        interfaces->engineTrace->traceRay(
            { rightStart, rightEnd }, MASK_SHOT, { entity }, trRight
        );
        rightOpenness += trRight.fraction;
    }

    leftOpenness /= 3.f;
    rightOpenness /= 3.f;

    float diff = std::fabsf(leftOpenness - rightOpenness);

    if (diff < 0.1f) {
        vote.side = ResolveSide::ORIGINAL;
        vote.confidence = 0.3f;
        vote.reason = "No clear freestanding side";
    }
    else if (leftOpenness > rightOpenness) {
        vote.side = ResolveSide::RIGHT;
        vote.confidence = (std::min)(diff * 2.f, 0.85f);
        vote.reason = "Left exposed, desync right";
    }
    else {
        vote.side = ResolveSide::LEFT;
        vote.confidence = (std::min)(diff * 2.f, 0.85f);
        vote.reason = "Right exposed, desync left";
    }

    vote.suggested_yaw = eyeYaw + maxDesync * static_cast<float>(vote.side);
    return vote;
}

// ============================================
// LAST MOVING RESOLVER
// ============================================

Resolver::MethodVote Resolver::resolveLastMoving(Entity* entity, ResolverData& info) noexcept {
    MethodVote vote;
    vote.method = ResolveMethod::LAST_MOVING;
    vote.valid = true;

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);

    float lbyDelta = angleDiff(info.lby.last_moving_lby, eyeYaw);

    if (std::fabsf(lbyDelta) < 10.f) {
        vote.side = ResolveSide::ORIGINAL;
        vote.confidence = 0.4f;
        vote.reason = "Last moving LBY close to eye yaw";
    }
    else {
        vote.side = lbyDelta > 0.f ? ResolveSide::LEFT : ResolveSide::RIGHT;
        vote.confidence = (std::min)(std::fabsf(lbyDelta) / maxDesync * 0.7f, 0.75f);
        vote.reason = "Using last moving LBY";
    }

    float timeSinceMoving = memory->globalVars->currenttime - info.last_moving_time;
    vote.confidence *= (std::max)(0.f, 1.f - timeSinceMoving / 3.f);

    vote.suggested_yaw = info.lby.last_moving_lby;
    return vote;
}

// ============================================
// PREDICTION RESOLVER
// ============================================

Resolver::MethodVote Resolver::resolvePrediction(Entity* entity, ResolverData& info) noexcept {
    MethodVote vote;
    vote.method = ResolveMethod::PREDICTION;
    vote.valid = true;

    const float eyeYaw = entity->eyeAngles().y;
    const float maxDesync = getMaxDesync(entity);

    if (info.jitter.pattern_detected) {
        int predictedSide = info.jitter.jitter_side * -1;
        vote.side = predictedSide > 0 ? ResolveSide::LEFT : ResolveSide::RIGHT;
        vote.confidence = 0.65f;
        vote.reason = "Jitter pattern prediction";
    }
    else if (info.lby.break_predicted) {
        vote.side = getLBYSide(entity, info);
        vote.confidence = 0.7f;
        vote.reason = "LBY break prediction";
    }
    else {
        const auto& stats = info.stats;

        int leftTotal = stats.left_hits + stats.left_misses;
        int rightTotal = stats.right_hits + stats.right_misses;

        if (leftTotal + rightTotal >= 3) {
            float leftSuccessRate = leftTotal > 0 ? static_cast<float>(stats.left_hits) / leftTotal : 0.5f;
            float rightSuccessRate = rightTotal > 0 ? static_cast<float>(stats.right_hits) / rightTotal : 0.5f;

            if (leftSuccessRate > rightSuccessRate + 0.1f) {
                vote.side = ResolveSide::LEFT;
                vote.confidence = leftSuccessRate * 0.7f;
                vote.reason = "Historical: left more successful";
            }
            else if (rightSuccessRate > leftSuccessRate + 0.1f) {
                vote.side = ResolveSide::RIGHT;
                vote.confidence = rightSuccessRate * 0.7f;
                vote.reason = "Historical: right more successful";
            }
            else {
                vote.side = ResolveSide::ORIGINAL;
                vote.confidence = 0.4f;
                vote.reason = "Historical: no clear preference";
            }
        }
        else {
            vote.side = ResolveSide::ORIGINAL;
            vote.confidence = 0.3f;
            vote.reason = "Insufficient data";
        }
    }

    vote.suggested_yaw = eyeYaw + maxDesync * static_cast<float>(vote.side);
    return vote;
}

// ============================================
// DETECTION METHODS
// ============================================

Resolver::ResolveMode Resolver::detectMode(Entity* entity, ResolverData& info) noexcept {
    if (!entity || !entity->getAnimstate())
        return ResolveMode::NONE;

    const bool onGround = (entity->flags() & FL_ONGROUND) != 0;
    const float speed = entity->velocity().length2D();

    // Check ladder
    if (entity->moveType() == MoveType::LADDER) {
        info.is_on_ladder = true;
        return ResolveMode::LADDER;
    }
    info.is_on_ladder = false;

    if (!onGround)
        return ResolveMode::AIR;

    // Check for fake duck
    if (detectFakeDuck(entity, info))
        return ResolveMode::FAKE_DUCK;

    // Check for fake walk
    if (detectFakeWalk(entity, info))
        return ResolveMode::SLOW_WALK;

    if (speed > MOVING_SPEED_THRESHOLD && speed < FAKE_WALK_THRESHOLD)
        return ResolveMode::SLOW_WALK;

    if (speed > MOVING_SPEED_THRESHOLD)
        return ResolveMode::MOVING;

    return ResolveMode::STANDING;
}

bool Resolver::detectJitter(Entity* entity, ResolverData& info) noexcept {
    auto& jitter = info.jitter;

    float currentYaw = entity->eyeAngles().y;
    jitter.yaw_history[jitter.history_index % HISTORY_SIZE] = currentYaw;
    jitter.history_index++;

    if (jitter.history_index < 8)
        return false;

    jitter.jitter_ticks = 0;
    jitter.static_ticks = 0;
    float yawSum = 0.f;

    for (int i = 0; i < 16; i++) {
        int idx = (jitter.history_index - i - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        yawSum += jitter.yaw_history[idx];

        if (i > 0) {
            int prevIdx = (jitter.history_index - i + HISTORY_SIZE) % HISTORY_SIZE;
            float delta = std::fabsf(angleDiff(
                jitter.yaw_history[idx],
                jitter.yaw_history[prevIdx]
            ));

            if (delta < 2.f)
                jitter.static_ticks++;
            else if (delta > JITTER_THRESHOLD)
                jitter.jitter_ticks++;

            // Store delta for pattern detection
            if (i < 16) {
                jitter.delta_history[i - 1] = angleDiff(
                    jitter.yaw_history[idx],
                    jitter.yaw_history[prevIdx]
                );
            }
        }
    }

    jitter.average_yaw = yawSum / 16.f;

    float variance = 0.f;
    for (int i = 0; i < 16; i++) {
        int idx = (jitter.history_index - i - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        float diff = jitter.yaw_history[idx] - jitter.average_yaw;
        variance += diff * diff;
    }
    jitter.yaw_variance = variance / 16.f;

    jitter.is_jittering = (jitter.jitter_ticks > jitter.static_ticks) &&
        (jitter.jitter_ticks >= 4) &&
        (jitter.yaw_variance > 100.f);

    if (jitter.is_jittering) {
        float lastDelta = angleDiff(currentYaw, jitter.yaw_history[
            (jitter.history_index - 2 + HISTORY_SIZE) % HISTORY_SIZE
        ]);
        jitter.jitter_side = lastDelta > 0.f ? 1 : -1;

        // Calculate jitter range
        float maxDelta = 0.f;
        for (int i = 0; i < 8; i++) {
            maxDelta = (std::max)(maxDelta, std::fabsf(jitter.delta_history[i]));
        }
        jitter.jitter_range = maxDelta;
    }

    return jitter.is_jittering;
}

bool Resolver::detectJitterPattern(Entity* entity, ResolverData& info) noexcept {
    auto& jitter = info.jitter;

    if (jitter.history_index < 16)
        return false;

    std::array<float, 8> deltas{};
    for (int i = 0; i < 8; i++) {
        int idx1 = (jitter.history_index - i - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        int idx2 = (jitter.history_index - i - 2 + HISTORY_SIZE) % HISTORY_SIZE;
        deltas[i] = angleDiff(jitter.yaw_history[idx1], jitter.yaw_history[idx2]);
    }

    // Check for alternating pattern (most common jitter)
    bool alternating = true;
    for (int i = 1; i < 7; i++) {
        if ((deltas[i] > 0) == (deltas[i - 1] > 0)) {
            alternating = false;
            break;
        }
    }

    if (alternating) {
        jitter.pattern_detected = true;
        jitter.pattern_length = 2;
        jitter.jitter_range = (std::fabsf(deltas[0]) + std::fabsf(deltas[1])) / 2.f;
        return true;
    }

    // Check for 3-tick pattern
    bool threePattern = true;
    for (int i = 3; i < 7; i++) {
        if (std::fabsf(deltas[i] - deltas[i - 3]) > 5.f) {
            threePattern = false;
            break;
        }
    }

    if (threePattern) {
        jitter.pattern_detected = true;
        jitter.pattern_length = 3;
        return true;
    }

    // Check for 4-tick pattern
    bool fourPattern = true;
    for (int i = 4; i < 8; i++) {
        if (std::fabsf(deltas[i] - deltas[i - 4]) > 5.f) {
            fourPattern = false;
            break;
        }
    }

    if (fourPattern) {
        jitter.pattern_detected = true;
        jitter.pattern_length = 4;
        return true;
    }

    jitter.pattern_detected = false;
    return false;
}

bool Resolver::detectLowDelta(Entity* entity, ResolverData& info) noexcept {
    auto* animState = entity->getAnimstate();
    if (!animState)
        return false;

    const float eyeYaw = entity->eyeAngles().y;
    const float footYaw = animState->footYaw;
    const float delta = std::fabsf(angleDiff(eyeYaw, footYaw));

    info.is_low_delta = delta < LOW_DELTA_THRESHOLD;
    return info.is_low_delta;
}

bool Resolver::detectFakeAngles(Entity* entity) noexcept {
    const Vector& eyeAngles = entity->eyeAngles();

    if (std::fabsf(eyeAngles.x) > 89.f)
        return true;

    if (std::fabsf(eyeAngles.x) > 75.f)
        return true;

    return false;
}

bool Resolver::detectFakeWalk(Entity* entity, ResolverData& info) noexcept {
    const float speed = entity->velocity().length2D();

    if (speed < 3.f || speed > FAKE_WALK_THRESHOLD)
        return false;

    const auto& moveLayer = info.animstate.layers[ANIMATION_LAYER_MOVEMENT_MOVE];

    if (moveLayer.weight > 0.1f && moveLayer.playbackRate < 0.4f) {
        info.is_fake_walking = true;
        return true;
    }

    info.is_fake_walking = false;
    return false;
}

bool Resolver::detectFakeDuck(Entity* entity, ResolverData& info) noexcept {
    auto* animState = entity->getAnimstate();
    if (!animState)
        return false;

    float duckAmount = animState->animDuckAmount;

    static std::array<float, 65> lastDuckAmount{};
    static std::array<int, 65> stuckTicks{};
    int index = entity->index();

    float duckDelta = std::fabsf(duckAmount - lastDuckAmount[index]);

    // Check for stuck duck amount (typical of fake duck)
    if (duckAmount > 0.3f && duckAmount < 0.7f && duckDelta < 0.01f) {
        stuckTicks[index]++;
    }
    else {
        stuckTicks[index] = 0;
    }

    lastDuckAmount[index] = duckAmount;

    if (stuckTicks[index] > 3) {
        info.is_fake_ducking = true;
        return true;
    }

    info.is_fake_ducking = false;
    return false;
}

// ============================================
// UTILITY METHODS
// ============================================

float Resolver::getMaxDesync(Entity* entity) noexcept {
    auto* animState = entity->getAnimstate();
    if (!animState)
        return 58.f;

    float duckAmount = animState->animDuckAmount;
    float speedFraction = std::clamp(animState->speedAsPortionOfWalkTopSpeed, 0.f, 1.f);
    float speedFactor = std::clamp(animState->speedAsPortionOfCrouchTopSpeed, 0.f, 1.f);

    float modifier = ((animState->walkToRunTransition * -0.3f) - 0.2f) * speedFraction + 1.f;

    if (duckAmount > 0.f) {
        modifier += (duckAmount * speedFactor) * (0.5f - modifier);
    }

    return 58.f * modifier;
}

float Resolver::buildServerAbsYaw(Entity* entity, float yaw) noexcept {
    const float eyeYaw = entity->eyeAngles().y;
    const float maxDelta = getMaxDesync(entity);

    float goalFeetYaw = normalizeYaw(yaw);
    float eyeFeetDelta = angleDiff(eyeYaw, goalFeetYaw);

    if (eyeFeetDelta > maxDelta)
        goalFeetYaw = eyeYaw - maxDelta;
    else if (eyeFeetDelta < -maxDelta)
        goalFeetYaw = eyeYaw + maxDelta;

    return normalizeYaw(goalFeetYaw);
}

float Resolver::getAtTargetYaw(Entity* entity) noexcept {
    if (!localPlayer)
        return 0.f;

    Vector delta = entity->getAbsOrigin() - localPlayer->getAbsOrigin();
    return std::atan2(delta.y, delta.x) * (180.f / 3.14159265f);
}

float Resolver::normalizeYaw(float yaw) noexcept {
    while (yaw > 180.f) yaw -= 360.f;
    while (yaw < -180.f) yaw += 360.f;
    return yaw;
}

float Resolver::angleDiff(float a, float b) noexcept {
    float diff = normalizeYaw(a - b);
    return diff;
}

float Resolver::approachAngle(float target, float value, float speed) noexcept {
    float delta = angleDiff(target, value);

    if (delta > speed)
        return value + speed;
    else if (delta < -speed)
        return value - speed;

    return target;
}

// ============================================
// RESET STATISTICS
// ============================================

void Resolver::resetStatistics() noexcept {
    for (auto& d : data) {
        d.stats.reset();
        d.misses = 0;
        d.hits = 0;
        d.total_shots = 0;
    }
    globalStats.reset();
    recentLogs.clear();

    Logger::addLog("[Resolver] Statistics reset");
}

void Resolver::resetPlayerStatistics(int index) noexcept {
    if (index < 1 || index > 64)
        return;

    data[index].stats.reset();
    data[index].misses = 0;
    data[index].hits = 0;
    data[index].total_shots = 0;

    Logger::addLog("[Resolver] Statistics reset for player " + std::to_string(index));
}