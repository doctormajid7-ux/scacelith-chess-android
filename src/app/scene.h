// Application scenes. The game itself is the "game" scene; every module can register extra
// viewer/test scenes (run with: scacelith --scene <name> [--shot out.png --frames N]).
#pragma once
#include "../platform/platform.h"
#include "../render/renderer.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AppContext {
    render::Renderer* renderer = nullptr;
    bool screenshotMode = false;   // deterministic run (fixed dt, no vsync)
    // This frame's time for chess clocks: the real time, capped at 2 s where the scenes' dt stops
    // at 0.1 s (slow frames, a window dragged), so the clocks keep up with the wall clock. The
    // fixed dt in screenshot mode.
    float clockDt = 0.0f;
    float fixedTime = -1.0f;       // --time: start time for deterministic scenes
    std::vector<std::string> args; // raw command line (scenes may parse extra options)
    // Per-frame stage timings (the Android loop logs them; the desktop keeps them at zero).
    double drawMs = 0.0;           // render + renderOverlay of this frame
    double swapMs = 0.0;           // swapBuffers of this frame (the vsync/GPU wait included)
    bool hasArg(const std::string& a) const;
    std::string argValue(const std::string& a, const std::string& def = "") const;
};

class Scene {
public:
    virtual ~Scene() = default;
    virtual bool init(AppContext& ctx) = 0;
    // Called once per frame. Return false to quit the app.
    virtual bool update(AppContext& ctx, float dt) = 0;
    // Fill the camera/environment and submit draw items (renderer.beginFrame is called by the
    // scene itself so it controls the camera).
    virtual void render(AppContext& ctx, float dt) = 0;
    // Drawn on the backbuffer after the 3D frame (UI). Optional.
    virtual void renderOverlay(AppContext&, float) {}
    virtual void shutdown(AppContext&) {}
};

using SceneFactory = std::function<std::unique_ptr<Scene>()>;
void registerScene(const char* name, const char* description, SceneFactory f);
std::unique_ptr<Scene> createScene(const std::string& name);
std::vector<std::pair<std::string, std::string>> listScenes();

#define SCACELITH_SCENE(NAME, DESC, CLASS)                                              \
    static const bool g_sceneReg_##CLASS = [] {                                           \
        registerScene(NAME, DESC, [] { return std::unique_ptr<Scene>(new CLASS()); });   \
        return true;                                                                      \
    }()
