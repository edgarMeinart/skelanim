// End-to-end editor test: runs the real App in a hidden GLFW window and drives it with synthetic
// ImGui mouse/keyboard events. Widgets are located by label through ImGui's item hooks
// (imgui built with IMGUI_ENABLE_TEST_ENGINE). Usage: app_tests <test.mp4> <scratch.ska>
#include "app/app.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

// --- ImGui item hooks: label -> last frame's rect ---------------------------

static std::unordered_map<ImGuiID, ImRect> gRects;
static std::unordered_map<std::string, ImGuiID> gLabels;

void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) { gRects[id] = bb; }
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    if (label) gLabels[label] = id;
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }

// --- Driver ---------------------------------------------------------------------

static App* app;

static void frame(int n = 1) {
    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < n; ++i) {
        gRects.clear();
        gLabels.clear();
        io.DisplaySize = ImVec2(1600, 900);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        GImGui->TestEngineHookItems = true;
        app->frame();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
}

static bool hasItem(const char* label) { return gLabels.count(label) && gRects.count(gLabels[label]); }

static ImVec2 itemCenter(const char* label) {
    if (!hasItem(label)) {
        std::printf("item not found: %s\n", label);
        ++failures;
        return ImVec2(-1000, -1000);
    }
    return gRects[gLabels[label]].GetCenter();
}

// World (video pixel) -> screen, using the viewport window position and the editor's pan/zoom.
static ImVec2 S(float x, float y) {
    const ImVec2 o = ImGui::FindWindowByName("Viewport")->Pos;
    return ImVec2(o.x + app->st.pan.x + x * app->st.zoom, o.y + app->st.pan.y + y * app->st.zoom);
}
static ImVec2 S(ska::Vec2 p) { return S(p.x, p.y); }

static ImGuiKeyChord cmd() { return ImGui::GetIO().ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl; }

static void mods(int m, bool down) {
    ImGuiIO& io = ImGui::GetIO();
    for (ImGuiKey k : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt, ImGuiMod_Super})
        if (m & k) io.AddKeyEvent(k, down);
}

static void key(ImGuiKey k, int m = 0) {
    ImGuiIO& io = ImGui::GetIO();
    mods(m, true);
    frame();
    io.AddKeyEvent(k, true);
    frame();
    io.AddKeyEvent(k, false);
    frame();
    mods(m, false);
    frame();
}

static void click(ImVec2 p, int button = 0) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(p.x, p.y);
    frame();
    io.AddMouseButtonEvent(button, true);
    frame();
    io.AddMouseButtonEvent(button, false);
    frame(2);
}

static void drag(ImVec2 a, ImVec2 b, int m = 0) {
    ImGuiIO& io = ImGui::GetIO();
    mods(m, true);
    io.AddMousePosEvent(a.x, a.y);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    for (int i = 1; i <= 15; ++i) {
        io.AddMousePosEvent(a.x + (b.x - a.x) * i / 15, a.y + (b.y - a.y) * i / 15);
        frame();
    }
    io.AddMouseButtonEvent(0, false);
    frame(2);
    mods(m, false);
    frame();
}

static void typeText(const char* s) {
    for (; *s; ++s) {
        ImGui::GetIO().AddInputCharacter(unsigned(*s));
        frame();
    }
}

// --- Pose helpers -----------------------------------------------------------

