#include "core/figure.h"

#include "core/skeleton_ops.h"

#include <cmath>
#include <vector>

namespace skel {
namespace {

constexpr FigureBone kFigure[] = {
    {"pelvis", nullptr, 0.00f, 0.50f, 0.00f, 0.42f, 0},
    {"spine", "pelvis", 0.00f, 0.42f, 0.00f, 0.21f, 0},
    {"neck", "spine", 0.00f, 0.21f, 0.00f, 0.14f, 0},
    {"head", "neck", 0.00f, 0.14f, 0.00f, 0.00f, 0},
    {"upper_arm_l", "spine", -0.08f, 0.23f, -0.13f, 0.37f, 1},
    {"forearm_l", "upper_arm_l", -0.13f, 0.37f, -0.15f, 0.50f, 1},
    {"upper_arm_r", "spine", 0.08f, 0.23f, 0.13f, 0.37f, 1},
    {"forearm_r", "upper_arm_r", 0.13f, 0.37f, 0.15f, 0.50f, 1},
    {"thigh_l", "pelvis", -0.05f, 0.50f, -0.06f, 0.73f, 2},
    {"shin_l", "thigh_l", -0.06f, 0.73f, -0.06f, 0.96f, 2},
    {"thigh_r", "pelvis", 0.05f, 0.50f, 0.06f, 0.73f, 2},
    {"shin_r", "thigh_r", 0.06f, 0.73f, 0.06f, 0.96f, 2},
};

constexpr float kFigureScale = 0.84f;  // figure height / canvas height
constexpr float kDegToRad = 3.14159265358979f / 180.0f;

// --- Dance ----------------------------------------------------------------------
// A pose gives world directions in degrees for the bones it moves (screen convention:
// 0 = right, 90 = down, -90 = up, 180 = left; "_l" bones are on the viewer's left) and a hip
// offset in figure units. Unlisted bones keep their rest direction.

struct Limb {
    const char* bone;
    float deg;
};
struct DancePose {
    float hipX = 0, hipY = 0;
    std::vector<Limb> limbs;
};

const DancePose& pointUp() {  // right arm to the sky, left hand on the hip, hip out right
    static const DancePose p{0.03f, 0.0f,
                             {{"spine", -100}, {"neck", -95}, {"head", -85},
                              {"upper_arm_r", -45}, {"forearm_r", -45},
                              {"upper_arm_l", 135}, {"forearm_l", 20},
                              {"thigh_l", 100}, {"shin_l", 92}, {"thigh_r", 85}, {"shin_r", 88}}};
    return p;
}
const DancePose& pointDown() {  // right arm sweeps down across the body, knees bend, hip out left
    static const DancePose p{-0.03f, 0.02f,
                             {{"spine", -80}, {"neck", -85}, {"head", -95},
                              {"upper_arm_r", 125}, {"forearm_r", 130},
                              {"upper_arm_l", 135}, {"forearm_l", 20},
                              {"thigh_l", 105}, {"shin_l", 80}, {"thigh_r", 75}, {"shin_r", 100}}};
    return p;
}
const DancePose& bounce() {  // squat with fists at the sides, ready for the letters
    static const DancePose p{0.0f, 0.03f,
                             {{"upper_arm_l", 120}, {"forearm_l", 70}, {"upper_arm_r", 60}, {"forearm_r", 110},
                              {"thigh_l", 105}, {"shin_l", 80}, {"thigh_r", 75}, {"shin_r", 100}}};
    return p;
}
const DancePose& letterY() {
    static const DancePose p{0.0f, 0.0f,
                             {{"upper_arm_l", -125}, {"forearm_l", -125}, {"upper_arm_r", -55}, {"forearm_r", -55},
                              {"thigh_l", 95}, {"thigh_r", 85}}};
    return p;
}
const DancePose& letterM() {  // elbows out, hands on the head
    static const DancePose p{0.0f, 0.01f,
                             {{"upper_arm_l", -130}, {"forearm_l", -24}, {"upper_arm_r", -50}, {"forearm_r", -156},
                              {"thigh_l", 98}, {"thigh_r", 82}}};
    return p;
}
const DancePose& letterC() {  // both arms on the viewer's left, ends curling right; body leans into it
    static const DancePose p{-0.02f, 0.0f,
                             {{"spine", -100}, {"neck", -100}, {"head", -100},
                              {"upper_arm_l", -150}, {"forearm_l", -40},
                              {"upper_arm_r", 165}, {"forearm_r", 60},
                              {"thigh_l", 95}, {"thigh_r", 85}}};
    return p;
}
const DancePose& letterA() {  // hands meet above the head
    static const DancePose p{0.0f, 0.0f,
                             {{"upper_arm_l", -110}, {"forearm_l", -40}, {"upper_arm_r", -70}, {"forearm_r", -140},
                              {"thigh_l", 97}, {"thigh_r", 83}}};
    return p;
}

// Converts a pose of world directions into local poses for every bone.
std::vector<ska::Pose> toLocal(const ska::Document& doc, const DancePose& dp, float figureHeight) {
    std::vector<ska::Pose> local;
    ska::restPose(doc, local);
    std::vector<ska::Affine> restWorld;
    ska::computeWorld(doc, local, restWorld);

    std::vector<float> world(doc.bones.size());
    for (size_t i = 0; i < doc.bones.size(); ++i) {
        world[i] = std::atan2(restWorld[i].b, restWorld[i].a);
        for (const Limb& l : dp.limbs)
            if (doc.bones[i].name == l.bone) world[i] = l.deg * kDegToRad;
        const int p = doc.bones[i].parent;
        local[i].rotation = ska::wrapPi(world[i] - (p < 0 ? 0.0f : world[size_t(p)]));
        if (p < 0) {
            local[i].translation.x += dp.hipX * figureHeight;
            local[i].translation.y += dp.hipY * figureHeight;
        }
    }
    return local;
}

}  // namespace

std::span<const FigureBone> exampleFigure() { return kFigure; }

ska::Vec2 figureToCanvas(ska::Vec2 canvas, float x, float y) {
    const float h = canvas.y * kFigureScale;
    return {canvas.x * 0.5f + x * h, canvas.y * 0.08f + y * h};
}

int findBone(const ska::Document& doc, const std::string& name) {
    for (size_t i = 0; i < doc.bones.size(); ++i)
        if (doc.bones[i].name == name) return int(i);
    return -1;
}

void addExampleFigure(ska::Document& doc, ska::Vec2 canvas, int maxStage) {
    for (const FigureBone& f : kFigure) {
        if (f.stage > maxStage) continue;
        const int parent = f.parent ? findBone(doc, f.parent) : -1;
        const int idx = addBone(doc, parent, figureToCanvas(canvas, f.hx, f.hy), figureToCanvas(canvas, f.tx, f.ty));
        if (idx >= 0) doc.bones[size_t(idx)].name = f.name;
    }
}

int addDancingQueenClip(ska::Document& doc, ska::Vec2 canvas) {
    // 30 fps, a beat every 15 frames. Letters are held by repeating the key.
    const std::pair<int, const DancePose*> timeline[] = {
        {0, &pointUp()},  {15, &pointDown()}, {30, &pointUp()},  {45, &pointDown()}, {60, &bounce()},
        {72, &letterY()}, {84, &letterY()},   {90, &letterM()},  {102, &letterM()},  {108, &letterC()},
        {120, &letterC()}, {126, &letterA()}, {138, &letterA()}, {149, &pointUp()},  // back to the start: loops
    };
    ska::Clip clip;
    clip.name = "dancing_queen";
    clip.fpsNum = 30;
    clip.fpsDen = 1;
    clip.frameCount = 150;
    const float figureHeight = canvas.y * kFigureScale;
    for (const auto& [frame, pose] : timeline) {
        ska::Key key;
        key.frame = frame;
        key.poses = toLocal(doc, *pose, figureHeight);
        clip.keys.push_back(std::move(key));
    }
    doc.clips.push_back(std::move(clip));
    return int(doc.clips.size()) - 1;
}

}  // namespace skel
