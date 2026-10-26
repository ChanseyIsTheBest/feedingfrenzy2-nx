// Counts the frames the game presents, for the video recorder (feedingfrenzy2_capture.cpp waits for each new one).
//
// The frame is D3DDevice_Present (feedingfrenzy2-nx: sub_8217EAD0, which ends with D3DDevice_Swap sub_8217E538, the only
// function of default.xex that calls the kernel's VdSwap). The hook calls the original and counts; it changes no PPC
// register.
//
// How the wrapping works: each recompiled function sub_XXXXXXXX is a weak alias of __imp__sub_XXXXXXXX. The strong
// definition here wins for every caller (direct, and indirect through the function table), and __imp__ is still the
// original.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>


#include "feedingfrenzy2_capture.h"
#include "feedingfrenzy2_pc_profiler.h"
#if FEEDINGFRENZY2_NATIVE_RENDERER
#include "me/me_native_system.h"
#endif
#if FEEDINGFRENZY2_RENUT_RENDERER
#include <thread>

#include "renut_engine/display_sync.h"  // 2026-10-20: banjo_display_sync (PaceRenutSwap)
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/banjo_shared_hooks.h"
#include "me/feedingfrenzy2_render_mode.h"
#endif

// feedingfrenzy2_fps = 60 (docs/feedingfrenzy2-notes.md, "60 frames a second").
//
// The Xbox 360 game presents every second vblank (its Direct3D device's presentation interval is 2), and its frame
// step (the function at 0x82 42F8A8) runs two logic updates of 1/60 s for every frame it draws. Presenting every
// vblank alone would therefore run the game at twice its speed. With the key at 60 the port sets the interval to
// one vblank and the frame step (through tools/fps_updates.py) asks feedingfrenzy2_fps_updates how many updates the frame
// gets: one for each 1/60 s that passed since the last frame, never fewer than one or more than the game's own two.
// So a frame that took one vblank gets one update, a frame that took two gets two (as on the Xbox 360), and the
// game's speed is the same at 60, at 30 and at anything between; below 30 it slows down as the Xbox 360 game does.
// 2026-10-20 (the user): 60 by default in every build (the deko3d renderer leaves the room); 30 was the default before.
REXCVAR_DEFINE_INT32(feedingfrenzy2_fps, 0, "FeedingFrenzy2",
                     "Frames a second the game presents: 60 (every vblank, one logic update per 1/60 s that passed: "
                     "the default) or 30 (the Xbox 360 game: a frame every second vblank, two logic updates per "
                     "frame). The game speed is the same either way");

namespace {

// bejeweled3-nx (2026-10-21): the device word that holds the presentation interval: +13984 in XDK 20764
// (out/renut_port/device_offsets.md: SynchronizeToPresentationInterval, sub_822F59F0, reads it there; the layout is
// Peggle 2's 21256 with two shifts, -24 from 11584 and -16 from 13600, so Peggle 2's +14000 is +13984 here). Peggle 1's
// XDK 7776 had it at +13596, as N&B's 7645.
// feedingfrenzy-nx (2026-10-26): not found yet in XDK 2571's device: 0 = not used (no 60 fps forcing).
// feedingfrenzy2-nx (2026-10-26): +13596 in Feeding Frenzy 2's XDK, as Peggle 1's: D3DDevice_SetRenderState_PresentInterval
// sub_82172F08 is `stw r4,13596(r3); blr` (Peggle 1's sub_824358F0, the same two instructions). Logged at Present; the
// 60 fps forcing stays off (kForce60) until the game's frame step is known to run by elapsed time.
constexpr uint32_t kDeviceIntervalOffset = 13596;
constexpr bool kForce60 = false;

bool Fps60() {
  static const bool on = REXCVAR_GET(feedingfrenzy2_fps) >= 60 && kDeviceIntervalOffset != 0 && kForce60;
  return on;
}

}  // namespace

