#include "export/screenshot.h"
#include <QImage>
#include <QDateTime>
#include <QFileInfo>
#include <QDebug>
#include <QRegularExpression>
#include <cstring>

// ---------------------------------------------------------------------------
// captureFBO
// ---------------------------------------------------------------------------
std::vector<unsigned char> ScreenshotExporter::captureFBO(GLuint fbo, int width, int height, bool transparent) {
    const int channels = transparent ? 4 : 3;
    const GLenum format = transparent ? GL_RGBA : GL_RGB;
    const GLenum type = GL_UNSIGNED_BYTE;

    // Allocate memory buffer to draw out raw active pixel vectors
    std::vector<unsigned char> raw(width * height * channels);

    // Bind current target framebuffer and read color attachment data safely
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, format, type, raw.data());

    // Flip vertically so the target image matches top-left standard window layouts
    std::vector<unsigned char> flipped(width * height * channels);
    const int rowSize = width * channels;
    for (int y = 0; y < height; ++y) {
        std::memcpy(
            flipped.data() + (size_t)y * rowSize,
            raw.data() + (size_t)(height - 1 - y) * rowSize,
            rowSize
            );
    }

    return flipped;
}

// ---------------------------------------------------------------------------
// saveToFile (Qt Quick / QImage Native Rewrite)
// ---------------------------------------------------------------------------
bool ScreenshotExporter::saveToFile(const QString& path, const std::vector<unsigned char>& pixels,
                                    int width, int height, const ExportConfig& config) {
    if (pixels.empty() || width <= 0 || height <= 0) {
        qWarning() << "Screenshot failure: Pixel array content invalid or empty dimensions.";
        return false;
    }

    // Map raw pixels onto a Qt image. The GL buffer is tight-packed
    // (GL_PACK_ALIGNMENT=1), but QImage's 4-arg ctor auto-strides to
    // 4-byte alignment for Format_RGB888, over-reading the last rows when
    // width*3 isn't a multiple of 4. Pass the tight stride explicitly.
    const int channels = config.transparentBackground ? 4 : 3;
    QImage::Format qFormat = config.transparentBackground ? QImage::Format_RGBA8888 : QImage::Format_RGB888;
    const int stride = width * channels;
    QImage img(pixels.data(), width, height, stride, qFormat);

    // Determine the string token layout Qt uses for formatting
    const char* formatStr = "PNG";
    switch (config.format) {
    case ExportFormat::JPEG: formatStr = "JPG"; break;
    case ExportFormat::BMP:  formatStr = "BMP"; break;
    case ExportFormat::PNG:
    default:                 formatStr = "PNG"; break;
    }

    // Save out the workspace image natively via file system
    bool success = img.save(path, formatStr, (config.format == ExportFormat::JPEG) ? config.quality : -1);

    if (!success) {
        qWarning() << "Failed to write target frame capture image data disk block to path:" << path;
    }
    return success;
}

// ---------------------------------------------------------------------------
// generateFilename
// ---------------------------------------------------------------------------
QString ScreenshotExporter::generateFilename(const QString& modelName, ExportFormat format) {
    QString base = modelName.isEmpty() ? "scene" : modelName;

    // Sanitize string fields: replace spaces and characters using Qt metrics
    base.replace(QRegularExpression("[^a-zA-Z0-9_]"), "_");

    // Pull precise local machine system time metrics cleanly
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss");

    QString ext = ".png";
    switch (format) {
    case ExportFormat::JPEG: ext = ".jpg"; break;
    case ExportFormat::BMP:  ext = ".bmp"; break;
    case ExportFormat::PNG:
    default:                 ext = ".png"; break;
    }

    return QString("%1_%2%3").arg(base, timestamp, ext);
}