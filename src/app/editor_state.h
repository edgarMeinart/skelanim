#pragma once

#include <imgui.h>
#include <ska/ska.h>

#include <string>

// Name of the shortcut modifier in UI text (ImGui maps Cmd to Ctrl on macOS).
#ifdef __APPLE__
#define SKELANIM_MOD "Cmd"
#else
#define SKELANIM_MOD "Ctrl"
#endif

enum class Mode { Setup, Animate };
enum class DragKind { None, Rotate, Translate, Stretch, IK, Create, Pan };

// Widgets the tutorial points at; their screen rects are recorded each frame by App::markUi.
enum class UiTarget { ImportVideo, ModeSetup, ModeAnimate, Track, Play, BoneName, FileMenu, Count };
struct UiRect {
    ImVec2 min, max;
    bool valid = false;
};

struct EditorState {
    Mode mode = Mode::Setup;
    int clip = 0;
    int frame = 0;
    int selectedBone = -1;
    bool createTool = false;
    bool playing = false;
    double playAccum = 0;

    float zoom = 1;
    ImVec2 pan{0, 0};
    bool onionSkin = true;
    bool showNames = true;
    float videoAlpha = 1.0f;

    DragKind drag = DragKind::None;
    ska::Vec2 dragStartWorld, dragPrevWorld;
    bool dragChanged = false;
    int dragKey = -1;           // Animate: key index created/used by the current drag, -1 until first move
    bool spacePanned = false;   // Space+drag panned; suppresses play toggle on Space release

    std::string docPath;
    bool dirty = false;
};
