#pragma once

#include <QPalette>
#include <QString>

enum class AppTheme { Dark, Light };

struct ThemeColors {
    // Window & panels
    QColor windowBg;
    QColor panelBg;
    QColor surfaceBg;
    QColor tooltipBg;

    // Borders
    QColor border;
    QColor borderLight;

    // Text
    QColor textPrimary;
    QColor textMuted;
    QColor textBright;
    QColor textDisabled;
    // Text intended to sit ON a colored/accent background (e.g. blue). White in
    // both themes so it never collapses into black-on-blue.
    QColor textOnAccent;

    // Accent
    QColor accent;
    QColor accentHover;
    QColor accentPressed;
    QColor selectionBg;
    QColor selectionText;

    // Buttons
    QColor buttonBg;
    QColor buttonHover;
    QColor buttonPressed;
    QColor buttonText;
    QColor buttonDisabled;
    QColor buttonBorder;

    // Inputs (combo, spin, line edit)
    QColor inputBg;
    QColor inputBorder;
    QColor inputHoverBorder;
    QColor inputFocusBorder;

    // Interactive states
    QColor hoverBg;
    QColor checkedBg;
    QColor checkedText;

    // Section header
    QColor sectionHeader;

    // Tabs
    QColor tabBg;
    QColor tabText;
    QColor tabSelectedBg;
    QColor tabSelectedText;
    QColor tabHoverBg;

    // Scrollbar
    QColor scrollbarHandle;
    QColor scrollbarHandleHover;

    // Status bar
    QColor statusBg;
    QColor statusText;

    // Collapsible groups
    QColor groupHeaderBg;
    QColor groupContentBg;

    // Quick bar
    QColor quickBarBg;
    QColor quickBarHandleBg;

    // Icon strip
    QColor iconNormal;
    QColor iconHover;
    QColor iconChecked;

    // Dock widget
    QColor dockTitleBg;

    // Frame separator
    QColor frameSeparator;
};

