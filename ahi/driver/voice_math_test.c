/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host test for the 32-bit arithmetic of the AHI channel model
 * (voice_model.c, "68060-safe arithmetic"). Every helper must return exactly
 * what the plain 64-bit expression, or the shared zz9k/voice.h function, does
 * for every input. The helpers are static, so this test includes the source.
 *
 * The inputs are the edges of each fast path (a 32-bit distance, a distance
 * that needs 48 bits, a step just below and at 65536, a quotient at 2^32) and
 * a few million random values.
 */

#include <stdio.h>
#include <stdlib.h>

#include "voice_model.c"

static int failures;

#define CHECK(cond, ...)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      if (failures < 20) {                                            \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);                   \
        printf(__VA_ARGS__);                                          \
        printf("\n");                                                 \
      }                                                               \
      failures++;                                                     \
    }                                                                 \
  } while (0)

static uint32_t rng_state = 0x2545F491U;

static uint32_t rnd(void)
{
  uint32_t x = rng_state;

  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state = x;
  return x;
}

/* A random value with a random number of significant bits, so small and
 * large operands are both common. */
static uint32_t rnd_bits(void)
{
  uint32_t bits = rnd() % 33U;

  return bits == 0U ? 0U : rnd() >> (32U - bits);
}

static uint64_t pass_frames_ref(int backward, uint32_t pos, uint32_t frac,
                                uint32_t bound, uint32_t step)
{
  return zz9k_voice_pass_frames(backward, pos, frac, bound, step);
}

static void check_pass(int backward, uint32_t pos, uint32_t frac,
                       uint32_t bound, uint32_t step)
{
  uint32_t want = (uint32_t)pass_frames_ref(backward, pos, frac, bound, step);
  uint32_t got = vm_pass_frames(backward, pos, frac, bound, step);

  CHECK(got == want,
        "pass_frames(bw=%d pos=%lu frac=%lu bound=%lu step=%lu): got %lu want %lu",
        backward, (unsigned long)pos, (unsigned long)frac,
        (unsigned long)bound, (unsigned long)step, (unsigned long)got,
        (unsigned long)want);
}

/* A pass of d whole frames to the bound. */
static void check_distance(uint32_t d, uint32_t frac, uint32_t step)
{
  static const uint32_t bounds[] = {0U, 5U, 0x1000U};
  uint32_t i;

  for (i = 0U; i < 3U; i++) {
    uint32_t b = bounds[i];

    if (d <= 0xFFFFFFFFU - b) {
      check_pass(1, b + d, frac, b, step);
      check_pass(0, b, frac, b + d, step);
    }
  }
  /* Forward with the position at the top of the range. */
  check_pass(0, 0xFFFFFFFFU - d, frac, 0xFFFFFFFFU, step);
  check_pass(1, 0xFFFFFFFFU, frac, 0xFFFFFFFFU - d, step);
}

static void test_mul32(void)
{
  static const uint32_t edge[] = {
      0U,          1U,          2U,          0xFFFFU,     0x10000U,
      0x10001U,    0x7FFFFFFFU, 0x80000000U, 0xFFFF0000U, 0xFFFFFFFEU,
      0xFFFFFFFFU, 262144U,     57600U,      0x12345678U};
  uint32_t i;
  uint32_t j;

  for (i = 0U; i < sizeof(edge) / sizeof(edge[0]); i++) {
    for (j = 0U; j < sizeof(edge) / sizeof(edge[0]); j++) {
      uint64_t want = (uint64_t)edge[i] * edge[j];

      CHECK(vm_mul32(edge[i], edge[j]) == want, "mul32(%lu, %lu)",
            (unsigned long)edge[i], (unsigned long)edge[j]);
    }
  }
  for (i = 0U; i < 4000000U; i++) {
    uint32_t a = rnd_bits();
    uint32_t b = rnd_bits();

    CHECK(vm_mul32(a, b) == (uint64_t)a * b, "mul32(%lu, %lu)",
          (unsigned long)a, (unsigned long)b);
  }
}

static void test_rate_step(void)
{
  uint32_t hz;

  for (hz = 0U; hz <= 400000U; hz++) {
    CHECK(vm_rate_step(hz) == zz9k_voice_rate_step(hz), "rate_step(%lu)",
          (unsigned long)hz);
  }
  for (hz = 0xFFFFFF00U; hz != 0U; hz++) {
    CHECK(vm_rate_step(hz) == zz9k_voice_rate_step(hz), "rate_step(%lu)",
          (unsigned long)hz);
  }
}

