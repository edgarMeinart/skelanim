#include "core/figure.h"
#include "core/history.h"
#include "core/skeleton_ops.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

constexpr float kPi = 3.14159265358979323846f;

using ska::Affine;
using ska::Pose;
using ska::Vec2;

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
bool near(Vec2 a, Vec2 b, float eps) { return near(a.x, b.x, eps) && near(a.y, b.y, eps); }

Pose pose(float tx, float ty, float rotDeg, float sx = 1, float sy = 1) {
    Pose p;
    p.translation = {tx, ty};
    p.rotation = rotDeg * kPi / 180;
    p.scale = {sx, sy};
    return p;
}

ska::Bone bone(const char* name, int parent, float length, Pose rest) {
    ska::Bone b;
    b.name = name;
    b.parent = parent;
    b.length = length;
    b.rest = rest;
    return b;
}

std::vector<Affine> world(const ska::Document& doc, const std::vector<Pose>& locals) {
    std::vector<Affine> w;
    ska::computeWorld(doc, locals, w);
    return w;
}

std::vector<Affine> restWorld(const ska::Document& doc) {
    std::vector<Pose> rest;
    ska::restPose(doc, rest);
    return world(doc, rest);
}

Vec2 headOf(const Affine& w) { return {w.tx, w.ty}; }
Vec2 tailOf(const Affine& w, float length) { return ska::apply(w, {length, 0}); }

float cross(Vec2 o, Vec2 a, Vec2 b) {
    return (a.x - o.x) * (b.y - a.y) - (a.y - o.y) * (b.x - a.x);
}

bool parentsValid(const ska::Document& doc) {
    for (size_t i = 0; i < doc.bones.size(); ++i)
        if (doc.bones[i].parent < -1 || doc.bones[i].parent >= int(i)) return false;
    for (const ska::Clip& c : doc.clips)
        for (const ska::Key& k : c.keys)
            if (k.poses.size() != doc.bones.size()) return false;
    return true;
}

ska::Key key(int frame, std::vector<Pose> poses) {
    ska::Key k;
    k.frame = frame;
    k.poses = std::move(poses);
    return k;
}

void testAddBone() {
    ska::Document doc;
    doc.clips.push_back({});
    doc.bones.push_back(bone("parent", -1, 30, pose(10, 20, 90, 2, 2)));
    doc.clips[0].keys.push_back(key(0, {pose(1, 2, 3)}));

    const int i = skel::addBone(doc, 0, {0, 0}, {0, 100});
    CHECK(i == 1);
    CHECK(doc.bones[1].name == "bone_1");
    CHECK(doc.bones[1].parent == 0);
    CHECK(doc.bones[1].color == 0xFF33CC66u);
    CHECK(near(doc.bones[1].rest.scale, {1, 1}, 1e-6f));
    const std::vector<Affine> w = restWorld(doc);
    CHECK(near(headOf(w[1]), {0, 0}, 1e-3f));
    CHECK(near(tailOf(w[1], doc.bones[1].length), {0, 100}, 1e-3f));
    CHECK(doc.clips[0].keys[0].poses.size() == 2);
    CHECK(parentsValid(doc));

    CHECK(skel::addBone(doc, 0, {5, 5}, {6, 6}) == -1);
    CHECK(doc.bones.size() == 2);

    const int r = skel::addBone(doc, -1, {10, 10}, {10, 60});
    CHECK(r == 2);
    CHECK(near(tailOf(restWorld(doc)[2], doc.bones[2].length), {10, 60}, 1e-3f));
}

void testDeleteBone() {
    ska::Document doc;
    doc.bones.push_back(bone("a", -1, 80, pose(100, 100, 30)));
    doc.bones.push_back(bone("b", 0, 60, pose(80, 0, -45, 1.5f, 1.5f)));
    doc.bones.push_back(bone("c", 1, 40, pose(60, 5, 20)));
    ska::Clip clip;
    clip.keys.push_back(key(0, {pose(90, 110, 10), pose(80, 0, 60, 0.8f, 0.8f), pose(60, -3, -70)}));
    clip.keys.push_back(key(10, {pose(120, 90, -20), pose(70, 10, -100), pose(55, 0, 45, 1.2f, 1.2f)}));
    doc.clips.push_back(clip);

    const ska::Document before = doc;
    const Affine restC = restWorld(before)[2];
    std::vector<Affine> keyC;
    for (const ska::Key& k : before.clips[0].keys) keyC.push_back(world(before, k.poses)[2]);

    skel::deleteBone(doc, 1);
    CHECK(doc.bones.size() == 2);
    CHECK(doc.bones[1].name == "c");
    CHECK(doc.bones[1].parent == 0);
    CHECK(parentsValid(doc));
    const Affine restAfter = restWorld(doc)[1];
    CHECK(near(headOf(restAfter), headOf(restC), 1e-3f));
    CHECK(near(tailOf(restAfter, 40), tailOf(restC, 40), 1e-3f));
    for (size_t k = 0; k < doc.clips[0].keys.size(); ++k) {
        const Affine w = world(doc, doc.clips[0].keys[k].poses)[1];
        CHECK(near(headOf(w), headOf(keyC[k]), 1e-3f));
        CHECK(near(tailOf(w, 40), tailOf(keyC[k], 40), 1e-3f));
    }

    // Deleting a root makes its children roots, keeping their world transforms.
    const Affine c0 = restWorld(doc)[1];
    skel::deleteBone(doc, 0);
    CHECK(doc.bones.size() == 1 && doc.bones[0].parent == -1);
    CHECK(near(headOf(restWorld(doc)[0]), headOf(c0), 1e-3f));
    CHECK(near(tailOf(restWorld(doc)[0], 40), tailOf(c0, 40), 1e-3f));
}