inline ThemeColors getThemeColors(AppTheme theme) {
    if (theme == AppTheme::Light) {
        return {
            // Window & panels
            QColor("#f0f0f0"),       // windowBg
            QColor("#ffffff"),       // panelBg
            QColor("#ffffff"),       // surfaceBg
            QColor("#ffffff"),       // tooltipBg

            // Borders
            QColor("#d0d0d0"),       // border
            QColor("#e0e0e0"),       // borderLight

            // Text
            QColor("#1e1e1e"),       // textPrimary
            QColor("#616161"),       // textMuted
            QColor("#000000"),       // textBright
            QColor("#888888"),       // textDisabled
            QColor("#ffffff"),       // textOnAccent

            // Accent
            QColor("#007acc"),       // accent
            QColor("#1a8ad4"),       // accentHover
            QColor("#005f9e"),       // accentPressed
            QColor("#d6ebff"),       // selectionBg
            QColor("#000000"),       // selectionText

            // Buttons
            QColor("#0e639c"),       // buttonBg
            QColor("#1177bb"),       // buttonHover
            QColor("#094771"),       // buttonPressed
            QColor("#ffffff"),       // buttonText
            QColor("#cccccc"),       // buttonDisabled
            QColor("#1177bb"),       // buttonBorder

            // Inputs
            QColor("#ffffff"),       // inputBg
            QColor("#cccccc"),       // inputBorder
            QColor("#888888"),       // inputHoverBorder
            QColor("#007acc"),       // inputFocusBorder

            // Interactive states
            QColor("#e5f1fb"),       // hoverBg
            QColor("#d6ebff"),       // checkedBg
            QColor("#000000"),       // checkedText

            // Section header
            QColor("#007acc"),       // sectionHeader

            // Tabs
            QColor("#f0f0f0"),       // tabBg
            QColor("#616161"),       // tabText
            QColor("#ffffff"),       // tabSelectedBg
            QColor("#000000"),       // tabSelectedText
            QColor("#e0e0e0"),       // tabHoverBg

            // Scrollbar
            QColor("rgba(97,97,97,0.50)"),  // scrollbarHandle
            QColor("rgba(97,97,97,0.85)"),  // scrollbarHandleHover

            // Status bar
            QColor("#007acc"),       // statusBg
            QColor("#ffffff"),       // statusText

            // Collapsible groups
            QColor("#e8e8e8"),       // groupHeaderBg
            QColor("#f5f5f5"),       // groupContentBg

            // Quick bar
            QColor("#ffffff"),       // quickBarBg
            QColor("#ffffff"),       // quickBarHandleBg

            // Icon strip
            QColor("#616161"),       // iconNormal
            QColor("#1e1e1e"),       // iconHover
            QColor("#000000"),       // iconChecked

            // Dock widget
            QColor("#f0f0f0"),       // dockTitleBg

            // Frame separator
            QColor("#d0d0d0"),       // frameSeparator
        };
    }

    // Dark theme (VS Code Dark+)
    return {
        // Window & panels
        QColor("#1e1e1e"),       // windowBg
        QColor("#252526"),       // panelBg
        QColor("#1e1e1e"),       // surfaceBg
        QColor("#252526"),       // tooltipBg

        // Borders
        QColor("#3c3c3c"),       // border
        QColor("#505050"),       // borderLight

        // Text
        QColor("#cccccc"),       // textPrimary
        QColor("#858585"),       // textMuted
        QColor("#ffffff"),       // textBright
        QColor("#9e9e9e"),       // textDisabled
        QColor("#ffffff"),       // textOnAccent

        // Accent
        QColor("#007acc"),       // accent
        QColor("#1c8cd9"),       // accentHover
        QColor("#094771"),       // accentPressed
        QColor("#264f78"),       // selectionBg
        QColor("#ffffff"),       // selectionText

        // Buttons
        QColor("#0e639c"),       // buttonBg
        QColor("#1177bb"),       // buttonHover
        QColor("#094771"),       // buttonPressed
        QColor("#ffffff"),       // buttonText
        QColor("#3c3c3c"),       // buttonDisabled
        QColor("#1177bb"),       // buttonBorder

        // Inputs
        QColor("#3c3c3c"),       // inputBg
        QColor("#3c3c3c"),       // inputBorder
        QColor("#505050"),       // inputHoverBorder
        QColor("#007acc"),       // inputFocusBorder

        // Interactive states
        QColor("#2a2d2e"),       // hoverBg
        QColor("#37373d"),       // checkedBg
        QColor("#ffffff"),       // checkedText

        // Section header
        QColor("#007acc"),       // sectionHeader

        // Tabs
        QColor("#2d2d2d"),       // tabBg
        QColor("#969696"),       // tabText
        QColor("#1e1e1e"),       // tabSelectedBg
        QColor("#ffffff"),       // tabSelectedText
        QColor("#353535"),       // tabHoverBg

        // Scrollbar
        QColor("rgba(121,121,121,0.4)"),  // scrollbarHandle
        QColor("rgba(121,121,121,0.7)"),  // scrollbarHandleHover

        // Status bar
        QColor("#007acc"),       // statusBg
        QColor("#ffffff"),       // statusText

        // Collapsible groups
        QColor("#2a2d2e"),       // groupHeaderBg
        QColor("#222222"),       // groupContentBg

        // Quick bar
        QColor("#252526"),       // quickBarBg
        QColor("#252526"),       // quickBarHandleBg

        // Icon strip
        QColor("#858585"),       // iconNormal
        QColor("#cccccc"),       // iconHover
        QColor("#ffffff"),       // iconChecked

        // Dock widget
        QColor("#252526"),       // dockTitleBg

        // Frame separator
        QColor("#3c3c3c"),       // frameSeparator
    };
}

// Global current theme colors — read-only access; use setThemeColors() to mutate.
inline const ThemeColors& currentThemeColors() {
    static ThemeColors instance = getThemeColors(AppTheme::Dark);
    return instance;
}

// Single mutation point — only MainWindow::applyTheme() should call this.
inline void setThemeColors(AppTheme theme) {
    const_cast<ThemeColors&>(currentThemeColors()) = getThemeColors(theme);
}

// Helper for colors with alpha — Qt stylesheet parser understands rgba().
inline QString rgbaStr(const QColor& c) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 2);
}

inline QPalette buildPalette(const ThemeColors& c) {
    QPalette p;
    p.setColor(QPalette::Window,              c.windowBg);
    p.setColor(QPalette::WindowText,          c.textPrimary);
    p.setColor(QPalette::Base,                c.surfaceBg);
    p.setColor(QPalette::AlternateBase,       c.panelBg);
    p.setColor(QPalette::ToolTipBase,         c.tooltipBg);
    p.setColor(QPalette::ToolTipText,         c.textPrimary);
    p.setColor(QPalette::Text,                c.textPrimary);
    p.setColor(QPalette::Button,              c.buttonBg);
    p.setColor(QPalette::ButtonText,         c.textPrimary);  // was: c.buttonText (white in light theme → invisible combo text)
    p.setColor(QPalette::BrightText,          c.textBright);
    p.setColor(QPalette::Link,                c.accent);
    p.setColor(QPalette::LinkVisited,         c.accentPressed);
    p.setColor(QPalette::Highlight,           c.selectionBg);
    p.setColor(QPalette::HighlightedText,     c.selectionText);
    p.setColor(QPalette::Mid,                 c.border);
    p.setColor(QPalette::Dark,                c.border);
    p.setColor(QPalette::Light,               c.borderLight);
    p.setColor(QPalette::Shadow,              c.border);
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.textDisabled);
    p.setColor(QPalette::Disabled, QPalette::Text,      c.textDisabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.textDisabled);
    return p;
}

