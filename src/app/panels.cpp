#include "app/panels.h"

#include "app/app.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

constexpr float kRadToDeg = 57.29577951308232f;
constexpr ImGuiSliderFlags kClamp = ImGuiSliderFlags_AlwaysClamp;

// ---------------------------------------------------------------------------
// Hierarchy

struct Reparent {
    int src = -1, dst = -2;
};

void acceptBoneDrop(Reparent& r, int dst) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SKA_BONE")) {
        r.src = *static_cast<const int*>(p->Data);
        r.dst = dst;
    }
    ImGui::EndDragDropTarget();
}

void boneNode(App& app, int i, const std::vector<std::vector<int>>& children, Reparent& r) {
    const ska::Bone& bone = app.doc.bones[size_t(i)];
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (app.st.selectedBone == i) flags |= ImGuiTreeNodeFlags_Selected;
    if (children[size_t(i)].empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(intptr_t(i)), flags, "%s", bone.name.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) app.st.selectedBone = i;
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("SKA_BONE", &i, sizeof i);
        ImGui::TextUnformatted(bone.name.c_str());
        ImGui::EndDragDropSource();
    }
    acceptBoneDrop(r, i);
    if (!open) return;
    for (int c : children[size_t(i)]) boneNode(app, c, children, r);
    ImGui::TreePop();
}

// ---------------------------------------------------------------------------
// Properties helpers

bool dragDegrees(const char* label, float& radians) {
    float deg = radians * kRadToDeg;
    if (!ImGui::DragFloat(label, &deg, 0.5f, -360.0f, 360.0f, "%.2f\xC2\xB0")) return false;
    radians = ska::wrapPi(deg / kRadToDeg);
    return true;
}

// Edits translation/rotation/scale of a pose. Undo tracking happens per widget, before the
// caller applies the change.
bool poseWidgets(App& app, ska::Pose& p) {
    bool changed = false;
    ska::Pose edit = p;
    if (ImGui::DragFloat2("Translation", &edit.translation.x, 0.5f)) changed = true;
    app.trackItemEdit();
    if (dragDegrees("Rotation", edit.rotation)) changed = true;
    app.trackItemEdit();
    if (ImGui::DragFloat2("Scale", &edit.scale.x, 0.01f, skel::kMinScale, 20.0f, "%.3f", kClamp)) changed = true;
    app.trackItemEdit();
    if (changed) {
        edit.scale.x = std::max(edit.scale.x, skel::kMinScale);
        edit.scale.y = std::max(edit.scale.y, skel::kMinScale);
        p = edit;
    }
    return changed;
}

// The color picker spans several items (and a popup), so the edit is opened on the first change
// and closed by App's end-of-frame flushEdit once nothing is active.
void colorWidget(App& app, uint32_t& color) {
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
    if (!ImGui::ColorEdit4("Color", &c.x)) return;
    if (!app.history.pending()) app.beginEdit();
    color = ImGui::ColorConvertFloat4ToU32(c);
}

void setupBoneProperties(App& app, int b) {
    ska::Bone& bone = app.doc.bones[size_t(b)];

    std::string name = bone.name;
    const bool nameChanged = ImGui::InputText("Name", &name);
    app.markUi(UiTarget::BoneName);
    app.trackItemEdit();
    if (nameChanged) app.doc.bones[size_t(b)].name = name;

    const ska::Bone& b2 = app.doc.bones[size_t(b)];
    ImGui::LabelText("Parent", "%s", b2.parent < 0 ? "(root)" : app.doc.bones[size_t(b2.parent)].name.c_str());

    float length = b2.length;
    const bool lenChanged = ImGui::DragFloat("Length", &length, 0.5f, 1.0f, 1e6f, "%.2f", kClamp);
    app.trackItemEdit();
    if (lenChanged) app.doc.bones[size_t(b)].length = std::max(1.0f, length);

    ImGui::SeparatorText("Rest pose");
    ska::Pose rest = app.doc.bones[size_t(b)].rest;
    if (poseWidgets(app, rest)) app.doc.bones[size_t(b)].rest = rest;

    uint32_t color = app.doc.bones[size_t(b)].color;
    colorWidget(app, color);
    app.doc.bones[size_t(b)].color = color;
}

void animateBoneProperties(App& app, int b) {
    ska::Clip& clip = app.clip();
    ImGui::LabelText("Name", "%s", app.doc.bones[size_t(b)].name.c_str());
    const bool keyed = skel::findKey(clip, app.st.frame) >= 0;
    ImGui::LabelText("Frame", "%d (%s)", app.st.frame, keyed ? "keyed" : "interpolated");

    std::vector<ska::Pose> pose;
    ska::sampleFrame(app.doc, clip, float(app.st.frame), pose);
    ska::Pose p = pose[size_t(b)];
    ImGui::SeparatorText("Pose");
    if (poseWidgets(app, p)) {
        const int k = skel::ensureKey(app.doc, clip, app.st.frame);
        clip.keys[size_t(k)].poses[size_t(b)] = p;
    }
}

