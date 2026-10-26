// Bejeweled 3: the ReXGlue application object (paths, GPU plugin choice, analysis aids).

#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#if FEEDINGFRENZY2_NATIVE_RENDERER
#include "me/me_native_system.h"
#include "me/feedingfrenzy2_render_mode.h"
#endif
#if FEEDINGFRENZY2_RENUT_RENDERER
#include <rex/ui/immediate_drawer.h>
#include "renut_engine/gpu/banjo_renut_capture.h"
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#endif
#include "feedingfrenzy2_capture.h"
#include "feedingfrenzy2_dlc.h"
#include "feedingfrenzy2_enhancements.h"
#include "feedingfrenzy2_input_script.h"
#include "feedingfrenzy2_language.h"
#include "feedingfrenzy2_pc_profiler.h"
#include "feedingfrenzy2_pgo.h"

#if defined(FEEDINGFRENZY2_BUILTIN_CONFIG)
#include "feedingfrenzy2_builtin_config.h"  // generated from app/feedingfrenzy2_switch_defaults.toml and feedingfrenzy2_switch.toml (CMake)
#endif

// The game's language (the user's request, 2026-10-13): see OnPostInitLogging.
REXCVAR_DEFINE_STRING(feedingfrenzy2_language, "", "FeedingFrenzy2",
                      "The game's language: en, ja, de, fr, es, it or pt; empty = the system's language (English when "
                      "the game does not have it)");
REXCVAR_DECLARE(uint32_t, user_language);  // the SDK's (xam_user.cpp)

// Where the native renderer writes the shaders its library does not have (raw microcode + rewritten containers), so
// they can be added to the shader package without rebuilding the NRO. The renderer reads the folders from
// MASSEFFECT_SHADER_DISCOVERY / MASSEFFECT_SHADER_MISSING, which the Switch cannot set from outside: the app sets
// them from this cvar. Relative paths are next to the executable. On by default on the Switch.
REXCVAR_DEFINE_STRING(feedingfrenzy2_shader_misses,
#if REX_PLATFORM_SWITCH
                      "shader_misses",
#else
                      "",
#endif
                      "FeedingFrenzy2",
                      "Folder for shaders the native renderer cannot identify (empty = off; relative = next to the "
                      "executable). Send its contents to extend feedingfrenzy2_shaders.mesp");

