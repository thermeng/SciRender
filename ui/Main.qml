import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import RendererQTUI 1.0 // Registers our custom C++ native view item namespace

ApplicationWindow {
    id: windowRoot
    width: 1024
    height: 768
    minimumWidth: 640
    minimumHeight: 480
    visible: true
    title: "RendererQT"
    color: "#1e1e1e"

    // keyboard shortcuts via Shortcut (window-level, no focus juggling).
    // ApplicationWindow is a QQuickWindow and has no Keys handler / focus property.
    Shortcut { sequence: "R";          onActivated: if (backendRenderer) backendRenderer.resetCamera() }
    Shortcut { sequence: "W";          onActivated: if (backendRenderer) backendRenderer.isWireframe = !backendRenderer.isWireframe }
    Shortcut { sequence: "G";          onActivated: if (backendRenderer) backendRenderer.isGridVisible = !backendRenderer.isGridVisible }
    Shortcut { sequence: "S";          onActivated: if (backendRenderer) { screenshotSaveDialog.currentFile = backendRenderer.generateScreenshotFilename(); screenshotSaveDialog.open(); } }
    Shortcut { sequence: "Left";       onActivated: if (backendRenderer) backendRenderer.azimuth(-5) }
    Shortcut { sequence: "Right";      onActivated: if (backendRenderer) backendRenderer.azimuth(5) }
    Shortcut { sequence: "Up";         onActivated: if (backendRenderer) backendRenderer.elevation(5) }
    Shortcut { sequence: "Down";       onActivated: if (backendRenderer) backendRenderer.elevation(-5) }
    Shortcut { sequence: "Ctrl+=";     onActivated: if (backendRenderer) backendRenderer.dolly(1.1) }
    Shortcut { sequence: "Ctrl+-";     onActivated: if (backendRenderer) backendRenderer.dolly(0.9) }

    // High-Performance Raw OpenGL Output Subwindow Area — wrapped in captureRoot so the
    // screenshot grabs viewport + legend overlays WITHOUT the left rail chrome (Option B).
    Rectangle {
        id: rail
        readonly property int panelWidth: 220
        property bool expanded: false
        property int activeSection: -1   // 0=Lighting 1=Clip 2=View 3=Colormap
        readonly property string sectionTitle:
            activeSection === 0 ? "Lighting" :
            activeSection === 1 ? "Slicing & Clipping" :
            activeSection === 2 ? "View & Display" :
            activeSection === 3 ? "Colormap" :
            activeSection === 4 ? "Vectors" :
            activeSection === 5 ? "Screenshot" : ""
        width: 48 + (expanded ? panelWidth : 0)
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        color: "#262626"
        z: 20

        Component.onCompleted: if(backendRenderer) backendRenderer.setSidebarWidth(width)
        onWidthChanged: if(backendRenderer) backendRenderer.setSidebarWidth(width)

        function toggleSection(n) {
            if (activeSection === n && expanded) {
                expanded = false
                activeSection = -1
            } else {
                activeSection = n
                expanded = true
            }
        }

        component RailButton : ToolButton {
            id: rootRailBtn
            hoverEnabled: true
            Accessible.role: Accessible.Button
            Accessible.name: ToolTip.text
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            width: 48; height: 44
            font.pixelSize: 20
            property bool active: false
            background: Rectangle {
                color: rootRailBtn.active ? "#3a3a3a" : (rootRailBtn.hovered ? "#333333" : "transparent")
                Rectangle { // active accent bar
                    width: 3; height: parent.height
                    color: "#4a90d9"
                    visible: rootRailBtn.active
                }
            }
        }

        // ---- Icon strip ----
        Column {
            id: iconStrip
            width: 48
            anchors.top: parent.top
            anchors.left: parent.left
            spacing: 2

            RailButton { text: "\u{1F4C2}"; ToolTip.text: "Open Mesh"; ToolTip.visible: hovered; onClicked: fileDialog.open() }
            RailButton { text: "\u{1F4A1}"; ToolTip.text: "Lighting"; ToolTip.visible: hovered; active: rail.activeSection === 0; onClicked: rail.toggleSection(0) }
            RailButton { text: "\u2702";    ToolTip.text: "Slicing & Clipping"; ToolTip.visible: hovered; active: rail.activeSection === 1; onClicked: rail.toggleSection(1) }
            RailButton { text: "\u{1F441}"; ToolTip.text: "View & Display"; ToolTip.visible: hovered; active: rail.activeSection === 2; onClicked: rail.toggleSection(2) }
            RailButton { text: "\u{1F3A8}"; ToolTip.text: "Colormap"; ToolTip.visible: hovered; active: rail.activeSection === 3; onClicked: rail.toggleSection(3) }
            RailButton { text: "\u{27A1}";    ToolTip.text: "Vectors"; ToolTip.visible: hovered; active: rail.activeSection === 4; onClicked: rail.toggleSection(4) }
            RailButton { text: "\u{1F4F7}"; ToolTip.text: "Screenshot"; ToolTip.visible: hovered; active: rail.activeSection === 5; onClicked: rail.toggleSection(5) }
        }

        // ---- Docked content panel (slides out to the right of the icon strip) ----
        Rectangle {
            id: panel
            width: rail.panelWidth
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: iconStrip.right
            color: "#2b2b2b"
            border.color: "#444"
            visible: rail.expanded
            clip: true

            // Panel header: section title + close button
            Row {
                id: panelHeader
                height: 32
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: 10
                anchors.rightMargin: 6
                spacing: 6
                Text {
                    text: rail.sectionTitle
                    color: "#dddddd"
                    font.pixelSize: 12
                    font.bold: true
                    elide: Text.ElideRight
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - closeBtn.width - parent.spacing
                }
                ToolButton {
                    id: closeBtn
                    text: "\u00D7"
                    font.pixelSize: 16
                    width: 28; height: 28
                    anchors.verticalCenter: parent.verticalCenter
                    hoverEnabled: true
                    Accessible.role: Accessible.Button
                    Accessible.name: "Collapse panel"
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    onClicked: rail.toggleSection(rail.activeSection)
                }
            }

            component LightSlider : Row {
                id: rootLightSlider
                required property string label
                required property real value
                required property real from
                required property real to
                required property real step
                required property var onSet
                spacing: 6
                Text { text: rootLightSlider.label; color: "#cccccc"; font.pixelSize: 11; width: 64; elide: Text.ElideRight }
                Slider { width: 96; from: rootLightSlider.from; to: rootLightSlider.to; stepSize: rootLightSlider.step; value: rootLightSlider.value; onMoved: rootLightSlider.onSet(value) }
                Text { text: rootLightSlider.value.toFixed(1); color: "#999999"; font.pixelSize: 10; width: 28 }
            }

            component ClipSlider : Row {
                id: rootClipSlider
                required property string label
                required property real value
                required property real from
                required property real to
                required property var onSet
                spacing: 6
                Text { text: rootClipSlider.label; color: "#cccccc"; font.pixelSize: 11; width: 56; elide: Text.ElideRight }
                Slider {
                    id: clipSlider
                    width: 84; from: rootClipSlider.from; to: rootClipSlider.to; value: rootClipSlider.value
                    onMoved: rootClipSlider.onSet(value)
                }
                TextField {
                    id: clipValueField
                    width: 48
                    font.pixelSize: 11
                    color: "#cccccc"
                    horizontalAlignment: Text.AlignRight
                    selectByMouse: true
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                    validator: DoubleValidator {
                        bottom: rootClipSlider.from
                        top: rootClipSlider.to
                        notation: DoubleValidator.StandardNotation
                    }
                    text: clipValueField.activeFocus ? clipValueField.text : rootClipSlider.value.toFixed(3)
                    onAccepted: commitClipValue()
                    onEditingFinished: commitClipValue()

                    function commitClipValue() {
                        let v = parseFloat(clipValueField.text)
                        if (isNaN(v)) { clipValueField.text = rootClipSlider.value.toFixed(3); return }
                        v = Math.min(rootClipSlider.to, Math.max(rootClipSlider.from, v))
                        rootClipSlider.onSet(v)
                    }
                }
            }

            Flickable {
                anchors.top: panelHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                contentWidth: width
                contentHeight: panelCol.height
                clip: true
                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                    width: 10
                }
                Column {
                    id: panelCol
                    x: 10
                    width: parent.width - 20
                    spacing: 4
                    Item { height: 8 }

                    // Lighting Controls — Light Kit
                    Column {
                        visible: rail.activeSection === 0
                        spacing: 4
                        width: parent.width
                        Button { text: "Reset"; onClicked: backendRenderer.resetLighting() }
                        CheckBox { text: "Light Markers"; checked: backendRenderer ? backendRenderer.showLightMarkers : false; onToggled: backendRenderer.showLightMarkers = checked }
                        Item { height: 4 }
                        LightSlider { label: "Int (Key)"; value: backendRenderer ? backendRenderer.lightKeyIntensity : 0; from: 0; to: 1;    step: 0.01; onSet: v => backendRenderer.lightKeyIntensity = v }
                        LightSlider { label: "Warm";      value: backendRenderer ? backendRenderer.lightWarm : 0;          from: 0; to: 1;    step: 0.01; onSet: v => backendRenderer.lightWarm = v }
                        LightSlider { label: "K:F";       value: backendRenderer ? backendRenderer.lightKF : 0;            from: 1; to: 15;   step: 0.1;  onSet: v => backendRenderer.lightKF = v }
                        LightSlider { label: "K:B";       value: backendRenderer ? backendRenderer.lightKB : 0;            from: 1; to: 15;   step: 0.1;  onSet: v => backendRenderer.lightKB = v }
                        LightSlider { label: "K:H";       value: backendRenderer ? backendRenderer.lightKH : 0;            from: 1; to: 15;   step: 0.1;  onSet: v => backendRenderer.lightKH = v }
                        LightSlider { label: "Key Az";  value: backendRenderer ? backendRenderer.lightKeyAzimuth : 0;    from: -180; to: 180; step: 1; onSet: v => backendRenderer.lightKeyAzimuth = v }
                        LightSlider { label: "Key El";  value: backendRenderer ? backendRenderer.lightKeyElevation : 0;   from: -90;  to: 90;  step: 1; onSet: v => backendRenderer.lightKeyElevation = v }
                        LightSlider { label: "Fill Az"; value: backendRenderer ? backendRenderer.lightFillAzimuth : 0;    from: -180; to: 180; step: 1; onSet: v => backendRenderer.lightFillAzimuth = v }
                        LightSlider { label: "Fill El"; value: backendRenderer ? backendRenderer.lightFillElevation : 0;  from: -90;  to: 90;  step: 1; onSet: v => backendRenderer.lightFillElevation = v }
                        LightSlider { label: "Back Az"; value: backendRenderer ? backendRenderer.lightBackAzimuth : 0;    from: -180; to: 180; step: 1; onSet: v => backendRenderer.lightBackAzimuth = v }
                        LightSlider { label: "Back El"; value: backendRenderer ? backendRenderer.lightBackElevation : 0;  from: -90;  to: 90;  step: 1; onSet: v => backendRenderer.lightBackElevation = v }
                        LightSlider { label: "Head Az"; value: backendRenderer ? backendRenderer.lightHeadAzimuth : 0;    from: -180; to: 180; step: 1; onSet: v => backendRenderer.lightHeadAzimuth = v }
                        LightSlider { label: "Head El"; value: backendRenderer ? backendRenderer.lightHeadElevation : 0;  from: -90;  to: 90;  step: 1; onSet: v => backendRenderer.lightHeadElevation = v }
                        LightSlider { label: "Ambient"; value: backendRenderer ? backendRenderer.matAmbient : 0;          from: 0; to: 1;    step: 0.01; onSet: v => backendRenderer.matAmbient = v }
                        LightSlider { label: "Diffuse"; value: backendRenderer ? backendRenderer.matDiffuse : 0;          from: 0; to: 1;    step: 0.01; onSet: v => backendRenderer.matDiffuse = v }
                    }

                    // Slicing & Clipping Controls
                    Column {
                        visible: rail.activeSection === 1
                        spacing: 4
                        width: parent.width
                        CheckBox { text: "Enable Clipping"; checked: backendRenderer ? backendRenderer.clipEnabled : false; onToggled: backendRenderer.clipEnabled = checked }
                        Text { text: "Cut planes (world units)"; color: "#888"; font.pixelSize: 10 }
                        ClipSlider { label: "Slice X"; value: backendRenderer ? backendRenderer.sliceHeightX : 0; from: backendRenderer ? backendRenderer.worldMinX : 0; to: backendRenderer ? backendRenderer.worldMaxX : 1; onSet: v => backendRenderer.sliceHeightX = v }
                        ClipSlider { label: "Slice Y"; value: backendRenderer ? backendRenderer.sliceHeightY : 0; from: backendRenderer ? backendRenderer.worldMinY : 0; to: backendRenderer ? backendRenderer.worldMaxY : 1; onSet: v => backendRenderer.sliceHeightY = v }
                        ClipSlider { label: "Slice Z"; value: backendRenderer ? backendRenderer.sliceHeightZ : 0; from: backendRenderer ? backendRenderer.worldMinZ : 0; to: backendRenderer ? backendRenderer.worldMaxZ : 1; onSet: v => backendRenderer.sliceHeightZ = v }
                        Text { text: "Keep side"; color: "#888"; font.pixelSize: 10 }
                        Row { spacing: 8
                            CheckBox { text: "Inv X"; checked: backendRenderer ? backendRenderer.invertX : false; onToggled: backendRenderer.invertX = checked }
                            CheckBox { text: "Inv Y"; checked: backendRenderer ? backendRenderer.invertY : false; onToggled: backendRenderer.invertY = checked }
                            CheckBox { text: "Inv Z"; checked: backendRenderer ? backendRenderer.invertZ : false; onToggled: backendRenderer.invertZ = checked }
                        }
                    }

                    // View & Display Controls
                    Column {
                        id: viewCol
                        visible: rail.activeSection === 2
                        spacing: 4
                        width: parent.width
                        property real rollPrev: 0
                        Text { text: "Orthographic view"; color: "#888"; font.pixelSize: 10 }
                        Row { spacing: 6
                            Button { text: "+X"; width: 48; onClicked: backendRenderer.snapToOrthoView(0) }
                            Button { text: "-X"; width: 48; onClicked: backendRenderer.snapToOrthoView(1) }
                            Button { text: "+Y"; width: 48; onClicked: backendRenderer.snapToOrthoView(2) }
                        }
                        Row { spacing: 6
                            Button { text: "-Y"; width: 48; onClicked: backendRenderer.snapToOrthoView(3) }
                            Button { text: "+Z"; width: 48; onClicked: backendRenderer.snapToOrthoView(4) }
                            Button { text: "-Z"; width: 48; onClicked: backendRenderer.snapToOrthoView(5) }
                        }
                        Text { text: "Quick axis snap"; color: "#888"; font.pixelSize: 10 }
                        Row { spacing: 6
                            Button { text: "X"; width: 48; onClicked: backendRenderer.snapToAxisView(0, false) }
                            Button { text: "Y"; width: 48; onClicked: backendRenderer.snapToAxisView(1, false) }
                            Button { text: "Z"; width: 48; onClicked: backendRenderer.snapToAxisView(2, false) }
                        }
                        Text { text: "Display"; color: "#888"; font.pixelSize: 10 }
                        CheckBox { text: "Wireframe"; checked: backendRenderer ? backendRenderer.isWireframe : false; onToggled: backendRenderer.isWireframe = checked }
                        CheckBox { text: "Grid";      checked: backendRenderer ? backendRenderer.isGridVisible : false; onToggled: backendRenderer.isGridVisible = checked }
                        CheckBox { text: "Surface";   checked: backendRenderer ? backendRenderer.isSurfaceVisible : false; onToggled: backendRenderer.isSurfaceVisible = checked }
                        CheckBox { text: "Auto-Rotate"; checked: backendRenderer ? backendRenderer.autoRotate : false; onToggled: backendRenderer.autoRotate = checked }
                        Text { text: "Scene"; color: "#888"; font.pixelSize: 10 }
                        Row { spacing: 6
                            Button { text: "Reset Cam"; width: 92; onClicked: backendRenderer.resetCamera() }
                        }
                        Button { text: "Background Color"; width: parent.width; onClicked: bgDialog.open() }
                        Text { text: "Camera roll"; color: "#888"; font.pixelSize: 10 }
                        LightSlider {
                            label: "Roll"; value: 0; from: -180; to: 180; step: 1
                            onSet: v => { backendRenderer.roll(v - viewCol.rollPrev); viewCol.rollPrev = v; }
                        }
                        Text { text: "Overlays"; color: "#888"; font.pixelSize: 10 }
                        CheckBox { text: "Gizmo"; checked: backendRenderer ? backendRenderer.isGizmoVisible : true; onToggled: backendRenderer.isGizmoVisible = checked }
                        CheckBox { text: "FPS HUD"; checked: backendRenderer ? backendRenderer.showFps : false; onToggled: backendRenderer.showFps = checked }
                        Text { text: "Colors"; color: "#888"; font.pixelSize: 10 }
                        Row { spacing: 6
                            Button { text: "Wireframe Color"; width: 100; onClicked: meshColorDialog.open() }
                            Button { text: "Surface Color"; width: 100; onClicked: surfaceColorDialog.open() }
                        }
                    }

                    // Colormap Selector Panel
                    Column {
                        visible: rail.activeSection === 3
                        spacing: 6
                        width: parent.width
                        Text { text: "Scalar field"; color: "#888"; font.pixelSize: 10 }
                        ComboBox {
                            id: scalarCombo
                            width: parent.width
                            enabled: backendRenderer ? backendRenderer.meshHasScalars : false
                            model: backendRenderer ? backendRenderer.availableScalars : []
                            currentIndex: backendRenderer ? backendRenderer.availableScalars.indexOf(backendRenderer.activeScalarName) : -1
                            onActivated: index => backendRenderer.setActiveScalarField(model[index])
                        }
                        Text { text: "Colormap"; color: "#888"; font.pixelSize: 10 }
                        ComboBox {
                            id: colormapCombo
                            width: parent.width
                            property var entries: {
                                var names = backendRenderer.getColormapNames();
                                var e = [];
                                for (var i = 0; i < names.length; ++i)
                                    e.push({ name: names[i], uri: backendRenderer.getColormapPreviewUri(i) });
                                return e;
                            }
                            model: entries
                            textRole: "name"
                            currentIndex: backendRenderer ? backendRenderer.colormapChoice : 0
                            onActivated: index => backendRenderer.colormapChoice = index
                            contentItem: RowLayout {
                                spacing: 8
                                Image {
                                    source: colormapCombo.entries[colormapCombo.currentIndex] ? colormapCombo.entries[colormapCombo.currentIndex].uri : ""
                                    sourceSize.width: 128; sourceSize.height: 16
                                    fillMode: Image.Stretch
                                    Layout.preferredWidth: 48; Layout.preferredHeight: 12
                                    verticalAlignment: Image.AlignVCenter
                                }
                                Text {
                                    text: colormapCombo.displayText
                                    color: "#ddd"; font.pixelSize: 12
                                    Layout.fillWidth: true; verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }
                            }
                            delegate: ItemDelegate {
                                width: colormapCombo.width
                                height: 24
                                RowLayout {
                                    spacing: 8
                                    anchors.fill: parent
                                    Image {
                                        source: modelData.uri
                                        sourceSize.width: 128; sourceSize.height: 16
                                        fillMode: Image.Stretch
                                        Layout.preferredWidth: 48; Layout.preferredHeight: 12
                                        verticalAlignment: Image.AlignVCenter
                                    }
                                    Text {
                                        text: modelData.name
                                        color: "#ddd"; font.pixelSize: 12
                                        Layout.fillWidth: true; verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }
                                }
                                highlighted: colormapCombo.highlightedIndex === index
                            }
                        }
                        CheckBox { text: "Reverse palette"; checked: backendRenderer ? backendRenderer.colormapReversed : false; onToggled: backendRenderer.colormapReversed = checked }
                        Text { text: "Scalar filter"; color: "#888"; font.pixelSize: 10 }
                        ClipSlider { label: "Min"; value: backendRenderer ? backendRenderer.filterMin : 0; from: backendRenderer ? backendRenderer.dataScalarMinQml : 0; to: backendRenderer ? backendRenderer.dataScalarMaxQml : 1; onSet: v => backendRenderer.filterMin = v }
                        ClipSlider { label: "Max"; value: backendRenderer ? backendRenderer.filterMax : 0; from: backendRenderer ? backendRenderer.dataScalarMinQml : 0; to: backendRenderer ? backendRenderer.dataScalarMaxQml : 1; onSet: v => backendRenderer.filterMax = v }
                    }

                    // Screenshot Controls
                    Column {
                        visible: rail.activeSection === 5
                        spacing: 4
                        width: parent.width
                        Button { text: "Save Screenshot"; width: parent.width; onClicked: { screenshotSaveDialog.currentFile = backendRenderer.generateScreenshotFilename(); screenshotSaveDialog.open(); } }
                        Text { text: "Options"; color: "#888"; font.pixelSize: 10 }
                        CheckBox { text: "Transparent (PNG)"; checked: backendRenderer ? backendRenderer.screenshotTransparent : false; onToggled: backendRenderer.screenshotTransparent = checked }
                        LightSlider { label: "JPEG Q"; value: backendRenderer ? backendRenderer.screenshotQuality : 95; from: 1; to: 100; step: 1; onSet: v => backendRenderer.screenshotQuality = v }
                    }

                    // Vectors Panel
                    Column {
                        visible: rail.activeSection === 4
                        spacing: 4
                        width: parent.width
                        CheckBox { text: "Show vectors"; checked: backendRenderer ? backendRenderer.showVectors : false; onToggled: backendRenderer.showVectors = checked }
                        Text { text: "Field"; color: "#888"; font.pixelSize: 10 }
                        ComboBox {
                            width: parent.width
                            model: backendRenderer ? backendRenderer.availableVectors : []
                            currentIndex: backendRenderer ? Math.max(0, availableVectors.indexOf(backendRenderer.vectorField)) : 0
                            onActivated: backendRenderer.setActiveVectorField(currentText)
                        }
                        Text { text: "Arrow scale"; color: "#888"; font.pixelSize: 10 }
                        LightSlider { label: "Scale"; value: backendRenderer ? backendRenderer.vectorScale : 1.0; from: 0.01; to: 5.0; step: 0.01; onSet: v => backendRenderer.vectorScale = v }
                        Text { text: "Stride (skip every N)"; color: "#888"; font.pixelSize: 10 }
                        LightSlider { label: "Stride"; value: backendRenderer ? backendRenderer.vectorStride : 1; from: 1; to: 20; step: 1; onSet: v => backendRenderer.vectorStride = v }
                        Row { spacing: 6
                            Button { text: "Vector Color"; width: 100; onClicked: vectorColorDialog.open() }
                        }
                        Text { text: "Colormap"; color: "#888"; font.pixelSize: 10 }
                        CheckBox { text: "Color by magnitude"; checked: backendRenderer ? backendRenderer.vectorUseColormap : false; onToggled: backendRenderer.vectorUseColormap = checked }
                        ComboBox {
                            id: vectorColormapCombo
                            width: parent.width
                            enabled: backendRenderer ? backendRenderer.vectorUseColormap : false
                            property var entries: {
                                var names = backendRenderer.getColormapNames();
                                var e = [];
                                for (var i = 0; i < names.length; ++i)
                                    e.push({ name: names[i], uri: backendRenderer.getColormapPreviewUri(i) });
                                return e;
                            }
                            model: entries
                            textRole: "name"
                            currentIndex: backendRenderer ? backendRenderer.vectorColormapChoice : 0
                            onActivated: index => backendRenderer.vectorColormapChoice = index
                            contentItem: RowLayout {
                                spacing: 8
                                Image {
                                    source: vectorColormapCombo.entries[vectorColormapCombo.currentIndex] ? vectorColormapCombo.entries[vectorColormapCombo.currentIndex].uri : ""
                                    sourceSize.width: 128; sourceSize.height: 16
                                    fillMode: Image.Stretch
                                    Layout.preferredWidth: 48; Layout.preferredHeight: 12
                                    verticalAlignment: Image.AlignVCenter
                                }
                                Text {
                                    text: vectorColormapCombo.displayText
                                    color: "#ddd"; font.pixelSize: 12
                                    Layout.fillWidth: true; verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }
                            }
                            delegate: ItemDelegate {
                                width: vectorColormapCombo.width
                                height: 24
                                RowLayout {
                                    spacing: 8
                                    anchors.fill: parent
                                    Image {
                                        source: modelData.uri
                                        sourceSize.width: 128; sourceSize.height: 16
                                        fillMode: Image.Stretch
                                        Layout.preferredWidth: 48; Layout.preferredHeight: 12
                                        verticalAlignment: Image.AlignVCenter
                                    }
                                    Text {
                                        text: modelData.name
                                        color: "#ddd"; font.pixelSize: 12
                                        Layout.fillWidth: true; verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }
                                }
                                highlighted: vectorColormapCombo.highlightedIndex === index
                            }
                        }
                        CheckBox { text: "Reverse palette"; enabled: backendRenderer ? backendRenderer.vectorUseColormap : false; checked: backendRenderer ? backendRenderer.vectorColormapReversed : false; onToggled: backendRenderer.vectorColormapReversed = checked }
                    }
                }
            }
        }

        ColorDialog {
            id: bgDialog
            selectedColor: backendRenderer ? backendRenderer.bgColor : "#000000"
            onAccepted: backendRenderer.bgColor = selectedColor
        }
        ColorDialog {
            id: meshColorDialog
            selectedColor: backendRenderer ? backendRenderer.meshColor : "#66e666"
            onAccepted: backendRenderer.meshColor = selectedColor
        }
        ColorDialog {
            id: surfaceColorDialog
            selectedColor: backendRenderer ? backendRenderer.surfaceColor : "#ffffff"
            onAccepted: backendRenderer.surfaceColor = selectedColor
        }
        ColorDialog {
            id: vectorColorDialog
            selectedColor: backendRenderer ? backendRenderer.vectorColor : "#3399ff"
            onAccepted: backendRenderer.vectorColor = selectedColor
        }
    }

    // Helper JavaScript function to strip out protocols securely across OS environments
    function urlToPath(urlStr) {
        if (Qt.platform.os === "windows") {
            return urlStr.replace("file:///", "");
        } else {
            return urlStr.replace("file://", "");
        }
    }

    Item {
        id: captureRoot
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.left: rail.right

        ViewportVisualizer {
            id: openGLViewport
            anchors.fill: parent
            renderer: backendRenderer // Links instance reference directly to C++ target

        // Drop zone overlay for dragging raw STL/VTK files directly into the viewport
        DropArea {
            anchors.fill: parent
            onDropped: (drop) => {
                if (drop.hasUrls) {
                    // Fixes cross-platform file path resolution for Win/Mac/Linux
                    let urlStr = drop.urls[0].toString();
                    let cleanPath = urlStr.startsWith("file://") ? urlToPath(urlStr) : urlStr;
                    backendRenderer.loadMesh(cleanPath);
                    drop.acceptProposedAction();
                }
            }
        }
    }

    FileDialog {
        id: fileDialog
        title: "Load Mesh"
        nameFilters: ["Mesh files (*.stl *.vtk)", "All files (*)"]
        onAccepted: {
            let urlStr = selectedFile.toString();
            let cleanPath = urlStr.startsWith("file://") ? windowRoot.urlToPath(urlStr) : urlStr;
            backendRenderer.loadMesh(cleanPath);
        }
    }

    FileDialog {
        id: screenshotSaveDialog
        title: "Save Screenshot"
        fileMode: FileDialog.SaveFile
        nameFilters: ["PNG Images (*.png)", "JPEG Images (*.jpg *.jpeg)", "BMP Images (*.bmp)", "All files (*)"]
        onAccepted: {
            let urlStr = selectedFile.toString();
            let cleanPath = urlStr.startsWith("file://") ? windowRoot.urlToPath(urlStr) : urlStr;
            // Option B — grab viewport + legend subtree (clean crop, no rail).
            // Transparent PNG keeps the original FBO-only path (legends excluded, since
            // QML overlays have no alpha to composite).
            if (backendRenderer && backendRenderer.screenshotTransparent) {
                backendRenderer.requestScreenshot(cleanPath);
            } else {
                captureRoot.grabToImage(function(grabResult) { grabResult.saveToFile(cleanPath); });
            }
        }
    }

    // turntable — ~30fps azimuth nudge while autoRotate is on
    Timer {
        interval: 33
        running: backendRenderer ? backendRenderer.autoRotate : false
        repeat: true
        onTriggered: if (backendRenderer) backendRenderer.azimuth(0.6)
    }

    // FPS HUD needs continuous frames; drive repaints only while shown
    Timer {
        interval: 16
        running: backendRenderer ? backendRenderer.showFps : false
        repeat: true
        onTriggered: openGLViewport.update()
    }

    // Centered drop prompt context overlay
    // on-screen perf HUD (top-right)
    Rectangle {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        width: hudText.width + 16
        height: hudText.height + 8
        color: "#000000aa"
        radius: 4
        visible: backendRenderer ? backendRenderer.showFps : false
        Text {
            id: hudText
            anchors.centerIn: parent
            text: backendRenderer ? backendRenderer.fpsText : ""
            color: "#7CFC00"
            font.pixelSize: 12
            font.family: "Consolas, Menlo, monospace"
        }
    }

    // Centered drop prompt context overlay
    Text {
        text: "Drag & Drop a .stl / .vtk file, or use \"Open Mesh\""
        color: "#888888"
        font.pixelSize: 16
        anchors.centerIn: parent
        visible: backendRenderer ? !backendRenderer.hasMeshLoaded : true
    }

    // Colorbar legend overlay (shown only for scalar-valued meshes)
    Column {
        id: colorbar
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        spacing: 4
        visible: backendRenderer ? (backendRenderer.hasMeshLoaded && backendRenderer.meshHasScalars) : false

        Text {
            text: backendRenderer ? backendRenderer.activeScalarName : ""
            color: "#dddddd"
            font.pixelSize: 12
        }

        Row {
            spacing: 8

            Rectangle {
                id: gradientBar
                width: 20
                height: 200
                border.color: "#555555"
                border.width: 1
                clip: true

                Repeater {
                    model: backendRenderer ? backendRenderer.colormapStops : []
                    delegate: Rectangle {
                        required property var modelData // Added to eliminate compiler mapping warning
                        width: 20
                        height: backendRenderer.colormapStops.length ? (200 / backendRenderer.colormapStops.length) : 0
                        y: 200 - (modelData[0] * 200 + height)
                        color: Qt.rgba(modelData[1], modelData[2], modelData[3], 1.0)
                    }
                }
            }

            Item {
                width: 55
                height: gradientBar.height

                Text {
                    text: backendRenderer ? backendRenderer.dataScalarMaxQml.toFixed(3) : ""
                    color: "#dddddd"
                    font.pixelSize: 11
                    anchors.left: parent.left
                    anchors.top: parent.top
                }

                Text {
                    text: backendRenderer ? backendRenderer.dataScalarMinQml.toFixed(3) : ""
                    color: "#dddddd"
                    font.pixelSize: 11
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                }
            }
        }
    }

    // Vector magnitude colorbar (SEPARATE legend, top-right, shown only when color-by-magnitude is on)
    Column {
        id: vectorColorbar
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        spacing: 4
        // gate on the CURRENT mesh actually having vector fields, not just the
        // persisted showVectors/vectorUseColormap toggles — otherwise the bar stays up after
        // loading a new mesh that has no vectors.
        visible: backendRenderer ? (backendRenderer.showVectors && backendRenderer.vectorUseColormap && backendRenderer.hasMeshLoaded && backendRenderer.availableVectors.length > 0) : false

        Text {
            text: (backendRenderer ? backendRenderer.vectorFieldName : "") + " | magnitude"
            color: "#dddddd"
            font.pixelSize: 12
        }

        Row {
            spacing: 8

            Rectangle {
                id: vectorGradientBar
                width: 20
                height: 200
                border.color: "#555555"
                border.width: 1
                clip: true

                Repeater {
                    model: backendRenderer ? backendRenderer.vectorColormapStops : []
                    delegate: Rectangle {
                        required property var modelData
                        width: 20
                        height: backendRenderer.vectorColormapStops.length ? (200 / backendRenderer.vectorColormapStops.length) : 0
                        y: 200 - (modelData[0] * 200 + height)
                        color: Qt.rgba(modelData[1], modelData[2], modelData[3], 1.0)
                    }
                }
            }

            Item {
                width: 60
                height: vectorGradientBar.height

                Text {
                    text: backendRenderer ? backendRenderer.vectorMagMax.toFixed(3) : ""
                    color: "#dddddd"
                    font.pixelSize: 11
                    anchors.left: parent.left
                    anchors.top: parent.top
                }

                Text {
                    text: backendRenderer ? backendRenderer.vectorMagMin.toFixed(3) : ""
                    color: "#dddddd"
                    font.pixelSize: 11
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                }
            }
        }
    }

    } // captureRoot

    // ---- Menu bar ----
    menuBar: MenuBar {
        Menu {
            title: "File"
            MenuItem { text: "Open Mesh..."; onTriggered: fileDialog.open() }
            Menu {
                title: "Open Recent"
                enabled: backendRenderer ? backendRenderer.recentFiles.length > 0 : false
                // rebuild the submenu from recentFiles each open
                onAboutToShow: {
                    // QQuickMenu has no clearMenuItems(); remove all current items
                    while (count > 0) { let it = itemAt(0); removeItem(it); it.destroy(); }
                    const list = backendRenderer ? backendRenderer.recentFiles : [];
                    for (let i = 0; i < list.length; ++i) {
                        const p = list[i];
                        const item = recentItem.createObject(this, { "text": p });
                        item.triggered.connect(() => backendRenderer.openRecent(p));
                        addItem(item);
                    }
                }
                Component { id: recentItem; MenuItem {} }
            }
            MenuItem { text: "Clear"; onTriggered: backendRenderer.clearMeshes() }
            MenuSeparator {}
            MenuItem { text: "Exit"; onTriggered: Qt.quit() }
        }
        Menu {
            title: "View"
            MenuItem { text: "Lighting"; onTriggered: rail.toggleSection(0) }
            MenuItem { text: "Slicing & Clipping"; onTriggered: rail.toggleSection(1) }
            MenuSeparator {}
            MenuItem { text: "Reset Camera"; onTriggered: backendRenderer.resetCamera() }
        }
    }

    // ---- Status bar ----
    footer: Rectangle {
        color: "#2a2a2a"
        height: 22
        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 8
            color: "#bbbbbb"
            font.pixelSize: 11
            text: (backendRenderer && backendRenderer.hasMeshLoaded)
                ? "Mesh: " + backendRenderer.currentMeshName + "   |   Pts: " + backendRenderer.pointCount + "   |   Tris: " + backendRenderer.triangleCount
                : "No mesh loaded"
        }
    }
}