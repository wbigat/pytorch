#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <omp.h>

#include <ATen/NumericUtils.h>
#include <ATen/core/PhiloxRNGEngine.h>
#include <ATen/native/Math.h>

#include <c10/util/Float8_e4m3fn.h>
#include <c10/util/Float8_e5m2.h>
#include <c10/util/BFloat16.h>
#include <c10/util/BFloat16-math.h>
#include <c10/util/generic_math.h>
#include <c10/util/Half.h>
#include <c10/util/TypeCast.h>

#if defined(CPU_CAPABILITY_AVX512) || defined(CPU_CAPABILITY_AVX2) || defined(CPU_CAPABILITY_ZVECTOR) || defined(CPU_CAPABILITY_NEON)
#define INDUCTOR_USE_VECTOR_TYPES() 1
#else
#define INDUCTOR_USE_VECTOR_TYPES() 0
#endif

#if INDUCTOR_USE_VECTOR_TYPES()
#include <ATen/cpu/vec/functional.h>
#include <ATen/cpu/vec/vec.h>
#endif

typedef at::Half half;
typedef at::BFloat16 bfloat16;

typedef at::Float8_e4m3fn float8_e4m3fn;
typedef at::Float8_e5m2 float8_e5m2;

template <typename T>
struct Welford {
  T mean = T(0);
  T m2 = T(0);
  T weight = T(0);
};


template <typename T>
struct IsVecType: std::false_type {};

#if INDUCTOR_USE_VECTOR_TYPES()
template <typename T>
struct IsVecType<at::vec::Vectorized<T>>: std::true_type {};
#endif

template <typename T>
Welford<T> welford_combine(const Welford<T> &a, const Welford<T> &b) {
  if constexpr (!IsVecType<T>::value) {
    if (a.weight == 0) {
      return b;
    }
    if (b.weight == 0) {
      return a;
    }
  }
  auto delta = b.mean - a.mean;
  auto new_weight = a.weight + b.weight;
  auto wb_over_w = b.weight / new_weight;
  if constexpr (IsVecType<T>::value) {
    // Guard against division by zero
    wb_over_w = T::blendv(wb_over_w, T(0), new_weight == T(0));
  }
  auto result = Welford<T>{
    a.mean + delta * wb_over_w,
    a.m2 + b.m2 + delta * delta * a.weight * wb_over_w,
    new_weight
  };
  return result;
}

template <typename T>
Welford<T> welford_combine(const Welford<T> &acc, T data) {
  // Add a single data point
  auto delta = data - acc.mean;
  auto new_weight = acc.weight + T(1);
  auto new_mean = acc.mean + delta / new_weight;
  auto new_delta = data - new_mean;
  auto result = Welford<T>{
    new_mean,
    acc.m2 + delta * new_delta,
    new_weight
  };
  return result;
}

// Refer to https://github.com/pytorch/pytorch/blob/b5b36cf0c4e1958f1ff25120f5d4beeef3288187/
// aten/src/ATen/native/SharedReduceOps.h#L419-L445
template <typename scalar_t>
inline bool greater_or_nan(scalar_t a, scalar_t b, int64_t idx_a, int64_t idx_b) {
  // If (a == b), then choose the one with lower idx, else max(a, b)
  if (at::_isnan(a)) {
    if (at::_isnan(b)) {
      return idx_a < idx_b;
    }
    return true;
  }
  return (a == b) ? idx_a < idx_b : (a > b);
}

template <typename scalar_t>
inline bool less_or_nan(scalar_t a, scalar_t b, int64_t idx_a, int64_t idx_b) {
  // If (a == b), then choose the one with lower idx, else min(a, b)
  if (at::_isnan(a)) {
    if (at::_isnan(b)) {
      return idx_a < idx_b;
    }
    return true;
  }
  return (a == b) ? idx_a < idx_b : (a < b);
}

#if INDUCTOR_USE_VECTOR_TYPES()
template <typename scalar_t>
inline at::vec::Vectorized<scalar_t> vec_shuffle_down(at::vec::Vectorized<scalar_t> x, size_t n) {
  using Vec = at::vec::Vectorized<scalar_t>;
  alignas(alignof(Vec)) scalar_t array[Vec::size()];
  x.store(array);
  for (size_t i = 0; i + n < Vec::size(); i += 2 * n) {
    array[i] = array[i + n];
  }
  return Vec::loadu(array);
}

