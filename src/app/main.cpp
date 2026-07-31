#include <QApplication>
#include <QPalette>
#include <QDebug>
#include <QIcon>
#include <QFile>
#include <QSurfaceFormat>

#include "ui/main_window.h"

int main(int argc, char *argv[]) {
    qDebug() << "[LAUNCH DIAGNOSTIC 1/4] Main entry executed. Allocating resources...";

    // Force desktop OpenGL 4.6 Core profile
    QSurfaceFormat glFormat;
    glFormat.setRenderableType(QSurfaceFormat::OpenGL);
    glFormat.setVersion(4, 6);
    glFormat.setProfile(QSurfaceFormat::CoreProfile);
    glFormat.setDepthBufferSize(24);
    glFormat.setSamples(0);
    QSurfaceFormat::setDefaultFormat(glFormat);

    qDebug() << "[LAUNCH DIAGNOSTIC 2/4] Graphics API bound to desktop OpenGL 4.6 Core.";

    QApplication app(argc, argv);

    // Window icon
    {
        const QString exeIcon = QApplication::applicationDirPath() + "/app_icon.ico";
        QIcon icon(QFile::exists(exeIcon) ? exeIcon : QStringLiteral(APP_ICON_SRC));
        if (!icon.isNull()) app.setWindowIcon(icon);
    }

    // Stable QSettings scope
    QCoreApplication::setOrganizationName("SciRender");
    QCoreApplication::setApplicationName("SciRender");

    // Dark Fusion palette
    QPalette pal = app.palette();
    const QColor rail(0x26, 0x26, 0x26);
    pal.setColor(QPalette::Window, rail);
    pal.setColor(QPalette::Button, rail);
    pal.setColor(QPalette::Base, rail);
    pal.setColor(QPalette::AlternateBase, rail.lighter(110));
    pal.setColor(QPalette::Highlight, QColor(0x3a, 0x3a, 0x3a));
    pal.setColor(QPalette::WindowText, QColor(0xdd, 0xdd, 0xdd));
    pal.setColor(QPalette::ButtonText, QColor(0xdd, 0xdd, 0xdd));
    pal.setColor(QPalette::Text, QColor(0xdd, 0xdd, 0xdd));
    app.setPalette(pal);

    qDebug() << "[LAUNCH DIAGNOSTIC 3/4] Palette applied.";

    MainWindow window;
    window.show();

    qDebug() << "[LAUNCH DIAGNOSTIC 4/4] MainWindow created and shown.";

    return app.exec();
}
