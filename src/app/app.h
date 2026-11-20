#pragma once

#include "app/editor_state.h"
#include "app/settings.h"
#include "app/tutorial.h"
#include "app/video_texture.h"
#include "core/history.h"
#include "core/skeleton_ops.h"
#include "video/video_decoder.h"

#include <ska/ska.h>

#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

// Owns the document and all editor state. Viewport and panels (free functions in
// viewport.cpp / panels.cpp) read and mutate it through the public members below.
class App {
public:
    // settingsPath: per-user preferences file; empty = defaults, nothing persisted.
    App(GLFWwindow* window, std::string settingsPath);
    ~App();

    void frame();
    void requestQuit();
    bool quitConfirmed() const { return quitConfirmed_; }

    ska::Document doc;
    EditorState st;
    History history;
    VideoDecoder video;
    VideoTexture tex;
    std::string videoError;
    bool fitRequested = false;
    Settings settings;
    Tutorial tutorial;

    void saveSettings();
    // Increments whenever the document is replaced (New / Open).
    uint64_t docGeneration() const { return docGeneration_; }
    // Screen rect of a tutorial target widget, recorded during the current frame.
    void markUi(UiTarget t);
    const UiRect& uiRect(UiTarget t) const { return uiRects_[size_t(t)]; }

    ska::Clip& clip() { return doc.clips[size_t(st.clip)]; }
    const ska::Clip& clip() const { return doc.clips[size_t(st.clip)]; }
    ImVec2 canvasSize() const;  // video size, or 1280x720 without video
    int lastKeyFrame() const;   // -1 when the clip has no keys

    void setFrame(int f);
    void setMode(Mode m);
    void selectClip(int index);

    // Undo helpers: pushUndo before a discrete mutation; begin/end around continuous edits.
    void pushUndo();
    void beginEdit();
    void endEdit(bool changed);
    void flushEdit();  // ends a pending continuous edit, recording it only if the doc changed
    // ImGui widget undo pattern; call right after the widget, before applying its value.
    void trackItemEdit();

    void undo();
    void redo();
    void loadClipVideo();
    void importVideo();                              // file dialog, then importVideoFile
    void importVideoFile(const std::string& path);   // stored as a canonical absolute path
    void showError(std::string msg);

    // Pose currently shown: rest pose in Setup, sampled clip pose in Animate.
    void displayedPose(std::vector<ska::Pose>& out) const;

    // Dialog-free file operations (the menu wraps them with file dialogs and the unsaved prompt).
    void newDocument();
    void openDocument(const std::string& path);
    bool saveTo(const std::string& path);
    bool unsavedPromptPending() const { return pending_ != Pending::None; }

private:
    friend struct TutorialSteps;  // tutorial actions reuse save/new with the unsaved prompt
    enum class Pending { None, New, Open, Quit };

    bool save();
    bool saveAs();
    void openDialog();
    void requestAction(Pending action);
    void runPending(Pending action);
    void afterHistoryChange();

    void handleShortcuts();
    void drawMenuBar();
    void drawModals();
    void updatePlayback();
    void updateTitle();

    GLFWwindow* window_;
    Pending pending_ = Pending::None;
    bool openUnsavedPopup_ = false;
    bool openErrorPopup_ = false;
    std::string errorModalMsg_;
    std::string loadedVideoPath_;
    std::string title_;
    bool quitConfirmed_ = false;
    std::string settingsPath_;
    uint64_t docGeneration_ = 0;
    UiRect uiRects_[size_t(UiTarget::Count)];
};