#ifdef CPU_CAPABILITY_AVX2
inline at::vec::Vectorized<float> vec_shuffle_down(at::vec::Vectorized<float> x, size_t n) {
  using vec_t = at::vec::Vectorized<float>;
#define SHUFFLE_MASK(z, y, x, w) ((z << 6) | (y << 4) | (x << 2) | w)
  switch (n) {
  case 1:
    return vec_t(_mm256_permute_ps(x, SHUFFLE_MASK(1, 1, 3, 3)));
  case 2:
    return vec_t(_mm256_permute_ps(x, SHUFFLE_MASK(2, 2, 2, 2)));
  case 4:
    return vec_t(_mm256_permute2f128_ps(x, x, SHUFFLE_MASK(1, 1, 1, 1)));
  }
  TORCH_CHECK(false, "Unhandled vec_shuffle_down value ", n);
}
#endif

template <typename scalar_t>
Welford<scalar_t> welford_vec_reduce_all(Welford<at::vec::Vectorized<scalar_t>> acc) {
  using Vec = at::vec::Vectorized<scalar_t>;
  for (size_t n = 1; n < Vec::size(); n *= 2) {
    auto shuffled = Welford<Vec>{
      vec_shuffle_down(acc.mean, n),
      vec_shuffle_down(acc.m2, n),
      vec_shuffle_down(acc.weight, n)
    };
    acc = welford_combine(acc, shuffled);
  }

  Welford<scalar_t> result;
  alignas(alignof(Vec)) scalar_t array[Vec::size()];
  acc.mean.store(array);
  result.mean = array[0];

  acc.m2.store(array);
  result.m2 = array[0];

  acc.weight.store(array);
  result.weight = array[0];

  return result;
}
#endif


template <typename T, typename U> inline typename std::common_type<T, U>::type mod(T a, U b) { return a % b; }
template <> inline float mod(float a, float b) { return std::fmod(a, b); }
template <> inline double mod(double a, double b) { return std::fmod(a, b); }

template <typename scalar_t>
inline scalar_t max_propagate_nan(scalar_t a, scalar_t b) {
  if (at::_isnan(a)) {
    return a;
  }
  return a > b ? a : b;
}

template <typename scalar_t>
inline scalar_t min_propagate_nan(scalar_t a, scalar_t b) {
  if (at::_isnan(a)) {
    return a;
  }
  return a < b ? a : b;
}

constexpr float uint32_to_uniform_float(uint32_t value) {
  // maximum value such that `MAX_INT * scale < 1.0` (with float rounding)
  constexpr float scale = 4.6566127342e-10;
  return static_cast<float>(value & 0x7FFFFFFF) * scale;
}

float normalized_rand_cpu(uint32_t seed, uint32_t offset) {
  return uint32_to_uniform_float(at::Philox4_32(seed, 0, offset)());
}

float randn_cpu(uint32_t seed, uint32_t offset) {
  at::Philox4_32 engine(seed, 0, offset);
  return engine.randn(10);
}

int64_t randint64_cpu(uint32_t seed, uint32_t offset, int64_t low, int64_t high) {
  auto gen = at::Philox4_32(seed, 0, offset);
  uint64_t r0 = gen();
  uint64_t r1 = gen();
  uint64_t result = r0 | (r1 << 32);
  return static_cast<int64_t>(result % (high - low)) + low;
}

template <typename T> struct AsIntegerType { typedef T type; };
template <> struct AsIntegerType<float> { typedef uint32_t type; };
template <> struct AsIntegerType<double> { typedef uint64_t type; };
template <> struct AsIntegerType<bfloat16> { typedef uint16_t type; };

template <typename T>
typename std::enable_if_t<!std::is_reduced_floating_point_v<T>, T>
inline fetch_value(volatile T *addr) {
  return *addr;
}

template <typename T>
typename std::enable_if_t<std::is_reduced_floating_point_v<T>, T>
inline fetch_value(volatile T *addr) {
  return T(addr->x, T::from_bits());
}

template <typename T>
typename std::enable_if_t<!std::is_integral_v<T>>
atomic_add(volatile T *addr, T offset) {
  typedef typename AsIntegerType<T>::type alt_type;

  static_assert(sizeof(std::atomic<alt_type>) == sizeof(T),
                "std::atomic issue");

  alt_type expected;

  alt_type desired;

  std::atomic<alt_type> *atomic_addr = (std::atomic<alt_type> *)addr;
  do {
    T val = fetch_value(addr);
    reinterpret_cast<T *>(&expected)[0] = val;
    reinterpret_cast<T *>(&desired)[0] = val + offset;
  } while (!atomic_addr->compare_exchange_weak(expected, desired,
                                               std::memory_order_relaxed));
}

