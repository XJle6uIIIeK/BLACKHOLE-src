#include "../Config.h"
#include "../Interfaces.h"
#include "../Memory.h"

#include "AimbotFunctions.h"
#include "Animations.h"

#include "../SDK/Angle.h"
#include "../SDK/ConVar.h"
#include "../SDK/Entity.h"
#include "../SDK/UserCmd.h"
#include "../SDK/Vector.h"
#include "../SDK/WeaponId.h"
#include "../SDK/GlobalVars.h"
#include "../SDK/PhysicsSurfaceProps.h"
#include "../SDK/WeaponData.h"
#include "../SDK/ModelInfo.h"
#include <omp.h>
#include <thread>
#include "../xor.h"
int maxThreadNum = (std::thread::hardware_concurrency());

struct PenetrationData
{
    float damage;
    int hitsTaken;
    Vector exitPoint;
    bool valid;
};

Vector AimbotFunction::calculateRelativeAngle(const Vector& source, const Vector& destination, const Vector& viewAngles) noexcept
{
    return ((destination - source).toAngle() - viewAngles).normalize();
}

static bool traceToExit(const Trace& enterTrace, const Vector& start, const Vector& direction, Vector& end, Trace& exitTrace, float range = 90.f, float step = 4.0f)
{
    float distance{ 0.0f };
    int previousContents{ 0 };

    while (distance <= range)
    {
        distance += step;
        Vector origin{ start + direction * distance };

        if (!previousContents)
            previousContents = interfaces->engineTrace->getPointContents(origin, 0x4600400B);

        const int currentContents = interfaces->engineTrace->getPointContents(origin, 0x4600400B);
        if (!(currentContents & 0x600400B) || (currentContents & 0x40000000 && currentContents != previousContents))
        {
            const Vector destination{ origin - (direction * step) };

            if (interfaces->engineTrace->traceRay({ origin, destination }, 0x4600400B, nullptr, exitTrace); exitTrace.startSolid && exitTrace.surface.flags & 0x8000)
            {
                if (interfaces->engineTrace->traceRay({ origin, start }, 0x600400B, { exitTrace.entity }, exitTrace); exitTrace.didHit() && !exitTrace.startSolid)
                    return true;

                continue;
            }

            if (exitTrace.didHit() && !exitTrace.startSolid)
            {
                if (memory->isBreakableEntity(enterTrace.entity) && memory->isBreakableEntity(exitTrace.entity))
                    return true;

                if (enterTrace.surface.flags & 0x0080 || (!(exitTrace.surface.flags & 0x0080) && exitTrace.plane.normal.dotProduct(direction) <= 1.0f))
                    return true;

                continue;
            }
            else {
                if (enterTrace.entity && enterTrace.entity->index() != 0 && memory->isBreakableEntity(enterTrace.entity))
                    return true;

                continue;
            }
        }
    }
    return false;
}

