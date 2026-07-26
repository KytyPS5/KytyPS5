import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: aboutView

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 18

        // Section Header
        Column {
            spacing: 4
            Text {
                text: "About KytyPS5"
                font.pixelSize: 22
                font.weight: Font.Bold
                color: window.textPrimary
            }
            Text {
                text: "PlayStation 5 open-source emulator project."
                font.pixelSize: 12
                color: window.textDim
            }
        }

        // About Information Container
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 14
            color: "#B80E121C"
            border.color: window.borderSubtle

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 24
                spacing: 20

                RowLayout {
                    spacing: 20

                    Image {
                        source: "qrc:/icons/logo.png"
                        Layout.preferredWidth: 80
                        Layout.preferredHeight: 80
                        fillMode: Image.PreserveAspectFit
                    }

                    Column {
                        spacing: 4
                        Text {
                            text: "KytyPS5 Emulator"
                            font.pixelSize: 20
                            font.weight: Font.Bold
                            color: window.textPrimary
                        }
                        Text {
                            text: "Version 0.1.0 · Open Source Experimental Build"
                            font.pixelSize: 12
                            color: window.accentTeal
                        }
                        Text {
                            text: "Built with Qt 6.8, Vulkan 1.3, and Clang toolchain."
                            font.pixelSize: 11
                            color: window.textDim
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: window.borderSubtle
                }

                // Attribution & License Box
                //
                // Asset licensing/attribution is intentionally hidden from the
                // main UI for now (see PR feedback); the GitHub repository
                // link and the Vecteezy attribution block remain available in
                // the source tree (assets/ATTRIBUTION.md) but are not surfaced
                // in the launcher view.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 10
                    color: "#1F080B12"
                    border.color: window.borderSubtle
                    visible: false

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 12

                        Text {
                            text: "Asset Attribution & Licensing"
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            color: window.textPrimary
                        }

                        Text {
                            text: "Game controller vector art provided by Vecteezy under the Vecteezy Free License with attribution required.\nSource: https://www.vecteezy.com/vector-art/7743127-game-controller-icon-vector-illustration"
                            font.pixelSize: 11
                            color: window.textSecondary
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Item { Layout.fillHeight: true }

                        RowLayout {
                            spacing: 12

                            Rectangle {
                                Layout.preferredWidth: 140
                                Layout.preferredHeight: 36
                                radius: 8
                                color: githubMouse.containsMouse ? "#262DD4BF" : "#1A1E293B"
                                border.color: window.borderSubtle

                                Text {
                                    anchors.centerIn: parent
                                    text: "GitHub Repository"
                                    font.pixelSize: 12
                                    color: window.textPrimary
                                }

                                MouseArea {
                                    id: githubMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: Qt.openUrlExternally("https://github.com/KytyPS5")
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
