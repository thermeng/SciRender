#include <QApplication>
#include <QDebug>
#include <QIcon>
#include <QFile>
#include <QSettings>
#include <QSurfaceFormat>

#include "ui/main_window.h"
#include "ui/theme.h"

int main(int argc, char *argv[]) {
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
    app.setStyle("Fusion");

    // Load persisted theme (default Dark if no setting saved)
    {
        QSettings s;
        int themeInt = s.value("theme", 0).toInt();
        AppTheme theme = (themeInt == 1) ? AppTheme::Light : AppTheme::Dark;
        ThemeColors colors = getThemeColors(theme);
        app.setPalette(buildPalette(colors));
        app.setStyleSheet(buildGlobalStylesheet(colors));
    }

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

    return app.exec();
}