static PenetrationData handleBulletPenetration(
    SurfaceData* enterSurfaceData,
    const Trace& enterTrace,
    const Vector& direction,
    float penetration,
    float damage,
    int hitsLeft) noexcept
{
    PenetrationData result{ 0.0f, 0, Vector{}, false };

    Vector end;
    Trace exitTrace;

    // Пробуем найти exit point с увеличенным range для толстых стен
    if (!traceToExit(enterTrace, enterTrace.endpos, direction, end, exitTrace, 180.f, 4.0f))
    {
        // Если не нашли exit - пробуем с меньшим step
        if (!traceToExit(enterTrace, enterTrace.endpos, direction, end, exitTrace, 90.f, 2.0f))
            return result;
    }

    SurfaceData* exitSurfaceData = interfaces->physicsSurfaceProps->getSurfaceData(exitTrace.surface.surfaceProps);

    float damageModifier = 0.16f;
    float penetrationModifier = (enterSurfaceData->penetrationmodifier + exitSurfaceData->penetrationmodifier) / 2.0f;

    // Material-specific modifiers
    if (enterSurfaceData->material == 71 || enterSurfaceData->material == 89) // Glass/Grate
    {
        damageModifier = 0.05f;
        penetrationModifier = 3.0f;
    }
    else if (enterTrace.contents >> 3 & 1 || enterTrace.surface.flags >> 7 & 1) // Water/Light
    {
        penetrationModifier = 1.0f;
    }

    // Same material bonus
    if (enterSurfaceData->material == exitSurfaceData->material)
    {
        if (exitSurfaceData->material == 85 || exitSurfaceData->material == 87) // Tile/Wood
            penetrationModifier = 3.0f;
        else if (exitSurfaceData->material == 76) // Cardboard
            penetrationModifier = 2.0f;
    }

    // Calculate thickness
    float thickness = (exitTrace.endpos - enterTrace.endpos).length();

    // Angle penalty - чем больше угол, тем больше урона теряем
    float angleFactor = (std::max)(0.0f, enterTrace.plane.normal.dotProduct(direction));
    angleFactor = std::clamp(angleFactor, 0.7f, 1.0f);

    // Damage calculation с учетом угла
    float damageReduction = (11.25f / penetration / penetrationModifier) +
        (damage * damageModifier) +
        (thickness / 24.0f / penetrationModifier);

    damageReduction /= angleFactor; // Penalty за плохой угол

    damage -= damageReduction;

    // Проверка минимального damage threshold
    if (damage < 1.0f)
        return result;

    result.damage = damage;
    result.exitPoint = exitTrace.endpos;
    result.valid = true;
    result.hitsTaken = 1;

    return result;
}

void AimbotFunction::calculateArmorDamage(float armorRatio, int armorValue, bool hasHeavyArmor, float& damage) noexcept
{
    auto armorScale = 1.0f;
    auto armorBonusRatio = 0.5f;

    if (hasHeavyArmor)
    {
        armorRatio *= 0.2f;
        armorBonusRatio = 0.33f;
        armorScale = 0.25f;
    }

    auto newDamage = damage * armorRatio;
    const auto estiminated_damage = (damage - damage * armorRatio) * armorScale * armorBonusRatio;

    if (estiminated_damage > armorValue)
        newDamage = damage - armorValue / armorBonusRatio;

    damage = newDamage;
}

bool AimbotFunction::canScan(Entity* entity, const Vector& destination, const WeaponInfo* weaponData, int minDamage, bool allowFriendlyFire) noexcept
{
    if (!localPlayer)
        return false;

    float damage = static_cast<float>(weaponData->damage);
    Vector start = localPlayer->getEyePosition();
    Vector direction = (destination - start).normalized();

    int hitsLeft = 4; // До 4 стен
    float currentDistance = 0.0f;
    float maxDistance = (destination - start).length();

    while (damage >= static_cast<float>(minDamage) && hitsLeft > 0)
    {
        Trace trace;
        Vector traceEnd = start + direction * (maxDistance - currentDistance);
        interfaces->engineTrace->traceRay({ start, traceEnd }, 0x4600400B, localPlayer.get(), trace);

        // Friendly fire check
        if (!allowFriendlyFire && trace.entity && trace.entity->isPlayer() &&
            !localPlayer->isOtherEnemy(trace.entity))
            return false;

        // Если достигли таргета
        if (trace.fraction == 1.0f)
            break;

        // Update distance
        float tracedDistance = trace.fraction * (maxDistance - currentDistance);
        currentDistance += tracedDistance;

        // Range modifier
        damage *= std::pow(weaponData->rangeModifier, currentDistance / 500.0f);

        // Если попали в entity и это наш target
        if (trace.entity == entity && trace.hitgroup > HitGroup::Generic &&
            trace.hitgroup <= HitGroup::RightLeg)
        {
            // Apply hitgroup modifier
            damage *= HitGroup::getDamageMultiplier(
                trace.hitgroup, weaponData,
                trace.entity->hasHeavyArmor(),
                static_cast<int>(trace.entity->getTeamNumber())
            );

            // Apply armor
            if (float armorRatio = weaponData->armorRatio / 2.0f;
                HitGroup::isArmored(trace.hitgroup, trace.entity->hasHelmet(),
                    trace.entity->armor(), trace.entity->hasHeavyArmor()))
            {
                calculateArmorDamage(armorRatio, trace.entity->armor(),
                    trace.entity->hasHeavyArmor(), damage);
            }

            return damage >= static_cast<float>(minDamage);
        }

        // Wall penetration
        const auto surfaceData = interfaces->physicsSurfaceProps->getSurfaceData(trace.surface.surfaceProps);

        if (surfaceData->penetrationmodifier < 0.1f)
            break;

        auto penResult = handleBulletPenetration(
            surfaceData, trace, direction,
            weaponData->penetration, damage, hitsLeft
        );

        if (!penResult.valid)
            break;

        damage = penResult.damage;
        start = penResult.exitPoint;
        hitsLeft--;
    }

    return false;
}

