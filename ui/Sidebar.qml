import QtQuick
import QtQuick.Controls

Rectangle {
    id: sidebarRoot
    width: 300
    color: "#252526" // Sleek dark mode panel
    border.color: "#3c3c3c"
    border.width: 1

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
            checked: backendRenderer.isWireframe
            onCheckedChanged: backendRenderer.isWireframe = checked

            contentItem: Text {
                text: parent.text
                color: "#ffffff"
                leftPadding: parent.indicator.width + parent.spacing
                verticalAlignment: Text.AlignVCenter
            }
        }

        CheckBox {
            text: "Show Dynamic Grid"
            checked: backendRenderer.isGridVisible
            onCheckedChanged: backendRenderer.isGridVisible = checked

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
                value: backendRenderer.lightInt
                onMoved: backendRenderer.lightInt = value
                width: 160
            }
        }

        // --- Multi-Scalar Field Control ---
        Text { text: "Active Colormap Lookup"; color: "#aaaaaa"; font.pixelSize: 14 }

        ComboBox {
            width: parent.width
            model: ["Cool to Warm", "Viridis", "Jet", "Grayscale"]
            currentIndex: backendRenderer.colormapChoice
            onCurrentIndexChanged: backendRenderer.colormapChoice = currentIndex
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
            text: backendRenderer.hasMeshLoaded
                  ? "Active File: " + backendRenderer.currentMeshName
                  : "No dataset loaded into workspace"
            color: backendRenderer.hasMeshLoaded ? "#4ec9b0" : "#ce9178"
            font.pixelSize: 12
            wrapMode: Text.Wrap
            width: parent.width
        }
    }
}