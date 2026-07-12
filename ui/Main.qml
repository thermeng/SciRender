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
        anchors.top: parent.top
        anchors.margins: 12
        spacing: 4
        visible: backendRenderer ? (backendRenderer.hasMeshLoaded && backendRenderer.hasMeshScalarsQml()) : false

        Text {
            text: "Scalar: " + (backendRenderer ? backendRenderer.getActiveScalarName() : "")
            color: "#dddddd"
            font.pixelSize: 12
        }

        // Vertical gradient bar built from the active colormap stops
        Rectangle {
            width: 20
            height: 200
            border.color: "#555555"
            border.width: 1
            // Clip so the gradient fills exactly this rect
            clip: true
            Repeater {
                model: backendRenderer ? backendRenderer.getColormapStops() : []
                // Each stop is [t, r, g, b] with t in 0..1, rgb in 0..1.
                // Draw from bottom (t=0) to top (t=1).
                delegate: Rectangle {
                    width: 20
                    height: 200 / backendRenderer.getColormapStops().length
                    y: 200 - (modelData[0] + 1.0 / backendRenderer.getColormapStops().length) * 200
                    color: Qt.rgba(modelData[1], modelData[2], modelData[3], 1.0)
                }
            }
        }

        Text { text: backendRenderer ? backendRenderer.getDataScalarMaxQml().toFixed(3) : ""; color: "#dddddd"; font.pixelSize: 11 }
        Text { text: backendRenderer ? backendRenderer.getDataScalarMinQml().toFixed(3) : ""; color: "#dddddd"; font.pixelSize: 11 }
    }
}
