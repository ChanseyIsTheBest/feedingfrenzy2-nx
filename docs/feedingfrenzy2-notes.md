# Feeding Frenzy 2 port: notes

This file records the port: what each step found, with its evidence. The project is a copy of feedingfrenzy-nx
(`6e27e27`: Feeding Frenzy 1's port, itself a copy of bejeweled3-nx 1.0.1). Identifiers were renamed
feedingfrenzy -> feedingfrenzy2 with `tools/port_rename.py`. Feeding Frenzy 1's notes (`feedingfrenzy-notes.md`),
Bejeweled 3's, Peggle 1's and Peggle 2's stay as the reference for everything taken from those ports; comments that
say "feedingfrenzy-nx", "bejeweled3-nx", "peggle1-nx" or "Peggle 2" record findings from those ports.

The user's request (2026-10-26): "could you please port feeding frenzy 2 with a native renderer work uninterupted".
The renderer is Renut on deko3d, as the user chose for Feeding Frenzy 1.

## The game (2026-10-26)

The user's package is in `D:\peggle 2\feeding frenzy 2` (extracted to `game_root`): an XBLA game, title **5841088C**,
copied to `assets/game_root` (53 entries, 57 MB; git-ignored).

- **`default.xex`** (`tools/xex_info.py`):
  - XEX2, original PE name `FeedingFrenzy2.exe`, base `0x82000000`, entry `0x821633B0`, image size `0x00570000`.
  - Identity: version 0.0.1.2, media `61473769`.
  - Imports: `xam.xex` (98) and `xboxkrnl.exe` (240), version 2.0.6534.
  - Static libraries: XDK **2.0.6534** (XAPILIB, D3D9, XBOXKRNL, LIBCPMT, D3DX9, XGRAPHC, XACT, XONLINE, X3DAUD,
    XAUD): between Feeding Frenzy 1's 2571 and Peggle 1's 7776, nearest Peggle 1.
- **Sections:** `.rdata 82000400-82085D40`, `.pdata 82085E00-820A06E0`, `.text 820B0000-824ED054`,
  `.data 824F0000-82541098`, `.idata 82550000`.
- **Data:** `Resource/resources.xpr` (14 MB) and `resources2.xpr` (28 MB), XACT audio, the achievement pictures,
  `ArcadeInfo.xml`. The splash is PopCap's; the developer Sprout Games.

## Code generation (2026-10-26)

- Futari's VMX keys stay on (`app/perf_overrides.toml`), as in Feeding Frenzy 1.
- **First run:** one "target not in any function", `0x82470BA0` (a this-adjusting thunk's target reached only from
  another thunk), declared in `app/overrides.toml`. **Pointer sweep** (`tools/pointer_fixpoint.py`): 338 functions.
- **First boot** (`boot1`): "Call to invalid or unregistered function at guest address 0x8233C8C8": a qsort comparator
  built in code (`lis r11,0x8234; addi r6,r11,-14136` at `0x82355528`, passed to qsort `sub_821F67E0`), which the
  pointer sweep had rejected as a fragment (its `bne` tail reaches the registered chunk at `0x8233C93C`, which reads
  r8 / r11). `tools/runtime_gap_resolver.py --apply` declared it; the only rejection of the sweep.
- **Addresses:**

  | What | Feeding Frenzy 2 | How |
  |---|---|---|
  | setjmp | `0x82454F20` | `stfd f14,0(r3); stfd f15,8(r3)` at `0x82454F3C`; 10 call sites |
  | longjmp | `0x82454C00` | `lfd f14,0(r7)` at `0x82454C3C`; 20 call sites |
  | memcpy | `0x821F4AC0` | 1,160 bytes, FNV-1a 64 `0xEE0EAEB1E0584297`: Peggle 2's `0x825F0AE0` (and Peggle 1's) byte for byte |
  | memset | `0x821F4800` | 160 bytes, `0x54D201DC8A21DABA`: Peggle 2's `0x825F10B0` |
  | memmove | `0x821F6720` | Feeding Frenzy 1's `sub_822CEDF0` (the same 48 instructions; its tail call reaches memcpy here) |
  | _stricmp | `0x824552D0` | Feeding Frenzy 1's `sub_822CFFE0` byte for byte (88 bytes) |
  | srand / rand | `0x821F4788` / `0x821F47B8` | Feeding Frenzy 1's code |
  | `_CxxThrowException` | `sub_821F7A88` | 40 call sites; the template at `0x82031370`; raises through `sub_8220A108` |
  | `D3DDevice_Present` | `sub_8217EAD0` | ends with `D3DDevice_Swap` `sub_8217E538`, the only `VdSwap` caller |
  | entry point | `0x821633B0` | XEX header |

  The CRT natives (`feedingfrenzy2_hot_guest.cpp`, `hand.txt`) are registered with these sizes and hashes; Feeding
  Frenzy 1's strstr has no match here, and its hot lists and hand-written native were removed.
