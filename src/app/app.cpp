#include "app/app.h"

#include "app/panels.h"
#include "app/viewport.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>  // IsKeyChordPressed with ImGuiInputFlags_Repeat
#include <nfd.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace {

constexpr float kTimelineHeight = 190;
constexpr float kRightColumnWidth = 340;
// NoBringToFrontOnFocus keeps the fixed panels below floating windows such as the tutorial.
constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

ska::Clip defaultClip(const std::string& name) {
    ska::Clip c;
    c.name = name;
    c.fpsNum = 30;
    c.fpsDen = 1;
    c.frameCount = 120;
    return c;
}

std::string fileName(const std::string& path) { return path.empty() ? "untitled" : fs::path(path).filename().string(); }

// Directory used to resolve relative video paths for a document stored at docPath. Canonical
// (symlinks resolved) because fs::relative() computes stored paths from canonical forms.
fs::path baseDir(const std::string& docPath) {
    std::error_code ec;
    if (docPath.empty()) return fs::current_path(ec);
    const fs::path dir = fs::absolute(fs::path(docPath), ec).parent_path();
    const fs::path canon = fs::weakly_canonical(dir, ec);
    return ec ? dir : canon;
}

fs::path resolveVideo(const std::string& videoPath, const std::string& docPath) {
    fs::path p(videoPath);
    return p.is_absolute() ? p : (baseDir(docPath) / p).lexically_normal();
}

}  // namespace

App::App(GLFWwindow* window, std::string settingsPath) : window_(window), settingsPath_(std::move(settingsPath)) {
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    loadSettings(settingsPath_, settings);
    newDocument();
    if (settings.showTutorial) tutorial.start();
}

void App::saveSettings() {
    if (!settingsPath_.empty() && !::saveSettings(settingsPath_, settings))
        showError("Could not write settings to " + settingsPath_);
}

void App::markUi(UiTarget t) { uiRects_[size_t(t)] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true}; }

App::~App() { tex.destroy(); }

// ---------------------------------------------------------------------------
// Document state

ImVec2 App::canvasSize() const {
    if (video.isOpen()) return ImVec2(float(video.width()), float(video.height()));
    return ImVec2(1280, 720);
}

int App::lastKeyFrame() const { return clip().keys.empty() ? -1 : clip().keys.back().frame; }

void App::displayedPose(std::vector<ska::Pose>& out) const {
    if (st.mode == Mode::Setup)
        ska::restPose(doc, out);
    else
        ska::sampleFrame(doc, clip(), float(st.frame), out);
}

void App::setFrame(int f) {
    const int last = int(clip().frameCount) - 1;
    st.frame = std::clamp(f, 0, std::max(0, last));
    if (video.isOpen() && video.decodeFrame(std::min(st.frame, video.frameCount() - 1)))
        tex.upload(video.rgba(), video.width(), video.height());
}

void App::setMode(Mode m) {
    st.mode = m;
    if (m == Mode::Animate) st.createTool = false;
}

void App::selectClip(int index) {
    st.clip = std::clamp(index, 0, int(doc.clips.size()) - 1);
    st.frame = 0;
    st.playing = false;
    loadClipVideo();
}

void App::newDocument() {
    doc = ska::Document{};
    doc.name = "untitled";
    ++docGeneration_;
    doc.clips.push_back(defaultClip("clip0"));
    st = EditorState{};
    history.clear();
    loadClipVideo();
    fitRequested = true;
}

void App::pushUndo() {
    flushEdit();
    history.push(doc);
    st.dirty = true;
}

void App::beginEdit() {
    flushEdit();
    history.begin(doc);
}

// Closes an open continuous edit whose widget vanished without reporting deactivation
// (e.g. the Properties panel switched bones mid-edit), so it neither leaks nor merges.
void App::flushEdit() {
    if (const ska::Document* before = history.pending()) endEdit(ska::write(*before) != ska::write(doc));
}

