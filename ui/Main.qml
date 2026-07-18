import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SciRenderUI 1.0 // Registers our custom C++ native view item namespace

ApplicationWindow {
    id: windowRoot
    width: 1024
    height: 768
    minimumWidth: 640
    minimumHeight: 480
    visible: true
    title: "SciRender"
    color: "#1e1e1e"

    // keyboard shortcuts via Shortcut (window-level, no focus juggling).
    // ApplicationWindow is a QQuickWindow and has no Keys handler / focus property.
    Shortcut { sequence: "R";          onActivated: if (backendSettings) backendSettings.resetCamera() }
    Shortcut { sequence: "W";          onActivated: if (backendSettings) backendSettings.isWireframe = !backendSettings.isWireframe }
    Shortcut { sequence: "G";          onActivated: if (backendSettings) backendSettings.isGridVisible = !backendSettings.isGridVisible }
    Shortcut { sequence: "S";          onActivated: if (backendSettings) { screenshotSaveDialog.currentFile = backendSettings.generateScreenshotFilename(); screenshotSaveDialog.open(); } }
    Shortcut { sequence: "Left";       onActivated: if (backendSettings) backendSettings.azimuth(-5) }
    Shortcut { sequence: "Right";      onActivated: if (backendSettings) backendSettings.azimuth(5) }
    Shortcut { sequence: "Up";         onActivated: if (backendSettings) backendSettings.elevation(5) }
    Shortcut { sequence: "Down";       onActivated: if (backendSettings) backendSettings.elevation(-5) }
    Shortcut { sequence: "Ctrl+=";     onActivated: if (backendSettings) backendSettings.dolly(1.1) }
    Shortcut { sequence: "Ctrl+-";     onActivated: if (backendSettings) backendSettings.dolly(0.9) }

    // High-Performance Raw OpenGL Output Subwindow Area ? wrapped in captureRoot so the
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
            activeSection === 5 ? "Screenshot" :
            activeSection === 6 ? "Mesh Info" : ""
        width: 48 + (expanded ? panelWidth : 0)
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        color: "#262626"
        z: 20

        Component.onCompleted: if(backendSettings) backendSettings.setSidebarWidth(width)
        onWidthChanged: if(backendSettings) backendSettings.setSidebarWidth(width)

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
            RailButton { text: "\u{1F4CA}"; ToolTip.text: "Mesh Info"; ToolTip.visible: hovered; active: rail.activeSection === 6; onClicked: rail.toggleSection(6) }
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

                    // Lighting Controls ? Light Kit
                    Column {
                        visible: rail.activeSection === 0
                        spacing: 4
                        width: parent.width
                        Button { text: "Reset"; onClicked: backendSettings.resetLighting() }
                        CheckBox { text: "Light Markers"; checked: backendSettings ? backendSettings.showLightMarkers : false; onToggled: backendSettings.showLightMarkers = checked }
                        Item { height: 4 }
                        LightSlider { label: "Int (Key)"; value: backendSettings ? backendSettings.lightKeyIntensity : 0; from: 0; to: 1;    step: 0.01; onSet: v => backendSettings.lightKeyIntensity = v }
                        LightSlider { label: "Warm";      value: backendSettings ? backendSettings.lightWarm : 0;          from: 0; to: 1;    step: 0.01; onSet: v => backendSettings.lightWarm = v }
                        LightSlider { label: "K:F";       value: backendSettings ? backendSettings.lightKF : 0;            from: 1; to: 15;   step: 0.1;  onSet: v => backendSettings.lightKF = v }
                        LightSlider { label: "K:B";       value: backendSettings ? backendSettings.lightKB : 0;            from: 1; to: 15;   step: 0.1;  onSet: v => backendSettings.lightKB = v }
                        LightSlider { label: "K:H";       value: backendSettings ? backendSettings.lightKH : 0;            from: 1; to: 15;   step: 0.1;  onSet: v => backendSettings.lightKH = v }
                        LightSlider { label: "Key Az";  value: backendSettings ? backendSettings.lightKeyAzimuth : 0;    from: -180; to: 180; step: 1; onSet: v => backendSettings.lightKeyAzimuth = v }
                        LightSlider { label: "Key El";  value: backendSettings ? backendSettings.lightKeyElevation : 0;   from: -90;  to: 90;  step: 1; onSet: v => backendSettings.lightKeyElevation = v }
                        LightSlider { label: "Fill Az"; value: backendSettings ? backendSettings.lightFillAzimuth : 0;    from: -180; to: 180; step: 1; onSet: v => backendSettings.lightFillAzimuth = v }
                        LightSlider { label: "Fill El"; value: backendSettings ? backendSettings.lightFillElevation : 0;  from: -90;  to: 90;  step: 1; onSet: v => backendSettings.lightFillElevation = v }
                        LightSlider { label: "Back Az"; value: backendSettings ? backendSettings.lightBackAzimuth : 0;    from: -180; to: 180; step: 1; onSet: v => backendSettings.lightBackAzimuth = v }
                        LightSlider { label: "Back El"; value: backendSettings ? backendSettings.lightBackElevation : 0;  from: -90;  to: 90;  step: 1; onSet: v => backendSettings.lightBackElevation = v }
                        LightSlider { label: "Head Az"; value: backendSettings ? backendSettings.lightHeadAzimuth : 0;    from: -180; to: 180; step: 1; onSet: v => backendSettings.lightHeadAzimuth = v }
                        LightSlider { label: "Head El"; value: backendSettings ? backendSettings.lightHeadElevation : 0;  from: -90;  to: 90;  step: 1; onSet: v => backendSettings.lightHeadElevation = v }
                        LightSlider { label: "Ambient"; value: backendSettings ? backendSettings.matAmbient : 0;          from: 0; to: 1;    step: 0.01; onSet: v => backendSettings.matAmbient = v }
                        LightSlider { label: "Diffuse"; value: backendSettings ? backendSettings.matDiffuse : 0;          from: 0; to: 1;    step: 0.01; onSet: v => backendSettings.matDiffuse = v }
                        LightSlider { label: "Specular";  value: backendSettings ? backendSettings.matSpecular : 0;  from: 0; to: 1;    step: 0.01; onSet: v => backendSettings.matSpecular = v }
                        LightSlider { label: "Shininess"; value: backendSettings ? backendSettings.matShininess : 0; from: 1; to: 100;  step: 1;    onSet: v => backendSettings.matShininess = v }
                    }

                    // Slicing & Clipping Controls
                    Column {
                        visible: rail.activeSection === 1
                        spacing: 4
                        width: parent.width
                        CheckBox { text: "Enable Clipping"; checked: backendSettings ? backendSettings.clipEnabled : false; onToggled: backendSettings.clipEnabled = checked }
                        Text { text: "Cut planes (world units)"; color: "#888"; font.pixelSize: 10 }
                        ClipSlider { label: "Slice X"; value: backendSettings ? backendSettings.sliceHeightX : 0; from: backendSettings ? backendSettings.worldMinX : 0; to: backendSettings ? backendSettings.worldMaxX : 1; onSet: v => backendSettings.sliceHeightX = v }
                        ClipSlider { label: "Slice Y"; value: backendSettings ? backendSettings.sliceHeightY : 0; from: backendSettings ? backendSettings.worldMinY : 0; to: backendSettings ? backendSettings.worldMaxY : 1; onSet: v => backendSettings.sliceHeightY = v }
                        ClipSlider { label: "Slice Z"; value: backendSettings ? backendSettings.sliceHeightZ : 0; from: backendSettings ? backendSettings.worldMinZ : 0; to: backendSettings ? backendSettings.worldMaxZ : 1; onSet: v => backendSettings.sliceHeightZ = v }
                        Text { text: "Keep side"; color: "#888"; font.pixelSize: 10 }
                        Row { spacing: 8
                            CheckBox { text: "Inv X"; checked: backendSettings ? backendSettings.invertX : false; onToggled: backendSettings.invertX = checked }
                            CheckBox { text: "Inv Y"; checked: backendSettings ? backendSettings.invertY : false; onToggled: backendSettings.invertY = checked }
                            CheckBox { text: "Inv Z"; checked: backendSettings ? backendSettings.invertZ : false; onToggled: backendSettings.invertZ = checked }
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
                            Button { text: "+X"; width: 48; onClicked: backendSettings.snapToOrthoView(0) }
                            Button { text: "-X"; width: 48; onClicked: backendSettings.snapToOrthoView(1) }
                            Button { text: "+Y"; width: 48; onClicked: backendSettings.snapToOrthoView(2) }
                        }
                        Row { spacing: 6
                            Button { text: "-Y"; width: 48; onClicked: backendSettings.snapToOrthoView(3) }
                            Button { text: "+Z"; width: 48; onClicked: backendSettings.snapToOrthoView(4) }
                            Button { text: "-Z"; width: 48; onClicked: backendSettings.snapToOrthoView(5) }
                        }
                        Text { text: "Quick axis snap"; color: "#888"; font.pixelSize: 10 }
                        Row { spacing: 6
                            Button { text: "X"; width: 48; onClicked: backendSettings.snapToAxisView(0, false) }
                            Button { text: "Y"; width: 48; onClicked: backendSettings.snapToAxisView(1, false) }
                            Button { text: "Z"; width: 48; onClicked: backendSettings.snapToAxisView(2, false) }
                        }
                        Text { text: "Display"; color: "#888"; font.pixelSize: 10 }
                        CheckBox { text: "Wireframe"; checked: backendSettings ? backendSettings.isWireframe : false; onToggled: backendSettings.isWireframe = checked }
                        CheckBox { text: "Grid";      checked: backendSettings ? backendSettings.isGridVisible : false; onToggled: backendSettings.isGridVisible = checked }
                        CheckBox { text: "Surface";   checked: backendSettings ? backendSettings.isSurfaceVisible : false; onToggled: backendSettings.isSurfaceVisible = checked }
                        CheckBox { text: "Auto-Rotate"; checked: backendSettings ? backendSettings.autoRotate : false; onToggled: backendSettings.autoRotate = checked }
                        CheckBox { text: "Level of Detail"; checked: backendSettings ? backendSettings.useLod : true; onToggled: backendSettings.useLod = checked }
                        Text { text: "Scene"; color: "#888"; font.pixelSize: 10 }
                        Row { spacing: 6
                            Button { text: "Reset Cam"; width: 92; onClicked: backendSettings.resetCamera() }
                        }
                        Button { text: "Background Color"; width: parent.width; onClicked: bgDialog.open() }
                        Text { text: "Camera roll"; color: "#888"; font.pixelSize: 10 }
                        LightSlider {
                            label: "Roll"; value: 0; from: -180; to: 180; step: 1
                            onSet: v => { backendSettings.roll(v - viewCol.rollPrev); viewCol.rollPrev = v; }
                        }
                        Text { text: "Overlays"; color: "#888"; font.pixelSize: 10 }
                        CheckBox { text: "Gizmo"; checked: backendSettings ? backendSettings.isGizmoVisible : true; onToggled: backendSettings.isGizmoVisible = checked }
                        CheckBox { text: "FPS HUD"; checked: backendSettings ? backendSettings.showFps : false; onToggled: backendSettings.showFps = checked }
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
                            enabled: backendSettings ? backendSettings.meshHasScalars : false
                            model: backendSettings ? backendSettings.availableScalars : []
                            currentIndex: backendSettings ? backendSettings.availableScalars.indexOf(backendSettings.activeScalarName) : -1
                            onActivated: index => backendSettings.setActiveScalarField(model[index])
                        }
                        Text { text: "Colormap"; color: "#888"; font.pixelSize: 10 }
                        ComboBox {
                            id: colormapCombo
                            width: parent.width
                            height: 34
                            property var entries: {
                                var names = backendSettings.getColormapNames();
                                var e = [];
                                for (var i = 0; i < names.length; ++i)
                                    e.push({ name: names[i], uri: backendSettings.getColormapPreviewUri(i) });
                                return e;
                            }
                            model: entries
                            textRole: "name"
                            currentIndex: backendSettings ? backendSettings.colormapChoice : 0
                            onActivated: index => backendSettings.colormapChoice = index
                            indicator: Item {}
                            leftPadding: 0; rightPadding: 0; topPadding: 0; bottomPadding: 0
                            background: Rectangle { color: "#2b2b2b"; border.color: "#444"; radius: 3 }
                            contentItem: Item {
                                anchors.fill: parent
                                clip: true
                                Image {
                                    source: colormapCombo.entries[colormapCombo.currentIndex] ? colormapCombo.entries[colormapCombo.currentIndex].uri : ""
                                    sourceSize.width: 250; sourceSize.height: 64
                                    fillMode: Image.Stretch
                                    horizontalAlignment: Image.AlignLeft
                                    verticalAlignment: Image.AlignVCenter
                                    anchors.fill: parent
                                    anchors.margins: 2
                                }
                                Text {
                                    text: "\u25BC"
                                    color: "#dddddd"
                                    font.pixelSize: 9
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }
                            popup: Popup {
                                y: colormapCombo.height
                                width: colormapCombo.width
                                contentItem: ListView {
                                    clip: true
                                    implicitHeight: contentHeight
                                    model: colormapCombo.delegateModel
                                    currentIndex: colormapCombo.highlightedIndex
                                    ScrollBar.vertical: ScrollBar {}
                                }
                                background: Rectangle { color: "#232323"; border.color: "#444" }
                            }
                            delegate: ItemDelegate {
                                width: colormapCombo.width
                                height: 36
                                padding: 2
                                contentItem: Image {
                                    source: modelData.uri
                                    sourceSize.width: 280; sourceSize.height: 64
                                    fillMode: Image.Stretch
                                    horizontalAlignment: Image.AlignLeft
                                    verticalAlignment: Image.AlignVCenter
                                    anchors.fill: parent
                                    anchors.margins: 2
                                }
                                highlighted: colormapCombo.highlightedIndex === index
                            }
                        }
                        CheckBox { text: "Reverse palette"; checked: backendSettings ? backendSettings.colormapReversed : false; onToggled: backendSettings.colormapReversed = checked }
                        CheckBox { text: "Show colorbar"; checked: backendSettings ? backendSettings.showScalarColorbar : true; onToggled: backendSettings.showScalarColorbar = checked }
                        Row {
                            spacing: 8
                            Text { text: "Colorbar ticks"; color: "#888"; font.pixelSize: 10; verticalAlignment: Text.AlignVCenter }
                            SpinBox {
                                id: colorbarTicksSpin
                                from: 2; to: 20; stepSize: 1
                                value: backendSettings ? backendSettings.colorbarTicks : 6
                                onValueChanged: backendSettings.colorbarTicks = value
                                width: 64
                            }
                        }
                        Text { text: "Scalar filter"; color: "#888"; font.pixelSize: 10 }
                        ClipSlider { label: "Min"; value: backendSettings ? backendSettings.filterMin : 0; from: backendSettings ? backendSettings.dataScalarMinQml : 0; to: backendSettings ? backendSettings.dataScalarMaxQml : 1; onSet: v => backendSettings.filterMin = v }
                        ClipSlider { label: "Max"; value: backendSettings ? backendSettings.filterMax : 0; from: backendSettings ? backendSettings.dataScalarMinQml : 0; to: backendSettings ? backendSettings.dataScalarMaxQml : 1; onSet: v => backendSettings.filterMax = v }
                    }

                    // Screenshot Controls
                    Column {
                        visible: rail.activeSection === 5
                        spacing: 4
                        width: parent.width
                        Button { text: "Save Screenshot"; width: parent.width; onClicked: { screenshotSaveDialog.currentFile = backendSettings.generateScreenshotFilename(); screenshotSaveDialog.open(); } }
                        Text { text: "Options"; color: "#888"; font.pixelSize: 10 }
                        CheckBox { text: "Transparent (PNG)"; checked: backendSettings ? backendSettings.screenshotTransparent : false; onToggled: backendSettings.screenshotTransparent = checked }
                        LightSlider { label: "JPEG Q"; value: backendSettings ? backendSettings.screenshotQuality : 95; from: 1; to: 100; step: 1; onSet: v => backendSettings.screenshotQuality = v }
                        LightSlider { label: "Supersample"; value: backendSettings ? backendSettings.screenshotScale : 1; from: 1; to: 3; step: 1; onSet: v => backendSettings.screenshotScale = v }
                    }

                    // Vectors Panel
                    Column {
                        visible: rail.activeSection === 4
                        spacing: 4
                        width: parent.width
                        CheckBox { text: "Show vectors"; checked: backendSettings ? backendSettings.showVectors : false; onToggled: backendSettings.showVectors = checked }
                        Text { text: "Field"; color: "#888"; font.pixelSize: 10 }
                        ComboBox {
                            width: parent.width
                            model: backendSettings ? backendSettings.availableVectors : []
                            currentIndex: backendSettings ? Math.max(0, backendSettings.availableVectors.indexOf(backendSettings.vectorField)) : 0
                            onActivated: backendSettings.setActiveVectorField(currentText)
                        }
                        Text { text: "Arrow scale"; color: "#888"; font.pixelSize: 10 }
                        LightSlider { label: "Scale"; value: backendSettings ? backendSettings.vectorScale : 1.0; from: 0.01; to: 5.0; step: 0.01; onSet: v => backendSettings.vectorScale = v }
                        CheckBox { text: "Scale arrow length by magnitude"; checked: backendSettings ? backendSettings.vectorScaleByMagnitude : false; onToggled: backendSettings.vectorScaleByMagnitude = checked }
                        Text { text: "Stride (skip every N)"; color: "#888"; font.pixelSize: 10 }
                        LightSlider { label: "Stride"; value: backendSettings ? backendSettings.vectorStride : 1; from: 1; to: 20; step: 1; onSet: v => backendSettings.vectorStride = v }
                        Row { spacing: 6
                            Button { text: "Vector Color"; width: 100; onClicked: vectorColorDialog.open() }
                        }
                        Text { text: "Colormap"; color: "#888"; font.pixelSize: 10 }
                        CheckBox { text: "Color by magnitude"; checked: backendSettings ? backendSettings.vectorUseColormap : false; onToggled: backendSettings.vectorUseColormap = checked }
                        ComboBox {
                            id: vectorColormapCombo
                            width: parent.width
                            enabled: backendSettings ? backendSettings.vectorUseColormap : false
                            property var entries: {
                                var names = backendSettings.getColormapNames();
                                var e = [];
                                for (var i = 0; i < names.length; ++i)
                                    e.push({ name: names[i], uri: backendSettings.getColormapPreviewUri(i) });
                                return e;
                            }
                            model: entries
                            textRole: "name"
                            currentIndex: backendSettings ? backendSettings.vectorColormapChoice : 0
                            onActivated: index => backendSettings.vectorColormapChoice = index
                            height: 34
                            indicator: Item {}
                            leftPadding: 0; rightPadding: 0; topPadding: 0; bottomPadding: 0
                            background: Rectangle { color: "#2b2b2b"; border.color: "#444"; radius: 3 }
                            contentItem: Item {
                                anchors.fill: parent
                                clip: true
                                Image {
                                    source: vectorColormapCombo.entries[vectorColormapCombo.currentIndex] ? vectorColormapCombo.entries[vectorColormapCombo.currentIndex].uri : ""
                                    sourceSize.width: 280; sourceSize.height: 64
                                    fillMode: Image.Stretch
                                    horizontalAlignment: Image.AlignLeft
                                    verticalAlignment: Image.AlignVCenter
                                    anchors.fill: parent
                                    anchors.margins: 4
                                }
                                Text {
                                    text: "\u25BC"
                                    color: "#dddddd"
                                    font.pixelSize: 9
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }
                            delegate: ItemDelegate {
                                width: vectorColormapCombo.width
                                height: 32
                                padding: 2
                                contentItem: Image {
                                    source: modelData.uri
                                    sourceSize.width: 280; sourceSize.height: 64
                                    fillMode: Image.Stretch
                                    horizontalAlignment: Image.AlignLeft
                                    verticalAlignment: Image.AlignVCenter
                                    anchors.fill: parent
                                    anchors.margins: 4
                                }
                                highlighted: vectorColormapCombo.highlightedIndex === index
                            }
                        }
                        CheckBox { text: "Reverse palette"; enabled: backendSettings ? backendSettings.vectorUseColormap : false; checked: backendSettings ? backendSettings.vectorColormapReversed : false; onToggled: backendSettings.vectorColormapReversed = checked }
                    }

                    Column {
                        visible: rail.activeSection === 6
                        spacing: 6
                        width: parent.width
                        Text { text: "Source"; color: "#888"; font.pixelSize: 10 }
                        Row { spacing: 6
                            Text { text: "Type"; color: "#888"; font.pixelSize: 11; width: 64 }
                            Text { text: backendSettings ? backendSettings.meshDataType : "?"; color: "#ddd"; font.pixelSize: 11 }
                        }
                        Row { spacing: 6
                            Text { text: "Format"; color: "#888"; font.pixelSize: 11; width: 64 }
                            Text { text: backendSettings ? backendSettings.meshFormat : "?"; color: "#ddd"; font.pixelSize: 11 }
                        }
                        Text { text: "Geometry"; color: "#888"; font.pixelSize: 10 }
                        Row { spacing: 6
                            Text { text: "Triangles"; color: "#888"; font.pixelSize: 11; width: 64 }
                            Text { text: backendSettings ? backendSettings.triangleCount : 0; color: "#ddd"; font.pixelSize: 11 }
                        }
                        Row { spacing: 6
                            Text { text: "Points"; color: "#888"; font.pixelSize: 11; width: 64 }
                            Text { text: backendSettings ? backendSettings.pointCount : 0; color: "#ddd"; font.pixelSize: 11 }
                        }
                        Text { text: "Bounding box"; color: "#888"; font.pixelSize: 10 }
                        GridLayout {
                            columns: 4
                            width: parent.width
                            columnSpacing: 4
                            Text { text: ""; color: "#888"; font.pixelSize: 10 }
                            Text { text: "X"; color: "#aaa"; font.pixelSize: 10 }
                            Text { text: "Y"; color: "#aaa"; font.pixelSize: 10 }
                            Text { text: "Z"; color: "#aaa"; font.pixelSize: 10 }
                            Text { text: "Min"; color: "#888"; font.pixelSize: 11 }
                            Text { text: backendSettings ? backendSettings.worldMinX.toFixed(3) : "0"; color: "#ddd"; font.pixelSize: 11 }
                            Text { text: backendSettings ? backendSettings.worldMinY.toFixed(3) : "0"; color: "#ddd"; font.pixelSize: 11 }
                            Text { text: backendSettings ? backendSettings.worldMinZ.toFixed(3) : "0"; color: "#ddd"; font.pixelSize: 11 }
                            Text { text: "Max"; color: "#888"; font.pixelSize: 11 }
                            Text { text: backendSettings ? backendSettings.worldMaxX.toFixed(3) : "0"; color: "#ddd"; font.pixelSize: 11 }
                            Text { text: backendSettings ? backendSettings.worldMaxY.toFixed(3) : "0"; color: "#ddd"; font.pixelSize: 11 }
                            Text { text: backendSettings ? backendSettings.worldMaxZ.toFixed(3) : "0"; color: "#ddd"; font.pixelSize: 11 }
                            Text { text: "Delta"; color: "#888"; font.pixelSize: 11 }
                            Text { text: backendSettings ? (backendSettings.worldMaxX - backendSettings.worldMinX).toFixed(3) : "0"; color: "#ddd"; font.pixelSize: 11 }
                            Text { text: backendSettings ? (backendSettings.worldMaxY - backendSettings.worldMinY).toFixed(3) : "0"; color: "#ddd"; font.pixelSize: 11 }
                            Text { text: backendSettings ? (backendSettings.worldMaxZ - backendSettings.worldMinZ).toFixed(3) : "0"; color: "#ddd"; font.pixelSize: 11 }
                        }
                    }
                }
            }
        }

        ColorDialog {
            id: bgDialog
            selectedColor: backendSettings ? backendSettings.bgColor : "#000000"
            onAccepted: backendSettings.bgColor = selectedColor
        }
        ColorDialog {
            id: meshColorDialog
            selectedColor: backendSettings ? backendSettings.meshColor : "#66e666"
            onAccepted: backendSettings.meshColor = selectedColor
        }
        ColorDialog {
            id: surfaceColorDialog
            selectedColor: backendSettings ? backendSettings.surfaceColor : "#ffffff"
            onAccepted: backendSettings.surfaceColor = selectedColor
        }
        ColorDialog {
            id: vectorColorDialog
            selectedColor: backendSettings ? backendSettings.vectorColor : "#3399ff"
            onAccepted: backendSettings.vectorColor = selectedColor
        }

        // ---- Help: About ----
        Dialog {
            id: aboutDialog
            title: "About SciRender"
            modal: true
            standardButtons: Dialog.Ok
            width: 420
            onAboutToShow: {
                x = Math.round((windowRoot.width - width) / 2)
                y = Math.round((windowRoot.height - height) / 2)
            }
            contentItem: Column {
                spacing: 10
                padding: 18
                width: aboutDialog.width - 36
                Text {
                    text: "SciRender"
                    font.bold: true; font.pixelSize: 18; color: "#dddddd"
                }
                Text {
                    text: "A Qt 6 + OpenGL scientific mesh rendering toolkit for VTK and STL datasets."
                    font.pixelSize: 12; color: "#bbbbbb"
                    width: parent.width - 36; wrapMode: Text.WordWrap
                }
                Text {
                    text: "GPU shaders, instanced vector glyphs, light kit, slicing/clipping, and colormaps."
                    font.pixelSize: 11; color: "#999999"
                    width: parent.width - 36; wrapMode: Text.WordWrap
                }
                Text {
                    text: "Build: MinGW 64-bit · Qt 6.11"
                    font.pixelSize: 10; color: "#777777"
                }
            }
        }

        // ---- Help: Keyboard Shortcuts ----
        Dialog {
            id: shortcutsDialog
            title: "Keyboard Shortcuts"
            modal: true
            standardButtons: Dialog.Ok
            width: 380
            onAboutToShow: {
                x = Math.round((windowRoot.width - width) / 2)
                y = Math.round((windowRoot.height - height) / 2)
            }
            contentItem: Column {
                spacing: 4
                padding: 18
                width: shortcutsDialog.width - 36
                property var rows: [
                    ["R", "Reset camera"],
                    ["W", "Toggle wireframe"],
                    ["G", "Toggle grid"],
                    ["S", "Save screenshot"],
                    ["? / ?", "Orbit (azimuth)"],
                    ["? / ?", "Elevation"],
                    ["Ctrl + =", "Zoom in"],
                    ["Ctrl + -", "Zoom out"]
                ]
                Repeater {
                    model: parent.rows
                    Row {
                        spacing: 12
                        width: parent.width - 36
                        Rectangle {
                            width: 64; height: 22; radius: 3; color: "#333333"
                            border.color: "#555"
                            Text {
                                anchors.centerIn: parent
                                text: modelData[0]
                                color: "#9cdcfe"; font.pixelSize: 11
                                font.family: "Consolas, Menlo, monospace"
                            }
                        }
                        Text {
                            text: modelData[1]
                            color: "#cccccc"; font.pixelSize: 12
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }
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
            settings: backendSettings // Links instance reference directly to C++ target

        // Drop zone overlay for dragging raw STL/VTK files directly into the viewport
        DropArea {
            anchors.fill: parent
            onDropped: (drop) => {
                if (drop.hasUrls) {
                    // Load every dropped mesh (first one becomes the active view);
                    // loadMesh() already surfaces any parse/format errors as a toast.
                    for (let i = 0; i < drop.urls.length; ++i) {
                        let urlStr = drop.urls[i].toString();
                        let cleanPath = urlStr.startsWith("file://") ? urlToPath(urlStr) : urlStr;
                        backendSettings.loadMesh(cleanPath);
                    }
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
            backendSettings.loadMesh(cleanPath);
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
            // All formats now route through the GL FBO capture so the on-screen
            // image (mesh + baked colorbar legends) matches the export exactly.
            backendSettings.requestScreenshot(cleanPath);
        }
    }

    // turntable ? ~30fps azimuth nudge while autoRotate is on
    Timer {
        interval: 33
        running: backendSettings ? backendSettings.autoRotate : false
        repeat: true
        onTriggered: if (backendSettings) backendSettings.azimuth(0.6)
    }

    // FPS HUD needs continuous frames; drive repaints only while shown
    Timer {
        interval: 16
        running: backendSettings ? backendSettings.showFps : false
        repeat: true
        onTriggered: openGLViewport.update()
    }

    // Centered drop prompt context overlay
    // on-screen perf HUD (top-right)
    Rectangle {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 0
        width: hudText.width + 16
        height: hudText.height + 8
        color: "#000000aa"
        radius: 4
        visible: backendSettings ? backendSettings.showFps : false
        Text {
            id: hudText
            anchors.centerIn: parent
            text: backendSettings ? backendSettings.fpsText : ""
            color: "#7CFC00"
            font.pixelSize: 12
            font.family: "Consolas, Menlo, monospace"
        }
    }

    // -- Floating View & Display quick-bar -------------------------------------
    // Persistent, always-accessible display controls overlaid on the top-right of
    // the viewport. Independent of the left rail, so switching rail sections never
    // hides these. Collapses to a tiny handle (state persisted via backendSettings).
    property bool quickBarCollapsed: backendSettings ? backendSettings.quickBarCollapsed : false
    onQuickBarCollapsedChanged: { if (backendSettings) backendSettings.quickBarCollapsed = quickBarCollapsed }

    // Collapsed handle — shown when the bar is collapsed.
    ToolButton {
        id: quickBarHandle
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        width: 30; height: 30
        z: 15
        text: "\u{25A6}" // ? display/panel glyph
        font.pixelSize: 15
        visible: captureRoot.quickBarCollapsed
        hoverEnabled: true
        Accessible.role: Accessible.Button
        Accessible.name: "Show display quick-bar"
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        background: Rectangle { color: quickBarHandle.hovered ? "#3a3a3a" : "#000000bb"; radius: 5; border.color: "#555" }
        onClicked: captureRoot.quickBarCollapsed = false
    }

    Rectangle {
        id: quickBar
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: (backendSettings && backendSettings.showFps) ? (hudText.height + 16) : 8
        anchors.rightMargin: 8
        width: quickBarRow.implicitWidth + 16
        height: quickBarRow.implicitHeight + 12
        color: "#000000bb"
        radius: 6
        border.color: "#555"
        z: 15
        visible: backendSettings ? (backendSettings.hasMeshLoaded && !captureRoot.quickBarCollapsed) : false
        clip: true

        RowLayout {
            id: quickBarRow
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 6
            spacing: 8

            component QBButton : ToolButton {
                id: qbBtn
                property bool active: false
                width: 30; height: 28
                hoverEnabled: true
                Accessible.role: Accessible.Button
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                background: Rectangle {
                    color: qbBtn.active ? "#4a90d9" : (qbBtn.hovered ? "#3a3a3a" : "#2a2a2a")
                    radius: 4; border.color: qbBtn.active ? "#6aa9e8" : "#444"
                }
            }

            // -- Display toggles --
            QBButton {
                text: "W"; ToolTip.text: "Wireframe"; ToolTip.visible: hovered
                Accessible.name: "Wireframe"
                active: backendSettings ? backendSettings.isWireframe : false
                onClicked: backendSettings.isWireframe = !backendSettings.isWireframe
            }
            QBButton {
                text: "G"; ToolTip.text: "Grid"; ToolTip.visible: hovered
                Accessible.name: "Grid"
                active: backendSettings ? backendSettings.isGridVisible : false
                onClicked: backendSettings.isGridVisible = !backendSettings.isGridVisible
            }
            QBButton {
                text: "S"; ToolTip.text: "Surface"; ToolTip.visible: hovered
                Accessible.name: "Surface"
                active: backendSettings ? backendSettings.isSurfaceVisible : false
                onClicked: backendSettings.isSurfaceVisible = !backendSettings.isSurfaceVisible
            }

            Rectangle { width: 1; height: 22; color: "#555" }

            // -- Orthographic view snaps --
            QBButton { text: "+X"; ToolTip.text: "Ortho +X"; ToolTip.visible: hovered; Accessible.name: "Ortho +X"; onClicked: backendSettings.snapToOrthoView(0) }
            QBButton { text: "\u2212X"; ToolTip.text: "Ortho -X"; ToolTip.visible: hovered; Accessible.name: "Ortho -X"; onClicked: backendSettings.snapToOrthoView(1) }
            QBButton { text: "+Y"; ToolTip.text: "Ortho +Y"; ToolTip.visible: hovered; Accessible.name: "Ortho +Y"; onClicked: backendSettings.snapToOrthoView(2) }
            QBButton { text: "\u2212Y"; ToolTip.text: "Ortho -Y"; ToolTip.visible: hovered; Accessible.name: "Ortho -Y"; onClicked: backendSettings.snapToOrthoView(3) }
            QBButton { text: "+Z"; ToolTip.text: "Ortho +Z"; ToolTip.visible: hovered; Accessible.name: "Ortho +Z"; onClicked: backendSettings.snapToOrthoView(4) }
            QBButton { text: "\u2212Z"; ToolTip.text: "Ortho -Z"; ToolTip.visible: hovered; Accessible.name: "Ortho -Z"; onClicked: backendSettings.snapToOrthoView(5) }

            Rectangle { width: 1; height: 22; color: "#555" }

            // -- Reset camera --
            QBButton {
                text: "\u21BB"; // ?
                ToolTip.text: "Reset Camera"; ToolTip.visible: hovered
                Accessible.name: "Reset Camera"
                onClicked: backendSettings.resetCamera()
            }
            // -- Collapse --
            QBButton {
                text: "\u00D7" // ×
                ToolTip.text: "Collapse quick-bar"; ToolTip.visible: hovered
                Accessible.name: "Collapse quick-bar"
                onClicked: captureRoot.quickBarCollapsed = true
            }
        }
    }

    // Centered drop prompt context overlay
    Text {
        text: "Drag & Drop a .stl / .vtk file, or use \"Open Mesh\""
        color: "#888888"
        font.pixelSize: 16
        anchors.centerIn: parent
        visible: backendSettings ? !backendSettings.hasMeshLoaded : true
    }

    // NOTE: The colorbar legends (scalar + vector magnitude) are now baked into
    // the GL viewport FBO by Renderer::drawColorbarLegends, so they appear in
    // screenshots (including transparent PNG). The previous QML overlay legends
    // were removed to avoid duplicate legends and to keep the live view in sync
    // with the captured image.

    // Status / error toast (surfaces load + parse failures from C++)
    Rectangle {
        id: statusToast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 28
        width: statusToastText.width + 28
        height: statusToastText.height + 14
        radius: 6
        color: "#cc2222"
        visible: backendSettings ? backendSettings.statusMessage !== "" : false
        Text {
            id: statusToastText
            anchors.centerIn: parent
            text: backendSettings ? backendSettings.statusMessage : ""
            color: "#ffffff"
            font.pixelSize: 12
            padding: 7
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
                enabled: backendSettings ? backendSettings.recentFiles.length > 0 : false
                // rebuild the submenu from recentFiles each open
                onAboutToShow: {
                    // QQuickMenu has no clearMenuItems(); remove all current items
                    while (count > 0) { let it = itemAt(0); removeItem(it); it.destroy(); }
                    const list = backendSettings ? backendSettings.recentFiles : [];
                    for (let i = 0; i < list.length; ++i) {
                        const p = list[i];
                        const item = recentItem.createObject(this, { "text": p });
                        item.triggered.connect(() => backendSettings.openRecent(p));
                        addItem(item);
                    }
                }
                Component { id: recentItem; MenuItem {} }
            }
            MenuItem { text: "Clear"; onTriggered: backendSettings.clearMeshes() }
            MenuSeparator {}
            MenuItem { text: "Exit"; onTriggered: Qt.quit() }
        }
        Menu {
            title: "View"
            MenuItem { text: "Lighting"; onTriggered: rail.toggleSection(0) }
            MenuItem { text: "Slicing & Clipping"; onTriggered: rail.toggleSection(1) }
            MenuSeparator {}
            MenuItem { text: "Reset Camera"; onTriggered: backendSettings.resetCamera() }
        }
        Menu {
            title: "Help"
            MenuItem { text: "Keyboard Shortcuts"; onTriggered: shortcutsDialog.open() }
            MenuItem { text: "About SciRender"; onTriggered: aboutDialog.open() }
            MenuSeparator {}
            MenuItem { text: "Documentation"; onTriggered: Qt.openUrlExternally("https://github.com/thermeng/SciRender") }
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
            text: (backendSettings && backendSettings.hasMeshLoaded)
                ? "Mesh: " + backendSettings.currentMeshName
                  + "   |   Type: " + backendSettings.meshDataType
                  + "   |   Points: " + backendSettings.pointCount
                  + "   |   Triangles: " + backendSettings.triangleCount
                : "No mesh loaded | drag a .stl / .vtk file, or use File > Open Mesh"
        }
    }
}
