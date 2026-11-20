#include "core/skeleton_ops.h"

#include <algorithm>
#include <cmath>

namespace skel {
namespace {

using ska::Affine;
using ska::Pose;
using ska::Vec2;

constexpr uint32_t kPalette[8] = {0xFF3399FFu, 0xFF33CC66u, 0xFFFF9933u, 0xFFCC66FFu,
                                  0xFF33CCCCu, 0xFFFF6666u, 0xFF99FF33u, 0xFFFFCC33u};

Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
float len(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
float angle(Vec2 v) { return std::atan2(v.y, v.x); }
Vec2 head(const Affine& m) { return {m.tx, m.ty}; }

std::vector<Affine> worldOf(const ska::Document& doc, const std::vector<Pose>& locals) {
    std::vector<Affine> world;
    ska::computeWorld(doc, locals, world);
    return world;
}

// Local pose of a bone with world transform `world` under parent world transform (or root if null).
Pose localUnder(const Affine* parentWorld, const Affine& world) {
    return ska::decompose(parentWorld ? ska::mul(ska::inverse(*parentWorld), world) : world);
}

// Applies fn(locals, isRest) to the rest pose and to every key's poses; rest is written back.
template <class Fn>
void forEachPoseSet(ska::Document& doc, Fn&& fn) {
    for (ska::Clip& clip : doc.clips)
        for (ska::Key& key : clip.keys) fn(key.poses);
    std::vector<Pose> rest;
    ska::restPose(doc, rest);
    fn(rest);
    for (size_t i = 0; i < doc.bones.size(); ++i) doc.bones[i].rest = rest[i];
}

}  // namespace

int addBone(ska::Document& doc, int parent, Vec2 headWorld, Vec2 tailWorld) {
    const Vec2 d = sub(tailWorld, headWorld);
    const float dist = len(d);
    if (dist < 2) return -1;
    if (parent < -1 || parent >= int(doc.bones.size())) parent = -1;

    Pose worldPose;
    worldPose.translation = headWorld;
    worldPose.rotation = angle(d);
    const Affine worldAff = ska::toAffine(worldPose);

    Pose local;
    Affine w = worldAff;
    if (parent >= 0) {
        std::vector<Pose> rest;
        ska::restPose(doc, rest);
        const Affine parentWorld = worldOf(doc, rest)[size_t(parent)];
        local = localUnder(&parentWorld, worldAff);
        local.scale = {1, 1};
        w = ska::mul(parentWorld, ska::toAffine(local));
    } else {
        local = worldPose;
    }

    const size_t n = doc.bones.size();
    ska::Bone bone;
    bone.name = "bone_" + std::to_string(n);
    bone.parent = parent;
    const float colLen = len({w.a, w.b});
    bone.length = colLen > 0 ? dist / colLen : dist;
    bone.rest = local;
    bone.color = kPalette[n % 8];
    doc.bones.push_back(bone);
    for (ska::Clip& clip : doc.clips)
        for (ska::Key& key : clip.keys) key.poses.push_back(local);
    return int(n);
}

void deleteBone(ska::Document& doc, int index) {
    if (index < 0 || index >= int(doc.bones.size())) return;
    const int grand = doc.bones[size_t(index)].parent;

    forEachPoseSet(doc, [&](std::vector<Pose>& locals) {
        const std::vector<Affine> world = worldOf(doc, locals);
        for (size_t c = 0; c < doc.bones.size(); ++c)
            if (doc.bones[c].parent == index)
                locals[c] = localUnder(grand >= 0 ? &world[size_t(grand)] : nullptr, world[c]);
    });

    for (ska::Bone& b : doc.bones)
        if (b.parent == index) b.parent = grand;
    doc.bones.erase(doc.bones.begin() + index);
    for (ska::Bone& b : doc.bones)
        if (b.parent > index) --b.parent;
    for (ska::Clip& clip : doc.clips)
        for (ska::Key& key : clip.keys) key.poses.erase(key.poses.begin() + index);
}

bool reparentBone(ska::Document& doc, int bone, int newParent, std::vector<int>& oldToNew) {
    const int n = int(doc.bones.size());
    if (bone < 0 || bone >= n || newParent < -1 || newParent >= n) return false;
    if (newParent == bone || doc.bones[size_t(bone)].parent == newParent) return false;
    for (int p = newParent; p >= 0; p = doc.bones[size_t(p)].parent)
        if (p == bone) return false;

    forEachPoseSet(doc, [&](std::vector<Pose>& locals) {
        const std::vector<Affine> world = worldOf(doc, locals);
        locals[size_t(bone)] =
            localUnder(newParent >= 0 ? &world[size_t(newParent)] : nullptr, world[size_t(bone)]);
    });
    doc.bones[size_t(bone)].parent = newParent;

    // Stable DFS from roots, children in index order.
    std::vector<std::vector<int>> children(static_cast<size_t>(n));
    std::vector<int> order;
    order.reserve(size_t(n));
    for (int i = 0; i < n; ++i)
        if (doc.bones[size_t(i)].parent >= 0) children[size_t(doc.bones[size_t(i)].parent)].push_back(i);
    std::vector<int> stack;
    for (int r = n - 1; r >= 0; --r)
        if (doc.bones[size_t(r)].parent < 0) stack.push_back(r);
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        order.push_back(i);
        const std::vector<int>& ch = children[size_t(i)];
        for (auto it = ch.rbegin(); it != ch.rend(); ++it) stack.push_back(*it);
    }