static float handleBulletPenetrationLegit(SurfaceData* enterSurfaceData, const Trace& enterTrace, const Vector& direction, Vector& result, float penetration, float damage) noexcept
{
    Vector end;
    Trace exitTrace;

    if (!traceToExit(enterTrace, enterTrace.endpos, direction, end, exitTrace))
        return -1.0f;

    SurfaceData* exitSurfaceData = interfaces->physicsSurfaceProps->getSurfaceData(exitTrace.surface.surfaceProps);

    float damageModifier = 0.16f;
    float penetrationModifier = (enterSurfaceData->penetrationmodifier + exitSurfaceData->penetrationmodifier) / 2.0f;

    if (enterSurfaceData->material == 71 || enterSurfaceData->material == 89) {
        damageModifier = 0.05f;
        penetrationModifier = 3.0f;
    }
    else if (enterTrace.contents >> 3 & 1 || enterTrace.surface.flags >> 7 & 1) {
        penetrationModifier = 1.0f;
    }

    if (enterSurfaceData->material == exitSurfaceData->material) {
        if (exitSurfaceData->material == 85 || exitSurfaceData->material == 87)
            penetrationModifier = 3.0f;
        else if (exitSurfaceData->material == 76)
            penetrationModifier = 2.0f;
    }

    damage -= 11.25f / penetration / penetrationModifier + damage * damageModifier + (exitTrace.endpos - enterTrace.endpos).squareLength() / 24.0f / penetrationModifier;

    result = exitTrace.endpos;
    return damage;
}

float AimbotFunction::getScanDamage(Entity* entity, const Vector& destination, const WeaponInfo* weaponData, int minDamage, bool allowFriendlyFire) noexcept
{
    if (!localPlayer)
        return 0.f;

    float damage{ static_cast<float>(weaponData->damage) };

    Vector start{ localPlayer->getEyePosition() };
    Vector direction{ destination - start };
    float maxDistance{ direction.length() };
    float curDistance{ 0.0f };
    direction /= maxDistance;

    int hitsLeft = 4;

    while (damage >= 1.0f && hitsLeft) {
        Trace trace;
        interfaces->engineTrace->traceRay({ start, destination }, 0x4600400B, localPlayer.get(), trace);

        if (!allowFriendlyFire && trace.entity && trace.entity->isPlayer() && !localPlayer->isOtherEnemy(trace.entity))
            return 0.f;

        if (trace.fraction == 1.0f)
            break;

        curDistance += trace.fraction * (maxDistance - curDistance);
        damage *= std::pow(weaponData->rangeModifier, curDistance / 500.0f);

        if (trace.entity == entity && trace.hitgroup > HitGroup::Generic && trace.hitgroup <= HitGroup::RightLeg) {
            damage *= HitGroup::getDamageMultiplier(trace.hitgroup, weaponData, trace.entity->hasHeavyArmor(), static_cast<int>(trace.entity->getTeamNumber()));

            if (float armorRatio{ weaponData->armorRatio / 2.0f }; HitGroup::isArmored(trace.hitgroup, trace.entity->hasHelmet(), trace.entity->armor(), trace.entity->hasHeavyArmor()))
                calculateArmorDamage(armorRatio, trace.entity->armor(), trace.entity->hasHeavyArmor(), damage);

            if (damage >= minDamage)
                return damage;
            return 0.f;
        }
        const auto surfaceData = interfaces->physicsSurfaceProps->getSurfaceData(trace.surface.surfaceProps);

        if (surfaceData->penetrationmodifier < 0.1f)
            break;

        damage = handleBulletPenetrationLegit(surfaceData, trace, direction, start, weaponData->penetration, damage);
        hitsLeft--;
    }
    return 0.f;
}