void App::endEdit(bool changed) {
    history.end(changed);
    if (changed) st.dirty = true;
}

void App::trackItemEdit() {
    if (ImGui::IsItemActivated()) beginEdit();
    if (ImGui::IsItemDeactivatedAfterEdit())
        endEdit(true);
    else if (ImGui::IsItemDeactivated())
        endEdit(false);
}

void App::afterHistoryChange() {
    st.dirty = true;
    st.clip = std::clamp(st.clip, 0, int(doc.clips.size()) - 1);
    st.frame = std::clamp(st.frame, 0, int(clip().frameCount) - 1);
    if (st.selectedBone >= int(doc.bones.size())) st.selectedBone = -1;
    if (clip().videoPath != loadedVideoPath_)
        loadClipVideo();
    else
        setFrame(st.frame);
}

void App::undo() {
    if (st.drag != DragKind::None || ImGui::IsAnyItemActive()) return;
    if (history.undo(doc)) afterHistoryChange();
}

void App::redo() {
    if (st.drag != DragKind::None || ImGui::IsAnyItemActive()) return;
    if (history.redo(doc)) afterHistoryChange();
}

void App::showError(std::string msg) {
    errorModalMsg_ = std::move(msg);
    openErrorPopup_ = true;
}

// ---------------------------------------------------------------------------
// Video

void App::loadClipVideo() {
    video.close();
    videoError.clear();
    loadedVideoPath_ = clip().videoPath;
    if (!clip().videoPath.empty()) {
        const fs::path path = resolveVideo(clip().videoPath, st.docPath);
        std::string err;
        if (!video.open(path.string(), err)) videoError = "Video not found: " + path.string();
    }
    setFrame(st.frame);
}

void App::importVideo() {
    nfdu8char_t* out = nullptr;
    const nfdu8filteritem_t filters[] = {{"Video", "mp4,mov,m4v,avi,mkv,webm"}};
    const nfdresult_t r = NFD_OpenDialogU8(&out, filters, 1, nullptr);
    ImGui::GetIO().ClearInputKeys();  // key-up events went to the native dialog
    if (r == NFD_ERROR) return showError(std::string("File dialog error: ") + NFD_GetError());
    if (r != NFD_OKAY) return;
    std::error_code ec;
    const std::string path = fs::absolute(fs::path(out), ec).lexically_normal().string();
    NFD_FreePathU8(out);
    importVideoFile(path);
}

void App::importVideoFile(const std::string& path) {
    VideoDecoder dec;
    std::string err;
    if (!dec.open(path, err)) return showError("Failed to open video " + path + ": " + err);

    pushUndo();
    ska::Clip& c = clip();
    // Canonical, like the paths openDocument resolves against baseDir(), so a save/open round
    // trip reproduces the same string even when the path went through a symlink (/tmp on macOS).
    std::error_code ec;
    const fs::path canon = fs::weakly_canonical(fs::absolute(fs::path(path), ec), ec);
    c.videoPath = ec ? path : canon.string();
    c.fpsNum = uint32_t(dec.fpsNum());
    c.fpsDen = uint32_t(dec.fpsDen());
    c.frameCount = uint32_t(std::max(dec.frameCount(), lastKeyFrame() + 1));
    video = std::move(dec);
    videoError.clear();
    loadedVideoPath_ = c.videoPath;
    setFrame(st.frame);
    fitRequested = true;
}

// ---------------------------------------------------------------------------
// File operations