void testReparent() {
    ska::Document doc;
    doc.bones.push_back(bone("a", -1, 50, pose(10, 10, 15)));
    doc.bones.push_back(bone("b", 0, 40, pose(50, 0, 30)));
    doc.bones.push_back(bone("c", 1, 30, pose(40, 0, -20)));
    doc.bones.push_back(bone("d", -1, 60, pose(200, 50, 90, 1.5f, 1.5f)));
    ska::Clip clip;
    clip.keys.push_back(key(0, {pose(0, 0, 5), pose(50, 0, 45), pose(40, 0, 10), pose(210, 40, 80)}));
    clip.keys.push_back(key(5, {pose(5, 5, -5), pose(50, 0, -30), pose(40, 0, 60), pose(190, 60, 120)}));
    doc.clips.push_back(clip);

    std::vector<int> map;
    CHECK(!skel::reparentBone(doc, 0, 2, map));  // cycle: c descends from a
    CHECK(!skel::reparentBone(doc, 1, 2, map));  // cycle: c is b's child
    CHECK(!skel::reparentBone(doc, 1, 1, map));  // self
    CHECK(!skel::reparentBone(doc, 1, 0, map));  // unchanged

    const ska::Document before = doc;
    CHECK(skel::reparentBone(doc, 1, 3, map));
    CHECK(parentsValid(doc));
    CHECK((map == std::vector<int>{0, 2, 3, 1}));
    CHECK(doc.bones[0].name == "a" && doc.bones[1].name == "d" && doc.bones[2].name == "b" &&
          doc.bones[3].name == "c");
    CHECK(doc.bones[2].parent == 1 && doc.bones[3].parent == 2);

    auto sameWorld = [&](const std::vector<Pose>& oldLocals, const std::vector<Pose>& newLocals) {
        const std::vector<Affine> wo = world(before, oldLocals), wn = world(doc, newLocals);
        bool ok = true;
        for (size_t i = 0; i < wo.size(); ++i) {
            const size_t j = size_t(map[i]);
            const float l = before.bones[i].length;
            ok = ok && near(headOf(wo[i]), headOf(wn[j]), 1e-3f) && near(tailOf(wo[i], l), tailOf(wn[j], l), 1e-3f);
        }
        return ok;
    };
    std::vector<Pose> ro, rn;
    ska::restPose(before, ro);
    ska::restPose(doc, rn);
    CHECK(sameWorld(ro, rn));
    for (size_t k = 0; k < doc.clips[0].keys.size(); ++k)
        CHECK(sameWorld(before.clips[0].keys[k].poses, doc.clips[0].keys[k].poses));
    // Bones whose parent did not change carry their poses unchanged to the new index.
    CHECK(near(doc.clips[0].keys[1].poses[3].rotation, before.clips[0].keys[1].poses[2].rotation, 1e-6f));
    CHECK(near(doc.clips[0].keys[1].poses[1].translation, before.clips[0].keys[1].poses[3].translation, 1e-6f));

    // Reparent to root.
    CHECK(skel::reparentBone(doc, 3, -1, map));
    CHECK(parentsValid(doc));
}

ska::Document twoChain(float bendDeg) {
    ska::Document doc;
    doc.bones.push_back(bone("upper", -1, 100, pose(0, 0, 0)));
    doc.bones.push_back(bone("lower", 0, 100, pose(100, 0, bendDeg)));
    return doc;
}