class FeedingFrenzy2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<FeedingFrenzy2App>(new FeedingFrenzy2App(ctx, "feedingfrenzy2", PPCImageConfig));
  }

  // The Switch build's fixed settings (app/feedingfrenzy2_switch_defaults.toml, compiled in): applied before feedingfrenzy2.toml,
  // which holds only the player's options (and may still override any of these). As sa2-nx does it.
  std::string_view BuiltInConfig() const override {
#if defined(FEEDINGFRENZY2_BUILTIN_CONFIG)
    return kFeedingFrenzy2BuiltInConfig;
#else
    return {};
#endif
  }
  // Without a feedingfrenzy2.toml next to the NRO, the player's default one is written there (app/feedingfrenzy2_switch.toml).
  std::string_view DefaultConfigFile() const override {
#if defined(FEEDINGFRENZY2_BUILTIN_CONFIG)
    return kFeedingFrenzy2PlayerConfig;
#else
    return {};
#endif
  }

  // The PGO-instrumented build saves its counts when the game loses focus (HOME menu): it may be closed from there.
  void OnWindowFocusChanged(bool focused) override {
    if (!focused) feedingfrenzy2::pgo::DumpNow();
  }

  void OnPostInitLogging() override {
    // The runtime defaults to no GPU plugin: select ReXGlue's Xenos implementation, keeping an explicit
    // command-line or config-file choice.
    if (rex::cvar::GetFlagSource("gpu_plugin") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
    // feedingfrenzy2_language: the game's language by name (main.rsb holds the string tables of en, de, es, fr, it, ja and
    // pt: the seven copies of <String id="293">, "Play Classic"). It sets the SDK's user_language, which XGetLanguage
    // reports to the game (the XDK's language numbers).
    // Empty: the system's language (feedingfrenzy2_language.cpp), unless user_language was set to another than English.
    std::string language = REXCVAR_GET(feedingfrenzy2_language);
    if (language.empty() && REXCVAR_GET(user_language) == 1) {
      std::string system_name;
      language = feedingfrenzy2::language::FromSystem(&system_name);
      REXLOG_INFO("[language] feedingfrenzy2_language empty: the system language ({}) gives \"{}\"", system_name, language);
    }
    if (!language.empty()) {
      static const std::pair<const char*, const char*> kLanguages[] = {
          {"en", "1"}, {"ja", "2"}, {"de", "3"}, {"fr", "4"}, {"es", "5"}, {"it", "6"}, {"pt", "9"}};
      const char* id = nullptr;
      for (const auto& [name, value] : kLanguages) {
        if (language.size() >= 2 && std::tolower(static_cast<unsigned char>(language[0])) == name[0] &&
            std::tolower(static_cast<unsigned char>(language[1])) == name[1]) {
          id = value;
        }
      }
      if (id) {
        rex::cvar::SetFlagByName("user_language", id);
        REXLOG_INFO("[language] \"{}\": user_language = {}", language, id);
      } else {
        REXLOG_WARN("[language] feedingfrenzy2_language = \"{}\" is not one of en, ja, de, fr, es, it, pt: unchanged",
                    language);
      }
    }
#if FEEDINGFRENZY2_NATIVE_RENDERER
    if (const std::string misses = REXCVAR_GET(feedingfrenzy2_shader_misses); !misses.empty()) {
      std::filesystem::path folder(misses);
      if (folder.is_relative()) folder = rex::filesystem::GetExecutableFolder() / folder;
      const std::string raw = (folder / "raw").string(), containers = (folder / "containers").string();
      auto set = [](const char* name, const std::string& value) {
        if (std::getenv(name)) return;  // an explicit environment variable wins (PC test runs)
#if defined(_WIN32)
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
      };
      set("MASSEFFECT_SHADER_DISCOVERY", raw);
      set("MASSEFFECT_SHADER_MISSING", containers);
    }
#endif
#if REX_PLATFORM_SWITCH && FEEDINGFRENZY2_NATIVE_RENDERER
    // The emulated GPU is far too slow on the console (masseffect-nx: menus at 1.5-2.5 fps): the Switch draws with the
    // native renderer unless feedingfrenzy2.toml next to the NRO says masseffect_renderer_native = false.
    if (rex::cvar::GetFlagSource("masseffect_renderer_native") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("masseffect_renderer_native", "true");
    }
#endif
    // The group masters (feedingfrenzy2_cpu_enhancements / feedingfrenzy2_noncpu_enhancements) and the [enhancements] line, before
    // anything reads the keys.
    feedingfrenzy2::enhancements::Apply();
  }

  // Without --game_data_root, use a game_root/ folder next to the executable (on the Switch:
  // sdmc:/switch/bejeweled3-nx/game_root), and on a PC build tree the repository's assets/game_root.
  // The folder may also be present only as its asset cache, game_root.nxpack + .nxidx (sdk pack_device.h;
  // tools/build_asset_pack.py): the runtime then mounts the pack. An explicit path always wins.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (folder.empty()) {
      return;
    }
    for (auto candidate = folder; !candidate.empty(); candidate = candidate.parent_path()) {
      for (const auto& sub : {std::filesystem::path("game_root"),
                              std::filesystem::path("assets") / "game_root"}) {
        auto pack = candidate / sub;
        pack += ".nxpack";
        if (std::filesystem::is_regular_file(candidate / sub / "default.xex", ec) ||
            std::filesystem::is_regular_file(pack, ec)) {
          paths.game_data_root = candidate / sub;
          return;
        }
      }
      if (candidate == candidate.parent_path()) {
        break;
      }
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Test runs: a scripted controller (feedingfrenzy2_input_script), added only when the script is set.
    config.input_factory = feedingfrenzy2::input_script::Wrap(std::move(config.input_factory));
    feedingfrenzy2::enhancements::ApplyGpuBoost();  // feedingfrenzy2_gpu_boost (the Switch in handheld mode only)
#if FEEDINGFRENZY2_RENUT_RENDERER
    // feedingfrenzy2_renderer = "renut" (2026-10-19): Renut's Direct3D-level renderer (reNut by masterspike52, as banjo-nx
    // carries it; src/renut_engine/gpu). No SDK graphics system and no GPU plugin: the renderer is the app's (the
    // SDK's detached mode); it starts in OnFinalizePaths, when the window exists. No PM4 ring, no EDRAM model.
    if (banjo::renut_gpu::Active()) {
      renut::gpu::ConfigureRuntime(config);
      REXLOG_INFO("graphics: Renut's Direct3D-level renderer (plume; no PM4 ring, no EDRAM model)");
      return;
    }
#endif
#if FEEDINGFRENZY2_NATIVE_RENDERER
    // Stage 4: with masseffect_renderer_native = true the graphics system is the app's own native Vulkan renderer
    // (src/me), and the emulated Xenos GPU plugin is not loaded (ReXApp only loads it when config.graphics is empty).
    if (me::native::Enabled()) {
      config.graphics = me::native::CreateGraphicsSystem();
      REXLOG_INFO("graphics: native Vulkan renderer (PM4 ring)");
    }
#endif
  }

  // Video recording of the game image for testing (feedingfrenzy2_video_s = N seconds after start; 0 = off). Taken from the
  // presenter's guest output, so it works minimized and nothing can cover it.
  void OnPostSetup() override {
    // The guest main thread's core of its own (feedingfrenzy2_main_core), before the module is launched: every guest thread
    // takes its cores as it starts.
    feedingfrenzy2::enhancements::ApplyCoreLayout();
    // DLC packages from dlc/ next to the executable go into the content store before the game starts looking (sa2-nx's
    // installer; Bejeweled 3 has no DLC, so nothing happens without the folder).
    feedingfrenzy2::dlc::InstallPackages(runtime());
#if FEEDINGFRENZY2_RENUT_RENDERER
    // Renut's renderer has no SDK presenter: its Present reads the frame back (banjo_renut_capture.h).
    if (banjo::renut_gpu::Active()) {
      feedingfrenzy2::capture::StartGrabber([](rex::ui::RawImage& image) { return renut::gpu::CaptureFrame(image); });
    } else
#endif
    feedingfrenzy2::capture::Start([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    });
    // PC sampling profiler (feedingfrenzy2_pc_profile_s = N: a report every N s in logs/pc_profile.log; off by default).
    feedingfrenzy2::pc_profiler::Start();
    // The native replacements' 10-second "[hot]" line (calls per native, checks, differences).
    feedingfrenzy2::enhancements::StartReport();
    // PC: F9 switches between the simulated docked and handheld modes (render resolution).
#if FEEDINGFRENZY2_NATIVE_RENDERER
    feedingfrenzy2::render::RegisterKeys();
#endif
    // The PGO-instrumented build (FEEDINGFRENZY2_PGO=generate) saves its counts to pgo/ every 2 minutes (nothing otherwise).
    feedingfrenzy2::pgo::Start();
  }