    oldToNew.assign(size_t(n), -1);
    for (int k = 0; k < n; ++k) oldToNew[size_t(order[size_t(k)])] = k;

    std::vector<ska::Bone> bones(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) {
        bones[size_t(k)] = std::move(doc.bones[size_t(order[size_t(k)])]);
        if (bones[size_t(k)].parent >= 0) bones[size_t(k)].parent = oldToNew[size_t(bones[size_t(k)].parent)];
    }
    doc.bones = std::move(bones);
    for (ska::Clip& clip : doc.clips)
        for (ska::Key& key : clip.keys) {
            std::vector<Pose> poses(static_cast<size_t>(n));
            for (int k = 0; k < n; ++k) poses[size_t(k)] = key.poses[size_t(order[size_t(k)])];
            key.poses = std::move(poses);
        }
    return true;
}

int findKey(const ska::Clip& clip, int frame) {
    auto it = std::lower_bound(clip.keys.begin(), clip.keys.end(), frame,
                               [](const ska::Key& k, int f) { return k.frame < f; });
    return it != clip.keys.end() && it->frame == frame ? int(it - clip.keys.begin()) : -1;
}

int ensureKey(const ska::Document& doc, ska::Clip& clip, int frame) {
    auto it = std::lower_bound(clip.keys.begin(), clip.keys.end(), frame,
                               [](const ska::Key& k, int f) { return k.frame < f; });
    if (it != clip.keys.end() && it->frame == frame) return int(it - clip.keys.begin());
    ska::Key key;
    key.frame = frame;
    key.interp = ska::Interp::Linear;
    ska::sampleFrame(doc, clip, float(frame), key.poses);
    return int(clip.keys.insert(it, std::move(key)) - clip.keys.begin());
}

bool deleteKey(ska::Clip& clip, int frame) {
    const int k = findKey(clip, frame);
    if (k < 0) return false;
    clip.keys.erase(clip.keys.begin() + k);
    return true;
}

int prevKeyFrame(const ska::Clip& clip, int frame) {
    auto it = std::lower_bound(clip.keys.begin(), clip.keys.end(), frame,
                               [](const ska::Key& k, int f) { return k.frame < f; });
    return it == clip.keys.begin() ? -1 : (it - 1)->frame;
}

int nextKeyFrame(const ska::Clip& clip, int frame) {
    auto it = std::upper_bound(clip.keys.begin(), clip.keys.end(), frame,
                               [](int f, const ska::Key& k) { return f < k.frame; });
    return it == clip.keys.end() ? -1 : it->frame;
}

void dragRotate(const ska::Document& doc, std::vector<Pose>& locals, int bone, Vec2 prevMouse, Vec2 mouse) {
    const Vec2 h = head(worldOf(doc, locals)[size_t(bone)]);
    Pose& p = locals[size_t(bone)];
    p.rotation = ska::wrapPi(p.rotation + angle(sub(mouse, h)) - angle(sub(prevMouse, h)));
}

void dragTranslate(const ska::Document& doc, std::vector<Pose>& locals, int bone, Vec2 worldDelta) {
    const int parent = doc.bones[size_t(bone)].parent;
    Vec2 d = worldDelta;
    if (parent >= 0) {
        const Affine inv = ska::inverse(worldOf(doc, locals)[size_t(parent)]);
        d = {inv.a * worldDelta.x + inv.c * worldDelta.y, inv.b * worldDelta.x + inv.d * worldDelta.y};
    }
    locals[size_t(bone)].translation.x += d.x;
    locals[size_t(bone)].translation.y += d.y;
}

void dragStretchLength(const ska::Document& doc, const std::vector<Pose>& locals, int bone, Vec2 mouse,
                       float& length) {
    const Affine w = worldOf(doc, locals)[size_t(bone)];
    const float colLen = len({w.a, w.b});
    if (colLen <= 0) return;
    const float d = dot(sub(mouse, head(w)), {w.a / colLen, w.b / colLen});
    length = std::max(1.0f, d / colLen);
}

void dragStretchScale(const ska::Document& doc, std::vector<Pose>& locals, int bone, Vec2 mouse) {
    const Affine w = worldOf(doc, locals)[size_t(bone)];
    const Vec2 h = head(w);
    const Vec2 axis = sub(ska::apply(w, {doc.bones[size_t(bone)].length, 0}), h);
    const float curLen = len(axis);
    if (curLen <= 0) return;
    const float d = dot(sub(mouse, h), {axis.x / curLen, axis.y / curLen});
    Pose& p = locals[size_t(bone)];
    p.scale.x = std::clamp(p.scale.x * d / curLen, kMinScale, 20.0f);
}

void solveIK2(const ska::Document& doc, std::vector<Pose>& locals, int bone, Vec2 target) {
    const float boneLen = doc.bones[size_t(bone)].length;
    // Rotates `i` so its head→tail direction points at the target.
    auto aim = [&](int i, const std::vector<Affine>& world) {
        const Affine& w = world[size_t(i)];
        const Vec2 h = head(w);
        const Vec2 t = ska::apply(w, {boneLen, 0});
        Pose& p = locals[size_t(i)];
        p.rotation = ska::wrapPi(p.rotation + angle(sub(target, h)) - angle(sub(t, h)));
    };

    const int parent = doc.bones[size_t(bone)].parent;
    std::vector<Affine> world = worldOf(doc, locals);
    if (parent < 0) {
        aim(bone, world);
        return;
    }

    const Vec2 A = head(world[size_t(parent)]);
    const Vec2 B = head(world[size_t(bone)]);
    const Vec2 T = ska::apply(world[size_t(bone)], {boneLen, 0});
    const float a = len(sub(B, A)), b = len(sub(T, B));
    if (a > 1e-4f && b > 1e-4f) {
        const float lo = std::fabs(a - b) + 1e-3f, hi = a + b - 1e-3f;
        const float dist = std::max(lo, std::min(len(sub(target, A)), hi));
        const float cr = cross(sub(B, A), sub(T, B));
        const float bendSign = cr < 0 ? -1.0f : 1.0f;
        const float alpha = std::acos(std::clamp((a * a + dist * dist - b * b) / (2 * a * dist), -1.0f, 1.0f));
        const float desired = angle(sub(target, A)) - bendSign * alpha;
        Pose& pp = locals[size_t(parent)];
        pp.rotation = ska::wrapPi(pp.rotation + ska::wrapPi(desired - angle(sub(B, A))));
        world = worldOf(doc, locals);
    }
    aim(bone, world);
}

}  // namespace skel
