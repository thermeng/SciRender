import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Dialogs
import RendererQTUI 1.0 // Registers our custom C++ native view item namespace

ApplicationWindow {
    width: 1024
    height: 768
    visible: true
    title: "RendererQT"
    color: "#1e1e1e" // base window background; the GL scene is drawn into a
                     // QQuickFramebufferObject and composited as a normal item on top.

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
                    let cleanPath = drop.urls[0].toString().replace("file:///", "");
                    backendRenderer.loadMesh(cleanPath);
                    drop.acceptProposedAction();
                }
            }
        }
    }

    // ---- Icon rail (VS Code / Blender activity-bar style) ----
    // Slim vertical strip of icon buttons on the left edge. Each opens a floating
    // popover next to it (auto-closes on click-away). Viewport stays full-bleed.
    Rectangle {
        id: rail
        width: 48
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        color: "#262626"
        z: 20

        // Gizmo offset follows the rail width (getSidebarWidth used by the MouseArea below).
        Component.onCompleted: backendRenderer.setSidebarWidth(width)

        component RailButton : ToolButton {
            width: 48; height: 44
            font.pixelSize: 20
            property bool active: false
            background: Rectangle {
                color: parent.active ? "#3a3a3a" : (parent.hovered ? "#333333" : "transparent")
                Rectangle { // active accent bar
                    width: 3; height: parent.height
                    color: "#4a90d9"
                    visible: parent.parent.active
                }
            }
        }

        Column {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 2

            RailButton { text: "\u{1F4C2}"; ToolTip.text: "Open Mesh"; ToolTip.visible: hovered; onClicked: fileDialog.open() }
            RailButton { text: "\u{1F4A1}"; ToolTip.text: "Lighting"; ToolTip.visible: hovered; active: lightingPopup.opened; onClicked: lightingPopup.opened ? lightingPopup.close() : lightingPopup.open() }
            RailButton { text: "\u2702";    ToolTip.text: "Slicing & Clipping"; ToolTip.visible: hovered; active: clipPopup.opened; onClicked: clipPopup.opened ? clipPopup.close() : clipPopup.open() }
            RailButton { text: "\u{1F441}"; ToolTip.text: "View & Display"; ToolTip.visible: hovered; active: viewPopup.opened; onClicked: viewPopup.opened ? viewPopup.close() : viewPopup.open() }
            RailButton { text: "\u{1F3A8}"; ToolTip.text: "Colormap"; ToolTip.visible: hovered; active: colormapPopup.opened; onClicked: colormapPopup.opened ? colormapPopup.close() : colormapPopup.open() }
        }
    }

    // Shared popover styling: floats to the right of the rail, dismissed on click-away.
    component FlyoutPopup : Popup {
        x: rail.width + 4
        padding: 10
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        background: Rectangle { color: "#2b2b2b"; border.color: "#444"; radius: 4 }
    }

    // Lighting popover
    FlyoutPopup {
        id: lightingPopup
        y: 48
        Column {
            spacing: 4
            component LightSlider : Row {
                required property string label
                required property real value
                required property real from
                required property real to
                required property real step
                required property var onSet
                spacing: 6
                Text { text: parent.label; color: "#cccccc"; font.pixelSize: 11; width: 70; elide: Text.ElideRight }
                Slider { width: 130; from: parent.from; to: parent.to; stepSize: parent.step; value: parent.value; onMoved: parent.onSet(value) }
                Text { text: parent.value.toFixed(1); color: "#999999"; font.pixelSize: 10; width: 30 }
            }
            LightSlider { label: "Intensity";   value: backendRenderer.lightInt;         from: 0; to: 3; step: 0.05; onSet: v => backendRenderer.lightInt = v }
            LightSlider { label: "Key Az";      value: backendRenderer.lightKeyAzimuth;  from: -180; to: 180; step: 1; onSet: v => backendRenderer.lightKeyAzimuth = v }
            LightSlider { label: "Key El";      value: backendRenderer.lightKeyElevation;from: -90; to: 90; step: 1; onSet: v => backendRenderer.lightKeyElevation = v }
            LightSlider { label: "Fill Az";     value: backendRenderer.lightFillAzimuth; from: -180; to: 180; step: 1; onSet: v => backendRenderer.lightFillAzimuth = v }
            LightSlider { label: "Fill El";     value: backendRenderer.lightFillElevation;from: -90; to: 90; step: 1; onSet: v => backendRenderer.lightFillElevation = v }
            LightSlider { label: "Back Az";     value: backendRenderer.lightBackAzimuth; from: -180; to: 180; step: 1; onSet: v => backendRenderer.lightBackAzimuth = v }
            LightSlider { label: "Back El";     value: backendRenderer.lightBackElevation;from: -90; to: 90; step: 1; onSet: v => backendRenderer.lightBackElevation = v }
            LightSlider { label: "Head Az";     value: backendRenderer.lightHeadAzimuth; from: -180; to: 180; step: 1; onSet: v => backendRenderer.lightHeadAzimuth = v }
            LightSlider { label: "Head El";     value: backendRenderer.lightHeadElevation;from: -90; to: 90; step: 1; onSet: v => backendRenderer.lightHeadElevation = v }
            LightSlider { label: "Key Int";     value: backendRenderer.lightKeyIntensity;from: 0; to: 10; step: 0.1; onSet: v => backendRenderer.lightKeyIntensity = v }
            LightSlider { label: "Fill Int";    value: backendRenderer.lightFillIntensity;from: 0; to: 10; step: 0.1; onSet: v => backendRenderer.lightFillIntensity = v }
            LightSlider { label: "Head Int";    value: backendRenderer.lightHeadIntensity;from: 0; to: 10; step: 0.1; onSet: v => backendRenderer.lightHeadIntensity = v }
            LightSlider { label: "Ambient";     value: backendRenderer.matAmbient;       from: 0; to: 1; step: 0.01; onSet: v => backendRenderer.matAmbient = v }
            LightSlider { label: "Diffuse";     value: backendRenderer.matDiffuse;       from: 0; to: 1; step: 0.01; onSet: v => backendRenderer.matDiffuse = v }
            LightSlider { label: "Specular";    value: backendRenderer.matSpecular;      from: 0; to: 1; step: 0.01; onSet: v => backendRenderer.matSpecular = v }
            LightSlider { label: "Shininess";   value: backendRenderer.matShininess;     from: 0; to: 1; step: 0.01; onSet: v => backendRenderer.matShininess = v }
        }
    }

    // Slicing & Clipping popover
    FlyoutPopup {
        id: clipPopup
        y: 92
        Column {
            spacing: 4
            component ClipSlider : Row {
                required property string label
                required property real value
                required property real from
                required property real to
                required property var onSet
                spacing: 6
                Text { text: parent.label; color: "#cccccc"; font.pixelSize: 11; width: 60; elide: Text.ElideRight }
                Slider { width: 140; from: parent.from; to: parent.to; value: parent.value; onMoved: parent.onSet(value) }
            }
            CheckBox { text: "Enable Clipping"; checked: backendRenderer.clipEnabled; onToggled: backendRenderer.clipEnabled = checked }
            Text { text: "Cut planes (world units)"; color: "#888"; font.pixelSize: 10 }
            ClipSlider { label: "Slice X"; value: backendRenderer.sliceHeightX; from: backendRenderer.worldMinX; to: backendRenderer.worldMaxX; onSet: v => backendRenderer.sliceHeightX = v }
            ClipSlider { label: "Slice Y"; value: backendRenderer.sliceHeightY; from: backendRenderer.worldMinY; to: backendRenderer.worldMaxY; onSet: v => backendRenderer.sliceHeightY = v }
            ClipSlider { label: "Slice Z"; value: backendRenderer.sliceHeightZ; from: backendRenderer.worldMinZ; to: backendRenderer.worldMaxZ; onSet: v => backendRenderer.sliceHeightZ = v }
            Text { text: "Keep side"; color: "#888"; font.pixelSize: 10 }
            Row { spacing: 10
                CheckBox { text: "Inv X"; checked: backendRenderer.invertX; onToggled: backendRenderer.invertX = checked }
                CheckBox { text: "Inv Y"; checked: backendRenderer.invertY; onToggled: backendRenderer.invertY = checked }
                CheckBox { text: "Inv Z"; checked: backendRenderer.invertZ; onToggled: backendRenderer.invertZ = checked }
            }
            Text { text: "Scalar filter"; color: "#888"; font.pixelSize: 10 }
            ClipSlider { label: "Min"; value: backendRenderer.filterMin; from: backendRenderer.dataScalarMinQml; to: backendRenderer.dataScalarMaxQml; onSet: v => backendRenderer.filterMin = v }
            ClipSlider { label: "Max"; value: backendRenderer.filterMax; from: backendRenderer.dataScalarMinQml; to: backendRenderer.dataScalarMaxQml; onSet: v => backendRenderer.filterMax = v }
        }
    }

    // View / camera & display popover
    FlyoutPopup {
        id: viewPopup
        y: 136
        Column {
            spacing: 4
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
            CheckBox { text: "Wireframe"; checked: backendRenderer.isWireframe; onToggled: backendRenderer.isWireframe = checked }
            CheckBox { text: "Grid";      checked: backendRenderer.isGridVisible; onToggled: backendRenderer.isGridVisible = checked }
            CheckBox { text: "Surface";   checked: backendRenderer.isSurfaceVisible; onToggled: backendRenderer.isSurfaceVisible = checked }
            Text { text: "Scene"; color: "#888"; font.pixelSize: 10 }
            Row { spacing: 6
                Button { text: "Reset Cam"; width: 100; onClicked: backendRenderer.resetCamera() }
                Button { text: "Screenshot"; width: 110; onClicked: screenshotSaveDialog.open() }
            }
            Button { text: "Background Color"; width: 216; onClicked: bgDialog.open() }
            ColorDialog {
                id: bgDialog
                selectedColor: backendRenderer ? backendRenderer.bgColor : "#000000"
                onAccepted: backendRenderer.bgColor = selectedColor
            }
        }
    }

    // Colormap popover
    FlyoutPopup {
        id: colormapPopup
        y: 180
        Column {
            spacing: 6
            Text { text: "Scalar field"; color: "#888"; font.pixelSize: 10 }
            ComboBox {
                id: scalarCombo
                width: 210
                enabled: backendRenderer ? backendRenderer.meshHasScalars : false
                model: backendRenderer ? backendRenderer.availableScalars : []
                currentIndex: backendRenderer ? backendRenderer.availableScalars.indexOf(backendRenderer.getActiveScalarNameQml()) : -1
                onActivated: index => backendRenderer.setActiveScalarField(model[index])
            }
            Text { text: "Colormap"; color: "#888"; font.pixelSize: 10 }
            ComboBox {
                id: colormapCombo
                width: 210
                model: backendRenderer ? backendRenderer.getColormapNames() : []
                currentIndex: backendRenderer ? backendRenderer.colormapChoice : 0
                onActivated: index => backendRenderer.colormapChoice = index
            }
        }
    }

    // Gizmo interaction overlay (matches the bottom-left GL gizmo rect exactly).
    // The gizmo is drawn at device px (sidebarWidth*dpr + 16, bottom-left, size*gizmoSize*dpr).
    // We place this MouseArea in logical QML coords at the same spot and convert on click.
    MouseArea {
        property int gizmoMargin: 16
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: backendRenderer ? backendRenderer.getSidebarWidth() + gizmoMargin : gizmoMargin
        anchors.bottomMargin: gizmoMargin
        width: backendRenderer ? backendRenderer.gizmoSize : 200
        height: backendRenderer ? backendRenderer.gizmoSize : 200
        acceptedButtons: Qt.LeftButton

        hoverEnabled: true
        onPositionChanged: (mouse) => {
            // mouse.x/y are LOCAL to this area; pickGizmoAxis needs ABSOLUTE window GL device px.
            // ponytail: gizmo drawn at absolute device px (sidebar*dpr+16, winH*dpr-16-size); x/y here are absolute window coords.
            const dpr = backendRenderer ? backendRenderer.devicePixelRatio : 1.0;
            const winH = parent.height * dpr;
            const glX = (x + mouse.x) * dpr;
            const glY = winH - ((y + mouse.y) * dpr);
            const axis = backendRenderer.pickGizmoAxis(glX, glY);
            backendRenderer.setHoveredAxis(axis);
        }
        onExited: backendRenderer.setHoveredAxis(-1);

        onClicked: (mouse) => {
            const dpr = backendRenderer ? backendRenderer.devicePixelRatio : 1.0;
            const winH = parent.height * dpr;
            const glX = (x + mouse.x) * dpr;
            const glY = winH - ((y + mouse.y) * dpr);
            const axis = backendRenderer.pickGizmoAxis(glX, glY);
            if (axis >= 0) backendRenderer.snapToAxisView(axis, false);
        }
        onDoubleClicked: (mouse) => {
            const dpr = backendRenderer ? backendRenderer.devicePixelRatio : 1.0;
            const winH = parent.height * dpr;
            const glX = (x + mouse.x) * dpr;
            const glY = winH - ((y + mouse.y) * dpr);
            const axis = backendRenderer.pickGizmoAxis(glX, glY);
            if (axis >= 0) backendRenderer.snapToAxisView(axis, true);
        }
    }

    FileDialog {
        id: fileDialog
        title: "Load Mesh"
        nameFilters: ["Mesh files (*.stl *.vtk)", "All files (*)"]
        onAccepted: {
            let cleanPath = selectedFile.toString().replace("file:///", "");
            backendRenderer.loadMesh(cleanPath);
        }
    }

    FileDialog {
        id: screenshotSaveDialog
        title: "Save Screenshot"
        fileMode: FileDialog.SaveFile
        nameFilters: ["PNG Images (*.png)", "JPEG Images (*.jpg *.jpeg)", "BMP Images (*.bmp)", "All files (*)"]
        onAccepted: {
            let cleanPath = selectedFile.toString().replace("file:///", "");
            backendRenderer.requestScreenshot(cleanPath);
        }
    }

    // Simple user prompt overlay shown when workspace is clear
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
            text: backendRenderer ? backendRenderer.getActiveScalarNameQml() : ""
            color: "#dddddd"
            font.pixelSize: 12
        }

        // Gradient bar + aligned min/max labels on a single row
        // Row {
        //     spacing: 6
        //     // Vertical gradient bar built from the active colormap stops
        //     Rectangle {
        //         width: 20
        //         height: 200
        //         border.color: "#555555"
        //         border.width: 1
        //         clip: true
        //         Repeater {
        //             model: backendRenderer ? backendRenderer.colormapStops : []
        //             // Each stop is [t, r, g, b] with t in 0..1, rgb in 0..1.
        //             // Fill bottom (t=0) to top (t=1); y is the strip's top edge.
        //             delegate: Rectangle {
        //                 width: 20
        //                 height: 200 / backendRenderer.colormapStops.length
        //                 y: 200 - (modelData[0] * 200 + height)
        //                 color: Qt.rgba(modelData[1], modelData[2], modelData[3], 1.0)
        //             }
        //         }
        //     }
        //     // Max at top of bar, min at bottom, vertically aligned to the bar.
        //     Item {
        //         width: 48; height: 200
        //         Text { text: backendRenderer ? backendRenderer.getDataScalarMaxQml().toFixed(3) : ""; color: "#dddddd"; font.pixelSize: 11; anchors.right: parent.right; anchors.top: parent.top }
        //         Text { text: backendRenderer ? backendRenderer.getDataScalarMinQml().toFixed(3) : ""; color: "#dddddd"; font.pixelSize: 11; anchors.right: parent.right; anchors.bottom: parent.bottom }
        //     }
        // }
        // Gradient bar + aligned min/max labels on a single row
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
                        width: 20
                        height: 200 / backendRenderer.colormapStops.length
                        y: 200 - (modelData[0] * 200 + height)
                        color: Qt.rgba(modelData[1], modelData[2], modelData[3], 1.0)
                    }
                }
            }

            // Fixed container where labels are anchored to exact geometric lines
            Item {
                width: 55 
                height: gradientBar.height
                
                Text { 
                    text: backendRenderer ? backendRenderer.getDataScalarMaxQml().toFixed(3) : ""
                    color: "#dddddd" 
                    font.pixelSize: 11
                    anchors.left: parent.left
                    anchors.top: parent.top // ponytail: max flush with top of bar
                }
                
                Text { 
                    text: backendRenderer ? backendRenderer.getDataScalarMinQml().toFixed(3) : ""
                    color: "#dddddd" 
                    font.pixelSize: 11
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom // ponytail: min flush with bottom of bar
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
            MenuItem { text: "Lighting"; onTriggered: lightingPopup.opened ? lightingPopup.close() : lightingPopup.open() }
            MenuItem { text: "Slicing & Clipping"; onTriggered: clipPopup.opened ? clipPopup.close() : clipPopup.open() }
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
