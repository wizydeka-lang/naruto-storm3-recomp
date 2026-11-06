// naruto - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>

#include <cstdlib>

#include "input_remap.h"

class NarutoApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NarutoApp>(new NarutoApp(ctx, "naruto",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // Controllers' buttons remapped by kind (input_remap.h).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.input_factory = REX_INPUT_BACKEND(input_remap::CreateInputSystem);
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}

#ifdef __ANDROID__
  // The default beside the executable is the read-only system app_process
  // folder; keep settings with the user data the Android activity passes.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    paths.config_path = paths.user_data_root / "naruto.toml";
  }

  // Android has a single window. In native-only mode the native view draws
  // to it (native_view.h), so Xenia's presenter lets go of it.
  bool SetupPresentation() override {
    if (!rex::ReXApp::SetupPresentation()) {
      return false;
    }
    const char* only = std::getenv("NARUTO_NATIVE_VIEW_ONLY");
    if (only && *only == '1' && window()) {
      window()->SetPresenter(nullptr);
    }
    return true;
  }
#endif
};
