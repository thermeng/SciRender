#include <QApplication>
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

    qDebug() << "[LAUNCH DIAGNOSTIC 3/4] Ready.";

    MainWindow window;
    window.show();

    qDebug() << "[LAUNCH DIAGNOSTIC 4/4] MainWindow created and shown.";

    return app.exec();
}