// Since C++20 float is supported by fetch_add, but the performance may not
// better than compare_exchange_weak, which can be checked by microbenchmark
// inductor_cpu_atomic.py
template <typename T>
typename std::enable_if_t<std::is_integral_v<T>>
atomic_add(volatile T *addr, T offset) {
  static_assert(sizeof(std::atomic<T>) == sizeof(T),
                "std::atomic issue");
  std::atomic<T> *atomic_addr = (std::atomic<T> *)addr;
  atomic_addr->fetch_add(offset, std::memory_order_relaxed);
}

#if defined(CPU_CAPABILITY_AVX512) || defined(CPU_CAPABILITY_AVX2) || defined(CPU_CAPABILITY_ZVECTOR) || defined(CPU_CAPABILITY_NEON)

template <typename scalar_t>
inline at::vec::Vectorized<float> cvt_lowp_fp_to_fp32(
    at::vec::Vectorized<scalar_t> src) {
  at::vec::Vectorized<float> res_vec1(0);
  at::vec::Vectorized<float> res_vec2(0);
  std::tie(res_vec1, res_vec2) = at::vec::convert_to_float<scalar_t>(src);
  return res_vec1;
}

template <typename scalar_t>
inline at::vec::Vectorized<scalar_t> cvt_fp32_to_lowp_fp(
    at::vec::Vectorized<float> src) {
  return at::vec::convert_from_float<scalar_t>(src, src);
}

inline at::vec::Vectorized<float> cvt_int64_to_fp32(at::vec::VectorizedN<int64_t,2> src) {
# if defined(CPU_CAPABILITY_AVX512)
  auto low = _mm512_cvtepi64_ps(src[0]);
  auto high = _mm512_cvtepi64_ps(src[1]);
  return _mm512_insertf32x8(_mm512_castps256_ps512(low), high, 1);
# elif defined(CPU_CAPABILITY_AVX2)
  auto low_double = at::vec::convert_to_fp_of_same_size<double>(src[0]);
  auto low = _mm256_cvtpd_ps(low_double);
  auto high_double = at::vec::convert_to_fp_of_same_size<double>(src[1]);
  auto high = _mm256_cvtpd_ps(high_double);
  return _mm256_insertf128_ps(_mm256_castps128_ps256(low), high, 1);
# else
  constexpr int float_vec_size = at::vec::Vectorized<float>::size();
  constexpr int int64_vec_size = at::vec::Vectorized<int64_t>::size();
  __at_align__ float result[float_vec_size];
  __at_align__ int64_t src_buf[int64_vec_size];
  for (int i = 0; i < 2; i++) {
    src[i].store(src_buf);
    for (int j = 0; j < int64_vec_size; j++) {
      result[i * int64_vec_size + j] = static_cast<float>(src_buf[j]);
    }
  }
  return at::vec::Vectorized<float>::loadu(result);
# endif
}

inline at::vec::VectorizedN<int64_t,2> cvt_fp32_to_int64(at::vec::Vectorized<float> src) {
  at::vec::VectorizedN<int64_t,2> result;
# if defined(CPU_CAPABILITY_AVX512)
  result[0] = _mm512_cvt_roundps_epi64(_mm512_castps512_ps256(src), _MM_FROUND_TO_ZERO |_MM_FROUND_NO_EXC);
  result[1] = _mm512_cvt_roundps_epi64(_mm512_extractf32x8_ps(src, 1), _MM_FROUND_TO_ZERO |_MM_FROUND_NO_EXC);
# elif defined(CPU_CAPABILITY_AVX2)
  auto int32_vec = at::vec::convert_to_int_of_same_size(src);
  result[0] = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(int32_vec));
  result[1] = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(int32_vec, 1));
# else
  constexpr int float_vec_size = at::vec::Vectorized<float>::size();
  constexpr int int64_vec_size = at::vec::Vectorized<int64_t>::size();
  __at_align__ float src_buf[float_vec_size];
  __at_align__ int64_t result_buf[int64_vec_size];
  src.store(src_buf);
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j < int64_vec_size; j++) {
      result_buf[j] = static_cast<int64_t>(src_buf[i * int64_vec_size + j]);
    }
    result[i] = at::vec::Vectorized<int64_t>::loadu(result_buf);
  }
# endif
  return result;
}