float segmentToSegment(const Vector& s1, const Vector& s2, const Vector& k1, const Vector& k2) noexcept
{
    static auto constexpr epsilon = 0.00000001f;

    auto u = s2 - s1;
    auto v = k2 - k1;
    auto w = s1 - k1;

    auto a = u.dotProduct(u); //-V525
    auto b = u.dotProduct(v);
    auto c = v.dotProduct(v);
    auto d = u.dotProduct(w);
    auto e = v.dotProduct(w);
    auto D = a * c - b * b;

    auto sn = 0.0f, sd = D;
    auto tn = 0.0f, td = D;

    if (D < epsilon)
    {
        sn = 0.0f;
        sd = 1.0f;
        tn = e;
        td = c;
    }
    else
    {
        sn = b * e - c * d;
        tn = a * e - b * d;

        if (sn < 0.0f)
        {
            sn = 0.0f;
            tn = e;
            td = c;
        }
        else if (sn > sd)
        {
            sn = sd;
            tn = e + b;
            td = c;
        }
    }

    if (tn < 0.0f)
    {
        tn = 0.0f;

        if (-d < 0.0f)
            sn = 0.0f;
        else if (-d > a)
            sn = sd;
        else
        {
            sn = -d;
            sd = a;
        }
    }
    else if (tn > td)
    {
        tn = td;

        if (-d + b < 0.0f)
            sn = 0.0f;
        else if (-d + b > a)
            sn = sd;
        else
        {
            sn = -d + b;
            sd = a;
        }
    }

    auto sc = fabs(sn) < epsilon ? 0.0f : sn / sd;
    auto tc = fabs(tn) < epsilon ? 0.0f : tn / td;

    auto dp = w + u * sc - v * tc;
    return dp.length();
}

bool intersectLineWithBb(Vector& start, Vector& end, Vector& min, Vector& max) noexcept
{
    float d1, d2, f;
    auto start_solid = true;
    auto t1 = -1.0f, t2 = 1.0f;

    const float s[3] = { start.x, start.y, start.z };
    const float e[3] = { end.x, end.y, end.z };
    const float mi[3] = { min.x, min.y, min.z };
    const float ma[3] = { max.x, max.y, max.z };

    bool result = start_solid || (t1 < t2&& t1 >= 0.0f);
#pragma omp parallel for num_threads(maxThreadNum)
    for (auto i = 0; i < 6; ++i) {
        if (i >= 3) {
            const auto j = i - 3;

            d1 = s[j] - ma[j];
            d2 = d1 + e[j];
        }
        else {
            d1 = -s[i] + mi[i];
            d2 = d1 - e[i];
        }

        if (d1 > 0.0f && d2 > 0.0f)
            result = false;

        if (d1 <= 0.0f && d2 <= 0.0f)
            continue;

        if (d1 > 0)
            start_solid = false;

        if (d1 > d2) {
            f = d1;
            if (f < 0.0f)
                f = 0.0f;

            f /= d1 - d2;
            if (f > t1)
                t1 = f;
        }
        else {
            f = d1 / (d1 - d2);
            if (f < t2)
                t2 = f;
        }
    }

    return result;
}

void inline sinCos(float radians, float* sine, float* cosine)
{
    *sine = sin(radians);
    *cosine = cos(radians);
}

Vector vectorRotate(Vector& in1, Vector& in2) noexcept
{
    auto vector_rotate = [](const Vector& in1, const matrix3x4& in2)
    {
        return Vector(in1.dotProduct(in2[0]), in1.dotProduct(in2[1]), in1.dotProduct(in2[2]));
    };
    auto angleMatrix = [](const Vector& angles, matrix3x4& matrix)
    {
        float sr, sp, sy, cr, cp, cy;

        sinCos(Helpers::deg2rad(angles[1]), &sy, &cy);
        sinCos(Helpers::deg2rad(angles[0]), &sp, &cp);
        sinCos(Helpers::deg2rad(angles[2]), &sr, &cr);

        // matrix = (YAW * PITCH) * ROLL
        matrix[0][0] = cp * cy;
        matrix[1][0] = cp * sy;
        matrix[2][0] = -sp;

        float crcy = cr * cy;
        float crsy = cr * sy;
        float srcy = sr * cy;
        float srsy = sr * sy;
        matrix[0][1] = sp * srcy - crsy;
        matrix[1][1] = sp * srsy + crcy;
        matrix[2][1] = sr * cp;

        matrix[0][2] = (sp * crcy + srsy);
        matrix[1][2] = (sp * crsy - srcy);
        matrix[2][2] = cr * cp;

        matrix[0][3] = 0.0f;
        matrix[1][3] = 0.0f;
        matrix[2][3] = 0.0f;
    };
    matrix3x4 m;
    angleMatrix(in2, m);
    return vector_rotate(in1, m);
}

