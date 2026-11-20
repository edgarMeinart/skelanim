// The built-in 12-bone example figure and the "Dancing Queen" disco clip made for it.
#pragma once

#include <ska/ska.h>

#include <span>
#include <string>

namespace skel {

// One bone of the example figure. Coordinates are figure units: x from the canvas centre,
// y from the top of the head, 1 unit = 84% of the canvas height. Parents precede children.
struct FigureBone {
    const char* name;
    const char* parent;  // nullptr = root
    float hx, hy, tx, ty;  // head, tail
    int stage;             // 0 spine chain (pelvis..head), 1 arms, 2 legs
};

std::span<const FigureBone> exampleFigure();

// Figure units -> canvas pixels for a canvas of the given size.
ska::Vec2 figureToCanvas(ska::Vec2 canvas, float x, float y);

// Index of the bone named `name`, or -1.
int findBone(const ska::Document& doc, const std::string& name);

// Appends the example bones of stages 0..maxStage (named, parented) to `doc`.
void addExampleFigure(ska::Document& doc, ska::Vec2 canvas, int maxStage = 2);

// Appends a 150-frame, 30 fps looping disco clip "dancing_queen": the Saturday-Night-Fever point
// up/down with hip sway, a bounce, then the Y-M-C-A arm letters. Needs the example figure's bone
// names (other bones keep their rest pose). Returns the new clip's index.
int addDancingQueenClip(ska::Document& doc, ska::Vec2 canvas);

}  // namespace skel
