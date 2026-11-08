#pragma once

#include <QOpenGLWidget>

// RAII guard for QOpenGLWidget::makeCurrent/doneCurrent.
// Ensures the context is released on all exit paths, including exceptions.
struct GlCurrentScope {
    QOpenGLWidget* w = nullptr;

    explicit GlCurrentScope(QOpenGLWidget* widget) : w(widget) {
        if (w) w->makeCurrent();
    }
    ~GlCurrentScope() {
        if (w) w->doneCurrent();
    }

    GlCurrentScope(const GlCurrentScope&) = delete;
    GlCurrentScope& operator=(const GlCurrentScope&) = delete;
    GlCurrentScope(GlCurrentScope&&) = delete;
    GlCurrentScope& operator=(GlCurrentScope&&) = delete;
};
