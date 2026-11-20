#include "app/viewport.h"

#include "app/app.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

enum class Part { None, Head, Tail, Body };
struct Hit {
    int bone = -1;
    Part part = Part::None;
};

constexpr float kHandleRadius = 7;  // head/tail pick radius (screen px)
constexpr float kBodyRadius = 6;
constexpr float kSnapRadius = 8;    // create tool snaps to the selected bone's tail

float dist(ImVec2 a, ImVec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

float segmentDist(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return dist(p, ImVec2(a.x + dx * t, a.y + dy * t));
}

uint32_t withAlpha(uint32_t col, uint32_t alpha) { return (col & 0x00FFFFFFu) | (alpha << IM_COL32_A_SHIFT); }

struct View {
    ImVec2 origin;
    ImVec2 pan;
    float zoom;
    ImVec2 toScreen(ska::Vec2 p) const { return ImVec2(origin.x + pan.x + p.x * zoom, origin.y + pan.y + p.y * zoom); }
    ska::Vec2 toWorld(ImVec2 s) const { return {(s.x - origin.x - pan.x) / zoom, (s.y - origin.y - pan.y) / zoom}; }
};

// Screen-space head/tail of each bone for a pose.
void boneEnds(const App& app, const View& v, const std::vector<ska::Pose>& locals, std::vector<ImVec2>& heads,
              std::vector<ImVec2>& tails) {
    std::vector<ska::Affine> world;
    ska::computeWorld(app.doc, locals, world);
    heads.resize(world.size());
    tails.resize(world.size());
    for (size_t i = 0; i < world.size(); ++i) {
        heads[i] = v.toScreen({world[i].tx, world[i].ty});
        tails[i] = v.toScreen(ska::apply(world[i], {app.doc.bones[i].length, 0}));
    }
}

Hit hitTest(ImVec2 m, const std::vector<ImVec2>& heads, const std::vector<ImVec2>& tails, int selected) {
    const int n = int(heads.size());
    if (selected >= 0 && selected < n && dist(m, tails[size_t(selected)]) <= kHandleRadius) return {selected, Part::Tail};
    auto test = [&](int i) -> Part {
        if (dist(m, heads[size_t(i)]) <= kHandleRadius) return Part::Head;
        if (segmentDist(m, heads[size_t(i)], tails[size_t(i)]) < kBodyRadius) return Part::Body;
        return Part::None;
    };
    if (selected >= 0 && selected < n)
        if (Part p = test(selected); p != Part::None) return {selected, p};
    for (int i = n - 1; i >= 0; --i) {
        if (i == selected) continue;
        if (Part p = test(i); p != Part::None) return {i, p};
    }
    return {};
}

void fitView(App& app, ImVec2 avail) {
    const ImVec2 c = app.canvasSize();
    app.st.zoom = std::clamp(std::min(avail.x / c.x, avail.y / c.y) * 0.95f, 0.05f, 20.0f);
    app.st.pan = ImVec2((avail.x - c.x * app.st.zoom) * 0.5f, (avail.y - c.y * app.st.zoom) * 0.5f);
}

// Applies one mouse-move step of the active bone drag.
void applyDrag(App& app, ska::Vec2 mouse) {
    EditorState& st = app.st;
    ska::Document& doc = app.doc;
    const int b = st.selectedBone;
    if (b < 0) return;

    std::vector<ska::Pose> rest;
    std::vector<ska::Pose>* locals;
    if (st.mode == Mode::Setup) {
        ska::restPose(doc, rest);
        locals = &rest;
    } else {
        if (st.dragKey < 0) st.dragKey = skel::ensureKey(doc, app.clip(), st.frame);  // auto-key
        locals = &app.clip().keys[size_t(st.dragKey)].poses;
    }

    switch (st.drag) {
        case DragKind::Rotate: skel::dragRotate(doc, *locals, b, st.dragPrevWorld, mouse); break;
        case DragKind::Translate:
            skel::dragTranslate(doc, *locals, b, {mouse.x - st.dragPrevWorld.x, mouse.y - st.dragPrevWorld.y});
            break;
        case DragKind::Stretch:
            if (st.mode == Mode::Setup)
                skel::dragStretchLength(doc, *locals, b, mouse, doc.bones[size_t(b)].length);
            else
                skel::dragStretchScale(doc, *locals, b, mouse);
            break;
        case DragKind::IK: skel::solveIK2(doc, *locals, b, mouse); break;
        default: break;
    }
    if (st.mode == Mode::Setup)
        for (size_t i = 0; i < doc.bones.size(); ++i) doc.bones[i].rest = rest[i];
    st.dragChanged = true;
}

void drawBone(ImDrawList* dl, ImVec2 head, ImVec2 tail, uint32_t color, bool selected) {
    const float len = dist(head, tail);
    if (len < 0.5f) {
        dl->AddCircleFilled(head, 5, withAlpha(color, 0xFF));
        return;
    }
    const ImVec2 dir((tail.x - head.x) / len, (tail.y - head.y) / len);
    const ImVec2 perp(-dir.y, dir.x);
    const float w = std::clamp(0.08f * len, 3.0f, 12.0f);
    const ImVec2 mid(head.x + dir.x * 0.15f * len, head.y + dir.y * 0.15f * len);
    const ImVec2 pts[4] = {head, ImVec2(mid.x + perp.x * w, mid.y + perp.y * w), tail,
                           ImVec2(mid.x - perp.x * w, mid.y - perp.y * w)};
    dl->AddConvexPolyFilled(pts, 4, withAlpha(color, 0x99));
    dl->AddPolyline(pts, 4, selected ? IM_COL32_WHITE : IM_COL32_BLACK, ImDrawFlags_Closed, selected ? 2.0f : 1.0f);
    dl->AddCircleFilled(head, 5, withAlpha(color, 0xFF));
    dl->AddCircle(head, 5, IM_COL32_BLACK);
}

void drawOnion(App& app, const View& v, ImDrawList* dl, int frame, uint32_t color) {
    const int k = skel::findKey(app.clip(), frame);
    if (k < 0) return;
    std::vector<ImVec2> heads, tails;
    boneEnds(app, v, app.clip().keys[size_t(k)].poses, heads, tails);
    for (size_t i = 0; i < heads.size(); ++i) dl->AddLine(heads[i], tails[i], color, 2.0f);
}

}  // namespace

