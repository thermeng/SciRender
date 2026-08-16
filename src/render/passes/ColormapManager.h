#pragma once

#include "render/foundation/gl_raii.h"

#include <string>

class ColormapManager {
public:
    ColormapManager() = default;
    ~ColormapManager() = default;

    GLuint scalarTexture() const { return scalarTex_; }
    GLuint vectorTexture() const { return vectorTex_; }
    GLuint streamlineTexture() const { return streamlineTex_; }
    GLuint volumeTexture() const { return volumeTex_; }

    int   scalarChoice() const { return scalarChoice_; }
    bool  scalarReversed() const { return scalarReversed_; }
    void  setScalarChoice(int c) { scalarChoice_ = c; }
    void  setScalarReversed(bool r) { scalarReversed_ = r; }

    int   vectorChoice() const { return vectorChoice_; }
    bool  vectorReversed() const { return vectorReversed_; }
    void  setVectorChoice(int c) { vectorChoice_ = c; }
    void  setVectorReversed(bool r) { vectorReversed_ = r; }
    void  markVectorLutDirty() { vectorLutDirty_ = true; }

    int   streamlineChoice() const { return streamlineChoice_; }
    bool  streamlineReversed() const { return streamlineReversed_; }
    void  setStreamlineChoice(int c) { streamlineChoice_ = c; }
    void  setStreamlineReversed(bool r) { streamlineReversed_ = r; }
    void  markStreamlineLutDirty() { streamlineLutDirty_ = true; }

    int   volumeChoice() const { return volumeChoice_; }
    bool  volumeReversed() const { return volumeReversed_; }
    void  setVolumeChoice(int c) { volumeChoice_ = c; }
    void  setVolumeReversed(bool r) { volumeReversed_ = r; }
    void  markVolumeLutDirty() { volumeLutDirty_ = true; }

    int   volumeSliceChoice() const { return volumeSliceChoice_; }
    bool  volumeSliceReversed() const { return volumeSliceReversed_; }
    void  setVolumeSliceChoice(int c) { volumeSliceChoice_ = c; }
    void  setVolumeSliceReversed(bool r) { volumeSliceReversed_ = r; }
    void  markVolumeSliceLutDirty() { volumeSliceLutDirty_ = true; }
    GLuint volumeSliceTexture() const { return volumeSliceTex_.get(); }

    void update();
    void shutdown();

private:
    void uploadLUT(GLuint& tex, int choice, bool reversed) const;

    int   scalarChoice_ = 3;
    bool  scalarReversed_ = false;
    GlTexture scalarTex_;
    int   lastScalarChoice_ = -1;
    bool  lastScalarReversed_ = false;

    int   vectorChoice_ = 3;
    bool  vectorReversed_ = false;
    GlTexture vectorTex_;
    int   lastVectorChoice_ = -1;
    bool  lastVectorReversed_ = false;
    bool  vectorLutDirty_ = true;

    int   streamlineChoice_ = 3;
    bool  streamlineReversed_ = false;
    GlTexture streamlineTex_;
    int   lastStreamlineChoice_ = -1;
    bool  lastStreamlineReversed_ = false;
    bool  streamlineLutDirty_ = true;

    int   volumeChoice_ = 3;
    bool  volumeReversed_ = false;
    GlTexture volumeTex_;
    int   lastVolumeChoice_ = -1;
    bool  lastVolumeReversed_ = false;
    bool  volumeLutDirty_ = true;

    int   volumeSliceChoice_ = 3;
    bool  volumeSliceReversed_ = false;
    GlTexture volumeSliceTex_;
    int   lastVolumeSliceChoice_ = -1;
    bool  lastVolumeSliceReversed_ = false;
    bool  volumeSliceLutDirty_ = true;
};


