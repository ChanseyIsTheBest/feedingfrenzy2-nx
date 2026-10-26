// h_821E7AC8.h: hand-written native of sub_821E7AC8 (this r3, mask r4): when this->[268] has a bit of `mask` that
// this->[264] lacks, this->[268] = (this->[268] & ~mask) | this->[264], then each child of the vector at this + 308
// (begin +312, end +316, the debug iterator's checks as traps) ORs its [268] into this->[268]; then a tail call of
// this->[12]'s virtual method 348 / 4 (r4 = mask) when this->[12] is set. Feeding Frenzy 1's sub_82256198 with other
// offsets (feedingfrenzy-nx's h_82256198.h). Reference: app/generated/hot/n_821E7AC8.h (docs/hand-natives.md,
// docs/optimising-functions.md). The hottest generated function of the emulator profile (out/renut_tests/emu/prof1:
// 152 of the generated code's 2,850 samples).
//
// Optimisations:
//   - the vector's begin and end words are loaded once: the copy reloads them three times an iteration, and its only
//     store (this + 268) cannot reach this + 312 / + 316;
//   - this->[268] is kept in a local between the loop's stores: each iteration still stores it (memory is the copy's
//     after every iteration), and the copy's reload of it reads the value stored the iteration before (the loop has no
//     other store; the child loads before it are loads; the vector's storage is not this object, so the store cannot
//     reach an element);
//   - every trap condition (begin > end, a length that is not a multiple of 4: p would step past end) is decided before
//     the first store, so such a call runs the copy from its start instead (nothing is done yet; begin / end are read
//     before the store here, after it in the copy: the store cannot reach them).
// Audit (the copy's effects in order and the context it leaves; cr0, cr6, ctr and xer are its function-local flags:
// the context keeps the caller's, except around the tail call):
//   - r10 = [264], r9 = [268], r11 = r9 & ~r10 (its `and.` with the mask is a dead record form: only cr0 sees the
//     masked value); the masked value's low word 0: return with r9, r10, r11 those values;
//   - else [268] = (r9 & ~mask) | r10, then for p from begin while p != end: [268] = child(p)->[268] | [268], stored;
//   - after the loop r7 = begin, r8 = end, r9 = end, r10 = end (the copy's last loads; its r9 steps from begin to end
//     in 64 bits, no wrap below 4 GB), r11 = this + 308 (64-bit add); r3 = this->[12]; zero: return with r3, r7-r11;
//   - else r11 = the vtable's word 348, ctr = r11; the call gets cr0 = gt (r3 != 0, unsigned), cr6 = eq (p == end),
//     xer = 0 (the copy's local, untouched), ctr, r3 and r7-r11 (r4, the mask, as it came); after the call the
//     context is the callee's, but cr0, cr6, ctr and xer get the caller's entry values back.
#pragma once

namespace feedingfrenzy2::hot::h_821E7AC8 {

template <bool kLog>
inline void Body(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  const uint64_t mask = ctx.r4.u64;
  const uint64_t w264 = REX_LOAD_D_U32(self, 264);
  const uint64_t w268 = REX_LOAD_D_U32(self, 268);
  const uint64_t lacks = w268 & ~w264;
  if (uint32_t(lacks & mask) == 0) {
    ctx.r9.u64 = w268;
    ctx.r10.u64 = w264;
    ctx.r11.u64 = lacks;
    return;
  }
  const uint32_t begin = REX_LOAD_D_U32(self, 312), end = REX_LOAD_D_U32(self, 316);
  if (begin > end || ((end - begin) & 3u) != 0) {
    n_821E7AC8::Native(ctx, base);  // the copy traps as the original does
    return;
  }
  uint32_t acc = uint32_t((w268 & ~mask) | w264);
  REX_STORE_D_U32(self, 268, acc);
  for (uint32_t p = begin; p != end; p += 4) {
    const uint32_t child = REX_LOAD_D_U32(p, 0);
    acc = uint32_t(REX_LOAD_D_U32(child, 268)) | acc;
    REX_STORE_D_U32(self, 268, acc);
  }
  ctx.r7.u64 = begin;
  ctx.r8.u64 = end;
  ctx.r9.u64 = end;
  ctx.r10.u64 = end;
  ctx.r11.s64 = ctx.r3.s64 + 308;
  const uint32_t object = REX_LOAD_D_U32(self, 12);
  ctx.r3.u64 = object;
  if (object == 0) return;
  ctx.r11.u64 = REX_LOAD_D_U32(REX_LOAD_D_U32(object, 0), 348);
  const PPCCRRegister entry_cr0 = ctx.cr0, entry_cr6 = ctx.cr6;
  const PPCRegister entry_ctr = ctx.ctr;
  const PPCXERRegister entry_xer = ctx.xer;
  PPCXERRegister xer{};
  PPCCRRegister cr0{}, cr6{};
  PPCRegister ctr{};
  cr0.compare<uint32_t>(object, 0, xer);
  cr6.compare<uint32_t>(end, end, xer);
  ctr.u64 = ctx.r11.u64;
  ctx.cr0 = cr0;
  ctx.cr6 = cr6;
  ctx.ctr = ctr;
  ctx.xer = xer;
  REX_CALL_INDIRECT_FUNC(ctr.u32);
  ctx.cr0 = entry_cr0;
  ctx.cr6 = entry_cr6;
  ctx.ctr = entry_ctr;
  ctx.xer = entry_xer;
}

inline void Native(PPCContext& ctx, uint8_t* base) { Body<false>(ctx, base); }

}  // namespace feedingfrenzy2::hot::h_821E7AC8