void drawViewport(App& app) {
    EditorState& st = app.st;
    ImGuiIO& io = ImGui::GetIO();

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.x = std::max(avail.x, 1.0f);
    avail.y = std::max(avail.y, 1.0f);
    if (app.fitRequested) {
        fitView(app, avail);
        app.fitRequested = false;
    }

    ImGui::InvisibleButton("canvas", avail,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = io.MousePos;

    // --- Zoom / pan -------------------------------------------------------
    if (hovered && io.MouseWheel != 0) {
        const View v{origin, st.pan, st.zoom};
        const ska::Vec2 w = v.toWorld(mouse);
        st.zoom = std::clamp(st.zoom * std::pow(1.1f, io.MouseWheel), 0.05f, 20.0f);
        st.pan = ImVec2(mouse.x - origin.x - w.x * st.zoom, mouse.y - origin.y - w.y * st.zoom);
    }
    if (st.drag == DragKind::None && hovered) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            st.drag = DragKind::Pan;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsKeyDown(ImGuiKey_Space)) {
            st.drag = DragKind::Pan;
            st.spacePanned = true;
        }
    }
    if (st.drag == DragKind::Pan) {
        st.pan.x += io.MouseDelta.x;
        st.pan.y += io.MouseDelta.y;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
            st.drag = DragKind::None;
    }

    const View view{origin, st.pan, st.zoom};
    const ska::Vec2 mouseWorld = view.toWorld(mouse);
    std::vector<ska::Pose> pose;
    std::vector<ImVec2> heads, tails;
    app.displayedPose(pose);
    boneEnds(app, view, pose, heads, tails);
    if (st.selectedBone >= int(heads.size())) st.selectedBone = -1;

    // --- Left press --------------------------------------------------------
    if (st.drag == DragKind::None && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (st.mode == Mode::Setup && st.createTool) {
            ska::Vec2 start = mouseWorld;
            if (st.selectedBone >= 0 && dist(mouse, tails[size_t(st.selectedBone)]) <= kSnapRadius)
                start = view.toWorld(tails[size_t(st.selectedBone)]);
            st.dragStartWorld = start;
            st.drag = DragKind::Create;
        } else if (const Hit hit = hitTest(mouse, heads, tails, st.selectedBone); hit.bone >= 0) {
            st.selectedBone = hit.bone;
            app.beginEdit();
            st.dragPrevWorld = st.dragStartWorld = mouseWorld;
            st.dragChanged = false;
            st.dragKey = -1;
            st.playing = false;
            if (hit.part == Part::Head)
                st.drag = DragKind::Translate;
            else if (hit.part == Part::Tail && io.KeyAlt)
                st.drag = DragKind::IK;
            else if (hit.part == Part::Tail && io.KeyCtrl)
                st.drag = DragKind::Stretch;
            else
                st.drag = DragKind::Rotate;
        } else {
            st.selectedBone = -1;
        }
    }

    // --- Drag / release ------------------------------------------------------
    const bool boneDrag = st.drag == DragKind::Rotate || st.drag == DragKind::Translate ||
                          st.drag == DragKind::Stretch || st.drag == DragKind::IK;
    if (boneDrag) {
        if (mouseWorld.x != st.dragPrevWorld.x || mouseWorld.y != st.dragPrevWorld.y) {
            applyDrag(app, mouseWorld);
            st.dragPrevWorld = mouseWorld;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            app.endEdit(st.dragChanged);
            st.drag = DragKind::None;
            st.dragKey = -1;
        }
    } else if (st.drag == DragKind::Create && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        st.drag = DragKind::None;
        if (dist(view.toScreen(st.dragStartWorld), mouse) >= 4) {
            app.beginEdit();  // addBone rejects drags under 2 world px; record only real additions
            const int idx = skel::addBone(app.doc, st.selectedBone, st.dragStartWorld, mouseWorld);
            app.endEdit(idx >= 0);
            if (idx >= 0) st.selectedBone = idx;
        }
    }

    // --- Context menu (Setup) -----------------------------------------------
    if (st.drag == DragKind::None && hovered && st.mode == Mode::Setup && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (const Hit hit = hitTest(mouse, heads, tails, st.selectedBone); hit.bone >= 0) {
            st.selectedBone = hit.bone;
            ImGui::OpenPopup("bone_ctx");
        }
    }
    if (ImGui::BeginPopup("bone_ctx")) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();  // keyboard nav is off
        if (ImGui::MenuItem("Add child bone")) st.createTool = true;
        if (ImGui::MenuItem("Delete bone") && st.selectedBone >= 0) {
            app.pushUndo();
            skel::deleteBone(app.doc, st.selectedBone);
            st.selectedBone = -1;
        }
        ImGui::EndPopup();
    }

    // Cursor feedback.
    if (hovered && st.drag == DragKind::None) {
        if (st.mode == Mode::Setup && st.createTool)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        else if (const Hit h = hitTest(mouse, heads, tails, st.selectedBone); h.part == Part::Head)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        else if (h.part != Part::None)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // --- Draw ----------------------------------------------------------------
    app.displayedPose(pose);  // input may have changed it
    boneEnds(app, view, pose, heads, tails);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
    const ImVec2 cs = app.canvasSize();
    const ImVec2 c0 = view.toScreen({0, 0}), c1 = view.toScreen({cs.x, cs.y});
    dl->AddRectFilled(c0, c1, IM_COL32(40, 40, 40, 255));
    if (app.video.isOpen() && app.tex.id)
        dl->AddImage(app.tex.imguiId(), c0, c1, ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, int(std::clamp(st.videoAlpha, 0.0f, 1.0f) * 255)));
    dl->AddRect(c0, c1, IM_COL32(110, 110, 110, 255));

    // Tutorial tracing guide: dashed example bones with joint dots.
    std::vector<std::pair<ska::Vec2, ska::Vec2>> guide;
    app.tutorial.guide(app, guide);
    for (const auto& [h, t] : guide) {
        const ImVec2 a = view.toScreen(h), b = view.toScreen(t);
        const float len = dist(a, b);
        const ImU32 col = IM_COL32(255, 255, 255, 170);
        for (float s = 0; s < len; s += 12) {
            const float e = std::min(s + 7, len);
            dl->AddLine(ImVec2(a.x + (b.x - a.x) * s / len, a.y + (b.y - a.y) * s / len),
                        ImVec2(a.x + (b.x - a.x) * e / len, a.y + (b.y - a.y) * e / len), col, 2.0f);
        }
        dl->AddCircle(a, 5, col, 0, 1.5f);
        dl->AddCircle(b, 3, col);
    }

    if (st.mode == Mode::Animate && st.onionSkin) {
        drawOnion(app, view, dl, skel::prevKeyFrame(app.clip(), st.frame), IM_COL32(80, 160, 255, 110));
        drawOnion(app, view, dl, skel::nextKeyFrame(app.clip(), st.frame), IM_COL32(120, 255, 120, 110));
    }

    for (size_t i = 0; i < heads.size(); ++i)
        if (int(i) != st.selectedBone) drawBone(dl, heads[i], tails[i], app.doc.bones[i].color, false);
    if (st.selectedBone >= 0) {
        const size_t s = size_t(st.selectedBone);
        drawBone(dl, heads[s], tails[s], app.doc.bones[s].color, true);
        dl->AddCircle(tails[s], 5, IM_COL32(255, 230, 0, 255), 0, 2.0f);
    }
    if (st.showNames) {
        for (size_t i = 0; i < heads.size(); ++i) {
            const ImVec2 mid((heads[i].x + tails[i].x) * 0.5f + 6, (heads[i].y + tails[i].y) * 0.5f - 6);
            const char* name = app.doc.bones[i].name.c_str();
            dl->AddText(ImVec2(mid.x + 1, mid.y + 1), IM_COL32_BLACK, name);
            dl->AddText(mid, IM_COL32_WHITE, name);
        }
    }
    if (st.drag == DragKind::Create)
        dl->AddLine(view.toScreen(st.dragStartWorld), mouse, IM_COL32(255, 230, 0, 255), 2.0f);

    char status[128];
    if (st.mode == Mode::Setup) {
        std::snprintf(status, sizeof status, "SETUP \xC2\xB7 %s", st.createTool ? "Create tool (B)" : "Select (B: create)");
    } else {
        const bool keyed = skel::findKey(app.clip(), st.frame) >= 0;
        std::snprintf(status, sizeof status, "ANIMATE \xC2\xB7 frame %d (%s)", st.frame, keyed ? "keyed" : "interpolated");
    }
    const ImVec2 sp(origin.x + 8, origin.y + 8);
    dl->AddText(ImVec2(sp.x + 1, sp.y + 1), IM_COL32_BLACK, status);
    dl->AddText(sp, st.mode == Mode::Setup ? IM_COL32(255, 200, 80, 255) : IM_COL32(120, 220, 255, 255), status);
    dl->PopClipRect();
}