// Called by the game's frame step before its second logic update (tools/fps_updates.py): 2 = run it, 1 = skip it.
// Game thread only.
extern "C" int feedingfrenzy2_fps_updates(void) {
  if (!Fps60()) return 2;
  using Clock = std::chrono::steady_clock;
  constexpr double kUpdate = 1.0 / 60.0;
  static Clock::time_point last = Clock::now();
  static double owed = 0.0;  // game time not yet run, in seconds
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("[fps] feedingfrenzy2_fps = 60: a frame every vblank, one logic update per 1/60 s that passed");
  }
  const Clock::time_point now = Clock::now();
  owed += std::chrono::duration<double>(now - last).count();
  last = now;
  // The frame has run its first update already. A second one when one and a half updates or more are owed (the
  // frame took two vblanks); what stays owed is kept small either way, so a long pause is not caught up later.
  const int updates = owed >= 1.5 * kUpdate ? 2 : 1;
  owed -= updates * kUpdate;
  if (owed > kUpdate) owed = kUpdate;
  if (owed < -kUpdate) owed = -kUpdate;
  // Every 10 s: the frames and the updates of the window (60 updates a second = the game's speed is held).
  static Clock::time_point window = now;
  static uint32_t frames = 0, ran = 0;
  ++frames;
  ran += uint32_t(updates);
  if (const double seconds = std::chrono::duration<double>(now - window).count(); seconds >= 10.0) {
    REXLOG_INFO("[fps] last {:.1f} s: {:.1f} frames and {:.1f} logic updates a second ({} frames with two updates)",
                seconds, frames / seconds, ran / seconds, ran - frames);
    window = now;
    frames = ran = 0;
  }
  return updates;
}

REXCVAR_DEFINE_INT32(feedingfrenzy2_diag_swap_stack, 0, "FeedingFrenzy2",
                     "PC diagnostic: log the host call stack (the game's call chain) of this swap; 0 = off");

#if FEEDINGFRENZY2_RENUT_RENDERER
REXCVAR_DEFINE_BOOL(feedingfrenzy2_renut_swap_pacing, true, "FeedingFrenzy2",
                    "Renut's renderer: the Swap waits for the presentation interval the game set (60 Hz vblanks), as "
                    "the console's does; false = frames as fast as they come (timing tests)");

namespace {

// Renut's renderer (feedingfrenzy2_renderer = "renut") presents without the console's vertical blank, and its Swap does not
// wait for one: the game would run as fast as the host allows (600-1,200 frames a second on the PC with a minimised
// window). The console's Swap waits for the flip, every 'interval' vblanks of 60 Hz (the device word at +14000: the
// game sets 2, feedingfrenzy2_fps = 60 makes it 1). So this waits for the previous release plus the interval; a frame that is
// already late goes at once (no catching up), as a late flip does.
void PaceRenutSwap(uint32_t interval) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point previous{};
  if (!REXCVAR_GET(feedingfrenzy2_renut_swap_pacing) || interval == 0) return;
  const auto frame = std::chrono::nanoseconds(16'666'667LL * std::min<uint32_t>(interval, 4));
  const auto now = Clock::now();
  // 2026-10-20 (banjo-nx's banjo_display_sync, as its frame limiter does it): with the display's refresh known (the
  // Switch, the display's vsync event), the release is a refresh plus the measured phase; a late frame goes at once.
  // Otherwise (the key off, the PC, the refresh not known yet) the timer below.
  banjo::display_sync::Start();
  {
    static uint64_t previous_release = 0;
    const auto to_ns = [](Clock::time_point t) {
      return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
    };
    bool anchored = false;
    const uint64_t release = banjo::display_sync::NextRelease(to_ns(now), previous_release, uint64_t(frame.count()),
                                                              &anchored);
    if (release) {
      previous_release = release;
      const Clock::time_point at{std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(release))};
      for (;;) {
        const auto t = Clock::now();
        if (t >= at) break;
        if (at - t > std::chrono::microseconds(400))
          std::this_thread::sleep_for(at - t - std::chrono::microseconds(400));
        else
          std::this_thread::yield();
      }
      banjo::display_sync::NoteRelease(release, anchored);
      previous = std::max(at, now);
      return;
    }
    previous_release = 0;
  }
  const auto due = previous + frame;
  if (previous == Clock::time_point{} || due <= now) {
    previous = now;
    return;
  }
  // The last part is yielded away instead of slept: Windows' sleeps overshoot by up to ~1 ms; Horizon's
  // svcSleepThread is precise, so the console spins only the last 0.4 ms (banjo-nx's frame limiter's tail).
#if REX_PLATFORM_SWITCH
  constexpr auto kTail = std::chrono::microseconds(400);