#if FEEDINGFRENZY2_RENUT_RENDERER
  // Renut's renderer draws the SDK's ImGui through its own plume drawer, and resizes its swapchain itself.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    if (banjo::renut_gpu::Active()) return renut::gpu::CreateOverlayDrawer();
    return rex::ReXApp::OnCreateImmediateDrawer();
  }
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    rex::ReXApp::OnWindowPixelSizeChanged(pixel_width, pixel_height);
    if (banjo::renut_gpu::Active()) renut::gpu::OnWindowResized();
  }
  // The window exists here: Renut's renderer comes up before the game starts.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    if (banjo::renut_gpu::Active()) renut::gpu::StartRenderer(app_context(), [this] { return imgui_drawer(); });
    return rex::ReXApp::OnFinalizePaths(defaults, std::move(resume));
  }
#endif

  void OnShutdown() override {
#if FEEDINGFRENZY2_RENUT_RENDERER
    if (banjo::renut_gpu::Active()) renut::gpu::ShutdownRenderer();
#endif
    feedingfrenzy2::enhancements::StopReport();
    feedingfrenzy2::pc_profiler::Stop();
    feedingfrenzy2::capture::Stop();
    feedingfrenzy2::pgo::Stop();
  }

  // Analysis aid: with FEEDINGFRENZY2_DUMP_IMAGE=<file>, write the loaded guest image (image_base..image_base+image_size) so
  // code pointers stored in data can be scanned for (tools/pointer_scan.py). Does nothing unless the variable is set.
  void OnPostLoadXexImage() override {
    if (!runtime() || !runtime()->memory()) {
      return;
    }
    const char* path = std::getenv("FEEDINGFRENZY2_DUMP_IMAGE");
    if (!path) {
      return;
    }
    const uint8_t* image =
        runtime()->memory()->TranslateVirtual<const uint8_t*>(PPCImageConfig.image_base);
    if (std::FILE* f = std::fopen(path, "wb")) {
      std::fwrite(image, 1, PPCImageConfig.image_size, f);
      std::fclose(f);
      REXLOG_INFO("guest image written to {}", path);
    }
  }
};
