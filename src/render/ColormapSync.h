#pragma once

#include <glad/gl.h>

struct RenderRenderState;
class ColormapManager;

class ColormapSync {
public:
    void apply(const RenderRenderState& state, ColormapManager& colormap);
    void shutdown() {}
};