// ---------------------------------------------------------------------------
// Timeline helpers

void clipRow(App& app) {
    ska::Document& doc = app.doc;
    EditorState& st = app.st;

    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("##clip", app.clip().name.c_str())) {
        int chosen = -1;
        for (int i = 0; i < int(doc.clips.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(doc.clips[size_t(i)].name.c_str(), i == st.clip) && i != st.clip) chosen = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
        if (chosen >= 0) app.selectClip(chosen);
    }
    ImGui::SameLine();
    if (ImGui::Button("+")) {
        app.pushUndo();
        ska::Clip c;
        c.name = "clip" + std::to_string(doc.clips.size());
        doc.clips.push_back(c);
        app.selectClip(int(doc.clips.size()) - 1);
    }
    ImGui::SameLine();
    static std::string renameBuf;
    if (ImGui::Button("Rename")) {
        renameBuf = app.clip().name;
        ImGui::OpenPopup("Rename clip");
    }
    if (ImGui::BeginPopup("Rename clip")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", &renameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("OK") || enter) {
            if (!renameBuf.empty() && renameBuf != app.clip().name) {
                app.pushUndo();
                app.clip().name = renameBuf;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(doc.clips.size() <= 1);
    if (ImGui::Button("Delete")) {
        app.pushUndo();
        doc.clips.erase(doc.clips.begin() + st.clip);
        app.selectClip(std::min(st.clip, int(doc.clips.size()) - 1));
    }
    ImGui::EndDisabled();

    const ska::Clip& c = app.clip();
    ImGui::SameLine(0, 20);
    ImGui::Text("fps %u/%u (%.2f)", c.fpsNum, c.fpsDen, double(c.fpsNum) / double(c.fpsDen));

    ImGui::SameLine(0, 20);
    ImGui::BeginDisabled(app.video.isOpen());
    int frames = int(c.frameCount);
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("Frames", &frames, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) {
        frames = std::max(frames, std::max(1, app.lastKeyFrame() + 1));
        if (uint32_t(frames) != c.frameCount) {
            app.pushUndo();
            app.clip().frameCount = uint32_t(frames);
            app.setFrame(st.frame);
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0, 20);
    if (c.videoPath.empty())
        ImGui::TextDisabled("No video");
    else if (!app.videoError.empty())
        ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s", app.videoError.c_str());
    else
        ImGui::TextUnformatted(c.videoPath.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Import Video...")) app.importVideo();
    app.markUi(UiTarget::ImportVideo);
    if (!c.videoPath.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Relink...")) app.importVideo();
    }
}

void transportRow(App& app) {
    EditorState& st = app.st;
    ska::Clip& clip = app.clip();
    auto jump = [&](int f) {
        st.playing = false;
        if (f >= 0) app.setFrame(f);
    };
    if (ImGui::Button("|<")) jump(0);
    ImGui::SameLine();
    if (ImGui::Button("<K")) jump(skel::prevKeyFrame(clip, st.frame));
    ImGui::SameLine();
    if (ImGui::Button("<")) jump(st.frame - 1);
    ImGui::SameLine();
    if (ImGui::Button(st.playing ? "Pause" : "Play", ImVec2(60, 0))) st.playing = !st.playing;
    app.markUi(UiTarget::Play);
    ImGui::SameLine();
    if (ImGui::Button(">")) jump(st.frame + 1);
    ImGui::SameLine();
    if (ImGui::Button("K>")) jump(skel::nextKeyFrame(clip, st.frame));
    ImGui::SameLine();
    if (ImGui::Button(">|")) jump(int(clip.frameCount) - 1);
    ImGui::SameLine(0, 20);
    ImGui::Text("Frame %d / %u", st.frame, clip.frameCount);

    const int key = skel::findKey(clip, st.frame);
    if (st.mode == Mode::Animate) {
        ImGui::SameLine(0, 20);
        ImGui::BeginDisabled(key >= 0);
        if (ImGui::Button("Set Key")) {
            app.pushUndo();
            skel::ensureKey(app.doc, app.clip(), st.frame);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(key < 0);
        if (ImGui::Button("Delete Key")) {
            app.pushUndo();
            skel::deleteKey(app.clip(), st.frame);
        }
        ImGui::EndDisabled();
    }
    if (key >= 0) {
        ImGui::SameLine(0, 20);
        int interp = int(clip.keys[size_t(key)].interp);
        ImGui::SetNextItemWidth(90);
        if (ImGui::Combo("Interp", &interp, "Linear\0Step\0") && interp != int(clip.keys[size_t(key)].interp)) {
            app.pushUndo();
            app.clip().keys[size_t(key)].interp = ska::Interp(interp);
        }
    }

    ImGui::SameLine(0, 20);
    if (ImGui::RadioButton("Setup", st.mode == Mode::Setup)) app.setMode(Mode::Setup);
    app.markUi(UiTarget::ModeSetup);
    ImGui::SameLine();
    if (ImGui::RadioButton("Animate", st.mode == Mode::Animate)) app.setMode(Mode::Animate);
    app.markUi(UiTarget::ModeAnimate);
}

void trackRow(App& app) {
    EditorState& st = app.st;
    const ska::Clip& clip = app.clip();
    const int fc = int(clip.frameCount);

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = std::max(ImGui::GetContentRegionAvail().x, 20.0f);
    const float h = 40, pad = 10, usable = w - 2 * pad;
    ImGui::InvisibleButton("track", ImVec2(w, h));
    app.markUi(UiTarget::Track);
    auto xOf = [&](int f) { return p0.x + pad + (fc > 1 ? float(f) * usable / float(fc - 1) : usable * 0.5f); };

    if (ImGui::IsItemActive()) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - p0.x - pad) / usable, 0.0f, 1.0f);
        const int f = int(std::lround(t * float(fc - 1)));
        st.playing = false;
        if (f != st.frame) app.setFrame(f);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(30, 30, 30, 255), 3);
    const int tick = std::max(1, int(std::lround(fc / 20.0)));
    for (int f = 0; f < fc; f += tick) {
        const float x = xOf(f);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + 8), IM_COL32(120, 120, 120, 255));
        char label[16];
        std::snprintf(label, sizeof label, "%d", f);
        dl->AddText(ImVec2(x + 2, p0.y + 1), IM_COL32(130, 130, 130, 255), label);
    }
    const float ky = p0.y + h * 0.65f;
    for (const ska::Key& k : clip.keys) {
        const float x = xOf(k.frame);
        const ImVec2 pts[4] = {ImVec2(x, ky - 6), ImVec2(x + 6, ky), ImVec2(x, ky + 6), ImVec2(x - 6, ky)};
        dl->AddConvexPolyFilled(pts, 4,
                                k.interp == ska::Interp::Step ? IM_COL32(255, 160, 0, 255) : IM_COL32(255, 220, 0, 255));
        dl->AddPolyline(pts, 4, IM_COL32_BLACK, ImDrawFlags_Closed, 1.0f);
    }
    const float cx = xOf(st.frame);
    dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p0.y + h), IM_COL32(255, 60, 60, 255), 2.0f);
}

}  // namespace

