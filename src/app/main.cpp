#include <QApplication>
#include <QDebug>
#include <QIcon>
#include <QFile>
#include <QSurfaceFormat>
#include <QPalette>

#include "ui/main_window.h"

static void applyDarkTheme(QApplication& app) {
    QPalette p;

    // Window
    p.setColor(QPalette::Window,          QColor("#1e1e1e"));
    p.setColor(QPalette::WindowText,     QColor("#cccccc"));
    p.setColor(QPalette::Base,           QColor("#1e1e1e"));
    p.setColor(QPalette::AlternateBase,  QColor("#252526"));
    p.setColor(QPalette::ToolTipBase,    QColor("#252526"));
    p.setColor(QPalette::ToolTipText,    QColor("#cccccc"));
    p.setColor(QPalette::Text,           QColor("#cccccc"));
    p.setColor(QPalette::Button,         QColor("#3c3c3c"));
    p.setColor(QPalette::ButtonText,     QColor("#cccccc"));
    p.setColor(QPalette::BrightText,     QColor("#ffffff"));
    p.setColor(QPalette::Link,           QColor("#3794ff"));
    p.setColor(QPalette::LinkVisited,    QColor("#b07dff"));

    // Selection
    p.setColor(QPalette::Highlight,       QColor("#264f78"));
    p.setColor(QPalette::HighlightedText, QColor("#ffffff"));

    // Disabled
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#6c6c6c"));
    p.setColor(QPalette::Disabled, QPalette::Text,      QColor("#6c6c6c"));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#6c6c6c"));

    // Mid / Dark / Light for 3D shading of widgets
    p.setColor(QPalette::Mid,   QColor("#353535"));
    p.setColor(QPalette::Dark,  QColor("#2b2b2b"));
    p.setColor(QPalette::Light, QColor("#454545"));
    p.setColor(QPalette::Shadow, QColor("#111111"));

    app.setPalette(p);

    // Minimal QSS for elements QPalette cannot fully style
    app.setStyleSheet(R"(
        /* --- Global Typography Handled via C++ app.setFont() --- */

        /* Menus & Toolbars */
        QMenuBar {
            background-color: #252526;
            color: #cccccc;
            border-bottom: 1px solid #3c3c3c;
        }
        QMenuBar::item { padding: 4px 8px; background: transparent; }
        QMenuBar::item:selected, QMenuBar::item:pressed { background-color: #094771; color: #ffffff; }
        
        QMenu {
            background-color: #252526;
            color: #cccccc;
            border: 1px solid #3c3c3c;
            padding: 4px 0;
        }
        QMenu::item { padding: 4px 24px 4px 28px; }
        QMenu::item:selected { background-color: #094771; color: #ffffff; }
        QMenu::separator { height: 1px; background: #3c3c3c; margin: 4px 8px; }

        /* Sliders */
        QSlider::groove:horizontal { height: 4px; background: #3c3c3c; border-radius: 2px; }
        QSlider::handle:horizontal { width: 12px; height: 12px; margin: -4px 0; background: #cccccc; border-radius: 6px; }
        QSlider::handle:horizontal:hover { background: #ffffff; }
        QSlider::handle:horizontal:pressed { background: #007acc; }
        QSlider::sub-page:horizontal { background: #007acc; border-radius: 2px; }
        QSlider::add-page:horizontal { background: #3c3c3c; border-radius: 2px; }

        /* Scrollbars */
        QScrollBar:vertical { width: 10px; background: transparent; }
        QScrollBar::handle:vertical { min-height: 30px; background: rgba(121,121,121,0.4); border-radius: 5px; }
        QScrollBar::handle:vertical:hover { background: rgba(121,121,121,0.7); }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
        
        QScrollBar:horizontal { height: 10px; background: transparent; }
        QScrollBar::handle:horizontal { min-width: 30px; background: rgba(121,121,121,0.4); border-radius: 5px; }
        QScrollBar::handle:horizontal:hover { background: rgba(121,121,121,0.7); }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }

        /* Inputs: ComboBox, LineEdit, SpinBoxes */
        QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {
            background-color: #3c3c3c;
            color: #cccccc;
            border: 1px solid #3c3c3c;
            border-radius: 2px;
            padding: 2px 6px;
            min-height: 20px;
        }
        QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover { border: 1px solid #505050; }
        QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border: 1px solid #007acc; }
        
        QComboBox::drop-down {
            subcontrol-origin: padding; subcontrol-position: top right;
            width: 16px; border-left: 1px solid #3c3c3c;
        }
        QComboBox::down-arrow {
            border-left: 4px solid transparent; border-right: 4px solid transparent;
            border-top: 5px solid #cccccc; width: 0px; height: 0px; margin-right: 4px;
        }
        QComboBox QAbstractItemView {
            background-color: #252526; color: #cccccc;
            selection-background-color: #094771; selection-color: #ffffff;
            border: 1px solid #3c3c3c; outline: none;
        }

        QSpinBox::up-button, QDoubleSpinBox::up-button {
            background-color: #3c3c3c; border-left: 1px solid #3c3c3c; border-bottom: 1px solid #3c3c3c; width: 16px;
        }
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            background-color: #3c3c3c; border-left: 1px solid #3c3c3c; width: 16px;
        }

        /* Checkboxes & Radio Buttons */
        QCheckBox, QRadioButton { color: #cccccc; spacing: 6px; }
        QCheckBox:hover, QRadioButton:hover { color: #ffffff; }
        
        QCheckBox::indicator {
            width: 14px; height: 14px; border: 1px solid #6c6c6c; border-radius: 2px; background-color: transparent;
        }
        QCheckBox::indicator:hover { border: 1px solid #007acc; }
        QCheckBox::indicator:checked { background-color: #007acc; border: 1px solid #007acc; image: url(:/src/resources/icons/checkmark.svg); }
        QCheckBox::indicator:checked:hover { background-color: #1c8cd9; border: 1px solid #1c8cd9; image: url(:/src/resources/icons/checkmark.svg); }

        QRadioButton::indicator {
            width: 14px; height: 14px; border: 1px solid #6c6c6c; border-radius: 7px; background-color: transparent;
        }
        QRadioButton::indicator:hover { border: 1px solid #007acc; }
        QRadioButton::indicator:checked { background-color: #007acc; border: 1px solid #007acc; }

        /* Buttons */
        QPushButton {
            background-color: #0e639c; color: #ffffff;
            border: none; border-radius: 2px; padding: 4px 12px; min-height: 20px;
            border: 1px solid #1177bb;
        }
        QPushButton:hover { background-color: #1177bb; border: 1px solid #1c8cd9;}
        QPushButton:pressed { background-color: #094771; border: 1px solid #094771;}
        QPushButton:disabled { background-color: #3c3c3c; color: #6c6c6c; border: 1px solid #3c3c3c;}

        QToolButton {
            background-color: transparent; color: #cccccc;
            border: none; border-radius: 2px; padding: 2px 4px;
        }
        QToolButton:hover { background-color: #2a2d2e; }
        QToolButton:checked { background-color: #37373d; }
        QToolButton:disabled { color: #6c6c6c; }

        /* Tabs */
        QTabWidget::pane { border: 1px solid #3c3c3c; background: #1e1e1e; }
        QTabBar::tab {
            background-color: #2d2d2d; color: #969696;
            border: 1px solid #3c3c3c; border-bottom: none;
            padding: 4px 10px; margin-right: -1px;
        }
        QTabBar::tab:selected { background-color: #1e1e1e; color: #ffffff; border-top: 2px solid #007acc; }
        QTabBar::tab:hover:!selected { background-color: #353535; color: #cccccc; }

        /* Status Bar, Docks, & Tooltips */
        QStatusBar { background-color: #007acc; color: #ffffff; border: none; }
        QStatusBar::item { border: none; }
        QStatusBar QLabel { color: #ffffff; background: transparent; }
        
        QToolTip { background-color: #252526; color: #cccccc; border: 1px solid #3c3c3c; padding: 4px 8px; }
        
        QDockWidget { color: #cccccc; }
        QDockWidget::title { background: #252526; border-bottom: 1px solid #3c3c3c; padding: 4px; }
        
        QScrollArea { border: none; background: transparent; }
        QSplitter::handle { background-color: #3c3c3c; }
        QMessageBox { background-color: #1e1e1e; color: #cccccc; }
        QFrame[frameShape="4"] { color: #3c3c3c; }
    )");
}

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

    // VS Code Dark+ inspired theme
    applyDarkTheme(app);

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
