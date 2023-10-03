// Auto-generated file. Do not edit!
//   Template: src/f16-vbinary/vopc-neonfp16arith.c.in
//   Generator: tools/xngen
//
// Copyright 2020 Google LLC
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>

#include <arm_neon.h>

#include <xnnpack/common.h>
#include <xnnpack/vbinary.h>


void xnn_f16_vmaxc_ukernel__neonfp16arith_x8(
    size_t batch,
    const void* restrict input_a,
    const void* restrict input_b,
    void* restrict output,
    const union xnn_f16_default_params params[restrict XNN_MIN_ELEMENTS(1)]) XNN_OOB_READS
{
  assert(batch != 0);
  assert(batch % sizeof(__fp16) == 0);
  assert(input_a != NULL);
  assert(input_b != NULL);
  assert(output != NULL);

  const __fp16* a = (const __fp16*) input_a;
  const __fp16* b = (const __fp16*) input_b;
  __fp16* o = (__fp16*) output;


  const float16x8_t vb = vld1q_dup_f16(b);
  for (; batch >= 8 * sizeof(__fp16); batch -= 8 * sizeof(__fp16)) {
    const float16x8_t va01234567 = vld1q_f16(a); a += 8;

    float16x8_t vy01234567 = vmaxq_f16(va01234567, vb);
    vst1q_f16(o, vy01234567); o += 8;
  }
  if XNN_UNLIKELY(batch != 0) {
    const float16x8_t va01234567 = vld1q_f16(a);

    float16x8_t vy01234567 = vmaxq_f16(va01234567, vb);

    float16x4_t vy0123 = vget_low_f16(vy01234567);
    if (batch & (4 * sizeof(__fp16))) {
      vst1_f16(o, vy0123); o += 4;
      vy0123 = vget_high_f16(vy01234567);
    }

    if (batch & (2 * sizeof(__fp16))) {
      vst1_lane_u32((void*) o, vreinterpret_u32_f16(vy0123), 0); o += 2;
      vy0123 = vext_f16(vy0123, vy0123, 2);
    }

    if (batch & (1 * sizeof(__fp16))) {
      vst1_lane_f16(o, vy0123, 0);
    }
  }
}
