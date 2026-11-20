#include "app/tutorial.h"

#include "app/app.h"
#include "app/settings.h"
#include "core/figure.h"

#include <imgui.h>
#include <imgui_internal.h>  // FindWindowByName for window targets

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <span>
#include <system_error>

#define MOD SKELANIM_MOD

namespace {

ska::Vec2 toCanvas(const App& app, float x, float y) {
    const ImVec2 c = app.canvasSize();
    return skel::figureToCanvas({c.x, c.y}, x, y);
}

// Local rotation offsets (radians, added to the rest pose) for the two example keys.
struct PoseDelta {
    const char* bone;
    float rotation;
};
constexpr PoseDelta kPoseA[] = {{"upper_arm_l", 0.5f}, {"forearm_l", 0.6f}, {"upper_arm_r", 0.15f},
                                {"forearm_r", -0.3f},  {"thigh_l", -0.3f},  {"shin_l", 0.6f},
                                {"thigh_r", 0.1f},     {"spine", 0.05f},    {"head", -0.05f}};
constexpr PoseDelta kPoseB[] = {{"upper_arm_r", -0.5f}, {"forearm_r", -0.6f}, {"upper_arm_l", -0.15f},
                                {"forearm_l", 0.3f},    {"thigh_r", 0.3f},    {"shin_r", -0.6f},
                                {"thigh_l", -0.1f},     {"spine", -0.05f},    {"head", 0.05f}};

bool isDefaultName(const std::string& n) {
    if (n.rfind("bone_", 0) != 0 || n.size() == 5) return false;
    return std::all_of(n.begin() + 5, n.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int maxDepth(const ska::Document& doc) {
    std::vector<int> depth(doc.bones.size(), 0);
    int best = -1;
    for (size_t i = 0; i < doc.bones.size(); ++i) {
        const int p = doc.bones[i].parent;
        depth[i] = p < 0 ? 0 : depth[size_t(p)] + 1;
        best = std::max(best, depth[i]);
    }
    return best;
}

// Number of bones having at least `minChildren` children, and the largest child count.
int branchingBones(const ska::Document& doc, int minChildren) {
    std::vector<int> children(doc.bones.size(), 0);
    for (const ska::Bone& b : doc.bones)
        if (b.parent >= 0) ++children[size_t(b.parent)];
    return int(std::count_if(children.begin(), children.end(), [&](int c) { return c >= minChildren; }));
}

size_t totalKeys(const ska::Document& doc) {
    size_t n = 0;
    for (const ska::Clip& c : doc.clips) n += c.keys.size();
    return n;
}

bool samePath(const std::string& a, const std::string& b) {
    std::error_code ec;
    return a == b || std::filesystem::equivalent(a, b, ec);
}

}  // namespace

// Step callbacks; a friend of App and Tutorial so they can use the editor's own operations.
struct TutorialSteps {
    enum class Target { None, Viewport, Hierarchy, Timeline, ImportVideo, ModeSetup, ModeAnimate, Track, Play, BoneName, FileMenu };
    // State Tutorial::update tracks during the step. ReSaves: the restore step keeps following
    // re-saves of the same document until it is replaced by New/Open.
    enum class Watch { None, Playback, SavedFile, ReSaves };

    using Done = bool (*)(const App&, const Tutorial&);
    using Action = void (*)(App&, Tutorial&);
    using Blocked = const char* (*)(const App&, const Tutorial&);  // non-null: reason "Do it" is unavailable

    // Replaces the bones with the example skeleton up to `stage` (inclusive) as one undo step.
    static void buildExample(App& app, int stage) {
        app.pushUndo();
        ska::Document& doc = app.doc;
        while (!doc.bones.empty()) skel::deleteBone(doc, int(doc.bones.size()) - 1);
        for (ska::Clip& c : doc.clips) c.keys.clear();
        const ImVec2 canvas = app.canvasSize();
        skel::addExampleFigure(doc, {canvas.x, canvas.y}, stage);
        app.setMode(Mode::Setup);
        app.st.selectedBone = int(doc.bones.size()) - 1;
        app.setFrame(0);
    }

    // Keys `frame` with the rest pose plus `deltas`; bones not in the table (a user-drawn
    // skeleton with other names) get a small alternating swing so the key still shows motion.
    static void poseKey(App& app, int frame, const PoseDelta* deltas, size_t n, float fallback) {
        ska::Document& doc = app.doc;
        std::vector<ska::Pose> pose;
        ska::restPose(doc, pose);
        for (size_t i = 0; i < pose.size(); ++i) {
            float d = (i % 2 ? -fallback : fallback);
            bool named = false;
            for (size_t k = 0; k < n; ++k)
                if (doc.bones[i].name == deltas[k].bone) {
                    d = deltas[k].rotation;
                    named = true;
                }
            if (!named && skel::findBone(doc, "pelvis") >= 0) d = 0;  // example skeleton: unlisted bones stay
            pose[i].rotation = ska::wrapPi(pose[i].rotation + d);
        }
        const int k = skel::ensureKey(doc, app.clip(), frame);
        app.clip().keys[size_t(k)].poses = pose;
    }

    static const char* needBones(const App& app, const Tutorial&) {
        return app.doc.bones.empty() ? "Build the skeleton first (earlier steps)." : nullptr;
    }

    static bool always(const App&, const Tutorial&) { return true; }

    // --- Steps ---------------------------------------------------------------
    struct Step {
        const char* title;
        const char* body;
        Target target;
        int guideStage;  // example stages to show as a tracing guide, -1 = none
        Done done;       // nullptr: informational step
        Action doIt;     // nullptr: no "Do it for me"
        Blocked blocked;
        Watch watch = Watch::None;
    };

    // Defined as a member so the step lambdas share the friend access.
    static std::span<const Step> steps();

    static bool restored(const App& app, const Tutorial& t) {
        const Tutorial::Saved& s = t.saved_;
        return !s.path.empty() && app.docGeneration() > s.generation && samePath(app.st.docPath, s.path) &&
               app.doc.bones.size() == s.bones && totalKeys(app.doc) == s.keys;
    }
};

using S = TutorialSteps;
using Target = TutorialSteps::Target;
using Watch = TutorialSteps::Watch;

std::span<const TutorialSteps::Step> TutorialSteps::steps() {
    static const Step kSteps[] = {
    {"Welcome",
     "This tutorial builds a complete 12-bone figure, animates it with two keys, saves it to a .ska "
     "file and restores it from that file.\n\n"
     "Each step points at the tool to use (yellow frame). Do the step yourself - it is ticked off "
     "automatically - or press \"Do it for me\". Back/Next move freely between steps; Help > Tutorial "
     "reopens this window at any time.\n\n"
     "\"Do it for me\" here starts a new, empty document (you are asked to save unsaved work).",
     Target::None, -1, [](const App& a, const Tutorial&) { return a.doc.bones.empty(); },
     [](App& a, Tutorial&) { a.requestAction(App::Pending::New); }, nullptr},

    {"The viewport",
     "The viewport shows the current video frame with the skeleton on top.\n\n"
     "Mouse wheel: zoom around the cursor\nMiddle-drag or Space+drag: pan\nF: fit the frame\n\n"
     "Without a video the bones are drawn on a 1280x720 canvas.",
     Target::Viewport, -1, nullptr, nullptr, nullptr},

    {"Hierarchy and Properties",
     "Hierarchy (right, top) shows the bone tree. Click a bone to select it; drag a bone onto another "
     "one (or onto \"(root)\") to reparent it - its position on screen is kept.\n\n"
     "Properties (right, bottom) edits the document and the selected bone with exact numbers.",
     Target::Hierarchy, -1, nullptr, nullptr, nullptr},

    {"The timeline",
     "Top row: clips (one skeleton can have many clips, e.g. run, walk, idle), frame count and the "
     "reference video.\nMiddle row: transport buttons, keys and the Setup/Animate mode.\n"
     "Bottom row: the track. Yellow diamonds are keys, the red line is the current frame; click or "
     "drag on it to scrub.",
     Target::Timeline, -1, nullptr, nullptr, nullptr},

    {"Reference video (optional)",
     "Import a video of the motion you want to capture: File > Import Video (" MOD "+I). "
     "Rotated phone videos are displayed upright, and the clip takes the video's frame rate and length.\n\n"
     "No video at hand? Press Next - everything else works on the empty canvas.",
     Target::ImportVideo, -1, [](const App& a, const Tutorial&) { return a.video.isOpen(); },
     [](App& a, Tutorial&) { a.importVideo(); }, nullptr},

    {"Setup mode and the Create tool",
     "Bones are created in Setup mode, which edits the rest pose (Tab toggles Setup/Animate).\n\n"
     "Press B to switch on the Create tool. The label in the viewport's top-left corner shows the "
     "mode and tool.",
     Target::ModeSetup, -1,
     [](const App& a, const Tutorial&) { return a.st.mode == Mode::Setup && a.st.createTool; },
     [](App& a, Tutorial&) {
         a.setMode(Mode::Setup);
         a.st.createTool = true;
     },
     nullptr},

    {"Draw the spine",
     "Trace the dashed guide from the hips upward with four drags: pelvis, spine, neck, head.\n\n"
     "Each drag creates one bone, a child of the selected bone. The new bone becomes selected, so "
     "consecutive drags form a chain; starting a drag on the selected bone's tail snaps to it.",
     Target::Viewport, 0,
     [](const App& a, const Tutorial&) { return a.doc.bones.size() >= 4 && maxDepth(a.doc) >= 3; },
     [](App& a, Tutorial&) { S::buildExample(a, 0); }, nullptr},

    {"Branch the arms",
     "Arms hang off the spine. Click the spine bone to select it (or select it in the Hierarchy), "
     "then drag the upper arm and the forearm. Select the spine again for the other arm.\n\n"
     "Also useful: Esc deselects (the next bone becomes a new root), right-click a bone > "
     "\"Add child bone\" or \"Delete bone\", and " MOD "+Z undoes.",
     Target::Viewport, 1,
     [](const App& a, const Tutorial&) { return a.doc.bones.size() >= 8 && branchingBones(a.doc, 3) >= 1; },
     [](App& a, Tutorial&) { S::buildExample(a, 1); }, nullptr},

    {"Add the legs",
     "Select the pelvis and draw thigh and shin for each leg. With 12 bones the figure is complete: "
     "the spine chain branches into both arms and the pelvis into both legs.",
     Target::Viewport, 2,
     [](const App& a, const Tutorial&) { return a.doc.bones.size() >= 12 && branchingBones(a.doc, 2) >= 2; },
     [](App& a, Tutorial&) { S::buildExample(a, 2); }, nullptr},

    {"Adjust and name bones",
     "Mouse controls on a bone: drag its head to move it, drag its body to rotate it, " MOD
     "+drag the tail handle of the selected bone to change its length.\n\n"
     "Name the bones in Properties > Name: games look bones up by name (e.g. to attach a sprite to "
     "\"forearm_l\").\n\n"
     "\"Do it for me\" names a 12-bone figure in drawing order (pelvis, spine, neck, head, arms, legs).",
     Target::BoneName, -1,
     [](const App& a, const Tutorial&) {
         return std::any_of(a.doc.bones.begin(), a.doc.bones.end(),
                            [](const ska::Bone& b) { return !isDefaultName(b.name); });
     },
     [](App& a, Tutorial&) {
         a.pushUndo();
         const auto figure = skel::exampleFigure();
         for (size_t i = 0; i < figure.size(); ++i) a.doc.bones[i].name = figure[i].name;
     },
     [](const App& a, const Tutorial&) -> const char* {
         return a.doc.bones.size() == skel::exampleFigure().size() ? nullptr : "Needs the 12-bone figure.";
     }},

    {"Animate mode",
     "Switch to Animate mode (Tab or the radio button). Setup edits the skeleton's rest pose; "
     "Animate edits poses on keyframes of the current clip.",
     Target::ModeAnimate, -1, [](const App& a, const Tutorial&) { return a.st.mode == Mode::Animate; },
     [](App& a, Tutorial&) { a.setMode(Mode::Animate); }, nullptr},

    {"Pose the first key",
     "On frame 0, pose the figure to match the video:\n"
     "- drag a bone's body to rotate it\n"
     "- Alt+drag the tail handle of a selected bone for 2-bone IK: drag a forearm's tail and the "
     "elbow bends to follow\n"
     "- " MOD "+drag the tail to stretch\n\n"
     "Editing a pose creates a key automatically. K sets a key, Shift+K deletes it.",
     Target::Viewport, -1, [](const App& a, const Tutorial&) { return !a.clip().keys.empty(); },
     [](App& a, Tutorial&) {
         a.pushUndo();
         a.setMode(Mode::Animate);
         S::poseKey(a, 0, kPoseA, std::size(kPoseA), 0.25f);
         a.setFrame(0);
     },
     S::needBones},

    {"Pose a second key",
     "Step forward with the Right arrow (or drag on the track) to about frame 15 and pose again.\n\n"
     "Left/Right step one frame, Shift+Left/Right jump to the previous/next key, Home/End go to the "
     "first/last frame. The onion skin shows the previous key in blue and the next one in green.\n\n"
     "\"Do it for me\" adds frame 15 with the opposite pose and a copy of frame 0 at frame 30, so "
     "the motion loops.",
     Target::Track, -1, [](const App& a, const Tutorial&) { return a.clip().keys.size() >= 2; },
     [](App& a, Tutorial&) {
         a.pushUndo();
         a.setMode(Mode::Animate);
         ska::Clip& c = a.clip();
         if (!a.video.isOpen()) c.frameCount = std::max<uint32_t>(31, uint32_t(a.lastKeyFrame() + 1));
         const int mid = std::min(15, int(c.frameCount - 1) / 2);
         if (skel::findKey(c, 0) < 0) S::poseKey(a, 0, kPoseA, std::size(kPoseA), 0.25f);
         S::poseKey(a, mid, kPoseB, std::size(kPoseB), -0.25f);
         if (2 * mid < int(c.frameCount)) {
             const int k = skel::ensureKey(a.doc, c, 2 * mid);
             c.keys[size_t(k)].poses = c.keys[size_t(skel::findKey(c, 0))].poses;
         }
         a.setFrame(mid);
     },
     S::needBones},

    {"Play it back",
     "Press Space (or Play) to watch the poses interpolate; press it again to stop.\n\n"
     "Keys interpolate linearly by default. Select Step in the Interp box (shown on a key frame) to "
     "hold a pose until the next key.",
     Target::Play, -1, [](const App&, const Tutorial& t) { return t.sawPlaying_; },
     [](App& a, Tutorial&) { a.st.playing = true; }, S::needBones, Watch::Playback},

    {"Save to a file",
     "Save with File > Save (" MOD "+S). The .ska file holds the skeleton, every clip and its keys; "
     "the video is stored as a path relative to the file.\n\n"
     "Games read it with the header-only include/ska/ska.h (format: docs/ska-format.md).",
     Target::FileMenu, -1,
     [](const App& a, const Tutorial&) { return !a.st.dirty && !a.st.docPath.empty(); },
     [](App& a, Tutorial&) { a.save(); }, S::needBones, Watch::SavedFile},

    {"Restore from the file",
     "Prove the round trip: File > New (" MOD "+N) clears the editor, then File > Open (" MOD
     "+O) and pick the file you just saved. Skeleton, keys and video come back.",
     Target::FileMenu, -1, S::restored,
     [](App& a, Tutorial& t) {
         a.newDocument();
         a.openDocument(t.saved_.path);
     },
     [](const App& a, const Tutorial& t) -> const char* {
         if (t.saved_.path.empty()) return "Save the document first (previous step).";
         if (a.st.dirty) return "Save your latest changes first.";
         return nullptr;
     },
     Watch::ReSaves},

    {"Done",
     "You built a skeleton, animated it, saved it and restored it.\n\n"
     "Quick reference:\n"
     "Tab  Setup / Animate          B  create tool\n"
     "Space  play / pause           K / Shift+K  set / delete key\n"
     "Left/Right  frame             Shift+Left/Right  prev/next key\n"
     "Alt+drag tail  IK             " MOD "+drag tail  stretch\n"
     "Del  delete bone (Setup)      F  fit view\n"
     MOD "+Z / " MOD "+Shift+Z  undo / redo",
     Target::None, -1, nullptr, nullptr, nullptr},
    };
    return kSteps;
}

// --- Tutorial -------------------------------------------------------------------

void Tutorial::start() {
    open_ = true;
    step_ = 0;
    sawPlaying_ = false;
    saved_ = {};
}

int Tutorial::stepCount() const { return int(S::steps().size()); }

bool Tutorial::stepDone(const App& app) const {
    const S::Step& s = S::steps()[step_];
    return s.done && s.done(app, *this);
}

void Tutorial::update(App& app) {
    if (!open_) return;
    const S::Step& s = S::steps()[step_];
    if (s.watch == Watch::Playback && app.st.playing) sawPlaying_ = true;
    const bool saved = !app.st.dirty && !app.st.docPath.empty();
    const bool sameDoc = saved_.path.empty() || app.docGeneration() == saved_.generation;
    if (saved && (s.watch == Watch::SavedFile || (s.watch == Watch::ReSaves && sameDoc)))
        saved_ = {app.st.docPath, app.doc.bones.size(), totalKeys(app.doc), app.docGeneration()};
}

void Tutorial::guide(const App& app, std::vector<std::pair<ska::Vec2, ska::Vec2>>& out) const {
    out.clear();
    if (!open_) return;
    const int stage = S::steps()[step_].guideStage;
    for (const skel::FigureBone& e : skel::exampleFigure())
        if (e.stage <= stage) out.push_back({toCanvas(app, e.hx, e.hy), toCanvas(app, e.tx, e.ty)});
}

namespace {

bool targetRect(const App& app, Target t, ImVec2& mn, ImVec2& mx) {
    auto window = [&](const char* name) {
        const ImGuiWindow* w = ImGui::FindWindowByName(name);
        if (!w || !w->Active) return false;
        mn = w->Pos;
        mx = ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
        return true;
    };
    auto widget = [&](UiTarget u) {
        const UiRect& r = app.uiRect(u);
        if (!r.valid) return false;
        mn = r.min;
        mx = r.max;
        return true;
    };
    switch (t) {
        case Target::None: return false;
        case Target::Viewport: return window("Viewport");
        case Target::Hierarchy: return window("Hierarchy");
        case Target::Timeline: return window("Timeline");
        case Target::ImportVideo: return widget(UiTarget::ImportVideo);
        case Target::ModeSetup: return widget(UiTarget::ModeSetup);
        case Target::ModeAnimate: return widget(UiTarget::ModeAnimate);
        case Target::Track: return widget(UiTarget::Track);
        case Target::Play: return widget(UiTarget::Play);
        case Target::BoneName: return widget(UiTarget::BoneName) || window("Properties");
        case Target::FileMenu: return widget(UiTarget::FileMenu);
    }
    return false;
}

ImVec2 clampTo(ImVec2 p, ImVec2 mn, ImVec2 mx) {
    return ImVec2(std::clamp(p.x, mn.x, mx.x), std::clamp(p.y, mn.y, mx.y));
}

// Pulsing frame around the target plus an arrow from the tutorial window to it.
void drawPointer(ImVec2 tmn, ImVec2 tmx, ImVec2 wmn, ImVec2 wmx) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 5.0f);
    const ImU32 col = IM_COL32(255, 200, 0, int(140 + 115 * pulse));
    dl->AddRect(ImVec2(tmn.x - 3, tmn.y - 3), ImVec2(tmx.x + 3, tmx.y + 3), col, 4.0f, 0, 2.0f + pulse * 2.0f);

    const ImVec2 tc((tmn.x + tmx.x) * 0.5f, (tmn.y + tmx.y) * 0.5f);
    const ImVec2 from = clampTo(tc, wmn, wmx);
    const ImVec2 to = clampTo(from, ImVec2(tmn.x - 4, tmn.y - 4), ImVec2(tmx.x + 4, tmx.y + 4));
    const float dx = to.x - from.x, dy = to.y - from.y, len = std::hypot(dx, dy);
    if (len < 24) return;  // target touches or contains the tutorial window
    const ImVec2 dir(dx / len, dy / len), perp(-dir.y, dir.x);
    const ImVec2 base(to.x - dir.x * 12, to.y - dir.y * 12);
    dl->AddLine(from, base, col, 3.0f);
    dl->AddTriangleFilled(to, ImVec2(base.x + perp.x * 7, base.y + perp.y * 7),
                          ImVec2(base.x - perp.x * 7, base.y - perp.y * 7), col);
}

}  // namespace