void vectorITransform(const Vector& in1, const matrix3x4& in2, Vector& out) noexcept
{
    out.x = (in1.x - in2[0][3]) * in2[0][0] + (in1.y - in2[1][3]) * in2[1][0] + (in1.z - in2[2][3]) * in2[2][0];
    out.y = (in1.x - in2[0][3]) * in2[0][1] + (in1.y - in2[1][3]) * in2[1][1] + (in1.z - in2[2][3]) * in2[2][1];
    out.z = (in1.x - in2[0][3]) * in2[0][2] + (in1.y - in2[1][3]) * in2[1][2] + (in1.z - in2[2][3]) * in2[2][2];
}

void vectorIRotate(Vector in1, matrix3x4 in2, Vector& out) noexcept
{
    out.x = in1.x * in2[0][0] + in1.y * in2[1][0] + in1.z * in2[2][0];
    out.y = in1.x * in2[0][1] + in1.y * in2[1][1] + in1.z * in2[2][1];
    out.z = in1.x * in2[0][2] + in1.y * in2[1][2] + in1.z * in2[2][2];
}

bool AimbotFunction::hitboxIntersection(const matrix3x4 matrix[MAXSTUDIOBONES], int iHitbox, StudioHitboxSet* set, const Vector& start, const Vector& end) noexcept
{
    auto VectorTransform_Wrapper = [](const Vector& in1, const matrix3x4 in2, Vector& out)
        {
            auto VectorTransform = [](const float* in1, const matrix3x4 in2, float* out)
                {
                    auto DotProducts = [](const float* v1, const float* v2)
                        {
                            return v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2];
                        };
                    out[0] = DotProducts(in1, in2[0]) + in2[0][3];
                    out[1] = DotProducts(in1, in2[1]) + in2[1][3];
                    out[2] = DotProducts(in1, in2[2]) + in2[2][3];
                };
            VectorTransform(&in1.x, in2, &out.x);
        };

    StudioBbox* hitbox = set->getHitbox(iHitbox);
    if (!hitbox)
        return false;

    if (hitbox->capsuleRadius == -1.f)
        return false;

    Vector mins, maxs;
    const auto isCapsule = hitbox->capsuleRadius != -1.f;
    if (isCapsule)
    {
        VectorTransform_Wrapper(hitbox->bbMin, matrix[hitbox->bone], mins);
        VectorTransform_Wrapper(hitbox->bbMax, matrix[hitbox->bone], maxs);
        const auto dist = segmentToSegment(start, end, mins, maxs);

        if (dist < hitbox->capsuleRadius)
            return true;
    }
    else
    {
        VectorTransform_Wrapper(vectorRotate(hitbox->bbMin, hitbox->offsetOrientation), matrix[hitbox->bone], mins);
        VectorTransform_Wrapper(vectorRotate(hitbox->bbMax, hitbox->offsetOrientation), matrix[hitbox->bone], maxs);

        vectorITransform(start, matrix[hitbox->bone], mins);
        vectorITransform(end, matrix[hitbox->bone], maxs);

        if (intersectLineWithBb(mins, maxs, hitbox->bbMin, hitbox->bbMax))
            return true;
    }
    return false;
}