static std::vector<ska::Affine> world(const std::vector<ska::Pose>& pose) {
    std::vector<ska::Affine> w;
    ska::computeWorld(app->doc, pose, w);
    return w;
}
static ska::Vec2 head(int b, const std::vector<ska::Pose>& pose) {
    const ska::Affine w = world(pose)[size_t(b)];
    return {w.tx, w.ty};
}
static ska::Vec2 tail(int b, const std::vector<ska::Pose>& pose) {
    return ska::apply(world(pose)[size_t(b)], {app->doc.bones[size_t(b)].length, 0});
}
static ska::Vec2 mid(int b, const std::vector<ska::Pose>& pose) {
    const ska::Vec2 h = head(b, pose), t = tail(b, pose);
    return {(h.x + t.x) / 2, (h.y + t.y) / 2};
}
static std::vector<ska::Pose> shown() {
    std::vector<ska::Pose> p;
    app->displayedPose(p);
    return p;
}
static float dist(ska::Vec2 a, ska::Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

// --- Test -------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: app_tests <test.mp4> <scratch.ska>\n");
        return 2;
    }
    const std::string videoPath = argv[1], skaPath = argv[2];

    if (!glfwInit()) return 77;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "app_tests", nullptr, nullptr);
    if (!window) {
        std::printf("app_tests: skipped (no OpenGL window available)\n");
        glfwTerminate();
        return 77;
    }
    glfwMakeContextCurrent(window);
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplOpenGL3_Init("#version 150");
    // Main scenario: tutorial disabled through the settings file.
    const std::string settingsPath = skaPath + ".settings.ini";
    {
        Settings off;
        off.showTutorial = false;
        CHECK(saveSettings(settingsPath, off));
    }
    app = new App(window, settingsPath);
    frame(3);
    CHECK(!app->tutorial.isOpen());

    // Import video: canvas, frame count, texture.
    app->importVideoFile(videoPath);
    frame(3);
    CHECK(app->video.isOpen());
    CHECK(app->clip().frameCount == 150);
    CHECK(app->canvasSize().x == 640 && app->canvasSize().y == 360);
    CHECK(app->tex.w == 640 && app->tex.h == 360);

    // Setup: B, three chained bones drawn by consecutive drags.
    key(ImGuiKey_B);
    CHECK(app->st.createTool);
    drag(S(320, 40), S(320, 140));
    drag(S(320, 140), S(300, 240));
    drag(S(300, 240), S(310, 340));
    CHECK(app->doc.bones.size() == 3);
    if (app->doc.bones.size() != 3) return 1;
    CHECK(app->doc.bones[1].parent == 0 && app->doc.bones[2].parent == 1);
    CHECK(dist(head(2, shown()), {300, 240}) < 1.0f);
    CHECK(dist(tail(2, shown()), {310, 340}) < 1.0f);
    CHECK(app->st.selectedBone == 2);

    // A drag under 2 video px (but >= 4 screen px when zoomed in) adds nothing and records no
    // undo step: the next undo must still remove bone_2.
    const float zoom = app->st.zoom;
    app->st.zoom = 4;
    frame();
    const ImVec2 c = ImGui::FindWindowByName("Viewport")->Rect().GetCenter();
    const std::vector<uint8_t> beforeTiny = ska::write(app->doc);
    drag(c, ImVec2(c.x + 4, c.y + 2));
    CHECK(ska::write(app->doc) == beforeTiny);
    app->st.zoom = zoom;
    key(ImGuiKey_Escape);
    CHECK(!app->st.createTool);
    key(ImGuiKey_Z, cmd());
    CHECK(app->doc.bones.size() == 2);
    key(ImGuiKey_Z, cmd() | ImGuiMod_Shift);
    CHECK(app->doc.bones.size() == 3);
    click(itemCenter("bone_2"));
    CHECK(app->st.selectedBone == 2);

    // Properties: drag the Length field; one undo step restores it.
    frame();
    const float len0 = app->doc.bones[2].length;
    const ImVec2 lenField = itemCenter("Length");
    drag(lenField, ImVec2(lenField.x + 40, lenField.y));
    CHECK(app->doc.bones[2].length > len0 + 5);
    key(ImGuiKey_Z, cmd());
    CHECK(app->doc.bones[2].length == len0);

    // Rename in Properties, then select another bone in the hierarchy while the field is active:
    // the rename must still become its own undo step and mark the document dirty.
    app->st.dirty = false;
    click(itemCenter("Name"));
    typeText("X");
    CHECK(app->doc.bones[2].name == "bone_2X");
    click(itemCenter("bone_0"));
    frame(2);
    CHECK(app->st.selectedBone == 0);
    CHECK(app->history.pending() == nullptr);
    CHECK(app->st.dirty);
    key(ImGuiKey_Z, cmd());
    CHECK(app->doc.bones[2].name == "bone_2");

    // Hierarchy drag-drop: bone_2 onto (root) keeps its world transform; undo restores the parent.
    frame();
    const ska::Vec2 tailBefore = tail(2, shown());
    drag(itemCenter("bone_2"), itemCenter("(root)"));
    CHECK(app->doc.bones[2].parent == -1);
    CHECK(dist(tail(2, shown()), tailBefore) < 1e-2f);
    key(ImGuiKey_Z, cmd());
    CHECK(app->doc.bones[2].parent == 1);

    // Viewport context menu: right-click bone_1, "Delete bone"; undo restores it.
    click(S(mid(1, shown())), 1);
    CHECK(app->st.selectedBone == 1);
    click(itemCenter("Delete bone"));
    CHECK(app->doc.bones.size() == 2);
    CHECK(app->doc.bones[1].parent == 0);  // former bone_2 reparented to bone_0
    key(ImGuiKey_Z, cmd());
    CHECK(app->doc.bones.size() == 3);

    // Shortcuts are ignored while a popup is open: Delete must not delete the selection.
    click(S(mid(2, shown())), 1);
    CHECK(app->st.selectedBone == 2);
    key(ImGuiKey_Delete);
    CHECK(app->doc.bones.size() == 3);
    key(ImGuiKey_Escape);  // closes the popup
    frame(2);

    // Animate: rotate the lower leg on frame 0 -> auto-key.
    key(ImGuiKey_Tab);
    CHECK(app->st.mode == Mode::Animate);
    const ska::Vec2 m2 = mid(2, shown());
    drag(S(m2), S(m2.x + 35, m2.y - 5));
    CHECK(app->clip().keys.size() == 1 && app->clip().keys[0].frame == 0);
    CHECK(app->st.selectedBone == 2);

    // Right x10, Alt-drag the tail: 2-bone IK, key at 10.
    for (int i = 0; i < 10; ++i) key(ImGuiKey_RightArrow);
    CHECK(app->st.frame == 10);
    CHECK(app->video.currentFrame() == 10);
    drag(S(tail(2, shown())), S(360, 300), ImGuiMod_Alt);
    CHECK(app->clip().keys.size() == 2 && app->clip().keys.back().frame == 10);
    CHECK(dist(tail(2, shown()), {360, 300}) < 1.0f);

    // Shift+Left -> previous key, video follows.
    key(ImGuiKey_LeftArrow, ImGuiMod_Shift);
    CHECK(app->st.frame == 0);
    CHECK(app->video.currentFrame() == 0);

    // Space plays, Space pauses.
    key(ImGuiKey_Space);
    CHECK(app->st.playing);
    frame(30);
    CHECK(app->st.frame > 5);
    key(ImGuiKey_Space);
    CHECK(!app->st.playing);

    // Undo removes the IK key, redo restores it, undo again.
    key(ImGuiKey_Z, cmd());
    CHECK(app->clip().keys.size() == 1);
    key(ImGuiKey_Z, cmd() | ImGuiMod_Shift);
    CHECK(app->clip().keys.size() == 2);
    key(ImGuiKey_Z, cmd());
    CHECK(app->clip().keys.size() == 1);

    // Save, then New + Open restores the identical document and its video.
    CHECK(app->st.dirty);
    CHECK(app->saveTo(skaPath));
    CHECK(!app->st.dirty);
    ska::Document onDisk;
    std::string err;
    CHECK(ska::readFile(skaPath, onDisk, &err));
    CHECK(!onDisk.clips.empty() && !std::filesystem::path(onDisk.clips[0].videoPath).is_absolute());
    const std::vector<uint8_t> saved = ska::write(app->doc);
    app->newDocument();
    frame(2);
    CHECK(app->doc.bones.empty() && !app->video.isOpen());
    app->openDocument(skaPath);
    frame(2);
    CHECK(ska::write(app->doc) == saved);
    CHECK(app->video.isOpen());
    CHECK(app->st.mode == Mode::Animate);

    // Edit in Setup, then quit: the unsaved-changes prompt blocks the quit.
    key(ImGuiKey_Tab);
    app->st.selectedBone = 2;
    key(ImGuiKey_Delete);
    CHECK(app->doc.bones.size() == 2);
    CHECK(!app->clip().keys.empty() && app->clip().keys[0].poses.size() == 2);
    app->requestQuit();
    frame(2);
    CHECK(app->unsavedPromptPending());
    CHECK(!app->quitConfirmed());

    // --- Tutorial: shown on first start, builds, animates, saves and restores the figure -------
    delete app;
    ImGui::ClosePopupToLevel(0, false);
    std::filesystem::remove(settingsPath);
    app = new App(window, settingsPath);
    frame(3);
    Tutorial& tut = app->tutorial;
    CHECK(tut.isOpen());
    CHECK(tut.step() == 0);
    auto next = [&] {
        const int before = tut.step();
        click(itemCenter("Next"));
        frame();  // the auto-resizing window takes one frame to fit the new step's text
        CHECK(tut.step() == before + 1);
    };
    auto doIt = [&] {
        click(itemCenter("Do it for me"));
        frame();
        CHECK(tut.stepDone(*app));
    };
    std::vector<std::pair<ska::Vec2, ska::Vec2>> guide;

    CHECK(tut.stepDone(*app));  // 0 welcome: empty document
    next();
    next();  // 1 viewport
    next();  // 2 hierarchy/properties
    next();  // 3 timeline
    // 4 video (user action), pointer target recorded
    CHECK(!tut.stepDone(*app));
    CHECK(app->uiRect(UiTarget::ImportVideo).valid);
    app->importVideoFile(videoPath);
    frame();
    CHECK(tut.stepDone(*app));
    next();
    // 5 create tool (user action: B)
    CHECK(!tut.stepDone(*app));
    key(ImGuiKey_B);
    CHECK(tut.stepDone(*app));
    next();
    // 6-8 skeleton stages via "Do it for me"; each shows its tracing guide
    tut.guide(*app, guide);
    CHECK(guide.size() == 4);
    doIt();
    CHECK(app->doc.bones.size() == 4);
    next();
    doIt();
    CHECK(app->doc.bones.size() == 8);
    next();
    tut.guide(*app, guide);
    CHECK(guide.size() == 12);
    doIt();
    CHECK(app->doc.bones.size() == 12);
    if (app->doc.bones.size() == 12) {
        auto parentName = [&](int i) {
            const int p = app->doc.bones[size_t(i)].parent;
            return p < 0 ? std::string("-") : app->doc.bones[size_t(p)].name;
        };
        CHECK(app->doc.bones[0].name == "pelvis" && parentName(0) == "-");
        CHECK(app->doc.bones[3].name == "head" && parentName(3) == "neck");
        CHECK(app->doc.bones[4].name == "upper_arm_l" && parentName(4) == "spine");
        CHECK(app->doc.bones[11].name == "shin_r" && parentName(11) == "thigh_r");
        // Example joints land where the guide drew them.
        tut.guide(*app, guide);
        CHECK(dist(tail(11, shown()), guide[11].second) < 0.5f);
    }
    next();
    CHECK(tut.stepDone(*app));  // 9 names: example bones are named
    next();
    // 10 animate mode (user action: Tab)
    key(ImGuiKey_Tab);
    CHECK(tut.stepDone(*app));
    next();
    doIt();  // 11 first key
    CHECK(app->clip().keys.size() == 1 && app->clip().keys[0].frame == 0);
    next();
    doIt();  // 12 second key + loop key
    CHECK(app->clip().keys.size() == 3);
    if (app->clip().keys.size() == 3) {
        const auto& keys = app->clip().keys;
        CHECK(keys[1].frame == 15 && keys[2].frame == 30);
        CHECK(keys[1].poses[4].rotation != keys[0].poses[4].rotation);  // arms swing between keys
        CHECK(keys[2].poses[4].rotation == keys[0].poses[4].rotation);  // loops back to the first pose
    }
    next();
    doIt();  // 13 play
    key(ImGuiKey_Space);
    CHECK(!app->st.playing);
    next();
    // 14 save (user action; the menu path opens a native dialog)
    CHECK(!tut.stepDone(*app));
    const std::string tutorialSka = skaPath + ".tutorial.ska";
    CHECK(app->saveTo(tutorialSka));
    frame();
    CHECK(tut.stepDone(*app));
    const std::vector<uint8_t> savedDoc = ska::write(app->doc);
    next();
    // 15 restore from the file. An edit after saving blocks "Do it for me" until re-saved; the
    // step then follows the re-saved content.
    CHECK(!tut.stepDone(*app));
    app->setFrame(5);
    key(ImGuiKey_K);  // adds a 4th key, document dirty
    CHECK(app->clip().keys.size() == 4 && app->st.dirty);
    CHECK(app->saveTo(tutorialSka));
    frame();
    const std::vector<uint8_t> resavedDoc = ska::write(app->doc);
    doIt();
    CHECK(ska::write(app->doc) == resavedDoc && resavedDoc != savedDoc);
    CHECK(app->doc.bones.size() == 12 && app->clip().keys.size() == 4);
    CHECK(app->video.isOpen());
    next();
    // 16 done: opt out of the startup tutorial, finish
    click(itemCenter("Don't show this tutorial on startup"));
    CHECK(!app->settings.showTutorial);
    click(itemCenter("Finish"));
    CHECK(!tut.isOpen());
    {
        Settings loaded;
        loadSettings(settingsPath, loaded);
        CHECK(!loaded.showTutorial);
    }
    delete app;
    app = new App(window, settingsPath);
    frame(2);
    CHECK(!app->tutorial.isOpen());
    std::filesystem::remove(tutorialSka);
    std::filesystem::remove(settingsPath);

    std::printf("app_tests: %s (%d failures)\n", failures ? "FAIL" : "OK", failures);
    delete app;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return failures ? 1 : 0;
}
