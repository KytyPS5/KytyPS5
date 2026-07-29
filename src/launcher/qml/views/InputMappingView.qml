import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Graphical Input Mapping view, mirroring SharpEmu's InputMappingWindow:
// a bordered card on the left containing the controller art with labeled
// hotspot callouts positioned over each DualSense button (D-PAD, face
// buttons, L1/R1/L2/R2, L3/R3, Options, Touchpad); a card on the right
// listing every binding with the current value, a set-key button and a
// clear button (mirrors SharpEmu's ButtonMappingPanel). A capture status
// line plus a Restore Defaults ghost button sit in the toolbar.
Item {
    id: inputView

    // Pad button currently awaiting a key capture; "" when idle.
    property string capturingPadName: ""

    // Hotspots positioned over the DualSense art. Each maps a SharpEmu-style
    // callout label to the KytyPS5 PadButtonName used by the bridge.
    // Canvas coordinates match InputMappingWindow.axaml exactly (610x420).
    property var hotspots: [
        { pad: "L1",        label: "L1",         x: 17,  y: 14  },
        { pad: "L2",        label: "L2",         x: 15,  y: 48  },
        { pad: "TouchPad",  label: "TOUCH",      x: 254, y: 36  },
        { pad: "Up",        label: "D-PAD UP",   x: 11,  y: 117 },
        { pad: "Left",      label: "D-PAD LEFT", x: 9,   y: 151 },
        { pad: "Right",     label: "D-PAD RIGHT",x: 9,   y: 185 },
        { pad: "Down",      label: "D-PAD DOWN", x: 8,   y: 219 },
        { pad: "L3",        label: "L3",         x: 0,   y: 298 },
        { pad: "Options",   label: "OPTIONS",    x: 196, y: 288 },
        { pad: "R1",        label: "R1",         x: 493, y: 17  },
        { pad: "R2",        label: "R2",         x: 493, y: 52  },
        { pad: "Triangle",  label: "TRIANGLE",   x: 504, y: 110 },
        { pad: "Circle",    label: "CIRCLE",     x: 505, y: 143 },
        { pad: "Square",    label: "SQUARE",     x: 506, y: 181 },
        { pad: "Cross",     label: "CROSS",      x: 505, y: 213 },
        { pad: "R3",        label: "R3",         x: 508, y: 314 }
    ]

    // Right-side mapping list order (matches SharpEmu's ButtonOrder).
    property var buttonOrder: [
        { pad: "Cross",     label: "Cross" },
        { pad: "Circle",    label: "Circle" },
        { pad: "Square",    label: "Square" },
        { pad: "Triangle",  label: "Triangle" },
        { pad: "Up",        label: "D-Pad Up" },
        { pad: "Down",      label: "D-Pad Down" },
        { pad: "Left",      label: "D-Pad Left" },
        { pad: "Right",     label: "D-Pad Right" },
        { pad: "L1",        label: "L1 (Bumper)" },
        { pad: "R1",        label: "R1 (Bumper)" },
        { pad: "L2",        label: "L2 (Trigger)" },
        { pad: "R2",        label: "R2 (Trigger)" },
        { pad: "L3",        label: "L3 (Stick Press)" },
        { pad: "R3",        label: "R3 (Stick Press)" },
        { pad: "Options",   label: "Options" },
        { pad: "TouchPad",  label: "Touchpad Click" }
    ]

    function prettyBinding(padName) {
        var v = launcherBridge.keyBindings ? launcherBridge.keyBindings[padName] : ""
        if (!v || v.length === 0) return "\u2014"
        if (v.indexOf("Mouse:") === 0) {
            var m = v.substring(6)
            if (m === "Left") return "Mouse \u00b7 Left"
            if (m === "Right") return "Mouse \u00b7 Right"
            if (m === "Middle") return "Mouse \u00b7 Middle"
            if (m === "MotionX") return "Mouse \u00b7 Motion X"
            if (m === "MotionY") return "Mouse \u00b7 Motion Y"
            return "Mouse \u00b7 " + m
        }
        return v
    }

    // Captures the next keyboard input and routes it to the capturing binding.
    Keys.onPressed: function(event) {
        if (capturingPadName === "") {
            return
        }
        if (event.key === Qt.Key_Escape) {
            capturingPadName = ""
            event.accepted = true
            return
        }
        // Skip pure modifier presses so the user can press Shift+X reasonably.
        if (event.key === Qt.Key_Shift || event.key === Qt.Key_Control ||
            event.key === Qt.Key_Alt || event.key === Qt.Key_Meta ||
            event.key === Qt.Key_AltGr) {
            event.accepted = true
            return
        }

        var name = launcherBridge.keyName(event.key)
        if (name && name.length > 0) {
            launcherBridge.setBinding(capturingPadName, name)
        }
        capturingPadName = ""
        event.accepted = true
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        // Page header
        Column {
            spacing: 4
            Layout.fillWidth: true
            Text {
                text: "Input Mapping"
                font.pixelSize: 22
                font.weight: Font.Bold
                color: window.textPrimary
            }
            Text {
                text: "Click a callout on the DualSense below (or a row on the right) then press a host key to bind it."
                font.pixelSize: 12
                color: window.textDim
                width: parent.width
                wrapMode: Text.WordWrap
            }
        }

        // Toolbar: capture status (left) + Restore Defaults ghost button (right)
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Rectangle {
                visible: inputView.capturingPadName !== ""
                implicitWidth: captureRow.implicitWidth + 18
                implicitHeight: 30
                radius: 999
                color: "#332DD4BF"
                border.color: window.accentTeal
                border.width: 1

                RowLayout {
                    id: captureRow
                    anchors.centerIn: parent
                    spacing: 6
                    Rectangle { Layout.preferredWidth: 6; Layout.preferredHeight: 6; radius: 3; color: window.accentAmber }
                    Text {
                        text: "Press a key (Esc to cancel)\u2026"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: window.accentTeal
                    }
                }
            }

            Text {
                visible: inputView.capturingPadName === ""
                text: "Click a hotspot to remap."
                font.pixelSize: 11
                color: window.textDim
            }

            Item { Layout.fillWidth: true }

            // Restore Defaults (ghost button)
            Rectangle {
                implicitWidth: 150
                implicitHeight: 36
                radius: 8
                color: restoreMouse.containsMouse ? "#1A1E293B" : "transparent"
                border.color: restoreMouse.containsMouse ? window.accentTeal : window.borderSubtle
                Behavior on color { ColorAnimation { duration: 120 } }

                Text {
                    anchors.centerIn: parent
                    text: "Restore Defaults"
                    font.pixelSize: 12
                    color: restoreMouse.containsMouse ? window.accentTeal : window.textPrimary
                }
                MouseArea {
                    id: restoreMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        var defs = launcherBridge.defaultKeyBindings()
                        var keys = Object.keys(defs)
                        for (var i = 0; i < keys.length; i++) {
                            launcherBridge.setBinding(keys[i], defs[keys[i]])
                        }
                    }
                }
            }
        }

        // Body: controller card (left) + mapping list card (right).
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 14

            // --- Controller art card (SharpEmu InputMappingWindow layout) ---
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: 620
                radius: 12
                color: "#0F0E121C"
                border.color: window.borderSubtle
                border.width: 1

                // Section title (SharpEmu style: muted, uppercase, letter-spaced)
                Text {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.margins: 16
                    text: "LAYOUT"
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    color: window.textSecondary
                    font.letterSpacing: 1.5
                }

                // SharpEmu uses a 610x420 canvas with the controller image at
                // 410x285 with horizontal margin 100 and vertical offsets
                // 68/67. Replicate that exactly so all callout coordinates
                // match SharpEmu's InputMappingWindow.axaml.
                Item {
                    id: canvas
                    anchors.centerIn: parent
                    width: 610
                    height: 420

                    Rectangle {
                        anchors.fill: parent
                        radius: 12
                        color: "#1A1E293B"
                        border.color: window.borderSubtle
                        border.width: 1
                        clip: true

                        Image {
                            x: 100
                            y: 68
                            width: 410
                            height: 285
                            source: "qrc:/icons/input-controller-white.png"
                            fillMode: Image.PreserveAspectFit
                            opacity: 0.92
                        }
                    }

                    // Connector lines from each callout to its button on the
                    // controller (SharpEmu InputMappingWindow.axaml polylines).
                    Repeater {
                        model: [
                            // Left side
                            [119, 27, 167, 31, 201, 95],
                            [117, 61, 158, 64, 177, 93],
                            [305, 62, 305, 124],
                            [113, 130, 159, 133, 184, 144],
                            [111, 164, 140, 161, 164, 164],
                            [111, 198, 143, 177, 204, 164],
                            [110, 232, 150, 195, 184, 184],
                            [102, 311, 162, 241, 243, 216],
                            [298, 301, 303, 271, 306, 244],
                            // Right side
                            [493, 30, 440, 48, 413, 96],
                            [493, 65, 456, 71, 433, 93],
                            [504, 123, 459, 124, 423, 135],
                            [506, 194, 462, 194, 397, 163],
                            [505, 156, 482, 155, 454, 163],
                            [505, 226, 468, 218, 425, 191],
                            [508, 327, 460, 265, 363, 217]
                        ]
                        delegate: Canvas {
                            x: 0
                            y: 0
                            width: canvas.width
                            height: canvas.height
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.reset()
                                ctx.strokeStyle = "white"
                                ctx.globalAlpha = 0.72
                                ctx.lineWidth = 1.5
                                ctx.beginPath()
                                var pts = modelData
                                ctx.moveTo(pts[0], pts[1])
                                ctx.lineTo(pts[2], pts[3])
                                ctx.lineTo(pts[4], pts[5])
                                ctx.stroke()
                            }
                        }
                    }

                    // Hotspot callouts. Each is a 102x26 rounded pill; clicking
                    // it enters keyboard-capture mode for that Pad button.
                    Repeater {
                        model: inputView.hotspots

                        delegate: Rectangle {
                            x: modelData.x
                            y: modelData.y
                            width: 102
                            height: 26
                            radius: 13
                            color: rowHot.containsMouse
                                   ? "#262DD4BF"
                                   : (inputView.capturingPadName === modelData.pad ? "#332DD4BF" : "#1B2230")
                            border.color: rowHot.containsMouse
                                          ? window.accentTeal
                                          : (inputView.capturingPadName === modelData.pad ? window.accentAmber : "#55FFFFFF")
                            border.width: 1
                            Behavior on color { ColorAnimation { duration: 120 } }

                            Text {
                                anchors.centerIn: parent
                                text: modelData.label
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                                color: rowHot.containsMouse || inputView.capturingPadName === modelData.pad
                                       ? "#FFFFFF"
                                       : "#E8ECF4"
                            }

                            MouseArea {
                                id: rowHot
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    inputView.forceActiveFocus()
                                    inputView.capturingPadName = modelData.pad
                                }
                            }
                        }
                    }
                }
            }

            // --- Right mapping list card (mirrors SharpEmu ButtonMappingPanel) ---
            Rectangle {
                Layout.fillHeight: true
                Layout.fillWidth: true
                radius: 12
                color: "#0F0E121C"
                border.color: window.borderSubtle
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 10

                    Text {
                        Layout.fillWidth: true
                        text: "BUTTONS"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: window.textSecondary
                        font.letterSpacing: 1.5
                    }

                    // Per-button mapping rows: label | current binding chip | set | clear
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        contentWidth: availableWidth
                        clip: true

                        ColumnLayout {
                            width: parent.width
                            spacing: 8

                            Repeater {
                                model: inputView.buttonOrder

                                delegate: Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 46
                                    radius: 8
                                    color: inputView.capturingPadName === modelData.pad
                                           ? "#332DD4BF"
                                           : "#1A1E293B"
                                    border.color: inputView.capturingPadName === modelData.pad
                                                  ? window.accentTeal
                                                  : (rowMouse.containsMouse ? "#332DD4BF" : "transparent")
                                    border.width: 1
                                    Behavior on color { ColorAnimation { duration: 120 } }

                                    // Full-row hover/click handler declared *before*
                                    // the RowLayout content so it is stacked below
                                    // the Set/Clear buttons and never eats their
                                    // clicks. Clicking empty row space still enters
                                    // capture mode for this pad button.
                                    MouseArea {
                                        id: rowMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            inputView.forceActiveFocus()
                                            inputView.capturingPadName = modelData.pad
                                        }
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 12
                                        anchors.rightMargin: 10
                                        spacing: 10

                                        Text {
                                            Layout.preferredWidth: 120
                                            text: modelData.label
                                            font.pixelSize: 12
                                            font.weight: Font.DemiBold
                                            color: window.textPrimary
                                            elide: Text.ElideRight
                                        }

                                        // Current binding value pill
                                        Rectangle {
                                            id: keyChip
                                            property bool capturing: inputView.capturingPadName === modelData.pad
                                            property bool captured: inputView.prettyBinding(modelData.pad) !== "\u2014"
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 28
                                            radius: 999
                                            color: capturing ? "#0F0E121C" : "transparent"
                                            border.color: capturing ? window.accentAmber
                                                       : (captured ? "#332DD4BF" : window.borderSubtle)
                                            border.width: 1
                                            Behavior on border.color { ColorAnimation { duration: 120 } }

                                            Text {
                                                anchors.centerIn: parent
                                                text: keyChip.capturing
                                                      ? "\u25cf Press a key\u2026"
                                                      : inputView.prettyBinding(modelData.pad)
                                                font.pixelSize: 12
                                                font.weight: Font.DemiBold
                                                color: keyChip.capturing
                                                       ? window.accentAmber
                                                       : (keyChip.captured ? window.accentTeal : window.textDim)
                                            }
                                        }

                                        // Set key (small ghost button)
                                        Rectangle {
                                            Layout.preferredWidth: 60
                                            Layout.preferredHeight: 28
                                            radius: 7
                                            color: setMouse.containsMouse ? "#1A1E293B" : "transparent"
                                            border.color: setMouse.containsMouse
                                                          ? window.accentTeal : window.borderSubtle
                                            border.width: 1
                                            Behavior on color { ColorAnimation { duration: 120 } }

                                            Text {
                                                anchors.centerIn: parent
                                                text: inputView.capturingPadName === modelData.pad ? "\u25cf" : "Set"
                                                font.pixelSize: 11
                                                font.weight: Font.DemiBold
                                                color: setMouse.containsMouse
                                                       ? window.accentTeal : window.textPrimary
                                            }
                                            MouseArea {
                                                id: setMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    if (inputView.capturingPadName === modelData.pad) {
                                                        inputView.capturingPadName = ""
                                                    } else {
                                                        inputView.forceActiveFocus()
                                                        inputView.capturingPadName = modelData.pad
                                                    }
                                                }
                                            }
                                        }

                                        // Clear binding
                                        Rectangle {
                                            Layout.preferredWidth: 28
                                            Layout.preferredHeight: 28
                                            visible: inputView.capturingPadName !== modelData.pad &&
                                                     inputView.prettyBinding(modelData.pad) !== "\u2014"
                                            radius: 7
                                            color: clearMouse.containsMouse ? "#33F87171" : "transparent"
                                            border.color: clearMouse.containsMouse ? "#F87171" : window.borderSubtle
                                            border.width: 1
                                            Behavior on color { ColorAnimation { duration: 120 } }

                                            Text {
                                                anchors.centerIn: parent
                                                text: "\u00d7"
                                                font.pixelSize: 14
                                                font.weight: Font.Bold
                                                color: clearMouse.containsMouse ? "#F87171" : window.textDim
                                            }
                                            MouseArea {
                                                id: clearMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: launcherBridge.setBinding(modelData.pad, "")
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "Tip: click a callout on the controller art, or any row here, then press the host key you want to assign. Esc cancels."
                        font.pixelSize: 11
                        color: window.textDim
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}
