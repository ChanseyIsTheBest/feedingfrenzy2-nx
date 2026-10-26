// D3D call trace (PC test aid, 2026-10-26: mapping Renut's hooks to Feeding Frenzy 2's Direct3D,
// docs/feedingfrenzy2-notes.md): every function of the Direct3D library (0x82167000-0x82186400) that code outside it
// calls (119, out/renut_map/api.txt) is hooked, but Present sub_8217EAD0 (the frame hook has it). With
// feedingfrenzy2_d3d_trace = <game frame>, the log gets, per function: its calls up to that frame, whether r3 was the
// device (the word Direct3D_CreateDevice sub_8217D478 fills through its r8: a field of the game's renderer here, not a
// global), its first arguments and caller; and the order of the calls during that frame. Off by default; the PC build
// only.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "feedingfrenzy2_capture.h"

REXCVAR_DEFINE_INT32(feedingfrenzy2_d3d_trace, 0, "FeedingFrenzy2",
                     "PC test aid: log the Direct3D API calls up to and during this game frame (0 = off)");

namespace {

constexpr uint32_t kCreateDevice = 0x8217D478;
uint32_t g_device_slot = 0;  // Direct3D_CreateDevice's r8
struct Stat {
  uint32_t addr = 0;
  uint64_t calls = 0, device_calls = 0;
  uint32_t first[3][6] = {};
};
std::mutex g_mutex;
std::vector<Stat> g_stats;
std::vector<std::string> g_frame_calls;
bool g_dumped = false;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t be;
  std::memcpy(&be, base + a, 4);
  return __builtin_bswap32(be);
}

void Trace(uint32_t addr, PPCContext& ctx, uint8_t* base) {
  const int32_t at = REXCVAR_GET(feedingfrenzy2_d3d_trace);
  if (at <= 0 || g_dumped) return;
  const uint64_t frame = g_feedingfrenzy2_game_frames.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (addr == kCreateDevice) g_device_slot = ctx.r8.u32;
  const uint32_t device = g_device_slot ? Load32(base, g_device_slot) : 0;
  Stat* s = nullptr;
  for (auto& x : g_stats) if (x.addr == addr) s = &x;
  if (!s) {
    g_stats.push_back({});
    s = &g_stats.back();
    s->addr = addr;
  }
  const bool is_dev = device && ctx.r3.u32 == device;
  if (s->calls < 3) {
    uint32_t* f = s->first[s->calls];
    f[0] = ctx.r3.u32; f[1] = ctx.r4.u32; f[2] = ctx.r5.u32; f[3] = ctx.r6.u32; f[4] = ctx.r7.u32;
    f[5] = uint32_t(ctx.lr);
  }
  // The shader containers (CreatePixelShader 8216CAD0, CreateVertexShader 8216CBE0) and the vertex elements
  // (CreateVertexDeclaration 8216B678): their first 96 bytes, on the first three calls.
  if (s->calls < 3 && (addr == 0x8216CAD0u || addr == 0x8216CBE0u || addr == 0x8216B678u) && ctx.r3.u32) {
    std::string hex;
    for (uint32_t o = 0; o < 96; o += 4) hex += fmt::format(" {:08X}", Load32(base, ctx.r3.u32 + o));
    REXLOG_INFO("[d3dtrace] {:08X} r3 {:08X}:{}", addr, ctx.r3.u32, hex);
  }
  // Every shader container created (virtual part + physical part) to out/ff_shaders/<ps|vs>_<n>.bin, for the
  // container-version study (XDK 2571 writes version 0x0E; Renut and XenosRecomp read 0x11).
  if ((addr == 0x8216CAD0u || addr == 0x8216CBE0u) && ctx.r3.u32) {
    const uint32_t c = ctx.r3.u32;
    const uint32_t size = Load32(base, c + 4) + Load32(base, c + 8);
    if (size > 0 && size < 0x40000) {
      static int n = 0;
      const std::string path = fmt::format("out/ff_shaders/{}_{:02}.bin", addr == 0x8216CAD0u ? "ps" : "vs", n++);
      std::filesystem::create_directories("out/ff_shaders");
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(base + c, 1, size, f);
        std::fclose(f);
      }
    }
  }
  ++s->calls;
  if (is_dev) ++s->device_calls;
  if (frame == uint64_t(at) && g_frame_calls.size() < 4000) {
    g_frame_calls.push_back(fmt::format("{:08X}{} r4={:X} r5={:X} r6={:X} lr={:08X}", addr, is_dev ? " dev" : "",
                                        ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr)));
  }
  if (frame > uint64_t(at)) {
    g_dumped = true;
    REXLOG_INFO("[d3dtrace] device 0x{:08X}; {} functions called up to frame {}", device, g_stats.size(), at);
    for (const auto& x : g_stats) {
      std::string first;
      for (uint64_t k = 0; k < x.calls && k < 3; ++k)
        first += fmt::format(" [{:X} {:X} {:X} {:X} {:X} lr {:08X}]", x.first[k][0], x.first[k][1], x.first[k][2],
                             x.first[k][3], x.first[k][4], x.first[k][5]);
      REXLOG_INFO("[d3dtrace] {:08X} calls {} (r3 = device {}):{}", x.addr, x.calls, x.device_calls, first);
    }
    REXLOG_INFO("[d3dtrace] frame {}: {} calls", at, g_frame_calls.size());
    for (const auto& c : g_frame_calls) REXLOG_INFO("[d3dtrace]   {}", c);
  }
}

}  // namespace