void App::openDocument(const std::string& path) {
    ska::Document loaded;
    std::string err;
    if (!ska::readFile(path, loaded, &err)) return showError("Failed to open " + path + ": " + err);

    // In memory, video paths are absolute; saveTo() writes them relative to the .ska file.
    for (ska::Clip& c : loaded.clips)
        if (!c.videoPath.empty()) c.videoPath = resolveVideo(c.videoPath, path).string();
    if (loaded.clips.empty()) loaded.clips.push_back(defaultClip("clip0"));

    doc = std::move(loaded);
    ++docGeneration_;
    history.clear();
    st = EditorState{};
    st.docPath = path;
    const bool anyKey = std::any_of(doc.clips.begin(), doc.clips.end(), [](const ska::Clip& c) { return !c.keys.empty(); });
    st.mode = anyKey ? Mode::Animate : Mode::Setup;
    loadClipVideo();
    fitRequested = true;
}

bool App::saveTo(const std::string& path) {
    ska::Document out = doc;
    const fs::path newDir = baseDir(path);
    for (ska::Clip& c : out.clips) {
        if (c.videoPath.empty()) continue;
        const fs::path abs = resolveVideo(c.videoPath, st.docPath);
        fs::path rel;
        try {
            rel = fs::relative(abs, newDir);
        } catch (const fs::filesystem_error&) {
        }
        c.videoPath = (rel.empty() ? abs : rel).generic_string();
    }
    std::string err;
    if (!ska::writeFile(path, out, &err)) {
        showError("Failed to save " + path + ": " + err);
        return false;
    }
    // Keep in-memory paths absolute so they stay valid if the document moves again.
    for (ska::Clip& c : doc.clips)
        if (!c.videoPath.empty()) c.videoPath = resolveVideo(c.videoPath, st.docPath).string();
    loadedVideoPath_ = clip().videoPath;
    st.docPath = path;
    st.dirty = false;
    return true;
}

bool App::save() { return st.docPath.empty() ? saveAs() : saveTo(st.docPath); }

bool App::saveAs() {
    nfdu8char_t* out = nullptr;
    const nfdu8filteritem_t filters[] = {{"SkelAnim", "ska"}};
    const std::string defName = st.docPath.empty() ? "untitled.ska" : fileName(st.docPath);
    const nfdresult_t r = NFD_SaveDialogU8(&out, filters, 1, nullptr, defName.c_str());
    ImGui::GetIO().ClearInputKeys();
    if (r == NFD_ERROR) {
        showError(std::string("File dialog error: ") + NFD_GetError());
        return false;
    }
    if (r != NFD_OKAY) return false;
    std::string path = out;
    NFD_FreePathU8(out);
    if (fs::path(path).extension() != ".ska") path += ".ska";
    return saveTo(path);
}

void App::openDialog() {
    nfdu8char_t* out = nullptr;
    const nfdu8filteritem_t filters[] = {{"SkelAnim", "ska"}};
    const nfdresult_t r = NFD_OpenDialogU8(&out, filters, 1, nullptr);
    ImGui::GetIO().ClearInputKeys();
    if (r == NFD_ERROR) return showError(std::string("File dialog error: ") + NFD_GetError());
    if (r != NFD_OKAY) return;
    const std::string path = out;
    NFD_FreePathU8(out);
    openDocument(path);
}

void App::requestAction(Pending action) {
    if (st.dirty) {
        pending_ = action;
        openUnsavedPopup_ = true;
    } else {
        runPending(action);
    }
}

void App::runPending(Pending action) {
    switch (action) {
        case Pending::New: newDocument(); break;
        case Pending::Open: openDialog(); break;
        case Pending::Quit: quitConfirmed_ = true; break;
        case Pending::None: break;
    }
}

void App::requestQuit() {
    if (pending_ == Pending::None) requestAction(Pending::Quit);
}

// ---------------------------------------------------------------------------
// Frame

