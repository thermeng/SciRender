#pragma once

#include <glad/glad.h>
#include <string>
#include <vector>
#include <QString>

enum class ExportFormat {
    PNG,
    JPEG,
    BMP
};

struct ExportConfig {
    QString filePath;
    ExportFormat format;
    bool transparentBackground;  // PNG only: if true, keep alpha channel transparency
    int quality;                 // JPEG quality 1-100
};

class ScreenshotExporter {
public:
    // Captures pixels directly from the target OpenGL Framebuffer Object (FBO)
    static std::vector<unsigned char> captureFBO(GLuint fbo, int width, int height, bool transparent);

    // Modern Qt Quick Replacement: Uses QImage native platform writers instead of STB
    static bool saveToFile(const QString& path, const std::vector<unsigned char>& pixels,
                           int width, int height, const ExportConfig& config);

    // Generates a default timestamp-based filename using QDateTime utilities
    static QString generateFilename(const QString& modelName, ExportFormat format);
};