std::vector<Vector> AimbotFunction::multiPoint(Entity* entity, const matrix3x4 matrix[MAXSTUDIOBONES], StudioBbox* hitbox, Vector localEyePos, int _hitbox, int _multiPointHead, int _multiPointBody)
{
    auto VectorTransformWrapper = [](const Vector& in1, const matrix3x4 in2, Vector& out)
        {
            auto VectorTransform = [](const float* in1, const matrix3x4 in2, float* out)
                {
                    auto dotProducts = [](const float* v1, const float* v2)
                        {
                            return v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2];
                        };
                    out[0] = dotProducts(in1, in2[0]) + in2[0][3];
                    out[1] = dotProducts(in1, in2[1]) + in2[1][3];
                    out[2] = dotProducts(in1, in2[2]) + in2[2][3];
                };
            VectorTransform(&in1.x, in2, &out.x);
        };

    Vector min, max, center;
    VectorTransformWrapper(hitbox->bbMin, matrix[hitbox->bone], min);
    VectorTransformWrapper(hitbox->bbMax, matrix[hitbox->bone], max);
    center = (min + max) * 0.5f;

    std::vector<Vector> vecArray;

    // Если мультипоинт выключен - возвращаем только центр
    if (_multiPointHead <= 0 && _hitbox == Hitboxes::Head)
    {
        vecArray.emplace_back(center);
        return vecArray;
    }
    if (_multiPointBody <= 0 && _hitbox != Hitboxes::Head)
    {
        vecArray.emplace_back(center);
        return vecArray;
    }

    // Рассчитываем направление к таргету
    Vector currentAngles = AimbotFunction::calculateRelativeAngle(localEyePos, center, Vector{});

    // Получаем forward, right, up вектора через Helpers::AngleVectors
    Vector forward, right, up;
    Helpers::AngleVectors(currentAngles, &forward, &right, &up);

    // Normalize vectors
    right = right.normalized();
    up = up.normalized();

    Vector left = right * -1.0f;
    Vector down = up * -1.0f;

    float multiPointHead = std::clamp(_multiPointHead, 0, 95) * 0.01f;
    float multiPointBody = std::clamp(_multiPointBody, 0, 95) * 0.01f;

    const float radius = hitbox->capsuleRadius;

    switch (_hitbox)
    {
    case Hitboxes::Head:
    {
        // CENTER POINT (highest priority)
        vecArray.emplace_back(center);

        // CARDINAL DIRECTIONS (4 points)
        vecArray.emplace_back(center + up * (radius * multiPointHead));      // Top
        vecArray.emplace_back(center + right * (radius * multiPointHead));   // Right
        vecArray.emplace_back(center + left * (radius * multiPointHead));    // Left
        vecArray.emplace_back(center + down * (radius * multiPointHead * 0.5f)); // Bottom (меньше чтобы не попасть в шею)

        // DIAGONAL DIRECTIONS (4 points) - для лучшего покрытия
        Vector topRight = (up + right).normalized();
        Vector topLeft = (up + left).normalized();
        Vector bottomRight = (down + right).normalized();
        Vector bottomLeft = (down + left).normalized();

        vecArray.emplace_back(center + topRight * (radius * multiPointHead * 0.85f));
        vecArray.emplace_back(center + topLeft * (radius * multiPointHead * 0.85f));
        vecArray.emplace_back(center + bottomRight * (radius * multiPointHead * 0.7f));
        vecArray.emplace_back(center + bottomLeft * (radius * multiPointHead * 0.7f));

        // ADAPTIVE POINTS - на основе угла врага (2 точки)
        if (entity && entity->getAnimstate())
        {
            float enemyYaw = entity->eyeAngles().y;
            float ourYaw = currentAngles.y;
            float yawDelta = Helpers::normalizeYaw(enemyYaw - ourYaw);

            // Если враг смотрит в нашу сторону - добавляем точки по бокам головы
            if (std::abs(yawDelta) < 90.0f)
            {
                Vector sideOffset = yawDelta > 0 ? right : left;
                vecArray.emplace_back(center + sideOffset * (radius * multiPointHead * 0.9f));
                vecArray.emplace_back(center + sideOffset * (radius * multiPointHead * 0.7f) + up * (radius * multiPointHead * 0.3f));
            }
            else
            {
                // Если смотрит в сторону - точки сзади головы
                Vector backOffset = forward * -1.0f;
                vecArray.emplace_back(center + backOffset * (radius * multiPointHead * 0.5f));
            }
        }

        break;
    }

    case Hitboxes::Neck:
    {
        // Шея - меньше точек, т.к. маленький хитбокс
        vecArray.emplace_back(center);
        vecArray.emplace_back(center + right * (radius * multiPointBody * 0.6f));
        vecArray.emplace_back(center + left * (radius * multiPointBody * 0.6f));
        break;
    }

    case Hitboxes::UpperChest:
    case Hitboxes::Thorax:
    case Hitboxes::LowerChest:
    case Hitboxes::Belly:
    {
        // CENTER
        vecArray.emplace_back(center);

        // SIDES (enhanced coverage)
        vecArray.emplace_back(center + right * (radius * multiPointBody));
        vecArray.emplace_back(center + left * (radius * multiPointBody));

        // ADDITIONAL COVERAGE для лучшего хита
        vecArray.emplace_back(center + right * (radius * multiPointBody * 0.6f));
        vecArray.emplace_back(center + left * (radius * multiPointBody * 0.6f));

        // Верх/низ хитбокса для вертикального покрытия
        vecArray.emplace_back(center + up * (radius * multiPointBody * 0.5f));
        vecArray.emplace_back(center + down * (radius * multiPointBody * 0.5f));

        break;
    }

    case Hitboxes::Pelvis:
    {
        // Таз - важный хитбокс для body aim
        vecArray.emplace_back(center);
        vecArray.emplace_back(center + right * (radius * multiPointBody * 0.75f));
        vecArray.emplace_back(center + left * (radius * multiPointBody * 0.75f));
        vecArray.emplace_back(center + up * (radius * multiPointBody * 0.5f));
        break;
    }

    case Hitboxes::LeftUpperArm:
    case Hitboxes::RightUpperArm:
    case Hitboxes::LeftForearm:
    case Hitboxes::RightForearm:
    case Hitboxes::LeftHand:
    case Hitboxes::RightHand:
    {
        // Для рук меньше точек
        vecArray.emplace_back(center);
        vecArray.emplace_back(center + right * (radius * multiPointBody * 0.7f));
        vecArray.emplace_back(center + left * (radius * multiPointBody * 0.7f));
        break;
    }

    case Hitboxes::LeftThigh:
    case Hitboxes::RightThigh:
    {
        // Бедра
        vecArray.emplace_back(center);
        vecArray.emplace_back(center + right * (radius * multiPointBody * 0.8f));
        vecArray.emplace_back(center + left * (radius * multiPointBody * 0.8f));
        vecArray.emplace_back(center + up * (radius * multiPointBody * 0.5f));
        break;
    }

    case Hitboxes::LeftCalf:
    case Hitboxes::RightCalf:
    {
        // Икры
        vecArray.emplace_back(center);
        vecArray.emplace_back(center + right * (radius * multiPointBody * 0.7f));
        vecArray.emplace_back(center + left * (radius * multiPointBody * 0.7f));
        break;
    }

    case Hitboxes::LeftFoot:
    case Hitboxes::RightFoot:
    {
        // Ступни - минимум точек
        vecArray.emplace_back(center);
        vecArray.emplace_back(center + right * (radius * multiPointBody * 0.5f));
        vecArray.emplace_back(center + left * (radius * multiPointBody * 0.5f));
        break;
    }

    default:
        vecArray.emplace_back(center);
        break;
    }

    return vecArray;
}

