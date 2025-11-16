#include "Resolver.h"
#include "Animations.h"
#include "../Helpers.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "../SDK/Engine.h"
#include "../SDK/Entity.h"
#include "../SDK/EngineTrace.h"
#include <algorithm>
#include <cmath>

namespace Resolver {

// Система истории промахов для smart-bruteforce
static int brute_angle_index[65] = {};
static constexpr float brute_angles[] = { 58.f, -58.f, 29.f, -29.f, 45.f, -45.f, 0.f };

static int miss_counter[65] = {};

float get_max_desync(Entity* e) {
    auto a = e->getAnimstate();
    float duck = a->animDuckAmount;
    float speed_frac = std::clamp(a->speedAsPortionOfWalkTopSpeed, 0.f, 1.f);
    float speed_fac = std::clamp(a->speedAsPortionOfCrouchTopSpeed, 0.f, 1.f);
    float mod = ((a->walkToRunTransition * -0.3f) - 0.2f) * speed_frac + 1.f;
    if (duck > 0) mod += ((duck * speed_fac) * (0.5f - mod));
    float trg = 58.f * mod;
    return std::clamp(trg, 0.f, 60.f);
}

static int detect_side(Entity* e) {
    Vector src3D = e->getEyePosition();
    Vector f, r, u;
    Helpers::AngleVectors(Vector(0, Helpers::calculate_angle(localPlayer->getAbsOrigin(), e->getAbsOrigin()).y, 0), &f, &r, &u);
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

void resolve_entity(Animations::Players& p, Animations::Players prev, Entity* e) {
    if (!e || !e->isAlive() || e->isDormant()) return;
    int idx = e->index();
    float velocity = e->velocity().length2D();

    // Animation layers анализ
    auto layers = e->animOverlays();
    float layer6_delta = std::abs(layers[6].playbackRate - prev.layers[6].playbackRate);
    int side = detect_side(e);
    float desync = get_max_desync(e);

    // Low-delta & extended/roll fix
    bool low_delta = std::abs(std::remainder(e->eyeAngles().y, e->getAnimstate()->footYaw)) < 35.f;
    if (low_delta) desync *= 0.5f;

    // Smart bruteforce: если >1 мисс на игрока — меняем угол
    float resolve_angle = 0.f;
    int miss = miss_counter[idx];
    if (miss == 0) {
        if (side == 1)      resolve_angle = desync;
        else if (side == -1)resolve_angle = -desync;
        else                resolve_angle = 0.f;
    } else {
        int br_idx = brute_angle_index[idx]% (sizeof(brute_angles)/sizeof(float));
        resolve_angle = brute_angles[br_idx];
    }

    // Реализация: записать угол в анимстэйт (footYaw)
    auto animstate = e->getAnimstate();
    if (animstate)
        animstate->footYaw = Helpers::normalizeYaw(e->eyeAngles().y + resolve_angle);
}

void onPlayerHurt(int attacker, int victim, int) noexcept {
    if (victim < 0 || victim >= 65) return;
    miss_counter[victim] = 0;
    brute_angle_index[victim] = 0;
}
void onPlayerMiss(int idx) noexcept {
    if (idx < 0 || idx >= 65) return;
    miss_counter[idx]++;
    brute_angle_index[idx]++;
}
void reset() noexcept {
    for(int i=0;i<65;++i){ miss_counter[i]=0;brute_angle_index[i]=0; }
}

// Подключаем в Animations — вызывать Resolver::resolve_entity для каждого врага
}