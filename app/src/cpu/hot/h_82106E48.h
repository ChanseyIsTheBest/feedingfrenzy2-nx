// h_82106E48.h: hand-written native of sub_82106E48 (this r3): the last element of a ring buffer (start this->[12],
// count this->[16], capacity this->[8], storage this->[4], 4-byte elements), through an iterator {0, this, index} it
// builds in its frame and hands to sub_821062A0 (the iterator's dereference, a leaf), with the debug checks as traps.
// Reference: app/generated/hot/n_82106E48.h, and n_821062A0.h for the callee (docs/hand-natives.md,
// docs/optimising-functions.md). Second and third in the emulator profile (out/renut_tests/emu/prof1: 76 samples
// each, the callee's mostly through this caller).
//
// Optimisations:
//   - the leaf callee inline (docs/hand-natives.md: a leaf the native reproduces exactly, every register it leaves
//     behind included): no call, no context round trip, no hook;
//   - the callee's loads of the iterator are the values just stored (the same words; zero-extended as its lwz gives
//     them), and its reloads of this->[12] / [16] the values loaded before (the only stores in between are the frame's
//     words below the caller's stack pointer, where no live object is);
//   - every trap condition (the copy's two, the callee's two) is decided before the first store, so such a call runs
//     the copy from its start (nothing is done yet).
// Audit (the copy's effects in order and the context it leaves; cr6 / cr0 / xer are function-local in both and never
// reach the context):
//   - [r1 - 8] = lr (32 bits), the back chain at r1 - 112 (REX_CHAIN_STORE), r1's low word - 112;
//   - end = [16] + [12] (64-bit add), index = end - 1; traps: start > end (32-bit), index > end or index < start;
//   - frame + 84 = this, frame + 88 = index, frame + 80 = 0 (in that order); the callee's context: r3 = frame + 80;
//   - the callee: r8 = this, r10 = index (its loads of the iterator), trap if this == 0; r11 = this, r7 = index,
//     r9 = [16], r11 = [16] + [12] (64 bits), trap unless index < that (32-bit); r11 = [8], r10 = index - [8] when
//     [8] <= index (64 bits), r11 = [4], r10 = (r10 << 2) as 32 bits, r3 = [r11 + r10] (32-bit sum);
//   - back: r1 + 112 (64 bits), r12 = [r1 - 8] (zero-extended), lr = r12. The context ends with r1, r3, r7-r12, lr.
#pragma once

namespace feedingfrenzy2::hot::h_82106E48 {

template <bool kLog>
inline void Body(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  const uint64_t start = REX_LOAD_D_U32(self, 12), count = REX_LOAD_D_U32(self, 16);
  const uint64_t end = count + start;
  const uint32_t index = uint32_t(int64_t(end) - 1);
  if (uint32_t(start) > uint32_t(end) || index > uint32_t(end) || index < uint32_t(start) || self == 0 ||
      !(index < uint32_t(end))) {
    n_82106E48::Native(ctx, base);  // the copy (and its callee) trap as the original does
    return;
  }
  PPCRegister r1 = ctx.r1;
  const uint64_t lr = ctx.lr;
  REX_STORE_D_U32(r1.u32, -8, uint32_t(lr));
  const uint32_t frame = -112 + r1.u32;
  REX_CHAIN_STORE(REX_STORE_U32(frame, r1.u32));
  r1.u32 = frame;
  REX_STORE_D_U32(frame, 84, self);
  REX_STORE_D_U32(frame, 88, index);
  REX_STORE_D_U32(frame, 80, 0u);
  // The callee, inline.
  const uint64_t capacity = REX_LOAD_D_U32(self, 8);
  uint64_t slot = index;
  if (!(uint32_t(capacity) > index)) slot = slot - capacity;
  const uint64_t storage = REX_LOAD_D_U32(self, 4);
  const uint32_t offset = uint32_t(uint32_t(slot) << 2);
  ctx.r3.u64 = REX_LOAD_U32(uint32_t(storage) + offset);
  ctx.r7.u64 = index;
  ctx.r8.u64 = self;
  ctx.r9.u64 = count;
  ctx.r10.u64 = offset;
  ctx.r11.u64 = storage;
  // Back in sub_82106E48.
  r1.s64 = r1.s64 + 112;
  const uint64_t r12 = REX_LOAD_D_U32(r1.u32, -8);
  ctx.lr = r12;
  ctx.r1 = r1;
  ctx.r12.u64 = r12;
}

inline void Native(PPCContext& ctx, uint8_t* base) { Body<false>(ctx, base); }

}  // namespace feedingfrenzy2::hot::h_82106E48
