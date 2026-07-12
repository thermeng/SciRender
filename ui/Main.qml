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

                    // LINK THE CONTEXT OBJECT DIRECTLY INTO THE SIDEBAR EXTENSION
                    backendRenderer: backendRenderer

                    onResetViewClicked: {
                        if (backendRenderer) {
                            backendRenderer.resetCamera();
                            openGLViewport.update();
                        }
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
                // Inside Main.qml -> ViewportVisualizer -> DropArea
                DropArea {
                    anchors.fill: parent
                    onDropped: (drop) => {
                        if (drop.hasUrls) {
                            drop.acceptProposedAction(); // Signal event handled completely
                            let cleanPath = drop.urls[0].toString().replace("file:///", "");
                            backendRenderer.loadMesh(cleanPath);
                            drop.accept(); // Terminate event bubble loops safely
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

                visible: backendRenderer ? !backendRenderer.hasMeshLoaded : true
            }
        }
    }
}