// ---------------------------------------------------------------------------
// Stylesheet helpers — each returns a self-contained QString for its widget
// category.  buildGlobalStylesheet() concatenates them all.
// ---------------------------------------------------------------------------

inline QString menuStylesheet(const ThemeColors& c) {
    return QStringLiteral(R"(
        QMenuBar {
            background-color: %1; color: %2;
            border-bottom: 1px solid %3;
        }
        QMenuBar::item { padding: 4px 8px; background: transparent; }
        QMenuBar::item:selected, QMenuBar::item:pressed {
            background-color: %4; color: %5;
        }
        QMenu {
            background-color: %1; color: %2;
            border: 1px solid %3; padding: 4px 0;
        }
        QMenu::item { padding: 4px 24px 4px 28px; }
        QMenu::item:selected { background-color: %4; color: %5; }
        QMenu::separator { height: 1px; background: %3; margin: 4px 8px; }
    )")
    .arg(c.panelBg.name())           // %1
    .arg(c.textPrimary.name())       // %2
    .arg(c.border.name())            // %3
    .arg(c.selectionBg.name())       // %4
    .arg(c.selectionText.name());    // %5  — selection text on menu items
}

inline QString sliderStylesheet(const ThemeColors& c) {
    return QStringLiteral(R"(
        QSlider::groove:horizontal {
            height: 4px; background: %1; border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 12px; height: 12px; margin: -4px 0;
            background: %2; border-radius: 6px;
        }
        QSlider::handle:horizontal:hover { background: %3; }
        QSlider::handle:horizontal:pressed { background: %4; }
        QSlider::sub-page:horizontal { background: %4; border-radius: 2px; }
        QSlider::add-page:horizontal { background: %1; border-radius: 2px; }
    )")
    .arg(c.border.name())            // %1  groove / add-page
    .arg(c.textPrimary.name())       // %2  handle base
    .arg(c.textBright.name())        // %3  handle hover
    .arg(c.accent.name());           // %4  handle pressed / sub-page
}

inline QString scrollbarStylesheet(const ThemeColors& c) {
    return QStringLiteral(R"(
        QScrollBar:vertical { width: 10px; background: transparent; }
        QScrollBar::handle:vertical {
            min-height: 30px; background: %1; border-radius: 5px;
        }
        QScrollBar::handle:vertical:hover { background: %2; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }

        QScrollBar:horizontal { height: 10px; background: transparent; }
        QScrollBar::handle:horizontal {
            min-width: 30px; background: %1; border-radius: 5px;
        }
        QScrollBar::handle:horizontal:hover { background: %2; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }
    )")
    .arg(rgbaStr(c.scrollbarHandle))       // %1  rgba alpha
    .arg(rgbaStr(c.scrollbarHandleHover)); // %2  rgba alpha
}