- **The frame hook** is on Present (as in Feeding Frenzy 1). The device's presentation interval is at `+13596`
  (`D3DDevice_SetRenderState_PresentInterval` `sub_82172F08` is `stw r4,13596(r3); blr`, as Peggle 1's): the game asks
  for **1** (every vblank), so no 60 fps forcing is needed (`kForce60` off).

## First boot on the PC (2026-10-26, emulated GPU)

`boot1` (60 s, A presses): the PopCap splash, the title ("PRESS A TO START", Sprout Games), the Story Mode menu, the
Sandy Shoal introduction (Boris the Butterfly Fish), play with the HUD (FRENZY!, score). 59 game frames a second; the
generated-code self-checks 0 differences.

## Renut's renderer at Feeding Frenzy 2's addresses (2026-10-26)

- **The device layout decides the base.** Renut's Direct3D layer in this tree came from Bejeweled 3 (XDK 20764:
  Peggle 2's layout with two shifts). XDK 6534's device is Peggle 1's (7776, N&B's 7645) up to the fields Renut uses:
  `D3DDevice_Swap` reads `+16560`, `+13608`, `+14844`, `+13612` exactly as Peggle 1's; aligned functions differ only by
  scheduling below about 21,500 (`out/renut_map/anchor.txt`, field pairs counted by a script). So the layer
  (`app/src/renut_engine/gpu/d3d`: `d3d.h`, `d3d_native.h`, `d3d_native_state.cpp`, `hooks/*.cpp`) was taken from
  peggle1-nx, with Peggle 1's layout constants in `backend/queued.cpp` (the private words 10880-11775, 39 chunks) and
  `render/draw.cpp` (declaration at `0x2FA0`, viewport at `0x3168`). The rest of the renderer stays Bejeweled 3's (its
  display ramp table among it).