void drawHierarchy(App& app) {
    const ska::Document& doc = app.doc;
    Reparent r;
    ImGui::Selectable("(root)", false);
    acceptBoneDrop(r, -1);

    std::vector<std::vector<int>> children(doc.bones.size());
    std::vector<int> roots;
    for (int i = 0; i < int(doc.bones.size()); ++i) {
        const int p = doc.bones[size_t(i)].parent;
        (p < 0 ? roots : children[size_t(p)]).push_back(i);
    }
    for (int i : roots) boneNode(app, i, children, r);
    if (doc.bones.empty()) ImGui::TextDisabled("Setup mode: press B and drag to draw bones.");

    if (r.src >= 0) {
        app.flushEdit();
        const ska::Document before = app.doc;
        std::vector<int> map;
        if (skel::reparentBone(app.doc, r.src, r.dst, map)) {
            app.history.push(before);
            app.st.dirty = true;
            if (app.st.selectedBone >= 0) app.st.selectedBone = map[size_t(app.st.selectedBone)];
        }
    }
}

void drawProperties(App& app) {
    ImGui::PushItemWidth(-110);
    ImGui::SeparatorText("Document");
    std::string name = app.doc.name;
    const bool nameChanged = ImGui::InputText("Doc name", &name);
    app.trackItemEdit();
    if (nameChanged) app.doc.name = name;
    float ppu = app.doc.pixelsPerUnit;
    const bool ppuChanged = ImGui::DragFloat("Pixels/unit", &ppu, 0.5f, 0.01f, 1e6f, "%.2f", kClamp);
    app.trackItemEdit();
    if (ppuChanged) app.doc.pixelsPerUnit = std::max(0.01f, ppu);

    const int b = app.st.selectedBone;
    ImGui::SeparatorText("Bone");
    if (b < 0 || b >= int(app.doc.bones.size())) {
        ImGui::TextDisabled("No bone selected");
    } else {
        ImGui::PushID(b);
        if (app.st.mode == Mode::Setup)
            setupBoneProperties(app, b);
        else
            animateBoneProperties(app, b);
        ImGui::PopID();
    }
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::SeparatorText("Mouse");
    ImGui::TextDisabled("Body drag: rotate   Head drag: move");
    ImGui::TextDisabled("Tail + Alt: 2-bone IK   Tail + " SKELANIM_MOD ": stretch");
    ImGui::TextDisabled("Wheel: zoom   Middle / Space+drag: pan");
}

void drawTimeline(App& app) {
    clipRow(app);
    transportRow(app);
    trackRow(app);
}