inline QString inputStylesheet(const ThemeColors& c) {
    return QStringLiteral(R"(
        QComboBox, QLineEdit {
            background-color: %1; color: %2;
            border: 1px solid %3; border-radius: 2px;
            padding: 2px 6px; min-height: 20px;
        }
        QSpinBox, QDoubleSpinBox {
            background-color: %1; color: %2;
            border: 1px solid %3; border-radius: 2px;
            padding: 2px 4px; min-height: 20px;
        }
        QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover {
            border: 1px solid %4;
        }
        QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {
            border: 1px solid %5;
        }
        QComboBox::drop-down {
            subcontrol-origin: padding; subcontrol-position: top right;
            width: 22px; border: none;
            background: transparent;
        }
        QComboBox::down-arrow {
            image: url(:/src/resources/icons/downarrow.svg);
            width: 12px; height: 12px; margin-right: 6px; color: %2;
        }
        QComboBox::item, QComboBox::item:selected {
            color: %2; background-color: %6;
        }
        QComboBox QAbstractItemView {
            background-color: %6; color: %2;
            selection-background-color: %7; selection-color: %8;
            border: 1px solid %3; outline: none;
        }
        QComboBox:editable QLineEdit {
            color: %2;
            background-color: %1;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button {
            subcontrol-origin: padding; subcontrol-position: top right;
            background-color: %1;
            border-bottom: 1px solid %3; width: 16px; height: 16px;
        }
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            subcontrol-origin: padding; subcontrol-position: bottom right;
            background-color: %1; width: 16px; height: 16px;
        }
    )")
    .arg(c.inputBg.name())           // %1  background
    .arg(c.textPrimary.name())       // %2  text / arrow
    .arg(c.inputBorder.name())       // %3  border
    .arg(c.inputHoverBorder.name())  // %4  hover border
    .arg(c.inputFocusBorder.name())  // %5  focus border
    .arg(c.panelBg.name())           // %6  dropdown view bg
    .arg(c.selectionBg.name())       // %7  dropdown selection bg
    .arg(c.selectionText.name());    // %8  dropdown selection text
}

inline QString checkboxStylesheet(const ThemeColors& c) {
    return QStringLiteral(R"(
        QCheckBox, QRadioButton { color: %1; spacing: 6px; }
        QCheckBox:hover, QRadioButton:hover { color: %2; }

        QCheckBox::indicator {
            width: 14px; height: 14px; border: 1px solid %3;
            border-radius: 2px; background-color: transparent;
        }
        QCheckBox::indicator:hover { border: 1px solid %4; }
        QCheckBox::indicator:checked {
            background-color: %4; border: 1px solid %4;
            image: url(:/src/resources/icons/checkmark.svg);
        }
        QCheckBox::indicator:checked:hover {
            background-color: %5; border: 1px solid %5;
            image: url(:/src/resources/icons/checkmark.svg);
        }

        QRadioButton::indicator {
            width: 14px; height: 14px; border: 1px solid %3;
            border-radius: 7px; background-color: transparent;
        }
        QRadioButton::indicator:hover { border: 1px solid %4; }
        QRadioButton::indicator:checked {
            background-color: %4; border: 1px solid %4;
        }
    )")
    .arg(c.textPrimary.name())       // %1  normal text
    .arg(c.textBright.name())        // %2  hover text
    .arg(c.border.name())            // %3  indicator border
    .arg(c.accent.name())            // %4  checked / hover indicator
    .arg(c.accentHover.name());      // %5  checked:hover indicator
}

inline QString buttonStylesheet(const ThemeColors& c) {
    return QStringLiteral(R"(
        QPushButton {
            background-color: %1; color: %2;
            border: 1px solid %3; border-radius: 2px;
            padding: 4px 12px; min-height: 20px;
        }
        QPushButton:hover { background-color: %12; border: 1px solid %4; }
        QPushButton:pressed { background-color: %5; border: 1px solid %5; }
        QPushButton:disabled { background-color: %6; color: %7; border: 1px solid %8; }
        QPushButton:focus { border: 1px solid %4; }

        QToolButton {
            background-color: transparent; color: %9;
            border: none; border-radius: 2px; padding: 2px 4px;
        }
        QToolButton:hover { background-color: %10; }
        QToolButton:checked { background-color: %11; }
        QToolButton:disabled { color: %7; }
    )")
    .arg(c.buttonBg.name())          // %1  button bg
    .arg(c.buttonText.name())        // %2  button text (was: selectionText — WRONG)
    .arg(c.buttonBorder.name())      // %3  button border
    .arg(c.accentHover.name())       // %4  hover border / focus border
    .arg(c.buttonPressed.name())     // %5  pressed
    .arg(c.buttonDisabled.name())    // %6  disabled bg
    .arg(c.textDisabled.name())      // %7  disabled text
    .arg(c.border.name())            // %8  disabled border
    .arg(c.textPrimary.name())       // %9  tool button text
    .arg(c.hoverBg.name())           // %10 tool button hover
    .arg(c.checkedBg.name())         // %11 tool button checked
    .arg(c.buttonHover.name());      // %12 button hover bg
}

