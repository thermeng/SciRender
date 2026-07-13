import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Dialogs
import RendererQTUI 1.0 // Registers our custom C++ native view item namespace

Window {
    width: 1024
    height: 768
    visible: true
    title: "RendererQT"
    color: "#1e1e1e" // base window background; the GL scene is drawn into a
                    // QQuickFramebufferObject and composited as a normal item on top.

    // High-Performance Raw OpenGL Output Subwindow Area
    ViewportVisualizer {
        id: openGLViewport
        anchors.fill: parent
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

    // Minimal load toolbar
    Row {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 12
        spacing: 8

        Button {
            text: "Open Mesh"
            onClicked: fileDialog.open()
        }
        Button {
            text: lightingPanel.visible ? "Hide Lighting" : "Lighting"
            onClicked: lightingPanel.visible = !lightingPanel.visible
        }

        ComboBox {
            id: colormapCombo
            width: 210
            model: backendRenderer ? backendRenderer.getColormapNames() : []
            currentIndex: backendRenderer ? backendRenderer.colormapChoice : 0
            onActivated: index => backendRenderer.colormapChoice = index
        }
    }

    // Collapsible Lighting control panel (slider block, no per-slider boilerplate)
    Column {
        id: lightingPanel
        visible: false
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.topMargin: 56
        anchors.leftMargin: 12
        width: 220
        spacing: 4
        opacity: 0.95

        // ponytail: one helper row factory keeps all 17 sliders to a single declarative line each
        component LightSlider : Row {
            required property string label
            required property real value
            required property real from
            required property real to
            required property real step
            required property var onSet
            spacing: 6
            Text { text: parent.label; color: "#cccccc"; font.pixelSize: 11; width: 70; elide: Text.ElideRight }
            Slider {
                width: 130; from: parent.from; to: parent.to; stepSize: parent.step
                value: parent.value
                onMoved: parent.onSet(value)
            }
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


    FileDialog {
        id: fileDialog
        title: "Load Mesh"
        nameFilters: ["Mesh files (*.stl *.vtk)", "All files (*)"]
        onAccepted: {
            let cleanPath = selectedFile.toString().replace("file:///", "");
            backendRenderer.loadMesh(cleanPath);
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
        visible: backendRenderer ? (backendRenderer.hasMeshLoaded && backendRenderer.hasMeshScalarsQml()) : false

        Text {
            text: "Scalar: " + (backendRenderer ? backendRenderer.getActiveScalarName() : "")
            color: "#dddddd"
            font.pixelSize: 12
        }

        // Gradient bar + aligned min/max labels on a single row
        Row {
            spacing: 6
            // Vertical gradient bar built from the active colormap stops
            Rectangle {
                width: 20
                height: 200
                border.color: "#555555"
                border.width: 1
                clip: true
                Repeater {
                    model: backendRenderer ? backendRenderer.colormapStops : []
                    // Each stop is [t, r, g, b] with t in 0..1, rgb in 0..1.
                    // Fill bottom (t=0) to top (t=1); y is the strip's top edge.
                    delegate: Rectangle {
                        width: 20
                        height: 200 / backendRenderer.colormapStops.length
                        y: 200 - (modelData[0] * 200 + height)
                        color: Qt.rgba(modelData[1], modelData[2], modelData[3], 1.0)
                    }
                }
            }
            // Max at top of bar, min at bottom, vertically aligned to the bar.
            Item {
                width: 48; height: 200
                Text { text: backendRenderer ? backendRenderer.getDataScalarMaxQml().toFixed(3) : ""; color: "#dddddd"; font.pixelSize: 11; anchors.right: parent.right; anchors.top: parent.top }
                Text { text: backendRenderer ? backendRenderer.getDataScalarMinQml().toFixed(3) : ""; color: "#dddddd"; font.pixelSize: 11; anchors.right: parent.right; anchors.bottom: parent.bottom }
            }
        }
    }
}
}