void App::frame() {
    for (UiRect& r : uiRects_) r.valid = false;
    tutorial.update(*this);
    handleShortcuts();
    drawMenuBar();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 pos = vp->WorkPos, size = vp->WorkSize;
    const float mainH = std::max(1.0f, size.y - kTimelineHeight);
    const float viewW = std::max(1.0f, size.x - kRightColumnWidth);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewW, mainH), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin("Viewport", nullptr,
                     kPanelFlags | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                         ImGuiWindowFlags_NoTitleBar))
        drawViewport(*this);
    ImGui::End();
    ImGui::PopStyleVar();

    ImGui::SetNextWindowPos(ImVec2(pos.x + viewW, pos.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kRightColumnWidth, mainH * 0.45f), ImGuiCond_Always);
    if (ImGui::Begin("Hierarchy", nullptr, kPanelFlags)) drawHierarchy(*this);
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(pos.x + viewW, pos.y + mainH * 0.45f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kRightColumnWidth, mainH * 0.55f), ImGuiCond_Always);
    if (ImGui::Begin("Properties", nullptr, kPanelFlags)) drawProperties(*this);
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(pos.x, pos.y + mainH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(size.x, kTimelineHeight), ImGuiCond_Always);
    if (ImGui::Begin("Timeline", nullptr, kPanelFlags)) drawTimeline(*this);
    ImGui::End();

    updatePlayback();
    tutorial.draw(*this);
    drawModals();
    // Safety net for edits whose widget disappeared mid-edit (see flushEdit).
    if (st.drag == DragKind::None && !ImGui::IsAnyItemActive()) flushEdit();
    updateTitle();
}

void App::updatePlayback() {
    if (!st.playing) {
        st.playAccum = 0;
        return;
    }
    const ska::Clip& c = clip();
    const double step = double(c.fpsDen) / double(c.fpsNum);
    st.playAccum += ImGui::GetIO().DeltaTime;
    int f = st.frame;
    while (st.playAccum >= step) {
        f = (f + 1) % int(c.frameCount);
        st.playAccum -= step;
    }
    if (f != st.frame) setFrame(f);
}

void App::updateTitle() {
    std::string t = "SkelAnim - " + fileName(st.docPath) + (st.dirty ? " *" : "");
    if (t != title_) {
        title_ = t;
        glfwSetWindowTitle(window_, title_.c_str());
    }
}