inline QString secondaryButtonStylesheet(const ThemeColors& c) {
    return QStringLiteral(R"(
        QPushButton#secondaryButton {
            background-color: transparent; color: %1;
            border: 1px solid %2; border-radius: 2px;
            padding: 4px 12px; min-height: 20px;
        }
        QPushButton#secondaryButton:hover {
            background-color: %3; color: %1;
            border: 1px solid %4;
        }
        QPushButton#secondaryButton:pressed {
            background-color: %3; color: %1;
            border: 1px solid %4;
        }
        QPushButton#secondaryButton:disabled {
            background-color: transparent; color: %5;
            border: 1px solid %5;
        }
        QPushButton#secondaryButton:focus {
            border: 1px solid %4;
        }
    )")
    .arg(c.textPrimary.name())       // %1  text
    .arg(c.accent.name())            // %2  border
    .arg(c.hoverBg.name())           // %3  hover bg
    .arg(c.accentHover.name())       // %4  hover border
    .arg(c.textDisabled.name());     // %5  disabled text
}

// Swatch buttons (color-chip rows in the Colors sections). Form-field look:
// left-aligned 11px label on the *input* background, so it reads as a labeled
// swatch rather than a blue command button. Centralized here so a theme toggle
// restyles them via the global app stylesheet instead of a per-widget inline sheet.
inline QString swatchButtonStylesheet(const ThemeColors& c) {
    return QStringLiteral(R"(
        QPushButton#swatchButton {
            text-align: left; font-size: 11px;
            background-color: %1; color: %2;
            border: 1px solid %3; border-radius: 2px;
            padding: 2px 6px;
        }
        QPushButton#swatchButton:hover {
            border: 1px solid %4;
        }
        QPushButton#swatchButton:disabled {
            background-color: %1; color: %5;
        }
    )")
        .arg(c.inputBg.name())       // %1  background (matches input field)
        .arg(c.textPrimary.name())   // %2  label text
        .arg(c.inputBorder.name())   // %3  border
        .arg(c.accent.name())        // %4  hover border
        .arg(c.textDisabled.name()); // %5  disabled text
}

inline QString tabStylesheet(const ThemeColors& c) {
    return QStringLiteral(R"(
        QTabWidget::pane { border: 1px solid %1; background: %2; }
        QTabBar::tab {
            background-color: %3; color: %4;
            border: 1px solid %1; border-bottom: none;
            padding: 4px 10px; margin-right: -1px;
        }
        QTabBar::tab:selected {
            background-color: %2; color: %5;
            border-top: 2px solid %6;
        }
        QTabBar::tab:hover:!selected { background-color: %7; color: %8; }
    )")
    .arg(c.border.name())            // %1  border / pane border
    .arg(c.surfaceBg.name())         // %2  pane bg / selected tab bg
    .arg(c.tabBg.name())             // %3  unselected tab bg
    .arg(c.tabText.name())           // %4  unselected tab text
    .arg(c.tabSelectedText.name())   // %5  selected tab text (was: selectionText)
    .arg(c.accent.name())            // %6  selected top accent
    .arg(c.tabHoverBg.name())        // %7  hover bg
    .arg(c.textPrimary.name());      // %8  hover text
}

inline QString statusStylesheet(const ThemeColors& c) {
    return QStringLiteral(R"(
        QStatusBar { background-color: %1; color: %2; border: none; }
        QStatusBar::item { border: none; }
        QStatusBar QLabel { color: %2; background: transparent; }

        QToolTip { background-color: %3; color: %4; border: 1px solid %5; padding: 4px 8px; }

        QDockWidget { color: %4; border: none; }
        QDockWidget::title { background: %6; border-bottom: 1px solid %5; padding: 4px; }

        QScrollArea { border: none; background: transparent; }
        QSplitter::handle { background-color: %5; }
        QMessageBox { background-color: %7; color: %4; }
        QFrame[frameShape="4"] { color: %5; }
    )")
    .arg(c.statusBg.name())          // %1  status bar bg
    .arg(c.statusText.name())        // %2  status bar text
    .arg(c.panelBg.name())           // %3  tooltip bg
    .arg(c.textPrimary.name())       // %4  tooltip/dock text
    .arg(c.border.name())            // %5  border
    .arg(c.dockTitleBg.name())       // %6  dock title bg
    .arg(c.surfaceBg.name());        // %7  message box bg
}

// ---------------------------------------------------------------------------
// Public entry point — concatenates all chunk helpers.
// ---------------------------------------------------------------------------
inline QString buildGlobalStylesheet(const ThemeColors& c) {
    return menuStylesheet(c)
        + sliderStylesheet(c)
        + scrollbarStylesheet(c)
        + inputStylesheet(c)
        + checkboxStylesheet(c)
         + buttonStylesheet(c)
         + secondaryButtonStylesheet(c)
         + swatchButtonStylesheet(c)
         + tabStylesheet(c)
        + statusStylesheet(c);
}
