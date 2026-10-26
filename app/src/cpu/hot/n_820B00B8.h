// sub_820B00B8 - wmemcmp: r3 = a, r4 = b, r5 = n (wide characters), returns r3 = 0, 1 or -1 (as 0x00000000FFFFFFFF).
// feedingfrenzy2-nx (2026-10-26): fourth in the emulator profile (out/renut_tests/emu/prof1: 52 samples), the first
// routine of .text (CRT-like, 3 call sites). 22 instructions:
//   n == 0 (32-bit) -> r3 = 0;
//   for each of the n positions (the count compared as 32 bits): a[i] != b[i] (lhz: unsigned big-endian halfwords) ->
//     r3 = a[i] >= b[i] ? 1 : 0xFFFFFFFF (`subfc; subfe; rlwinm r11,r11,0,0,30; addi r3,r11,1`: the result is not
//     sign-extended);
//   all equal -> r3 = 0.
// Big-endian halfwords compare as their bytes do (the high byte first), so the first difference and its sign are
// memcmp's over 2n bytes. Ranges that wrap around 4 GB or straddle 0xE0000000 (the PC hosts' physical-range offset
// changes there) are declined (the original runs).
// Registers: the original leaves r5 and r8-r11 as scratch; the native writes r3 only.
#pragma once
#include <cstring>

#include "../feedingfrenzy2_hot_common.h"

namespace feedingfrenzy2::hot::n_820B00B8 {

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t a = ctx.r3.u32, b = ctx.r4.u32, n = ctx.r5.u32;
  if (n == 0) {
    ctx.r3.u64 = 0;
    return true;
  }
  const uint64_t bytes = uint64_t(n) * 2;
  for (const uint32_t p : {a, b}) {
    if (uint64_t(p) + bytes > 0x100000000ull) return false;
    if (p < 0xE0000000u && uint64_t(p) + bytes > 0xE0000000ull) return false;
  }
  const int r = std::memcmp(Raw(base, a), Raw(base, b), size_t(bytes));
  ctx.r3.u64 = r == 0 ? 0 : (r > 0 ? 1 : 0xFFFFFFFFull);
  return true;
}

inline void Writes(const PPCContext&, const uint8_t*, feedingfrenzy2::hot::Writes&) {}  // no stores

}  // namespace feedingfrenzy2::hot::n_820B00B8
