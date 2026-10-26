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

## Optimising the hot functions (2026-10-26, from the emulator)

The user: "do some optimisation you did for feeding frenzy 1 on 2; if you can only get 60 functions that's fine, just
hand optimise the first couple and follow the process for the others". The method is docs/optimising-functions.md, as
in Feeding Frenzy 1.

- **Profile** (Ryujinx `prof1`: 330 s, Stage 1 played with the stick the whole time, `switch_profile_report` /
  `switch_profile_stacks`; `out/renut_tests/emu/prof1/rex_profile.log`, map
  `out/nx/elf/feedingfrenzy2_dev_20261026_1815.map`; `tools/switch_hot_functions.py --top 5000 --from-report 4`, all
  guest threads): 238 generated functions sampled, 170 of them eligible (tiny leaves, one function with atomics
  `sub_821E06A0` and the addresses our sources hook left out; `sub_82125138`, a bare `blr` called through pointers, is
  the hottest tiny one). The main thread is about 4-5 ms of a 16.7 ms frame: the game waits for the frame most of it.
- **Hand-written natives** (the top of the profile):
  - `h_821E7AC8.h` (152 samples, the hottest): Feeding Frenzy 1's `sub_82256198` with other offsets (this->[268] |=
    each child's [268] over the vector at +308, the debug iterator's traps, then a tail call of the virtual method
    348 / 4): begin / end loaded once, the running OR in a register (still stored every iteration), the trap cases run
    the copy before any store;
  - `h_82106E48.h` (76 samples, with its callee `sub_821062A0` another 76): a ring buffer's back() through an iterator
    built in the frame; the leaf callee inline, every register it leaves behind reproduced, the frame's stores kept,
    the four trap cases running the copy before any store;
  - `n_820B00B8.h`, **wmemcmp** (52 samples): the host's memcmp over 2n bytes (big-endian halfwords compare as their
    bytes), the result 1 or `0x00000000FFFFFFFF` as the original's `subfc / subfe / rlwinm / addi` gives it; registered
    with its size and hash like the CRT natives (`Cmp::kReturn`), in `hand.txt`.
