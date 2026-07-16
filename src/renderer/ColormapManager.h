#pragma once

#include <glad/glad.h>

#include <string>

// Owns the scalar and vector-magnitude colormap LUTs (1D textures) plus the
// active palette choice/reversed state for each. Handles GL texture upload and
// lazy re-upload when the choice or reversal changes. Holds no rendering loop
// logic; renderFrame() queries the uploaded texture handles via accessors.
class ColormapManager {
public:
    ColormapManager() = default;
    ~ColormapManager() = default;

    // --- scalar (surface) LUT -------------------------------------------------
    int   scalarChoice() const { return scalarChoice_; }
    bool  scalarReversed() const { return scalarReversed_; }
    void  setScalarChoice(int c) { scalarChoice_ = c; }
    void  setScalarReversed(bool r) { scalarReversed_ = r; }
    GLuint scalarTexture() const { return scalarTex_; }

    // --- vector magnitude LUT (independent of scalar) -------------------------
    int   vectorChoice() const { return vectorChoice_; }
    bool  vectorReversed() const { return vectorReversed_; }
    void  setVectorChoice(int c) { vectorChoice_ = c; }
    void  setVectorReversed(bool r) { vectorReversed_ = r; }
    GLuint vectorTexture() const { return vectorTex_; }
    void  markVectorLutDirty() { vectorLutDirty_ = true; }

    // Ensures both LUT textures reflect the current choice/reversed state.
    // Uploads only when something changed (choice, reversal, or dirty flag).
    void update();

    // Deletes GL textures. Call only with a current GL context.
    void shutdown();

    // QML legend helpers (sample the active palette into [t,r,g,b] stops).
    // `reversed` is folded in so the legend matches the uploaded texture.
    std::string scalarStop(int steps, int index) const; // replaced by QVariantList at call site
    int scalarChoiceForStops() const { return scalarChoice_; }
    bool scalarReversedForStops() const { return scalarReversed_; }
    int vectorChoiceForStops() const { return vectorChoice_; }
    bool vectorReversedForStops() const { return vectorReversed_; }

private:
    int   scalarChoice_ = 3; // default CoolWarm
    bool  scalarReversed_ = false;
    GLuint scalarTex_ = 0;
    int   lastScalarChoice_ = -1;
    bool  lastScalarReversed_ = false;

    int   vectorChoice_ = 3;
    bool  vectorReversed_ = false;
    GLuint vectorTex_ = 0;
    int   lastVectorChoice_ = -1;
    bool  lastVectorReversed_ = false;
    bool  vectorLutDirty_ = true;

    void uploadLUT(GLuint& tex, int choice, bool reversed) const;
};
