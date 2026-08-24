#include <QApplication>
#include <QDebug>
#include <QIcon>
#include <QFile>
#include <QSurfaceFormat>

#include "ui/main_window.h"

int main(int argc, char *argv[]) {
    // Crash handler for post-animation terminate
    std::set_terminate([](){
        qCritical() << "[CRASH] std::terminate called";
        try { std::rethrow_exception(std::current_exception()); }
        catch (const std::exception& e) { qCritical() << "  exception:" << e.what(); }
        catch (...) { qCritical() << "  unknown exception"; }
        std::abort();
    });
    qDebug() << "[LAUNCH DIAGNOSTIC 1/4] Main entry executed. Allocating resources...";

    // Stable QSettings scope (must be set before any QSettings access)
    QCoreApplication::setOrganizationName("SciRender");
    QCoreApplication::setApplicationName("SciRender");

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
    app.setStyle("modernwindows");

    // Window icon
    {
        const QString exeIcon = QApplication::applicationDirPath() + "/app_icon.ico";
        QIcon icon(QFile::exists(exeIcon) ? exeIcon : QStringLiteral(APP_ICON_SRC));
        if (!icon.isNull()) app.setWindowIcon(icon);
    }

    qDebug() << "[LAUNCH DIAGNOSTIC 3/4] Ready.";

    MainWindow window;
    window.show();

    qDebug() << "[LAUNCH DIAGNOSTIC 4/4] MainWindow created and shown.";

    int ret = 0;
    try { ret = app.exec(); }
    catch (const std::exception& e) { qCritical() << "[CRASH] app.exec exception:" << e.what(); ret = -1; }
    catch (...) { qCritical() << "[CRASH] app.exec unknown exception"; ret = -1; }
    qDebug() << "[SHUTDOWN] app.exec returned" << ret;
    return ret;
}
