import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

Item {
    id: libraryView
    property var gameModel: launcherBridge.getGameModel()
    property var filteredModel: launcherBridge.getGameFilterModel()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 20

        // Library Header & Search Bar
        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Column {
                spacing: 4
                Text {
                    text: "Game Library"
                    font.pixelSize: 22
                    font.weight: Font.Bold
                    color: window.textPrimary
                }
                Text {
                    text: filteredModel.rowCount() + " games installed · Ready to launch"
                    font.pixelSize: 12
                    color: window.textDim
                }
            }

            Item { Layout.fillWidth: true }

            // Custom Dark Search Bar Container
            Rectangle {
                Layout.preferredWidth: 280
                Layout.preferredHeight: 38
                radius: 8
                color: "#1A1E293B"
                border.color: searchInput.activeFocus ? window.accentTeal : window.borderSubtle

                Image {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    source: "qrc:/icons/icon-search.svg"
                    width: 16
                    height: 16
                    fillMode: Image.PreserveAspectFit
                    opacity: 0.6
                }

                TextField {
                    id: searchInput
                    anchors.fill: parent
                    anchors.leftMargin: 36
                    anchors.rightMargin: 12
                    verticalAlignment: TextInput.AlignVCenter
                    color: window.textPrimary
                    font.pixelSize: 12
                    selectByMouse: true
                    placeholderText: "Search library..."
                    placeholderTextColor: window.textDim
                    background: Item {}
                    text: filteredModel.search
                    onTextChanged: filteredModel.search = text
                }
            }

            // Rescan / Add Button
            Rectangle {
                Layout.preferredWidth: 38
                Layout.preferredHeight: 38
                radius: 8
                color: addFolderMouse.containsMouse ? "#262DD4BF" : "#1A1E293B"
                border.color: addFolderMouse.containsMouse ? window.accentTeal : window.borderSubtle

                Text {
                    anchors.centerIn: parent
                    text: "+"
                    font.pixelSize: 20
                    font.weight: Font.Bold
                    color: addFolderMouse.containsMouse ? window.accentTeal : window.textPrimary
                }

                MouseArea {
                    id: addFolderMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: launcherBridge.openFolderDialog()
                }
            }
        }

        // Main Library Body
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 14
            color: "#B80E121C"
            border.color: window.borderSubtle

            // Empty State
            ColumnLayout {
                id: emptyState
                anchors.centerIn: parent
                spacing: 16
                visible: filteredModel.rowCount() === 0

                // Empty State Text
                Column {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 8

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: gameModel.rowCount() === 0 ? "No Games Found" : "No Matching Games"
                        font.pixelSize: 22
                        font.weight: Font.Bold
                        color: window.textPrimary
                    }

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: gameModel.rowCount() === 0
                            ? "Your PlayStation 5 library is empty. Add a game directory to scan and populate your collection."
                            : "No installed games match your search. Try a different keyword."
                        font.pixelSize: 13
                        color: window.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        width: Math.min(libraryView.width - 120, 460)
                    }
                }

                Item { Layout.preferredHeight: 4; visible: gameModel.rowCount() === 0 }

                // Add Directory Button
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 200
                    Layout.preferredHeight: 42
                    radius: 10
                    color: addBtnMouse.containsMouse ? "#34D399" : window.accentTeal
                    visible: gameModel.rowCount() === 0

                    RowLayout {
                        anchors.centerIn: parent
                        spacing: 8
                        Text {
                            text: "+  Add Game Folder"
                            font.pixelSize: 13
                            font.weight: Font.Bold
                            color: "#060910"
                        }
                    }

                    MouseArea {
                        id: addBtnMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: launcherBridge.openFolderDialog()
                    }
                }
            }

            // Grid State
            GridView {
                id: gameGrid
                anchors.fill: parent
                anchors.margins: 20
                visible: filteredModel.rowCount() > 0
                model: filteredModel
                cellWidth: 160
                cellHeight: 220
                clip: true

                delegate: Rectangle {
                    width: 140
                    height: 200
                    radius: 12
                    color: gameMouse.containsMouse ? "#1A1E293B" : "transparent"
                    border.color: (window.selectedGameTitle === model.title) ? window.accentTeal : (gameMouse.containsMouse ? window.borderSubtle : "transparent")

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 8

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 124
                            radius: 8
                            color: "#080B12"
                            clip: true

                            Image {
                                id: coverImg
                                anchors.fill: parent
                                source: model.icon
                                fillMode: Image.PreserveAspectCrop
                                opacity: 0.9
                            }
                        }

                        Column {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                text: model.title
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                color: window.textPrimary
                                width: parent.width
                                elide: Text.ElideRight
                            }
                            Text {
                                text: model.serial !== "Unknown" ? model.serial : "PS5 App"
                                font.pixelSize: 10
                                color: window.textDim
                                width: parent.width
                                elide: Text.ElideRight
                            }
                        }
                    }

                    MouseArea {
                        id: gameMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            launcherBridge.setSelectedGameTitle(model.title)
                            launcherBridge.setSelectedGameDesc("Serial: " + model.serial + "\nPath: " + model.path)
                            launcherBridge.setSelectedGameBg(model.icon)
                            launcherBridge.setSelectedGameIcon(model.iconReal)
                            launcherBridge.setSelectedGamePath(model.path)
                            launcherBridge.setSelectedGameSerial(model.serial)
                        }
                    }
                }
            }
        }
    }
}
