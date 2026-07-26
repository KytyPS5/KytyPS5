import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import "views"

ApplicationWindow {
    id: window
    width: 1280
    height: 720
    minimumWidth: 960
    minimumHeight: 600
    visible: true
    visibility: Window.Maximized
    title: "KytyPS5 v0.1.0"
    flags: Qt.Window | Qt.FramelessWindowHint

    // Color Theme Tokens
    readonly property color bgDeep: "#060910"
    readonly property color bgCard: "#B80E121C" // semi-transparent dark glass
    readonly property color borderSubtle: "#33262D3E"
    readonly property color accentTeal: "#2DD4BF"
    readonly property color accentGreen: "#34D399"
    readonly property color accentAmber: "#FBBF24"
    readonly property color accentRed: "#F87171"
    readonly property color accentViolet: "#A78BFA"
    readonly property color textPrimary: "#E8ECF4"
    readonly property color textSecondary: "#94A3B8"
    readonly property color textDim: "#5C6B82"

    // Active Selection State
    property string activeTab: "library"
    property bool isConsoleOpen: false
    property bool isSidebarExpanded: false

    // SharpEmu-style initials helper: up to 2 leading uppercase letters of
    // the first two significant words (digits/letters), used as the cover art
    // placeholder when a game has no icon0.png.
    function computeInitials(title) {
        if (title === null || title === undefined || String(title).length === 0) return "?"
        var cleaned = String(title).replace(/[^0-9A-Za-z ]/g, " ").trim()
        if (cleaned.length === 0) return "?"
        var parts = cleaned.split(/\s+/).filter(function(p) { return p.length > 0 })
        if (parts.length === 0) return "?"
        if (parts.length === 1) {
            var word = parts[0]
            return word.substring(0, Math.min(2, word.length)).toUpperCase()
        }
        return (parts[0].charAt(0) + parts[1].charAt(0)).toUpperCase()
    }

    Connections {
        target: launcherBridge
        function onRequestMinimize() {
            window.showMinimized()
        }
        function onEmulatorStarted() {
            console.log("Emulator Started!")
        }
        function onEmulatorFinished() {
            console.log("Emulator Finished!")
        }
    }

    background: Rectangle {
        color: window.bgDeep
    }

    // Dynamic Blurred Backdrop
    Item {
        id: backdropContainer
        anchors.fill: parent

        Image {
            id: backdropImage
            anchors.centerIn: parent
            width: launcherBridge.selectedGameIcon !== "" ? parent.width : Math.min(parent.width * 0.45, 420)
            height: launcherBridge.selectedGameIcon !== "" ? parent.height : Math.min(parent.height * 0.45, 420)
            fillMode: Image.PreserveAspectFit
            source: launcherBridge.selectedGameIcon !== "" ? launcherBridge.selectedGameIcon : "qrc:/icons/logo.png"
            opacity: 0.25

            Behavior on source {
                NumberAnimation { target: backdropImage; property: "opacity"; from: 0; to: 0.25; duration: 400 }
            }
        }

        MultiEffect {
            anchors.fill: backdropImage
            source: backdropImage
            blurEnabled: true
            blur: 0.8
            blurMax: 36
        }

        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#B3060910" }
                GradientStop { position: 0.6; color: "#D9060910" }
                GradientStop { position: 1.0; color: "#F2060910" }
            }
        }
    }

    // Main App Shell
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Custom Title Bar
        Rectangle {
            id: titleBar
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            color: "#E6060910"
            border.color: window.borderSubtle
            border.width: 1

            // Drag Handler for Frameless Window
            MouseArea {
                anchors.fill: parent
                anchors.rightMargin: 135
                property point clickPos: "0,0"

                onPressed: {
                    clickPos = Qt.point(mouse.x, mouse.y)
                }

                onPositionChanged: {
                    if (pressed && window.visibility !== Window.Maximized) {
                        var delta = Qt.point(mouse.x - clickPos.x, mouse.y - clickPos.y)
                        window.x += delta.x
                        window.y += delta.y
                    }
                }

                onDoubleClicked: {
                    if (window.visibility === Window.Maximized) {
                        window.showNormal()
                    } else {
                        window.showMaximized()
                    }
                }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 0
                spacing: 10

                Image {
                    source: "qrc:/icons/icon-sidebar-white.svg"
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                    fillMode: Image.PreserveAspectFit
                }

                Text {
                    text: "KytyPS5"
                    font.pixelSize: 13
                    font.weight: Font.Bold
                    color: window.textPrimary
                }

                Rectangle {
                    color: "#1F2DD4BF"
                    border.color: "#332DD4BF"
                    radius: 8
                    implicitWidth: verText.implicitWidth + 12
                    implicitHeight: 18

                    Text {
                        id: verText
                        anchors.centerIn: parent
                        text: "v0.1.0 · UNOFFICIAL"
                        font.pixelSize: 9
                        font.weight: Font.Bold
                        color: window.accentTeal
                    }
                }

                Item { Layout.fillWidth: true }

                // Window Control Buttons (Minimize, Maximize/Restore, Close)
                Row {
                    spacing: 0
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter

                    // Minimize
                    Rectangle {
                        width: 44
                        height: 38
                        color: minMouse.containsMouse ? "#26FFFFFF" : "transparent"

                        Text {
                            anchors.centerIn: parent
                            text: "─"
                            font.pixelSize: 12
                            color: window.textPrimary
                        }

                        MouseArea {
                            id: minMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: window.showMinimized()
                        }
                    }

                    // Maximize / Restore
                    Rectangle {
                        width: 44
                        height: 38
                        color: maxMouse.containsMouse ? "#26FFFFFF" : "transparent"

                        Text {
                            anchors.centerIn: parent
                            text: window.visibility === Window.Maximized ? "❐" : "☐"
                            font.pixelSize: 12
                            color: window.textPrimary
                        }

                        MouseArea {
                            id: maxMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                if (window.visibility === Window.Maximized) {
                                    window.showNormal()
                                } else {
                                    window.showMaximized()
                                }
                            }
                        }
                    }

                    // Close / Cross
                    Rectangle {
                        width: 44
                        height: 38
                        color: closeMouse.containsMouse ? "#EF4444" : "transparent"

                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            font.pixelSize: 14
                            color: closeMouse.containsMouse ? "#FFFFFF" : window.textPrimary
                        }

                        MouseArea {
                            id: closeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: window.close()
                        }
                    }
                }
            }
        }

        // Body Area
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Left Collapsible Sidebar
            Rectangle {
                id: sidebar
                Layout.fillHeight: true
                Layout.preferredWidth: window.isSidebarExpanded ? 210 : 62
                color: "#E6080B12"
                border.color: window.borderSubtle
                border.width: 1

                Behavior on Layout.preferredWidth {
                    NumberAnimation { duration: 250; easing.type: Easing.InOutQuad }
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 4

                    // Clean Sidebar Toggle Button
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        Layout.leftMargin: 6
                        Layout.rightMargin: 6
                        Layout.topMargin: 6
                        Layout.bottomMargin: 4
                        radius: 8
                        color: toggleMouse.containsMouse ? "#262DD4BF" : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 12

                            Text {
                                text: window.isSidebarExpanded ? "◀" : "▶"
                                font.pixelSize: 13
                                font.weight: Font.Bold
                                color: window.accentTeal
                                Layout.alignment: Qt.AlignVCenter
                            }

                            Text {
                                text: "Collapse Sidebar"
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                color: toggleMouse.containsMouse ? window.accentTeal : window.textSecondary
                                visible: window.isSidebarExpanded
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }

                        MouseArea {
                            id: toggleMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: window.isSidebarExpanded = !window.isSidebarExpanded
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: window.borderSubtle
                    }

                    // Navigation Items
                    Repeater {
                        model: [
                            { id: "library",  label: "Library",  icon: "qrc:/icons/icon-library.svg" },
                            { id: "settings", label: "Settings", icon: "qrc:/icons/icon-settings.svg" },
                            { id: "input",    label: "Input",    icon: "qrc:/icons/icon-input.svg" },
                            { id: "about",     label: "About",    icon: "qrc:/icons/icon-about.svg" }
                        ]

                        delegate: Rectangle {
                            id: navItem
                            Layout.fillWidth: true
                            Layout.preferredHeight: 40
                            Layout.leftMargin: 6
                            Layout.rightMargin: 6
                            radius: 8
                            color: window.activeTab === modelData.id ? "#262DD4BF" : (navItemMouse.containsMouse ? "#1A1E293B" : "transparent")

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                spacing: 12

                                Item {
                                    Layout.preferredWidth: 20
                                    Layout.preferredHeight: 20

                                    Image {
                                        id: iconImg
                                        anchors.fill: parent
                                        source: modelData.icon
                                        fillMode: Image.PreserveAspectFit
                                    }

                                    MultiEffect {
                                        anchors.fill: parent
                                        source: iconImg
                                        colorization: window.activeTab === modelData.id ? 1.0 : 0.0
                                        colorizationColor: window.accentTeal
                                        visible: window.activeTab === modelData.id
                                    }
                                }

                                Text {
                                    text: modelData.label
                                    font.pixelSize: 13
                                    font.weight: window.activeTab === modelData.id ? Font.DemiBold : Font.Normal
                                    color: window.activeTab === modelData.id ? window.accentTeal : window.textPrimary
                                    visible: window.isSidebarExpanded
                                }
                            }

                            MouseArea {
                                id: navItemMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: window.activeTab = modelData.id
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // Center Content View Area
            StackLayout {
                id: mainStack
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: {
                    switch (window.activeTab) {
                        case "library":  return 0;
                        case "settings": return 1;
                        case "input":    return 2;
                        case "about":    return 3;
                        default: return 0;
                    }
                }

                // Tab Views
                LibraryView {}
                SettingsView {}
                InputMappingView {}
                AboutView {}
            }

            // Right Action & Status Panel
            Rectangle {
                id: rightPanel
                Layout.fillHeight: true
                Layout.preferredWidth: 270
                color: "#E6080B12"
                border.color: window.borderSubtle
                border.width: 1

                property bool hasGame: launcherBridge.selectedGameTitle !== "No game selected" &&
                                      launcherBridge.selectedGameTitle.length > 0

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 14

                    // Large Emblem Header. Mirrors the sharpemu launch-bar
                    // cover: shows the game's real icon0.png when available,
                    // a teal initials tile when the game has no cover art,
                    // and a small monogram tile as the empty-state placeholder.
                    Rectangle {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 8
                        Layout.bottomMargin: 8
                        width: 140
                        height: 140
                        radius: 20
                        color: "#0D2DD4BF"
                        border.color: "#262DD4BF"
                        border.width: 1
                        clip: true

                        // 1) Real game cover (icon0.png / pic0.png).
                        Image {
                            anchors.fill: parent
                            anchors.margins: 14
                            source: launcherBridge.selectedGameIcon !== "" ? launcherBridge.selectedGameIcon : ""
                            fillMode: Image.PreserveAspectFit
                            visible: launcherBridge.selectedGameIcon !== ""
                            opacity: 0.95
                        }

                        // 2) SharpEmu-style initials placeholder, shown when
                        //    a game is selected but has no cover art.
                        Item {
                            anchors.centerIn: parent
                            visible: rightPanel.hasGame && launcherBridge.selectedGameIcon === ""
                            width: 110
                            height: 110

                            Rectangle {
                                anchors.fill: parent
                                radius: 18
                                color: "#1A1E293B"
                                border.color: "#332DD4BF"
                                border.width: 1
                            }

                            Text {
                                anchors.centerIn: parent
                                text: window.computeInitials(launcherBridge.selectedGameTitle)
                                font.pixelSize: 40
                                font.weight: Font.Bold
                                color: "#E8ECF4"
                                opacity: 0.85
                            }
                        }

                        // 3) Empty-state placeholder (no game selected):
                        //    show the bundled KytyPS5 application logo.
                        Image {
                            anchors.centerIn: parent
                            source: "qrc:/icons/logo.png"
                            width: 110
                            height: 110
                            fillMode: Image.PreserveAspectFit
                            opacity: 0.85
                            visible: !rightPanel.hasGame
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: window.borderSubtle
                    }

                    // Empty state: prompt to pick a game.
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: !rightPanel.hasGame
                        spacing: 8

                        Item { Layout.fillHeight: true }

                        Text {
                            text: "No Game Selected"
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            color: window.textSecondary
                            Layout.alignment: Qt.AlignHCenter
                        }
                        Text {
                            text: "Pick a game from your library to see details and launch it."
                            font.pixelSize: 11
                            color: window.textDim
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            Layout.fillWidth: true
                            Layout.topMargin: 4
                        }

                        Item { Layout.fillHeight: true }
                    }

                    // Selected state: game info + actions.
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: rightPanel.hasGame
                        spacing: 14

                        // Game Info Section
                        Text {
                            text: launcherBridge.selectedGameTitle
                            font.pixelSize: 15
                            font.weight: Font.Bold
                            color: window.textPrimary
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Text {
                            text: launcherBridge.selectedGameDesc
                            font.pixelSize: 11
                            color: window.textDim
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Item { Layout.fillHeight: true }

                        // Launch Button (Teal Accent)
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 42
                            radius: 8
                            color: launchMouse.containsMouse ? "#34D399" : window.accentTeal

                            RowLayout {
                                anchors.centerIn: parent
                                spacing: 8
                                Text {
                                    text: "\u25B6  Launch"
                                    font.pixelSize: 13
                                    font.weight: Font.Bold
                                    color: "#060910"
                                }
                            }

                            MouseArea {
                                id: launchMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    if (launcherBridge.selectedGameTitle !== "No game selected") {
                                        launcherBridge.launchGame(launcherBridge.selectedGamePath)
                                    }
                                }
                            }
                        }

                        // Stop Button (Dark Glass Accent)
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 42
                            radius: 8
                            color: stopMouse.containsMouse ? "#33EF4444" : "#1A1E293B"
                            border.color: stopMouse.containsMouse ? "#EF4444" : window.borderSubtle

                            RowLayout {
                                anchors.centerIn: parent
                                spacing: 8
                                Text {
                                    text: "\u25A0  Stop"
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    color: stopMouse.containsMouse ? "#F87171" : window.textPrimary
                                }
                            }

                            MouseArea {
                                id: stopMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: launcherBridge.stopGame()
                            }
                        }

                        // Console Button (Elevated Glass Accent)
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 42
                            radius: 8
                            color: consoleMouse.containsMouse ? "#332DD4BF" : "#1A1E293B"
                            border.color: consoleMouse.containsMouse ? window.accentTeal : window.borderSubtle

                            RowLayout {
                                anchors.centerIn: parent
                                spacing: 8
                                Text {
                                    text: "=  Console"
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    color: consoleMouse.containsMouse ? window.accentTeal : window.textPrimary
                                }
                            }

                            MouseArea {
                                id: consoleMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: window.isConsoleOpen = !window.isConsoleOpen
                            }
                        }
                    }
                }
            }
        }
    }
}
