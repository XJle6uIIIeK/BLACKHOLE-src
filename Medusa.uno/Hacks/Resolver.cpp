#include "Resolver.h"
#include "Animations.h"
#include "AimbotFunctions.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "../SDK/Entity.h"
#include "../SDK/EngineTrace.h"
#include "../SDK/Engine.h"
#include "../Helpers.h"
#include <algorithm>
#include <array>
#include <deque>
#include <cstring>

namespace Resolver {

static int missed_shots[65] = {};
static int brute_angle_index[65] = {};

// Углы для brute (максимум — все популярные)
constexpr float brute_angles[] = { 58.f, -58.f, 45.f, -45.f, 29.f, -29.f, 0.f, 90.f, -90.f, 15.f, -15.f };
constexpr int brute_len = sizeof(brute_angles) / sizeof(float);

struct PlayerHistory {
    float last_eye[32]{};
    int tick_last_seen = 0;
    int force_side = 0;
    bool jitter = false;
    int jitter_tick = 0;
    int static_tick = 0;
    float last_jitter_yaw = 0.f;
};
static PlayerHistory player_history[65] = {};

float get_max_desync(Entity* ent) {
    auto a = ent->getAnimstate();
    if (!a) return 58.f;
    float duck = a->animDuckAmount;
    float sf1 = std::clamp(a->speedAsPortionOfWalkTopSpeed, 0.f, 1.f);
    float sf2 = std::clamp(a->speedAsPortionOfCrouchTopSpeed, 0.f, 1.f);
    float mod = ((a->walkToRunTransition * -0.3f) - 0.2f) * sf1 + 1.f;
    if (duck > 0) mod += (duck * sf2) * (0.5f - mod);
    return std::clamp(58.f * mod, 0.f, 60.f);
}

int trace_freestanding(Entity* e) {
    Vector src3D = e->getEyePosition();
    Vector f, r, u;
    float base_yaw = Helpers::calculate_angle(localPlayer->getAbsOrigin(), e->getAbsOrigin()).y;
    Helpers::AngleVectors(Vector(0, base_yaw, 0), &f, &r, &u);
    Trace tr;
    interfaces->engineTrace->traceRay({src3D, src3D + f * 384.f}, MASK_SHOT, {e}, tr);
    float back = (tr.endpos - tr.startpos).length();
    interfaces->engineTrace->traceRay(Ray(src3D + r*35, src3D + f*384.f + r*35), MASK_SHOT, {e}, tr);
    float right = (tr.endpos - tr.startpos).length();
    interfaces->engineTrace->traceRay(Ray(src3D - r*35, src3D + f*384.f - r*35), MASK_SHOT, {e}, tr);
    float left = (tr.endpos - tr.startpos).length();
    if (left > right) return 1;
    if (right > left) return -1;
    return 0;
}

// Обработка jitter — чистый анализ yaw дельты + частота смен
bool detect_jitter_yaw(int idx, float current_yaw) {
    auto& hist = player_history[idx];
    float delta = std::fabs(Helpers::angleDiff(current_yaw, hist.last_jitter_yaw));
    hist.last_jitter_yaw = current_yaw;
    if (delta >= 20) {
        hist.jitter_tick++;
    } else {
        hist.static_tick++;
    }
    if (hist.jitter_tick > 5 && (hist.jitter_tick > hist.static_tick)) {
        hist.jitter_tick = 0;
        hist.static_tick = 0;
        return true;
    }
    if (hist.static_tick > 32) { hist.static_tick = 0; hist.jitter_tick = 0; }
    return false;
}

void resolve_entity(Animations::Players& p, Animations::Players prev, Entity* e) {
    if (!e || !e->isAlive() || e->isDormant()) return;
    int idx = e->index();
    float vel = e->velocity().length2D();
    auto layers = e->animOverlays();
    auto prev_layers = prev.layers;

    // Анимации low-delta, extended, дробл. brute
    float desync = get_max_desync(e);
    bool low_delta = std::fabs(std::remainder(e->eyeAngles().y, e->getAnimstate()->footYaw)) < 35.f;
    if (low_delta) desync *= 0.5f;

    // Detect jitter по частотному анализу
    bool jitter = detect_jitter_yaw(idx, e->eyeAngles().y);
    bool force_center = false;

    // Stand/Walk/Air/OnShot logics
    float brute_angle = 0.f;
    int miss = missed_shots[idx];
    int br_idx = brute_angle_index[idx]%brute_len;
    int side = trace_freestanding(e);

    if (vel < 15.f && (e->flags() & FL_ONGROUND)) {
        // Stand resolver
        if (!jitter && miss == 0) {
            if (side == 1)      brute_angle = desync;
            else if (side == -1)brute_angle = -desync;
            else                brute_angle = 0.f;
        } else {
            brute_angle = brute_angles[br_idx];
            if (jitter) brute_angle /= 2.f;
        }
    } else if (vel >= 15.f && (e->flags() & FL_ONGROUND)) {
        // Walk resolver — вектор движения
        Vector dir = e->velocity();
        float walk_yaw = Helpers::rad2deg(std::atan2(dir.y, dir.x));
        brute_angle = Helpers::angleDiff(walk_yaw, e->eyeAngles().y);
        brute_angle = std::clamp(brute_angle, -desync, desync);
    } else {
        // Air resolver — просто brute rotate по сторонам
        brute_angle = brute_angles[br_idx];
    }

    // Применить корректировку в анимстейт (footYaw)
    auto anim = e->getAnimstate();
    if (anim)
        anim->footYaw = Helpers::normalizeYaw(e->eyeAngles().y + brute_angle);
}

void onPlayerMiss(int idx) noexcept {
    if (idx < 0 || idx >= 65) return;
    missed_shots[idx]++;
    brute_angle_index[idx]++;
}
void onPlayerHurt(int attacker, int victim, int) noexcept {
    if (victim < 0 || victim >= 65) return;
    missed_shots[victim] = 0;
    brute_angle_index[victim] = 0;
}
void reset() noexcept {
    for(int i=0;i<65;++i){ missed_shots[i]=0;brute_angle_index[i]=0;player_history[i]={}; }
}

} // namespace Resolver