static void test_pass_frames_edges(void)
{
  static const uint32_t steps[] = {
      0U,     1U,     2U,     3U,      7U,      255U,    256U,
      4096U,  7040U,  32768U, 57600U,  65534U,  65535U,  65536U,
      65537U, 98304U, 262144U, 0x7FFFFFFFU, 0x80000000U, 0xFFFFFFFFU};
  static const uint32_t fracs[] = {0U,      1U,          0x7FFFU,    0x8000U,
                                   0xFFFFU, 0x10000U,    0x10001U,
                                   0xFFFFFFFFU};
  uint32_t si;
  uint32_t fi;
  uint32_t d;
  int k;

  for (si = 0U; si < sizeof(steps) / sizeof(steps[0]); si++) {
    uint32_t step = steps[si];

    for (fi = 0U; fi < sizeof(fracs) / sizeof(fracs[0]); fi++) {
      uint32_t frac = fracs[fi];

      for (d = 0U; d < 300U; d++) {
        check_distance(d, frac, step);
      }
      for (d = 65500U; d < 65600U; d++) {
        check_distance(d, frac, step);
        check_distance(0xFFFFFFFFU - d + 65500U, frac, step);
      }
      /* Distances whose pass is 2^32 frames or one frame short or over: d *
       * 65536 / step near 2^32 means d near step * 65536. */
      for (k = -3; k <= 3; k++) {
        check_distance((step << 16) + (uint32_t)k, frac, step);
        check_distance((step << 16 >> 1) + (uint32_t)k, frac, step);
        check_distance(0xFFFFFFFFU / (step ? step : 1U) + (uint32_t)k, frac,
                       step);
        check_distance(0xFFFFFFFFU - (uint32_t)(k < 0 ? -k : k), frac, step);
      }
      /* The quotient at 2^32 - 1, 2^32: need = step * q + r. */
      for (k = -2; k <= 2; k++) {
        uint64_t need = (uint64_t)step * 0xFFFFFFFFULL + (uint64_t)(int64_t)k;
        uint64_t whole = (need + frac) >> 16;

        if (whole <= 0xFFFFFFFFULL) {
          check_distance((uint32_t)whole, frac, step);
          check_distance((uint32_t)whole + 1U, frac, step);
        }
      }
    }
  }
}

static void test_pass_frames_random(void)
{
  uint32_t i;

  for (i = 0U; i < 6000000U; i++) {
    uint32_t step;
    uint32_t frac;
    uint32_t d;

    switch (rnd() % 4U) {
      case 0U:
        step = rnd() % 262145U; /* every rate of the engine */
        break;
      case 1U:
        step = rnd() % 65536U;
        break;
      case 2U:
        step = 65536U + rnd() % 32U;
        break;
      default:
        step = rnd_bits();
        break;
    }
    frac = (rnd() % 8U) != 0U ? rnd() & 0xFFFFU : rnd_bits();
    d = rnd_bits();
    check_distance(d, frac, step);
    check_pass(rnd() & 1U, rnd_bits(), frac, rnd_bits(), step);
  }
}

/* The model's level math as it was before the 32-bit paths. */
static void levels_ref(uint32_t master_m16, uint32_t channels, int32_t volume,
                       int32_t pan, uint16_t *left, uint16_t *right)
{
  uint64_t hp;
  uint32_t gain;
  uint32_t l;
  uint32_t r;
  uint64_t factor;

  vm_ideal_levels(volume, pan, &l, &r);
  if (channels == 0U) {
    channels = 1U;
  }
  hp = ((uint64_t)master_m16 << 8) / channels;
  gain = (uint32_t)((hp + 0xFFFFU) >> 16);
  if (gain == 0U && master_m16 != 0U) {
    gain = 1U;
  }
  if (gain > VM_MASTER_MAX_GAIN) {
    gain = VM_MASTER_MAX_GAIN;
  }
  if (gain == 0U) {
    *left = 0U;
    *right = 0U;
    return;
  }
  factor = hp / gain;
  l = (uint32_t)(((uint64_t)l * factor) >> 16);
  r = (uint32_t)(((uint64_t)r * factor) >> 16);
  *left = (uint16_t)(l > 0xFFFFU ? 0xFFFFU : l);
  *right = (uint16_t)(r > 0xFFFFU ? 0xFFFFU : r);
}

static void test_levels(void)
{
  static const uint32_t masters[] = {0U,          1U,          0xFFU,
                                     0x10000U,    0x20000U,    0xFFFFFFU,
                                     0x1000000U,  0x1000001U,  0x7FFFFFFFU,
                                     0xFFFFFFFFU};
  uint32_t i;
  uint32_t mi;
  uint32_t ch;

  for (mi = 0U; mi < sizeof(masters) / sizeof(masters[0]); mi++) {
    for (ch = 0U; ch <= 40U; ch++) {
      for (i = 0U; i < 400U; i++) {
        int32_t vol = (int32_t)rnd_bits();
        int32_t pan = (int32_t)rnd_bits();
        uint16_t l0, r0, l1, r1;

        if (i == 0U) {
          vol = 0x10000;
          pan = 0x8000;
        }
        levels_ref(masters[mi], ch, vol, pan, &l0, &r0);
        vm_levels(masters[mi], ch, vol, pan, &l1, &r1);
        CHECK(l0 == l1 && r0 == r1,
              "levels(master=%lu ch=%lu vol=%ld pan=%ld): %u,%u want %u,%u",
              (unsigned long)masters[mi], (unsigned long)ch, (long)vol,
              (long)pan, l1, r1, l0, r0);
      }
    }
  }
  for (i = 0U; i < 1000000U; i++) {
    uint32_t master = rnd_bits();
    uint32_t channels = rnd() % 33U;
    int32_t vol = (int32_t)rnd_bits();
    int32_t pan = (int32_t)rnd_bits();
    uint16_t l0, r0, l1, r1;

    levels_ref(master, channels, vol, pan, &l0, &r0);
    vm_levels(master, channels, vol, pan, &l1, &r1);
    CHECK(l0 == l1 && r0 == r1, "levels(master=%lu ch=%lu)",
          (unsigned long)master, (unsigned long)channels);
  }
}

int main(void)
{
  test_mul32();
  test_rate_step();
  test_pass_frames_edges();
  test_pass_frames_random();
  test_levels();
  if (failures != 0) {
    printf("voice_math_test: %d FAILURES\n", failures);
    return 1;
  }
  printf("voice_math_test: ok\n");
  return 0;
}