void testIK() {
    for (float bend : {30.0f, -30.0f}) {
        ska::Document doc = twoChain(bend);
        std::vector<Pose> locals;
        ska::restPose(doc, locals);
        std::vector<Affine> w = world(doc, locals);
        const float signBefore = cross(headOf(w[0]), headOf(w[1]), tailOf(w[1], 100));

        const Vec2 target{120, 80};
        skel::solveIK2(doc, locals, 1, target);
        w = world(doc, locals);
        const Vec2 tail = tailOf(w[1], 100);
        CHECK(near(tail, target, 0.5f));
        const float signAfter = cross(headOf(w[0]), headOf(w[1]), tail);
        CHECK(signBefore * signAfter > 0);
    }

    // Unreachable: chain points straight at the target.
    ska::Document doc = twoChain(30);
    std::vector<Pose> locals;
    ska::restPose(doc, locals);
    const Vec2 target{300, 400};
    skel::solveIK2(doc, locals, 1, target);
    const std::vector<Affine> w = world(doc, locals);
    const float want = std::atan2(target.y, target.x);
    const Vec2 b = headOf(w[1]), t = tailOf(w[1], 100);
    CHECK(near(std::atan2(b.y, b.x), want, 0.01f));
    CHECK(near(std::atan2(t.y - b.y, t.x - b.x), want, 0.01f));

    // Root bone: aims at the target.
    ska::Document root = twoChain(0);
    ska::restPose(root, locals);
    skel::solveIK2(root, locals, 0, {0, 50});
    CHECK(near(locals[0].rotation, kPi / 2, 1e-4f));
}

void testManipulators() {
    ska::Document doc = twoChain(0);
    doc.bones[0].rest = pose(0, 0, 90, 2, 2);
    std::vector<Pose> locals;
    ska::restPose(doc, locals);

    skel::dragRotate(doc, locals, 0, {10, 0}, {0, 10});
    CHECK(near(std::fabs(locals[0].rotation), kPi, 1e-4f));

    ska::restPose(doc, locals);
    skel::dragTranslate(doc, locals, 1, {0, 20});  // parent: rot 90, scale 2 → local (+10, 0)
    CHECK(near(locals[1].translation, {110, 0}, 1e-4f));
    skel::dragTranslate(doc, locals, 0, {3, 4});
    CHECK(near(locals[0].translation, {3, 4}, 1e-6f));

    ska::restPose(doc, locals);
    float length = 0;
    skel::dragStretchLength(doc, locals, 0, {7, 60}, length);  // world col0 length 2
    CHECK(near(length, 30, 1e-4f));
    skel::dragStretchLength(doc, locals, 0, {0, -60}, length);
    CHECK(length == 1);

    skel::dragStretchScale(doc, locals, 0, {0, 100});  // tail at (0,200)
    CHECK(near(locals[0].scale.x, 1, 1e-4f));
    skel::dragStretchScale(doc, locals, 0, {0, -10});
    CHECK(near(locals[0].scale.x, skel::kMinScale, 1e-6f));
}

void testKeys() {
    ska::Document doc = twoChain(0);
    ska::Clip clip;
    CHECK(skel::ensureKey(doc, clip, 3) == 0);
    CHECK(near(clip.keys[0].poses[1].translation, {100, 0}, 1e-6f));
    clip.keys.clear();

    clip.keys.push_back(key(0, {pose(0, 0, 0), pose(100, 0, 0)}));
    clip.keys.push_back(key(10, {pose(10, 20, 90), pose(100, 0, -40, 2, 1)}));
    std::vector<Pose> expected;
    ska::sampleFrame(doc, clip, 5, expected);

    CHECK(skel::ensureKey(doc, clip, 10) == 1);
    CHECK(clip.keys.size() == 2);
    const int k = skel::ensureKey(doc, clip, 5);
    CHECK(k == 1);
    CHECK(clip.keys.size() == 3);
    CHECK(clip.keys[0].frame == 0 && clip.keys[1].frame == 5 && clip.keys[2].frame == 10);
    CHECK(clip.keys[1].interp == ska::Interp::Linear);
    for (size_t i = 0; i < expected.size(); ++i) {
        const Pose& p = clip.keys[1].poses[i];
        CHECK(near(p.translation, expected[i].translation, 1e-6f));
        CHECK(near(p.rotation, expected[i].rotation, 1e-6f));
        CHECK(near(p.scale, expected[i].scale, 1e-6f));
    }
    CHECK(near(clip.keys[1].poses[0].rotation, kPi / 4, 1e-5f));
    CHECK(skel::ensureKey(doc, clip, 20) == 3);

    CHECK(skel::findKey(clip, 5) == 1);
    CHECK(skel::findKey(clip, 6) == -1);
    CHECK(skel::prevKeyFrame(clip, 5) == 0);
    CHECK(skel::prevKeyFrame(clip, 0) == -1);
    CHECK(skel::prevKeyFrame(clip, 7) == 5);
    CHECK(skel::nextKeyFrame(clip, 5) == 10);
    CHECK(skel::nextKeyFrame(clip, 20) == -1);
    CHECK(skel::nextKeyFrame(clip, -3) == 0);
    CHECK(skel::deleteKey(clip, 5));
    CHECK(!skel::deleteKey(clip, 5));
    CHECK(clip.keys.size() == 3);
}