inline at::vec::Vectorized<int32_t> cvt_int64_to_int32(at::vec::VectorizedN<int64_t,2> src) {
# if defined(CPU_CAPABILITY_AVX512)
  auto low = _mm512_cvtepi64_epi32(src[0]);
  auto high = _mm512_cvtepi64_epi32(src[1]);
  return _mm512_inserti32x8(_mm512_castsi256_si512(low), high, 1);
# elif defined(CPU_CAPABILITY_AVX2)
  auto low = _mm256_shuffle_epi32(src[0], _MM_SHUFFLE(2, 0, 2, 0));
  auto high = _mm256_shuffle_epi32(src[1], _MM_SHUFFLE(2, 0, 2, 0));
  auto low_perm = _mm256_permute4x64_epi64(low, _MM_SHUFFLE(3, 1, 2, 0));
  auto high_perm = _mm256_permute4x64_epi64(high, _MM_SHUFFLE(3, 1, 2, 0));
  return _mm256_blend_epi32(low_perm, high_perm, 0xF0);
# else
  constexpr int int32_vec_size = at::vec::Vectorized<int32_t>::size();
  constexpr int int64_vec_size = at::vec::Vectorized<int64_t>::size();
  __at_align__ int32_t result[int32_vec_size];
  __at_align__ int64_t src_buf[int64_vec_size];
  for (int i = 0; i < 2; i++) {
    src[i].store(src_buf);
    for (int j = 0; j < int64_vec_size; j++) {
      result[i * int64_vec_size + j] = static_cast<int32_t>(src_buf[j]);
    }
  }
  return at::vec::Vectorized<int32_t>::loadu(result);
# endif
}

inline at::vec::VectorizedN<int64_t,2> cvt_int32_to_int64(at::vec::Vectorized<int32_t> src) {
  at::vec::VectorizedN<int64_t,2> result;
# if defined(CPU_CAPABILITY_AVX512)
  result[0] = _mm512_cvtepi32_epi64(_mm512_castsi512_si256(src));
  result[1] = _mm512_cvtepi32_epi64(_mm512_extracti32x8_epi32(src, 1));
# elif defined(CPU_CAPABILITY_AVX2)
  result[0] = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(src));
  result[1] = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(src, 1));
#else
  constexpr int int32_vec_size = at::vec::Vectorized<int32_t>::size();
  constexpr int int64_vec_size = at::vec::Vectorized<int64_t>::size();
  __at_align__ int32_t src_buf[int32_vec_size];
  __at_align__ int64_t result_buf[int64_vec_size];
  src.store(src_buf);
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j < int64_vec_size; j++) {
      result_buf[j] = static_cast<int64_t>(src_buf[i * int64_vec_size + j]);
    }
    result[i] = at::vec::Vectorized<int64_t>::loadu(result_buf);
  }
# endif
  return result;
}

template <typename T, std::enable_if_t<std::is_same_v<T, int8_t> || std::is_same_v<T, uint8_t>, int> = 0>
inline at::vec::Vectorized<int32_t> cvt_int8_to_int32(at::vec::Vectorized<T> src) {
# if defined(CPU_CAPABILITY_AVX512)
  auto src128 = _mm512_castsi512_si128(src);
  if constexpr (std::is_same_v<T, int8_t>) {
    return _mm512_cvtepi8_epi32(src128);
  } else {
    return _mm512_cvtepu8_epi32(src128);
  }
# elif defined(CPU_CAPABILITY_AVX2)
  auto src128 = _mm256_castsi256_si128(src);
  if constexpr (std::is_same_v<T, int8_t>) {
    return _mm256_cvtepi8_epi32(src128);
  } else {
    return _mm256_cvtepu8_epi32(src128);
  }
# else
  constexpr int int32_vec_size = at::vec::Vectorized<int32_t>::size();
  constexpr int int8_vec_size = at::vec::Vectorized<T>::size();
  __at_align__ int32_t result[int32_vec_size];
  __at_align__ T src_buf[int8_vec_size];
  src.store(src_buf);
  for (int i = 0; i < int32_vec_size; i++) {
    result[i] = static_cast<int32_t>(src_buf[i]);
  }
  return at::vec::Vectorized<int32_t>::loadu(result);
# endif
}

template <typename T, std::enable_if_t<std::is_same_v<T, int8_t> || std::is_same_v<T, uint8_t>, int> = 0>
inline at::vec::VectorizedN<int64_t, 2> cvt_int8_to_int64(at::vec::Vectorized<T> src) {
  return cvt_int32_to_int64(cvt_int8_to_int32(src));
}

#endif

#ifdef CPU_CAPABILITY_NEON
namespace at::vec {
template <typename T>
typename std::enable_if_t<std::is_same_v<T, uint8_t> || std::is_same_v<T, int8_t>, at::vec::Vectorized<float>>
inline convert_int8_to_float(at::vec::Vectorized<T> src) {
  // Note: this function only convert inputs number of elements equal to at::vec::Vectorized<float>.size()
  T data[at::vec::Vectorized<T>::size()];
  float rc[at::vec::Vectorized<float>::size()];
  src.store(data);
  for(auto i = 0; i < at::vec::Vectorized<float>::size(); ++i) {
          rc[i] = static_cast<float>(data[i]);
  }
  return at::vec::Vectorized<float>::loadu(rc);
}
}
#endif
