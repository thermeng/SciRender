import QtQuick
import QtQuick.Controls

Rectangle {
    id: sidebarRoot
    width: 300
    color: "#252526" // Sleek dark mode panel
    border.color: "#3c3c3c"
    border.width: 1

    property QtObject backendRenderer: null
    signal resetViewClicked()

    Column {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 20

        Text {
            text: "Engine Controls"
            color: "#ffffff"
            font.pixelSize: 20
            font.bold: true
        }

        Rectangle {
            width: parent.width - 40
            height: 1
            color: "#3c3c3c"
        }

        // --- Render Style Options ---
        Text { text: "Visualization Mode"; color: "#aaaaaa"; font.pixelSize: 14 }

        CheckBox {
            text: "Wireframe Overlay"
            checked: backendRenderer ? backendRenderer.isWireframe : false
            onCheckedChanged: if (backendRenderer) backendRenderer.isWireframe = checked

            contentItem: Text {
                text: parent.text
                color: "#ffffff"
                leftPadding: parent.indicator.width + parent.spacing
                verticalAlignment: Text.AlignVCenter
            }
        }

        CheckBox {
            text: "Show Dynamic Grid"
            checked: backendRenderer ? backendRenderer.isGridVisible : false
            onCheckedChanged: if (backendRenderer) backendRenderer.isGridVisible = checked

            contentItem: Text {
                text: parent.text
                color: "#ffffff"
                leftPadding: parent.indicator.width + parent.spacing
                verticalAlignment: Text.AlignVCenter
            }
        }

        // --- Lighting Kit Section ---
        Text { text: "Studio Lighting kit"; color: "#aaaaaa"; font.pixelSize: 14 }

        Row {
            spacing: 10
            Text { text: "Intensity:"; color: "#ffffff"; width: 60; anchors.verticalCenter: parent.verticalCenter }
            Slider {
                    id: lightSlider
                    from: 0.0
                    to: 2.0
                    // 2. USE AN EXPLICIT NULL GUARD SO IT NEVER EVALUATES "null.lightInt"
                    value: sidebarRoot.backendRenderer ? sidebarRoot.backendRenderer.lightInt : 1.0
                    onMoved: if (sidebarRoot.backendRenderer) sidebarRoot.backendRenderer.lightInt = value
                    width: 160
                }
        }
        // --- Multi-Scalar Field Control ---
        Text { text: "Active Colormap Lookup"; color: "#aaaaaa"; font.pixelSize: 14 }

        ComboBox {
            model: backendRenderer ? backendRenderer.getColormapNames() : []
            currentIndex: backendRenderer ? backendRenderer.colormapChoice : 0
            onCurrentIndexChanged: if (backendRenderer) backendRenderer.colormapChoice = currentIndex
        }

        // --- Native Action Triggers ---
        Button {
            text: "Reset Camera Orientation"
            width: parent.width
            onClicked: sidebarRoot.resetViewClicked()
        }

        Item { height: 20; width: 1 } // Spacer

        // --- Live File Diagnostics ---
        Text {
            text: (backendRenderer && backendRenderer.hasMeshLoaded)
                  ? "Active File: " + backendRenderer.currentMeshName
                  : "No dataset loaded into workspace"
            color: (backendRenderer && backendRenderer.hasMeshLoaded) ? "#4ec9b0" : "#ce9178"
        }
    }
}