std::vector<Vector> multiPointDynamic(
    Entity* entity,
    matrix3x4* matrix,
    StudioBbox* hitbox,
    Vector localEyePos,
    int hitboxIndex,
    int headScale,
    int bodyScale
) noexcept {

    // Вычисляем расстояние до цели
    const float distance = localEyePos.distTo(
        matrix[hitbox->bone].origin()
    );

    // Динамическая корректировка scale на основе расстояния
    float distanceMultiplier = 1.0f;

    if (distance < 500.0f) {
        // Близко - увеличиваем количество точек
        distanceMultiplier = 1.3f;
    }
    else if (distance > 1500.0f) {
        // Далеко - уменьшаем для производительности
        distanceMultiplier = 0.7f;
    }

    int adjustedHeadScale = static_cast<int>(headScale * distanceMultiplier);
    int adjustedBodyScale = static_cast<int>(bodyScale * distanceMultiplier);

    // Вызываем оригинальную функцию с adjusted values
    return AimbotFunction::multiPoint(
        entity,
        matrix,
        hitbox,
        localEyePos,
        hitboxIndex,
        adjustedHeadScale,
        adjustedBodyScale
    );
}

// AimbotFunctions.cpp - УЛУЧШЕННАЯ ВЕРСИЯ hitChance

