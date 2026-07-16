#include "render/ColormapManager.h"
#include "core/Colormaps.h"

void ColormapManager::uploadLUT(GLuint& tex, int choice, bool reversed) const {
    std::vector<unsigned char> pd; pd.reserve(256 * 3);
    for (int i = 0; i < 256; ++i) {
        float t = static_cast<float>(i) / 255.0f;
        float s = reversed ? (1.0f - t) : t;
        glm::vec3 rgb = Colormaps::evaluate(s, static_cast<ColormapType>(choice));
        pd.push_back(static_cast<unsigned char>(rgb.r * 255.0f));
        pd.push_back(static_cast<unsigned char>(rgb.g * 255.0f));
        pd.push_back(static_cast<unsigned char>(rgb.b * 255.0f));
    }
    if (tex == 0) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_1D, tex);
    glTexImage1D(GL_TEXTURE_1D, 0, GL_RGB, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, pd.data());
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_1D, 0);
}

void ColormapManager::update() {
    if (scalarTex_ == 0 || scalarChoice_ != lastScalarChoice_ || scalarReversed_ != lastScalarReversed_) {
        uploadLUT(scalarTex_, scalarChoice_, scalarReversed_);
        lastScalarChoice_ = scalarChoice_;
        lastScalarReversed_ = scalarReversed_;
    }

    if (vectorTex_ == 0 || vectorLutDirty_ ||
        vectorChoice_ != lastVectorChoice_ || vectorReversed_ != lastVectorReversed_) {
        uploadLUT(vectorTex_, vectorChoice_, vectorReversed_);
        lastVectorChoice_ = vectorChoice_;
        lastVectorReversed_ = vectorReversed_;
        vectorLutDirty_ = false;
    }
}

void ColormapManager::shutdown() {
    if (scalarTex_) { glDeleteTextures(1, &scalarTex_); scalarTex_ = 0; }
    if (vectorTex_) { glDeleteTextures(1, &vectorTex_); vectorTex_ = 0; }
}