#else
  constexpr auto kTail = std::chrono::microseconds(1500);
#endif
  for (;;) {
    const auto t = Clock::now();
    if (t >= due) break;
    const auto left = due - t;
    if (left > kTail)
      std::this_thread::sleep_for(left - kTail);
    else
      std::this_thread::yield();
  }
  previous = due;
}

}  // namespace
#endif

// feedingfrenzy-nx (2026-10-26): the frame is Feeding Frenzy's D3DDevice_Present (sub_820B17B0, once a frame), which
// calls D3DDevice_Swap (sub_820B1260, the only caller of VdSwap). Renut's renderer replaces Present (its Swap is
// native, so the guest's never runs): this hook shares the address with it (BANJO_RENUT_HOOK_SHARED, draw.cpp).
// feedingfrenzy2-nx (2026-10-26): Feeding Frenzy 2's D3DDevice_Present is sub_8217EAD0 (Peggle 1's sub_824436D8: 76
// instructions, it copies the 28-byte block at +12648, resolves through sub_821799E0, kicks off through sub_82176B90 and
// ends with D3DDevice_Swap sub_8217E538 (the only VdSwap caller) with r4 = +14828, r5 = device + 13640).
extern "C" REX_FUNC(__imp__sub_8217EAD0);
REX_HOOK_RAW(sub_8217EAD0) {  // D3DDevice_Present
  // Which presentation interval the game asks for (1 = every vblank, 2 = every second one), when it changes.
  if (kDeviceIntervalOffset) {
    static uint32_t last_interval = ~0u;
    uint32_t interval_be;
    std::memcpy(&interval_be, base + ctx.r3.u32 + kDeviceIntervalOffset, 4);
    const uint32_t interval = __builtin_bswap32(interval_be);
    if (interval != last_interval) {
      last_interval = interval;
      REXLOG_INFO("[fps] the game's presentation interval is {} (frame {})", interval,
                  g_feedingfrenzy2_game_frames.load(std::memory_order_relaxed));
    }
  }
  if (const int32_t at = REXCVAR_GET(feedingfrenzy2_diag_swap_stack);
      at > 0 && g_feedingfrenzy2_game_frames.load(std::memory_order_relaxed) == static_cast<uint64_t>(at)) {
    feedingfrenzy2::pc_profiler::LogHostStack("D3DDevice_Swap");
  }
  // The device's presentation interval (the word at +14000: 2 = every second vblank; the swap reads it when it
  // queues the flip, in the function at 0x82 61F990).
  if (Fps60()) {
    uint8_t* const interval = base + ctx.r3.u32 + kDeviceIntervalOffset;
    const uint8_t two[4] = {0, 0, 0, 2}, one[4] = {0, 0, 0, 1};
    if (!std::memcmp(interval, two, 4)) std::memcpy(interval, one, 4);
  }
#if FEEDINGFRENZY2_RENUT_RENDERER
  if (::banjo::renut_gpu::Active()) {
    uint8_t interval_bytes[4];
    std::memcpy(interval_bytes, base + ctx.r3.u32 + kDeviceIntervalOffset, 4);
    if (!kDeviceIntervalOffset) interval_bytes[3] = 1;
    renut_native_sub_8217EAD0(ctx, base);  // Renut's Present (resolves and presents)
    // Renut's Swap does not reach the kernel's VdSwap, which advances the SDK's frame clock (clock_per_swap_hz, the
    // deterministic runs that compare Renut with the emulated GPU frame for frame; a no-op otherwise).
    rex::chrono::Clock::NotifyGuestSwap();
    const uint64_t frames = g_feedingfrenzy2_game_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    PaceRenutSwap(interval_bytes[3]);
    feedingfrenzy2::render::OnPresented(frames);  // PC: the docked / handheld test toggles (the native ring calls it there)
    return;
  }
#endif
  __imp__sub_8217EAD0(ctx, base);
  g_feedingfrenzy2_game_frames.fetch_add(1, std::memory_order_relaxed);
#if FEEDINGFRENZY2_NATIVE_RENDERER
  // The native ring thread waits in a WAIT_REG_MEM for the game's frame pacing word most of each frame
  // (docs/feedingfrenzy2-notes.md): check it again now rather than at the end of masseffect_native_wait_reg_mem_us.
  me::native::SignalGuestProgress();
#endif
}