bool AimbotFunction::hitChance(Entity* localPlayer, Entity* entity, StudioHitboxSet* set, const matrix3x4 matrix[MAXSTUDIOBONES], Entity* activeWeapon, const Vector& destination, const UserCmd* cmd, const int hitChance) noexcept
{
    static auto isSpreadEnabled = interfaces->cvar->findVar("weapon_accuracy_nospread");
    if (!hitChance || isSpreadEnabled->getInt() >= 1)
        return true;

    // Adaptive sampling - начинаем с меньшего количества сэмплов
    constexpr int minSeeds = 64;
    constexpr int maxSeeds = 256;
    int currentMaxSeeds = minSeeds;

    const Angle angles(destination + cmd->viewangles);

    const auto weapSpread = activeWeapon->getSpread();
    const auto weapInaccuracy = activeWeapon->getInaccuracy();
    const auto localEyePosition = localPlayer->getEyePosition();
    const auto range = activeWeapon->getWeaponData()->range;

    const auto weaponIndex = activeWeapon->itemDefinitionIndex2();

    int hits = 0;
    int totalShots = 0;
    int hitsNeed = static_cast<int>(static_cast<float>(maxSeeds) * (static_cast<float>(hitChance) / 100.f));

    // Адаптивный сэмплинг в несколько этапов
    for (int stage = 0; stage < 3; ++stage)
    {
        const int stageSeeds = stage == 0 ? 64 : (stage == 1 ? 128 : 256);

#pragma omp parallel for reduction(+:hits,totalShots) num_threads(maxThreadNum)
        for (int i = totalShots; i < stageSeeds; ++i)
        {
            memory->randomSeed(i + 1);
            float inaccuracy = memory->randomFloat(0.f, 1.f);
            float spread = memory->randomFloat(0.f, 1.f);
            const float spreadX = memory->randomFloat(0.f, 2.f * static_cast<float>(M_PI));
            const float spreadY = memory->randomFloat(0.f, 2.f * static_cast<float>(M_PI));

            // Revolver special case
            if (weaponIndex == WeaponId::Revolver && (cmd->buttons & UserCmd::IN_ATTACK2))
            {
                inaccuracy = 1.f - inaccuracy * inaccuracy;
                spread = 1.f - spread * spread;
            }

            inaccuracy *= weapInaccuracy;
            spread *= weapSpread;

            Vector spreadView{
                (cosf(spreadX) * inaccuracy) + (cosf(spreadY) * spread),
                (sinf(spreadX) * inaccuracy) + (sinf(spreadY) * spread)
            };

            Vector direction{
                (angles.forward + (angles.right * spreadView.x) + (angles.up * spreadView.y)) * range
            };

            static Trace trace;
            interfaces->engineTrace->clipRayToEntity({ localEyePosition, localEyePosition + direction }, 0x4600400B, entity, trace);

            if (trace.entity == entity)
                ++hits;

            ++totalShots;
        }

        // Early termination checks после каждого stage
        float currentHitRate = static_cast<float>(hits) / static_cast<float>(totalShots);
        float requiredHitRate = static_cast<float>(hitChance) / 100.f;

        // Если явно проходим
        if (currentHitRate >= requiredHitRate + 0.15f)
            return true;

        // Если явно не проходим
        if (currentHitRate < requiredHitRate - 0.15f && stage > 0)
            return false;

        // Если близко к границе - продолжаем на следующий stage
        if (stage < 2 && std::abs(currentHitRate - requiredHitRate) < 0.1f)
            continue;

        // На последнем stage - точное сравнение
        if (stage == 2)
        {
            int actualHitsNeed = static_cast<int>(static_cast<float>(totalShots) * requiredHitRate);
            return hits >= actualHitsNeed;
        }
    }

    return false;
}
