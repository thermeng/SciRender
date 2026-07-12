import QtQuick
import QtQuick.Window
import QtQuick.Controls
import RendererQTUI 1.0 // Registers our custom C++ native view item namespace

Window {
    width: 1024
    height: 768
    visible: true
    title: "High-Performance Qt Graphics Engine Client"
    color: "#1e1e1e"

    Row {
        anchors.fill: parent

        // 1. Control Parameters Interface Panel
        Sidebar {
            id: controlSidebar
            height: parent.height
            onResetViewClicked: {
                backendRenderer.resetCamera();
                openGLViewport.update(); // Signal viewport visual redraw request
            }
        }

        // 2. High-Performance Raw OpenGL Output Subwindow Area
        Rectangle {
            width: parent.width - controlSidebar.width
            height: parent.height
            color: "#000000" // Standard fallback viewport color

            ViewportVisualizer {
                id: openGLViewport
                anchors.fill: parent
                renderer: backendRenderer // Links instance reference directly to C++ target

                // Drop zone overlay for dragging raw STL/VTK files directly into the UI viewport frame
                DropArea {
                    anchors.fill: parent
                    onDropped: (drop) => {
                        if (drop.hasUrls) {
                            // Extract file path string, removing 'file:///' prefix protocol
                            let cleanPath = drop.urls[0].toString().replace("file:///", "");
                            backendRenderer.loadMesh(cleanPath);
                        }
                    }
                }
            }

            // Simple user prompt overlay shown when workspace is clear
            Text {
                text: "Drag & Drop .stl or .vtk file here to visualize"
                color: "#555555"
                font.pixelSize: 16
                anchors.centerIn: parent
                visible: !backendRenderer.hasMeshLoaded
            }
        }
    }
}