void Tutorial::draw(App& app) {
    if (!open_) return;
    const S::Step& s = S::steps()[step_];
    const int n = stepCount();

    // Top-right corner of the viewport; movable afterwards.
    const ImGuiWindow* vp = ImGui::FindWindowByName("Viewport");
    if (vp) ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 12, vp->Pos.y + 12), ImGuiCond_Appearing, ImVec2(1, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(420, 0), ImVec2(420, FLT_MAX));
    bool keepOpen = true;
    if (!ImGui::Begin("Tutorial", &keepOpen,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        if (!keepOpen) open_ = false;
        return;
    }

    ImGui::TextDisabled("Step %d of %d", step_ + 1, n);
    ImGui::ProgressBar(float(step_ + 1) / float(n), ImVec2(-1, 4), "");
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextUnformatted(s.title);
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", s.body);
    ImGui::Spacing();

    if (s.done) {
        if (s.done(app, *this)) {
            if (s.done == S::restored)
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "Done: restored %zu bones and %zu keys from %s.",
                                   saved_.bones, saved_.keys, std::filesystem::path(saved_.path).filename().string().c_str());
            else
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "Done - press Next.");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1), "Waiting for you...");
        }
    }
    ImGui::Separator();

    ImGui::BeginDisabled(step_ == 0);
    if (ImGui::Button("Back")) --step_;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (step_ + 1 < n) {
        if (ImGui::Button("Next")) ++step_;
    } else if (ImGui::Button("Finish")) {
        open_ = false;
    }
    if (s.doIt) {
        ImGui::SameLine();
        const char* blocked = s.blocked ? s.blocked(app, *this) : nullptr;
        ImGui::BeginDisabled(blocked != nullptr);
        if (ImGui::Button("Do it for me")) s.doIt(app, *this);
        ImGui::EndDisabled();
        if (blocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", blocked);
        if (blocked) ImGui::TextDisabled("%s", blocked);
    }

    bool dontShow = !app.settings.showTutorial;
    if (ImGui::Checkbox("Don't show this tutorial on startup", &dontShow)) {
        app.settings.showTutorial = !dontShow;
        app.saveSettings();
    }

    const ImVec2 wmn = ImGui::GetWindowPos();
    const ImVec2 wmx(wmn.x + ImGui::GetWindowSize().x, wmn.y + ImGui::GetWindowSize().y);
    ImGui::End();
    if (!keepOpen) open_ = false;

    ImVec2 tmn, tmx;
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (open_ && !popup && targetRect(app, S::steps()[step_].target, tmn, tmx)) drawPointer(tmn, tmx, wmn, wmx);
}