- **The map** (`tools/renut_hook_map_ff2.txt`, applied by `tools/renut_hook_remap_ff2.py`): 299 hooks at this game's
  addresses, 53 commented out.
  - The **222 state functions** pair by pair from the two games' state tables: Feeding Frenzy 2's at `0x824F7608`
    (render, 101) and `0x824F7AC8` (sampler, 20), found by Peggle 1's defaults at stride 12 (one match each);
    every getter and setter maps one to one (`out/renut_map/state_tables.txt`).
  - The rest by **field signature** (r3-relative loads and stores with their widths, immediates, calls, imports,
    size; scheduling does not matter) against Peggle 1's functions: most score 1.00. The small lock functions were
    read instruction by instruction: `LockRect` (`sub_82168C50`) and the surface's (`sub_821690C0`) take Peggle 1's
    entry arguments, but their internal `D3D_Lock2DSurface` has one argument fewer here, so the internal lock
    functions stay unhooked.
  - **The draw functions are in a second block** (`0x8220BB30-0x8220C858`, next to the CRT's streaming copies):
    BeginVertices `sub_8220BB30`, DrawVerticesUP `sub_8220BFD0` (BeginVertices, memcpy, then `m_pRing = [+13444]`,
    Renut's `kDeviceOffsetVerticesRingEnd`), DrawVertices `sub_8220C018`, DrawIndexedVertices `sub_8220C400`.
  - XDK 6534's AcquireThreadOwnership / ReleaseThreadOwnership (`sub_8217D2E0` / `sub_8217D248`) take the device's
    critical section, unlike Peggle 1's, but write the same owner word (`+10888`): mapped to Renut's.
- **The trace** (`app/src/feedingfrenzy2_d3d_trace.cpp`, PC test aid, off by default): the 119 Direct3D functions
  game code calls (`out/renut_map/api.txt`). The device (`0x4001A480`) is created once (`CreateDevice(0, 1, 0, 0,
  params, &out)`, the output a field of the game's renderer). Left as the game's code: its run-time **shader
  compiler** (the `+19888` object's functions, `vs_3_0` / `ps_3_0` tokens: the game compiles all its shaders at start),
  the pure texture-layout helpers, `Set{Vertex,Pixel}ShaderConstantF` (`sub_8216AD68` / `sub_8216AE48`: they write the
  constants at `+0x780` / `+0x1780` and the dirty masks at +0 / +8, the device front Renut reads), `GetDeviceCaps`'
  adapter form, and the debug screenshot path (`sub_82185548`-`sub_82186290`, not called).
- **Guest data:** the build qword `0x824F7BB8` (`0x0002000019864001`, XDK 6534; stored at `+10880` as in Peggle 1), the
  `VdGlobalDevice` slot `0x8200057C`, the caps `0x82019180`, the format table `0x82018A90`, the full viewport
  `0x82018DD0`, the HiZ owner `0x825349FC`, XMemFree `sub_822088A8`, XPhysicalAlloc's large-page flag `0x8253FB98`,
  GetCurrentThreadId `sub_82208CE0`; no export functions in XDK 6534 (the export skip words 0).
- **Shaders:** the game compiles 26 at start (22 pixel, 4 vertex), version 0x11 containers (`0x102A1100` /
  `0x102A1101`). The trace run wrote them; named by XXH3 they are `out/renut_shader_dump`.
- **PC, Renut on Vulkan** (`renut1`): the splash, title, menu, introduction, play and the HUD as with the emulated GPU;
  no shader missing, no warning in the log.

## The first Switch build (2026-10-26)

- **Shaders:** XenosRecomp with `BANJO_HLSL_OVERRIDE` / `BANJO_DEKO3D_SPIRV_DIR` (docs/building.md, section 3), then
  `deko3d_shaders.py` (26 of 26 compile with uam: 4 vertex of 2 KB, 22 pixel, median 1.3 KB), `deko3d_glsl_opt.py
  --check`, `deko3d_shader_package.py` (44 KB), `renut_switch_shader_cache.py`. All 26 are the game's run-time shaders,
  so the NRO carries them all.
- **Build:** `tools/win/build_switch.ps1 -Pgo none -Define "FEEDINGFRENZY2_SWITCH_LTO=OFF"` (a development build: no
  LTO, no PGO). NRO 28.5 MB, "Feeding Frenzy 2" 0.1.0 with the game's `title_image.png` as the development icon
  (`out/icon_dev.jpg`, scaled to 256x256; game data, not committed).
- **`tools/check_switch_build.py`:** all checks passed, with:
  - KNOWN_HALTS: none (the memcpy is Peggle 2's; no `b .` in the generated code);
  - STREAMING_COPIES: `sub_8220A7C4` and `sub_8220B020` (72 vperm128 and one lvsl each, as Peggle 1's): every one of
    their 72 two-register lookups and 165 byte reversals intact;
  - the vector fused multiply-adds of `vmx_fused_madd`: 291 in 52 functions, the generated code's `vmx_madd_ps` sites.
- **Ryujinx** (`emu1`, 150 s; SD `switch\feedingfrenzy2-nx` a junction to `out/emu_sd/feedingfrenzy2-nx` in the Peggle
  data folder; `tools/build_asset_pack.py` made the game pack, 52 files, 57 MB): the PopCap splash, the loading
  screen, the main menu (Single Player, Multiplayer, Leaderboards, Achievements, Help & Options, Return to Arcade), the
  Shipwreck Strait map, Stage 1 played with the stick (score 110, eaten once: "SORRY"); 60.00 game frames a second,
  Renut's render thread 2.2-2.6 ms a frame, no shader missing, no error in the log.
- **Delivered:** `out/dist/dev_20261026_feedingfrenzy2/feedingfrenzy2-nx/` (NRO `55f581e0...`, the game pack,
  README.txt); ELF and map `out/nx/elf/feedingfrenzy2_dev_20261026_1815.*`.