bool same(const ska::Document& a, const ska::Document& b) { return ska::write(a) == ska::write(b); }

void testHistory() {
    History h;
    ska::Document doc = twoChain(10);
    doc.clips.push_back({});
    CHECK(!h.canUndo() && !h.canRedo());

    h.begin(doc);
    h.end(false);
    CHECK(!h.canUndo());

    const ska::Document v0 = doc;
    h.push(doc);
    skel::addBone(doc, 1, {0, 0}, {50, 50});
    const ska::Document v1 = doc;

    h.begin(doc);
    doc.bones[0].rest.rotation = 1.25f;
    h.begin(doc);  // ignored: already pending
    doc.bones[0].rest.rotation = 2.5f;
    h.end(true);
    const ska::Document v2 = doc;

    CHECK(h.undo(doc));
    CHECK(same(doc, v1));
    CHECK(h.undo(doc));
    CHECK(same(doc, v0));
    CHECK(!h.undo(doc));
    CHECK(h.redo(doc));
    CHECK(same(doc, v1));
    CHECK(h.redo(doc));
    CHECK(same(doc, v2));
    CHECK(!h.redo(doc));

    CHECK(h.undo(doc));
    h.push(doc);  // new edit clears redo
    CHECK(!h.canRedo());

    h.clear();
    for (int i = 0; i < 250; ++i) h.push(doc);
    int undos = 0;
    while (h.undo(doc)) ++undos;
    CHECK(undos == 200);
}

// World direction (degrees) of a named bone at a frame of a clip.
float worldDeg(const ska::Document& doc, const ska::Clip& clip, int frame, const char* bone) {
    std::vector<Pose> local;
    ska::sampleFrame(doc, clip, float(frame), local);
    std::vector<Affine> world;
    ska::computeWorld(doc, local, world);
    const Affine& w = world[size_t(skel::findBone(doc, bone))];
    return std::atan2(w.b, w.a) * 180.0f / kPi;
}

void testDancingQueen() {
    const Vec2 canvas{1280, 720};
    ska::Document doc;
    skel::addExampleFigure(doc, canvas);
    CHECK(doc.bones.size() == 12);
    CHECK(skel::findBone(doc, "shin_r") == 11 && doc.bones[11].parent == skel::findBone(doc, "thigh_r"));

    const int ci = skel::addDancingQueenClip(doc, canvas);
    CHECK(ci == 0 && doc.clips.size() == 1);
    const ska::Clip& clip = doc.clips[0];
    CHECK(clip.name == "dancing_queen" && clip.fpsNum == 30 && clip.fpsDen == 1 && clip.frameCount == 150);
    CHECK(clip.keys.size() == 14);
    for (size_t k = 0; k < clip.keys.size(); ++k) {
        CHECK(clip.keys[k].poses.size() == doc.bones.size());
        if (k > 0) CHECK(clip.keys[k].frame > clip.keys[k - 1].frame);
    }
    CHECK(clip.keys.back().frame == 149);

    // The choreography lands: disco point up-right, then down-left; Y arms up and out.
    CHECK(near(worldDeg(doc, clip, 0, "upper_arm_r"), -45, 0.01f));
    CHECK(near(worldDeg(doc, clip, 0, "forearm_l"), 20, 0.01f));
    CHECK(near(worldDeg(doc, clip, 15, "forearm_r"), 130, 0.01f));
    CHECK(near(worldDeg(doc, clip, 72, "upper_arm_l"), -125, 0.01f));
    CHECK(near(worldDeg(doc, clip, 72, "upper_arm_r"), -55, 0.01f));
    // Hip sway moves the root; unlisted bones keep their rest direction (pelvis points up).
    CHECK(clip.keys[0].poses[0].translation.x > doc.bones[0].rest.translation.x);
    CHECK(clip.keys[1].poses[0].translation.x < doc.bones[0].rest.translation.x);
    CHECK(near(worldDeg(doc, clip, 0, "pelvis"), -90, 0.01f));
    // Loops: last key repeats the first pose.
    for (size_t b = 0; b < doc.bones.size(); ++b)
        CHECK(near(clip.keys.back().poses[b].rotation, clip.keys.front().poses[b].rotation, 1e-6f));

    // Valid .ska content.
    ska::Document back;
    std::string err;
    const std::vector<uint8_t> bytes = ska::write(doc);
    CHECK(ska::read(bytes.data(), bytes.size(), back, &err));
}

}  // namespace

int main() {
    testAddBone();
    testDeleteBone();
    testReparent();
    testIK();
    testManipulators();
    testKeys();
    testHistory();
    testDancingQueen();
    std::printf("core_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
