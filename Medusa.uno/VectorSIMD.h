#pragma once
#pragma once
#include <immintrin.h> // SSE/AVX intrinsics
#include "SDK/Vector.h"

// SIMD-оптимизированные векторные операции
namespace VectorSIMD {

    // ✅ Быстрое вычисление расстояния (SSE2)
    inline float distanceFast(const Vector& a, const Vector& b) noexcept {
        __m128 v1 = _mm_set_ps(0.f, a.z, a.y, a.x);
        __m128 v2 = _mm_set_ps(0.f, b.z, b.y, b.x);

        __m128 diff = _mm_sub_ps(v1, v2);
        __m128 squared = _mm_mul_ps(diff, diff);

        // Горизонтальная сумма
        __m128 sum1 = _mm_hadd_ps(squared, squared);
        __m128 sum2 = _mm_hadd_ps(sum1, sum1);

        return _mm_cvtss_f32(_mm_sqrt_ss(sum2));
    }

    // ✅ Быстрое вычисление 2D расстояния
    inline float distance2DFast(const Vector& a, const Vector& b) noexcept {
        __m128 v1 = _mm_set_ps(0.f, 0.f, a.y, a.x);
        __m128 v2 = _mm_set_ps(0.f, 0.f, b.y, b.x);

        __m128 diff = _mm_sub_ps(v1, v2);
        __m128 squared = _mm_mul_ps(diff, diff);

        __m128 sum = _mm_hadd_ps(squared, squared);
        return _mm_cvtss_f32(_mm_sqrt_ss(sum));
    }

    // ✅ Батч-обработка расстояний (обработка 4х целей за раз)
    inline void batchDistance(const Vector& origin,
        const Vector* targets, // массив из 4 векторов
        float* results) noexcept {

        __m128 ox = _mm_set1_ps(origin.x);
        __m128 oy = _mm_set1_ps(origin.y);
        __m128 oz = _mm_set1_ps(origin.z);

        // Загружаем 4 цели
        __m128 tx = _mm_set_ps(targets[3].x, targets[2].x, targets[1].x, targets[0].x);
        __m128 ty = _mm_set_ps(targets[3].y, targets[2].y, targets[1].y, targets[0].y);
        __m128 tz = _mm_set_ps(targets[3].z, targets[2].z, targets[1].z, targets[0].z);

        // Вычисляем разницу
        __m128 dx = _mm_sub_ps(tx, ox);
        __m128 dy = _mm_sub_ps(ty, oy);
        __m128 dz = _mm_sub_ps(tz, oz);

        // Возводим в квадрат
        dx = _mm_mul_ps(dx, dx);
        dy = _mm_mul_ps(dy, dy);
        dz = _mm_mul_ps(dz, dz);

        // Суммируем
        __m128 sum = _mm_add_ps(_mm_add_ps(dx, dy), dz);

        // Извлекаем sqrt
        __m128 distances = _mm_sqrt_ps(sum);

        // Сохраняем результаты
        _mm_store_ps(results, distances);
    }

    // ✅ Быстрое dot product
    inline float dotProductFast(const Vector& a, const Vector& b) noexcept {
        __m128 v1 = _mm_set_ps(0.f, a.z, a.y, a.x);
        __m128 v2 = _mm_set_ps(0.f, b.z, b.y, b.x);

        __m128 mul = _mm_mul_ps(v1, v2);
        __m128 sum1 = _mm_hadd_ps(mul, mul);
        __m128 sum2 = _mm_hadd_ps(sum1, sum1);

        return _mm_cvtss_f32(sum2);
    }

    // ✅ Быстрая нормализация
    inline Vector normalizeFast(const Vector& v) noexcept {
        __m128 vec = _mm_set_ps(0.f, v.z, v.y, v.x);

        __m128 squared = _mm_mul_ps(vec, vec);
        __m128 sum1 = _mm_hadd_ps(squared, squared);
        __m128 sum2 = _mm_hadd_ps(sum1, sum1);

        __m128 length = _mm_sqrt_ss(sum2);
        __m128 lengthBroadcast = _mm_shuffle_ps(length, length, 0);

        __m128 normalized = _mm_div_ps(vec, lengthBroadcast);

        alignas(16) float result[4];
        _mm_store_ps(result, normalized);

        return Vector(result[0], result[1], result[2]);
    }

    // ✅ Batch transform (трансформация 4х точек за раз)
    struct Matrix4x4 {
        __m128 rows[4];
    };

    inline void batchTransform(const Matrix4x4& mat,
        const Vector* input, // 4 вектора
        Vector* output) noexcept {

        for (int i = 0; i < 4; i++) {
            __m128 v = _mm_set_ps(1.f, input[i].z, input[i].y, input[i].x);

            __m128 x = _mm_dp_ps(mat.rows[0], v, 0xF1);
            __m128 y = _mm_dp_ps(mat.rows[1], v, 0xF2);
            __m128 z = _mm_dp_ps(mat.rows[2], v, 0xF4);

            __m128 result = _mm_or_ps(_mm_or_ps(x, y), z);

            alignas(16) float res[4];
            _mm_store_ps(res, result);

            output[i] = Vector(res[0], res[1], res[2]);
        }
    }
}
