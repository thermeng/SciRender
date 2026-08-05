#include "render/ColormapManager.h"
#include "core/Colormaps.h"
#include <glad/gl.h>

void ColormapManager::uploadLUT(GLuint& texOut, int choice, bool reversed) const {
    std::vector<unsigned char> pd; pd.reserve(256 * 3);
    for (int i = 0; i < 256; ++i) {
        float t = static_cast<float>(i) / 255.0f;
        float s = reversed ? (1.0f - t) : t;
        glm::vec3 rgb = Colormaps::evaluate(s, static_cast<ColormapType>(choice));
        pd.push_back(static_cast<unsigned char>(rgb.r * 255.0f));
        pd.push_back(static_cast<unsigned char>(rgb.g * 255.0f));
        pd.push_back(static_cast<unsigned char>(rgb.b * 255.0f));
    }
    GlTexture tex;
    if (texOut) tex.reset(texOut);
    if (!tex.has()) glCreateTextures(GL_TEXTURE_1D, 1, tex.ptr());
    glTextureStorage1D(tex, 1, GL_RGB8, 256);
    glTextureSubImage1D(tex, 0, 0, 256, GL_RGB, GL_UNSIGNED_BYTE, pd.data());
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    texOut = tex.release();
}

void ColormapManager::update() {
    if (!scalarTex_.has() || scalarChoice_ != lastScalarChoice_ || scalarReversed_ != lastScalarReversed_) {
        GLuint raw = scalarTex_.get();
        uploadLUT(raw, scalarChoice_, scalarReversed_);
        if (raw != scalarTex_.get()) scalarTex_.reset(raw);
        lastScalarChoice_ = scalarChoice_;
        lastScalarReversed_ = scalarReversed_;
    }

    if (!vectorTex_.has() || vectorLutDirty_ ||
        vectorChoice_ != lastVectorChoice_ || vectorReversed_ != lastVectorReversed_) {
        GLuint raw = vectorTex_.get();
        uploadLUT(raw, vectorChoice_, vectorReversed_);
        if (raw != vectorTex_.get()) vectorTex_.reset(raw);
        lastVectorChoice_ = vectorChoice_;
        lastVectorReversed_ = vectorReversed_;
        vectorLutDirty_ = false;
    }

    if (!streamlineTex_.has() || streamlineLutDirty_ ||
        streamlineChoice_ != lastStreamlineChoice_ || streamlineReversed_ != lastStreamlineReversed_) {
        GLuint raw = streamlineTex_.get();
        uploadLUT(raw, streamlineChoice_, streamlineReversed_);
        if (raw != streamlineTex_.get()) streamlineTex_.reset(raw);
        lastStreamlineChoice_ = streamlineChoice_;
        lastStreamlineReversed_ = streamlineReversed_;
        streamlineLutDirty_ = false;
    }

    if (!volumeTex_.has() || volumeLutDirty_ ||
        volumeChoice_ != lastVolumeChoice_ || volumeReversed_ != lastVolumeReversed_) {
        GLuint raw = volumeTex_.get();
        uploadLUT(raw, volumeChoice_, volumeReversed_);
        if (raw != volumeTex_.get()) volumeTex_.reset(raw);
        lastVolumeChoice_ = volumeChoice_;
        lastVolumeReversed_ = volumeReversed_;
        volumeLutDirty_ = false;
    }
}

void ColormapManager::shutdown() {
    scalarTex_.reset();
    vectorTex_.reset();
    streamlineTex_.reset();
    volumeTex_.reset();
}