- **Localized copies:** the hottest 60 eligible functions (35 in `localized.txt`, 25 in `localized_bulk.txt`; the two
  hand-written ones among them as their reference). **Write-backs of the changed registers only**
  (tools/hand_native_dirty_sync.py): 1,762 register stores kept of 4,066 in the 57 copies it limits (56 % removed);
  smart sync at 41 of 236 call sites (464 write-back / reload pairs fewer). `sub_821C8C88` and `sub_821FA8C0` (jump
  tables) and `sub_821667B4` (an SEH funclet's XER use) keep all of their write-backs; 15 copies inline tiny callees.
- **Checks:**
  - PC verify mode (`verify1`, Renut, Stage 1 played, 150 s): 37,275,392 checked calls of the 11 leaf copies and the
    C runtime natives, wmemcmp's 7,119,616 among them, 0 differences. Calls in the run: `h_82106E48` 33.7 million,
    `h_821E7AC8` 6.4 million (copies with calls: audited in their headers against their copies).
  - `tools/check_switch_build.py`: all checks passed (the streaming copies' lookups, 291 vector fused multiply-adds).
- **Measured** in Ryujinx with the same script and length:
  - `prof2` (the optimised NRO) against `prof3` (the same NRO with the 61 new natives switched off by their keys,
    so the same build and code layout): the 61 functions 1,409 samples -> 931 (the rest of the guest code 638 -> 596:
    0.71 normalised); the main thread's median 5.27 -> 4.94 ms a game frame. `h_821E7AC8` 205 -> 154, back() with its
    callee 311 -> 130, wmemcmp 55 -> 11. `sub_820B0708` rose (55 -> 81) while its call chain (the inline wcslen,
    `sub_820B0488`, wmemcmp) fell 174 -> 148: the time moved between them.
  - `prof2` dipped to 53-57 fps in some windows where `prof3` held 60: with the main thread at 5 ms of 16.7, those are
    the emulator host's (the display log shows the misses), not the guest code's. Against `prof1` (the earlier NRO)
    the scenes differ too much to compare whole frames (Feeding Frenzy 1's notes: the same).
- Delivered: the optimised NRO (`4ae71e4e...`) in `out/dist/dev_20261026_feedingfrenzy2/feedingfrenzy2-nx/`, replacing
  the first one; ELF and map `out/nx/elf/feedingfrenzy2_dev_opt_20261026.*`. A development build (no LTO, no PGO).

## Up to four players (2026-10-26)

The user: "please add option to support/map controls for up to 4 players in feeding frenzy 2, just reuse code from
peggle 2".

- **Peggle 2's controller code is already this tree's.** The SDK came through Peggle 1 and Bejeweled 3 with Peggle 2's
  (sa2-nx's) Switch driver: one slot per controller (handheld, players No1-No4) routed to the four guest users
  (`SlotAssignment`), single Joy-Cons sideways (with the stick turned, Peggle 2 1.0.2's fix), the controller applet on
  L + R + ZR, a button map per player (`input_p1_*` to `input_p4_*`), and the guest sign-in of controllers 2-4
  (`xam_guest_signin`, on in the Switch defaults). Compared file by file with peggle2-nx: the input code is the same;
  the kernel has Peggle 1's two later fixes on top (XUserGetName names a connected controller as its guest would be,
  the leaderboard reads fail).
- **The game's multiplayer:** Multiplayer -> Co-op Story, Co-op 'Lite', Party Games, FrenzyFest; a Select Players
  screen with four slots ("Press A to join", "Press Y to sign in"). PC (`mp1`, `REX_FEEDINGFRENZY2_INPUT_SCRIPT_P2`):
  player 2 joins with A and plays Co-op Story next to player 1 (two fish, two score panels). `mp2`: player 2's Y signs
  controller 2 in as the profile's guest ("[XAM] controller 2 signed in as guest User(1) of the profile"; the slot shows
  "User(1)"). Players 3 and 4 and the Switch driver's per-player maps are not run (the PC uses the SDL driver; the
  scripted pads land behind Ryujinx's own controllers).
- **The option:** `app/feedingfrenzy2_switch.toml` (the player's file, written next to the NRO at the first start)
  now explains the controllers as Peggle 2's does: up to four players, the controller types, L + R + ZR, the layout
  by position, single Joy-Cons, and each player's map with examples for players 1-4. It replaces Bejeweled 3's text,
  which named the wrong game and described the layout wrongly ("A is the right button"). Ryujinx `firststart` (no
  toml on the SD): the NRO wrote the new file, identical to the source. A player's existing file is kept; the keys work
  in it when added by hand.
- Delivered: the NRO `4fc8e5e6...` (the optimised build with the new file) in
  `out/dist/dev_20261026_feedingfrenzy2/feedingfrenzy2-nx/`, README.txt updated; ELF and map
  `out/nx/elf/feedingfrenzy2_dev_opt_mp_20261026.*`.

## The toml headers (2026-10-26)

The user: "fix the header on both feeding frenzy 1 and 2 if they need to be fixed". The headers copied from
bejeweled3-nx still named Bejeweled 3: the code generator's manifest (with Bejeweled 3's title id 584111D6 and its
default.xex facts), the built-in Switch settings and the speed options. Each now names Feeding Frenzy 2, the manifest
with this game's default.xex (title 5841088C, entry 0x821633B0, image 0x00570000, XDK 6534, FeedingFrenzy2.exe 0.0.1.2).
The player's file was already fixed with the controller section. Comments only: the generated code is the same.
- Delivered: the rebuilt NRO (`54a8cded...`: the same code, the new built-in files) in `out/dist/dev_20261026_feedingfrenzy2/feedingfrenzy2-nx/`; ELF and map `out/nx/elf/feedingfrenzy2_dev_opt_hdr_20261026.*`. `tools/check_switch_build.py`: all checks passed.

## 1.0.0 (2026-10-26)

The user: "set both nro version to 1.0.0 and author is ChanseyIsTheBest. compile both with logging off and with LTO
enabled like peggle 2 or bejeweled 3 ... we don't need PGO. then upload both to github and also make web installer for
both games".

- **The NRO:** version 1.0.0, author ChanseyIsTheBest, the icon `out/icon.jpg` (the user's art, 256x256; not committed),
  logging off (the built-in settings; only a crash writes `logs/rex/rex_crash.log`), link-time optimisation on, no
  profile-guided optimisation (`build_switch.ps1 -Pgo none -Define "FEEDINGFRENZY2_SWITCH_LTO=ON"`). NRO `d3caf767...`;
  `tools/check_switch_build.py`: all checks passed.
- **Ryujinx** (`rel*`): the release NRO with the game pack the installer page made (`test/package_node.mjs` of the
  installer, the same code as the page): splash, menus and play at 60 FPS once the first start's shader compiling is
  over.
- **Published:** https://github.com/ChanseyIsTheBest/feedingfrenzy2-nx (release v1.0.0 with `feedingfrenzy2.nro`) and the installer page
  https://chanseyisthebest.github.io/feedingfrenzy2-nx-installer/ (repository feedingfrenzy2-nx-installer, made from bejeweled3-nx-installer
  without its shader step: the game compiles all of its 26 shaders at run time and the NRO carries them). The
  installer's game pack is the tested one byte for byte apart from the pack id (a per-build nonce).
