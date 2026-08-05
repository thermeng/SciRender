#include "render/ColormapSync.h"
#include "render/renderer.h"
#include "render/ColormapManager.h"

void ColormapSync::apply(const RenderRenderState& state, ColormapManager& colormap) {
    colormap.setScalarChoice(state.colormapChoice);
    colormap.setScalarReversed(state.colormapReversed);
    colormap.setVectorChoice(state.vectorColormapChoice);
    colormap.setVectorReversed(state.vectorColormapReversed);
    colormap.setStreamlineChoice(state.streamlineColormapChoice);
    colormap.setStreamlineReversed(state.streamlineColormapReversed);
    colormap.setVolumeChoice(state.volumeColormapChoice);
    colormap.setVolumeReversed(state.volumeColormapReversed);
    colormap.update();
}
