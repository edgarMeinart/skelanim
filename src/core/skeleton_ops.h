// Skeleton and keyframe editing operations. The invariants `parent < index` and
// `key.poses.size() == bones.size()` are maintained only by these functions.
#pragma once

#include <ska/ska.h>

#include <vector>

namespace skel {

constexpr float kMinScale = 0.05f;

// Appends a bone under `parent` (-1 = root) whose rest pose spans headWorld→tailWorld.
// Returns the new index, or -1 if the bone would be shorter than 2 px.
int addBone(ska::Document& doc, int parent, ska::Vec2 headWorld, ska::Vec2 tailWorld);

// Removes a bone; its direct children move to its parent keeping their world transforms.
void deleteBone(ska::Document& doc, int index);

// Moves `bone` under `newParent` keeping world transforms, then reorders bones topologically.
// Returns false for self/descendant/unchanged parents. oldToNew maps old to new bone indices.
bool reparentBone(ska::Document& doc, int bone, int newParent, std::vector<int>& oldToNew);

// Index of the key at `frame`, or -1.
int findKey(const ska::Clip& clip, int frame);

// Index of the key at `frame`, inserting one with the sampled pose if none exists.
int ensureKey(const ska::Document& doc, ska::Clip& clip, int frame);

// Removes the key at `frame`; false if none.
bool deleteKey(ska::Clip& clip, int frame);

// Nearest key frame strictly before / after `frame`, or -1.
int prevKeyFrame(const ska::Clip& clip, int frame);
int nextKeyFrame(const ska::Clip& clip, int frame);

// Manipulators below act on `locals` (rest pose or a key's poses).

// Rotates `bone` about its head by the angle the mouse swept from prevMouse to mouse.
void dragRotate(const ska::Document& doc, std::vector<ska::Pose>& locals, int bone, ska::Vec2 prevMouse,
                ska::Vec2 mouse);

// Moves `bone` by a world-space delta.
void dragTranslate(const ska::Document& doc, std::vector<ska::Pose>& locals, int bone, ska::Vec2 worldDelta);

// Setup mode: sets `length` so the tail follows the mouse projected on the bone axis (min 1).
void dragStretchLength(const ska::Document& doc, const std::vector<ska::Pose>& locals, int bone, ska::Vec2 mouse,
                       float& length);

// Animate mode: scales the bone's local x so the tail follows the mouse projected on the bone axis.
void dragStretchScale(const ska::Document& doc, std::vector<ska::Pose>& locals, int bone, ska::Vec2 mouse);

// Two-bone IK: bends `bone` and its parent so the tail reaches `target`, preserving the bend side.
// Roots just aim at the target.
void solveIK2(const ska::Document& doc, std::vector<ska::Pose>& locals, int bone, ska::Vec2 target);

}  // namespace skel