void App::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    auto chord = [](ImGuiKeyChord c) { return ImGui::IsKeyChordPressed(c); };
    auto chordRepeat = [](ImGuiKeyChord c) { return ImGui::IsKeyChordPressed(c, ImGuiInputFlags_Repeat); };

    // Modal prompts, menus and context popups own the keyboard.
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        st.spacePanned = false;
        return;
    }

    if (chord(ImGuiMod_Ctrl | ImGuiKey_N)) requestAction(Pending::New);
    if (chord(ImGuiMod_Ctrl | ImGuiKey_O)) requestAction(Pending::Open);
    if (chord(ImGuiMod_Ctrl | ImGuiKey_S)) save();
    if (chord(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) saveAs();
    if (chord(ImGuiMod_Ctrl | ImGuiKey_I)) importVideo();
    if (chordRepeat(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
    if (chordRepeat(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) || chordRepeat(ImGuiMod_Ctrl | ImGuiKey_Y)) redo();

    // Space toggles playback on release, unless it was held to pan the viewport.
    if (ImGui::IsKeyReleased(ImGuiKey_Space)) {
        if (!st.spacePanned && !io.WantTextInput && io.KeyMods == ImGuiMod_None) st.playing = !st.playing;
        st.spacePanned = false;
    }

    if (io.WantTextInput || st.drag != DragKind::None) return;

    const int nframes = int(clip().frameCount);
    if (chordRepeat(ImGuiKey_LeftArrow)) {
        st.playing = false;
        setFrame(st.frame - 1);
    }
    if (chordRepeat(ImGuiKey_RightArrow)) {
        st.playing = false;
        setFrame(st.frame + 1);
    }
    if (chordRepeat(ImGuiMod_Shift | ImGuiKey_LeftArrow)) {
        const int f = skel::prevKeyFrame(clip(), st.frame);
        if (f >= 0) {
            st.playing = false;
            setFrame(f);
        }
    }
    if (chordRepeat(ImGuiMod_Shift | ImGuiKey_RightArrow)) {
        const int f = skel::nextKeyFrame(clip(), st.frame);
        if (f >= 0) {
            st.playing = false;
            setFrame(f);
        }
    }
    if (chord(ImGuiKey_Home)) {
        st.playing = false;
        setFrame(0);
    }
    if (chord(ImGuiKey_End)) {
        st.playing = false;
        setFrame(nframes - 1);
    }
    if (chord(ImGuiKey_K) && st.mode == Mode::Animate && skel::findKey(clip(), st.frame) < 0) {
        pushUndo();
        skel::ensureKey(doc, clip(), st.frame);
    }
    if (chord(ImGuiMod_Shift | ImGuiKey_K) && skel::findKey(clip(), st.frame) >= 0) {
        pushUndo();
        skel::deleteKey(clip(), st.frame);
    }
    if (chord(ImGuiKey_Tab)) setMode(st.mode == Mode::Setup ? Mode::Animate : Mode::Setup);
    if (chord(ImGuiKey_B) && st.mode == Mode::Setup) st.createTool = !st.createTool;
    if ((chord(ImGuiKey_Delete) || chord(ImGuiKey_Backspace)) && st.mode == Mode::Setup && st.selectedBone >= 0) {
        pushUndo();
        skel::deleteBone(doc, st.selectedBone);
        st.selectedBone = -1;
    }
    if (chord(ImGuiKey_Escape)) {
        if (st.createTool)
            st.createTool = false;
        else
            st.selectedBone = -1;
    }
    if (chord(ImGuiKey_F)) fitRequested = true;
}

void App::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    const bool fileOpen = ImGui::BeginMenu("File");
    markUi(UiTarget::FileMenu);
    if (fileOpen) {
        if (ImGui::MenuItem("New", SKELANIM_MOD "+N")) requestAction(Pending::New);
        if (ImGui::MenuItem("Open...", SKELANIM_MOD "+O")) requestAction(Pending::Open);
        if (ImGui::MenuItem("Save", SKELANIM_MOD "+S")) save();
        if (ImGui::MenuItem("Save As...", SKELANIM_MOD "+Shift+S")) saveAs();
        ImGui::Separator();
        if (ImGui::MenuItem("Import Video...", SKELANIM_MOD "+I")) importVideo();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) requestQuit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", SKELANIM_MOD "+Z", false, history.canUndo())) undo();
        if (ImGui::MenuItem("Redo", SKELANIM_MOD "+Shift+Z", false, history.canRedo())) redo();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Onion skin", nullptr, &st.onionSkin);
        ImGui::MenuItem("Bone names", nullptr, &st.showNames);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Video opacity", &st.videoAlpha, 0.0f, 1.0f, "%.2f");
        if (ImGui::MenuItem("Fit view", "F")) fitRequested = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Tutorial")) tutorial.start();
        if (ImGui::MenuItem("Show tutorial on startup", nullptr, &settings.showTutorial)) saveSettings();
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::drawModals() {
    if (openUnsavedPopup_) {
        ImGui::OpenPopup("Unsaved changes");
        openUnsavedPopup_ = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The document has unsaved changes.");
        Pending next = Pending::None;
        bool close = false;
        if (ImGui::Button("Save", ImVec2(100, 0))) {
            close = true;
            if (save()) next = pending_;
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(100, 0))) {
            close = true;
            next = pending_;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
        if (close) {
            pending_ = Pending::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        if (next != Pending::None) runPending(next);
    } else if (pending_ != Pending::None && !ImGui::IsPopupOpen("Unsaved changes")) {
        pending_ = Pending::None;  // popup was dismissed some other way
    }

    // Opened after the prompt so errors raised by its Save button stack on top.
    if (openErrorPopup_) {
        ImGui::OpenPopup("Error");
        openErrorPopup_ = false;
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
        ImGui::TextUnformatted(errorModalMsg_.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("OK", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