#define FF_D3D_TRACE(ADDR)                            extern "C" REX_FUNC(__imp__sub_##ADDR);            REX_HOOK_RAW(sub_##ADDR) {                            Trace(0x##ADDR##u, ctx, base);                     __imp__sub_##ADDR(ctx, base);                    }

FF_D3D_TRACE(82167044)
FF_D3D_TRACE(821670C0)
FF_D3D_TRACE(82167128)
FF_D3D_TRACE(82167158)
FF_D3D_TRACE(821671F0)
FF_D3D_TRACE(82167208)
FF_D3D_TRACE(82167290)
FF_D3D_TRACE(821673B0)
FF_D3D_TRACE(82167840)
FF_D3D_TRACE(82168B88)
FF_D3D_TRACE(82168B98)
FF_D3D_TRACE(82168C50)
FF_D3D_TRACE(82168C68)
FF_D3D_TRACE(82168D20)
FF_D3D_TRACE(82168D28)
FF_D3D_TRACE(82168D30)
FF_D3D_TRACE(82168DE0)
FF_D3D_TRACE(82168F00)
FF_D3D_TRACE(82169028)
FF_D3D_TRACE(821690C0)
FF_D3D_TRACE(821690E0)
FF_D3D_TRACE(82169128)
FF_D3D_TRACE(82169148)
FF_D3D_TRACE(8216A110)
FF_D3D_TRACE(8216A130)
FF_D3D_TRACE(8216A148)
FF_D3D_TRACE(8216A678)
FF_D3D_TRACE(8216AD68)
FF_D3D_TRACE(8216AE48)
FF_D3D_TRACE(8216AF28)
FF_D3D_TRACE(8216AF88)
FF_D3D_TRACE(8216AFE8)
FF_D3D_TRACE(8216B040)
FF_D3D_TRACE(8216B098)
FF_D3D_TRACE(8216B0E8)
FF_D3D_TRACE(8216B3A0)
FF_D3D_TRACE(8216B570)
FF_D3D_TRACE(8216B678)
FF_D3D_TRACE(8216B7E8)
FF_D3D_TRACE(8216B7F0)
FF_D3D_TRACE(8216C570)
FF_D3D_TRACE(8216C5A0)
FF_D3D_TRACE(8216C5B8)
FF_D3D_TRACE(8216C5D0)
FF_D3D_TRACE(8216C5E8)
FF_D3D_TRACE(8216C600)
FF_D3D_TRACE(8216C618)
FF_D3D_TRACE(8216C648)
FF_D3D_TRACE(8216C660)
FF_D3D_TRACE(8216C918)
FF_D3D_TRACE(8216CAD0)
FF_D3D_TRACE(8216CBE0)
FF_D3D_TRACE(8216DE10)
FF_D3D_TRACE(8216DE18)
FF_D3D_TRACE(8216EAA8)
FF_D3D_TRACE(8216F6C8)
FF_D3D_TRACE(8216F6E0)
FF_D3D_TRACE(8216F6F8)
FF_D3D_TRACE(8216FD68)
FF_D3D_TRACE(8216FD70)
FF_D3D_TRACE(8216FD78)
FF_D3D_TRACE(8216FD80)
FF_D3D_TRACE(8216FD98)
FF_D3D_TRACE(8216FDB0)
FF_D3D_TRACE(8216FDC8)
FF_D3D_TRACE(8216FDE0)
FF_D3D_TRACE(8216FDE8)
FF_D3D_TRACE(8216FDF0)
FF_D3D_TRACE(8216FF50)
FF_D3D_TRACE(8216FF68)
FF_D3D_TRACE(8216FFB0)
FF_D3D_TRACE(82171128)
FF_D3D_TRACE(82171250)
FF_D3D_TRACE(821715F0)
FF_D3D_TRACE(82171650)
FF_D3D_TRACE(82171688)
FF_D3D_TRACE(82171718)
FF_D3D_TRACE(821717A8)
FF_D3D_TRACE(82171838)
FF_D3D_TRACE(82171D40)
FF_D3D_TRACE(82172858)
FF_D3D_TRACE(82172F08)
FF_D3D_TRACE(82172F18)
FF_D3D_TRACE(82172F38)
FF_D3D_TRACE(821730C8)
FF_D3D_TRACE(82173A28)
FF_D3D_TRACE(82173B48)
FF_D3D_TRACE(82173BD8)
FF_D3D_TRACE(82173C20)
FF_D3D_TRACE(82174600)
FF_D3D_TRACE(821748D8)
FF_D3D_TRACE(821748E0)
FF_D3D_TRACE(82174EA0)
FF_D3D_TRACE(82174F18)
FF_D3D_TRACE(82174F98)
FF_D3D_TRACE(82174FE8)
FF_D3D_TRACE(821750B0)
FF_D3D_TRACE(82175100)
FF_D3D_TRACE(82175F20)
FF_D3D_TRACE(821767B0)
FF_D3D_TRACE(82176B90)
FF_D3D_TRACE(82177588)
FF_D3D_TRACE(82177920)
FF_D3D_TRACE(821779E0)
FF_D3D_TRACE(82177B60)
FF_D3D_TRACE(82177CC0)
FF_D3D_TRACE(82178A58)
FF_D3D_TRACE(821792C0)
FF_D3D_TRACE(821799E0)
FF_D3D_TRACE(8217AFB0)
FF_D3D_TRACE(8217AFC8)
FF_D3D_TRACE(8217D1F0)
FF_D3D_TRACE(8217D248)
FF_D3D_TRACE(8217D2E0)
FF_D3D_TRACE(8217D478)
FF_D3D_TRACE(82185548)
FF_D3D_TRACE(82185968)
FF_D3D_TRACE(82186290)
