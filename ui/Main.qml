import QtQuick
import QtQuick.Window
import QtQuick.Controls
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

    // High-Performance Raw OpenGL Output Subwindow Area
    ViewportVisualizer {
        id: openGLViewport
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.left: rail.right
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

    // Helper JavaScript function to strip out protocols securely across OS environments
    function urlToPath(urlStr) {
        if (Qt.platform.os === "windows") {
            return urlStr.replace("file:///", "");
        } else {
            return urlStr.replace("file://", "");
        }
    }

    // ---- Expandable icon rail ----
    Rectangle {
        id: rail
        readonly property int panelWidth: 220
        property bool expanded: false
        property int activeSection: -1   // 0=Lighting 1=Clip 2=View 3=Colormap
        readonly property string sectionTitle:
            activeSection === 0 ? "Lighting" :
            activeSection === 1 ? "Slicing & Clipping" :
            activeSection === 2 ? "View & Display" :
            activeSection === 3 ? "Colormap" : ""
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

                    // Lighting Controls
                    Column {
                        visible: rail.activeSection === 0
                        spacing: 4
                        width: parent.width
                        LightSlider { label: "Intensity";   value: backendRenderer ? backendRenderer.lightInt : 0;         from: 0; to: 3; step: 0.05; onSet: v => backendRenderer.lightInt = v }
                        LightSlider { label: "Key Az";      value: backendRenderer ? backendRenderer.lightKeyAzimuth : 0;  from: -180; to: 180; step: 1; onSet: v => backendRenderer.lightKeyAzimuth = v }
                        LightSlider { label: "Key El";      value: backendRenderer ? backendRenderer.lightKeyElevation : 0;from: -90; to: 90; step: 1; onSet: v => backendRenderer.lightKeyElevation = v }
                        LightSlider { label: "Fill Az";     value: backendRenderer ? backendRenderer.lightFillAzimuth : 0; from: -180; to: 180; step: 1; onSet: v => backendRenderer.lightFillAzimuth = v }
                        LightSlider { label: "Fill El";     value: backendRenderer ? backendRenderer.lightFillElevation : 0;from: -90; to: 90; step: 1; onSet: v => backendRenderer.lightFillElevation = v }
                        LightSlider { label: "Back Az";     value: backendRenderer ? backendRenderer.lightBackAzimuth : 0; from: -180; to: 180; step: 1; onSet: v => backendRenderer.lightBackAzimuth = v }
                        LightSlider { label: "Back El";     value: backendRenderer ? backendRenderer.lightBackElevation : 0;from: -90; to: 90; step: 1; onSet: v => backendRenderer.lightBackElevation = v }
                        LightSlider { label: "Head Az";     value: backendRenderer ? backendRenderer.lightHeadAzimuth : 0; from: -180; to: 180; step: 1; onSet: v => backendRenderer.lightHeadAzimuth = v }
                        LightSlider { label: "Head El";     value: backendRenderer ? backendRenderer.lightHeadElevation : 0;from: -90; to: 90; step: 1; onSet: v => backendRenderer.lightHeadElevation = v }
                        LightSlider { label: "Key Int";     value: backendRenderer ? backendRenderer.lightKeyIntensity : 0;from: 0; to: 10; step: 0.1; onSet: v => backendRenderer.lightKeyIntensity = v }
                        LightSlider { label: "Fill Int";    value: backendRenderer ? backendRenderer.lightFillIntensity : 0;from: 0; to: 10; step: 0.1; onSet: v => backendRenderer.lightFillIntensity = v }
                        LightSlider { label: "Head Int";    value: backendRenderer ? backendRenderer.lightHeadIntensity : 0;from: 0; to: 10; step: 0.1; onSet: v => backendRenderer.lightHeadIntensity = v }
                        LightSlider { label: "Ambient";     value: backendRenderer ? backendRenderer.matAmbient : 0;       from: 0; to: 1; step: 0.01; onSet: v => backendRenderer.matAmbient = v }
                        LightSlider { label: "Diffuse";     value: backendRenderer ? backendRenderer.matDiffuse : 0;       from: 0; to: 1; step: 0.01; onSet: v => backendRenderer.matDiffuse = v }
                        LightSlider { label: "Specular";    value: backendRenderer ? backendRenderer.matSpecular : 0;      from: 0; to: 1; step: 0.01; onSet: v => backendRenderer.matSpecular = v }
                        LightSlider { label: "Shininess";   value: backendRenderer ? backendRenderer.matShininess : 0;     from: 0; to: 1; step: 0.01; onSet: v => backendRenderer.matShininess = v }
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
                        Text { text: "Scalar filter"; color: "#888"; font.pixelSize: 10 }
                        ClipSlider { label: "Min"; value: backendRenderer ? backendRenderer.filterMin : 0; from: backendRenderer ? backendRenderer.dataScalarMinQml : 0; to: backendRenderer ? backendRenderer.dataScalarMaxQml : 1; onSet: v => backendRenderer.filterMin = v }
                        ClipSlider { label: "Max"; value: backendRenderer ? backendRenderer.filterMax : 0; from: backendRenderer ? backendRenderer.dataScalarMinQml : 0; to: backendRenderer ? backendRenderer.dataScalarMaxQml : 1; onSet: v => backendRenderer.filterMax = v }
                    }

                    // View & Display Controls
                    Column {
                        visible: rail.activeSection === 2
                        spacing: 4
                        width: parent.width
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
                        Text { text: "Scene"; color: "#888"; font.pixelSize: 10 }
                        Row { spacing: 6
                            Button { text: "Reset Cam"; width: 92; onClicked: backendRenderer.resetCamera() }
                            Button { text: "Screenshot"; width: 100; onClicked: screenshotSaveDialog.open() }
                        }
                        Button { text: "Background Color"; width: parent.width; onClicked: bgDialog.open() }
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
                            model: backendRenderer ? backendRenderer.getColormapNames() : []
                            currentIndex: backendRenderer ? backendRenderer.colormapChoice : 0
                            onActivated: index => backendRenderer.colormapChoice = index
                        }
                    }
                }
            }
        }

        ColorDialog {
            id: bgDialog
            selectedColor: backendRenderer ? backendRenderer.bgColor : "#000000"
            onAccepted: backendRenderer.bgColor = selectedColor
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
            backendRenderer.requestScreenshot(cleanPath);
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
                    text: backendRenderer ? backendRenderer.getDataScalarMaxQml().toFixed(3) : ""
                    color: "#dddddd"
                    font.pixelSize: 11
                    anchors.left: parent.left
                    anchors.top: parent.top
                }

                Text {
                    text: backendRenderer ? backendRenderer.getDataScalarMinQml().toFixed(3) : ""
                    color: "#dddddd"
                    font.pixelSize: 11
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                }
            }
        }
    }

    // ---- Menu bar ----
    menuBar: MenuBar {
        Menu {
            title: "File"
            MenuItem { text: "Open Mesh..."; onTriggered: fileDialog.open() }
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