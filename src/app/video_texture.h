#pragma once

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <cstdint>

// RGBA8 texture holding the current video frame; drawn through ImDrawList::AddImage.
struct VideoTexture {
    GLuint id = 0;
    int w = 0, h = 0;

    void upload(const uint8_t* rgba, int w, int h);
    void destroy();
    ImTextureID imguiId() const { return (ImTextureID)(intptr_t)id; }
};
