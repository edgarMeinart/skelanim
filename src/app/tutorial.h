#pragma once

#include <ska/ska.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class App;

// Guided, non-modal walkthrough. Each step explains a feature, points at the UI that performs it,
// detects when the user has done it, and offers "Do it for me". Together the steps build a
// 12-bone figure, pose two keys, save it to a .ska file and restore it from that file.
class Tutorial {
public:
    struct Step;

    void start();
    void close() { open_ = false; }
    bool isOpen() const { return open_; }
    int step() const { return step_; }
    int stepCount() const;
    bool stepDone(const App& app) const;

    void update(App& app);  // per frame, before drawing: tracks playback and the saved file
    void draw(App& app);    // tutorial window + pointer at the step's target

    // Example skeleton segments (world space) to trace in the current step, if any.
    void guide(const App& app, std::vector<std::pair<ska::Vec2, ska::Vec2>>& out) const;

    // Saved-file record (set when the save step completes) used by the restore step.
    struct Saved {
        std::string path;
        size_t bones = 0, keys = 0;
        uint64_t generation = 0;
    };

private:
    friend struct TutorialSteps;
    bool open_ = false;
    int step_ = 0;
    bool sawPlaying_ = false;
    Saved saved_;
};
