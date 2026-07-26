import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Emulator Settings view. Mirrors the legacy ConfigurationEditDialog
// (src/launcher/forms/configuration_edit_dialog.ui) from the precedent launcher:
// the exact same options, in the same order, but rendered in the new dark
// QML design language. Every value here is persisted to QSettings by
// LauncherQML and forwarded to the emulator as --flags on launch (see
// launcher_qml.cpp::launchGame), so the view only exposes settings the
// emulator actually honors -- no audio/CPU/SMT/env knobs of its own.
Item {
    id: settingsView

    // ----- Reusable styled controls (mirror the existing view design language)

    component SectionCombo: ComboBox {
        id: comboRoot
        implicitWidth: 220
        implicitHeight: 36

        delegate: ItemDelegate {
            width: comboRoot.width
            contentItem: Text {
                text: modelData
                color: highlighted ? window.accentTeal : window.textPrimary
                font.pixelSize: 12
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle { color: highlighted ? "#262DD4BF" : "#080B12" }
        }

        contentItem: Text {
            leftPadding: 12
            rightPadding: 24
            text: comboRoot.displayText
            font.pixelSize: 12
            color: window.textPrimary
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            color: "#1A1E293B"
            border.color: comboRoot.activeFocus ? window.accentTeal : window.borderSubtle
            border.width: 1
            radius: 8
        }

        popup: Popup {
            y: comboRoot.height + 4
            width: comboRoot.width
            implicitHeight: contentItem.implicitHeight
            padding: 4
            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: comboRoot.popup.visible ? comboRoot.delegateModel : null
                currentIndex: comboRoot.highlightedIndex
            }
            background: Rectangle {
                color: "#080B12"
                border.color: window.borderSubtle
                radius: 8
            }
        }
    }

    component PathRow: Rectangle {
        id: rowRoot
        property string titleText
        property string descText
        property string pathValue
        property string pathPlaceholder
        signal pathEdited(string newPath)

        Layout.fillWidth: true
        Layout.preferredHeight: 84
        radius: 10
        color: "#1F080B12"
        border.color: window.borderSubtle

        RowLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 16

            Column {
                Layout.fillWidth: true
                spacing: 2
                Text { text: rowRoot.titleText; font.pixelSize: 14; font.weight: Font.DemiBold; color: window.textPrimary }
                Text { text: rowRoot.descText; font.pixelSize: 11; color: window.textDim }
            }

            TextField {
                Layout.preferredWidth: 240
                Layout.preferredHeight: 36
                text: rowRoot.pathValue
                placeholderText: rowRoot.pathPlaceholder
                color: window.textPrimary
                font.pixelSize: 12
                placeholderTextColor: window.textDim
                selectByMouse: true
                onEditingFinished: rowRoot.pathEdited(text)

                background: Rectangle {
                    color: "#1A1E293B"
                    border.color: parent.activeFocus ? window.accentTeal : window.borderSubtle
                    border.width: 1
                    radius: 8
                }
            }
        }
    }

    component ToggleRow: Rectangle {
        id: toggleRoot
        property string titleText
        property string descText
        property color accent: window.accentTeal
        property bool checked: false
        signal toggled(bool value)

        Layout.fillWidth: true
        Layout.preferredHeight: 60
        radius: 10
        color: "#1F080B12"
        border.color: window.borderSubtle

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 14
            spacing: 12

            Column {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    text: toggleRoot.titleText
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    color: window.textPrimary
                }
                Text {
                    text: toggleRoot.descText
                    font.pixelSize: 11
                    color: window.textDim
                    width: parent.width
                    wrapMode: Text.WordWrap
                }
            }

            // Custom switch (matches the dark theme's teal accent)
            Rectangle {
                Layout.preferredWidth: 46
                Layout.preferredHeight: 26
                radius: 13
                color: toggleRoot.checked ? toggleRoot.accent : "#0F1117"
                border.color: toggleRoot.checked ? toggleRoot.accent : window.borderSubtle
                border.width: 1
                Behavior on color { ColorAnimation { duration: 120 } }

                Rectangle {
                    x: toggleRoot.checked ? parent.width - width - 4 : 4
                    y: 3
                    width: 18
                    height: 18
                    radius: 9
                    color: "#FFFFFF"
                    Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.InOutQuad } }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: toggleRoot.toggled(!toggleRoot.checked)
                }
            }
        }
    }

    // A labelled row whose trailing control is a SectionCombo. Mirrors the
    // precedent stylized combo rows used by the original logging/graphics
    // views; title+description column on the left, dropdown on the right.
    component ComboRow: Rectangle {
        id: crow
        property string titleText
        property string descText
        property var comboModel: []
        property int comboIndex: 0
        signal comboChanged(int index)

        Layout.fillWidth: true
        Layout.preferredHeight: 68
        radius: 10
        color: "#1F080B12"
        border.color: window.borderSubtle

        RowLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 16

            Column {
                Layout.fillWidth: true
                spacing: 2
                Text { text: crow.titleText; font.pixelSize: 14; font.weight: Font.DemiBold; color: window.textPrimary }
                Text { text: crow.descText;  font.pixelSize: 11; color: window.textDim; width: parent.width; wrapMode: Text.WordWrap }
            }

            SectionCombo {
                id: c
                model: crow.comboModel
                currentIndex: crow.comboIndex
                onCurrentIndexChanged: crow.comboChanged(currentIndex)
            }
        }
    }

    // A labelled row whose trailing control is a SpinBox (used for vblank).
    component SpinRow: Rectangle {
        id: srow
        property string titleText
        property string descText
        property int spinFrom: 30
        property int spinTo: 360
        property int spinValue: 60
        signal spinChanged(int value)

        Layout.fillWidth: true
        Layout.preferredHeight: 68
        radius: 10
        color: "#1F080B12"
        border.color: window.borderSubtle

        RowLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 16

            Column {
                Layout.fillWidth: true
                spacing: 2
                Text { text: srow.titleText; font.pixelSize: 14; font.weight: Font.DemiBold; color: window.textPrimary }
                Text { text: srow.descText;  font.pixelSize: 11; color: window.textDim; width: parent.width; wrapMode: Text.WordWrap }
            }

            SpinBox {
                id: s
                from: srow.spinFrom
                to: srow.spinTo
                value: srow.spinValue
                editable: true
                implicitWidth: 140
                implicitHeight: 36
                onValueModified: srow.spinChanged(value)

                contentItem: TextField {
                    text: s.textFromValue(s.value)
                    horizontalAlignment: Qt.AlignHCenter
                    verticalAlignment: Qt.AlignVCenter
                    font.pixelSize: 12
                    color: window.textPrimary
                    readOnly: !s.editable
                    selectByMouse: true
                    background: Rectangle {
                        color: "#1A1E293B"
                        border.color: parent.activeFocus ? window.accentTeal : window.borderSubtle
                        border.width: 1
                        radius: 8
                    }
                }

                up.indicator: Rectangle {
                    x: s.mirrored ? 0 : parent.width - width
                    height: parent.height
                    width: 32
                    color: s.up.pressed ? "#262DD4BF" : "transparent"
                    border.color: window.borderSubtle
                    radius: 8
                    Text { anchors.centerIn: parent; text: "+"; font.pixelSize: 14; color: window.textPrimary }
                }

                down.indicator: Rectangle {
                    x: s.mirrored ? parent.width - width : 0
                    height: parent.height
                    width: 32
                    color: s.down.pressed ? "#262DD4BF" : "transparent"
                    border.color: window.borderSubtle
                    radius: 8
                    Text { anchors.centerIn: parent; text: "\u2212"; font.pixelSize: 14; color: window.textPrimary }
                }

                background: Rectangle {
                    color: "#1A1E293B"
                    border.color: s.activeFocus ? window.accentTeal : window.borderSubtle
                    border.width: 1
                    radius: 8
                }
            }
        }
    }

    // ----- View body

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 18

        Column {
            spacing: 4
            Text {
                text: "Settings"
                font.pixelSize: 22
                font.weight: Font.Bold
                color: window.textPrimary
            }
            Text {
                text: "Emulator configuration. Changes apply on the next game launch."
                font.pixelSize: 12
                color: window.textDim
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 14
            color: "#B80E121C"
            border.color: window.borderSubtle

            ScrollView {
                anchors.fill: parent
                anchors.margins: 20
                contentWidth: availableWidth
                clip: true

                ColumnLayout {
                    width: parent.width
                    spacing: 14

                    // --- Diagnostics & validation (checkboxes group) ---
                    Text {
                        text: "DIAGNOSTICS & VALIDATION"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: window.textSecondary
                        font.letterSpacing: 1.5
                        Layout.topMargin: 4
                    }

                    ToggleRow {
                        titleText: "Vulkan validation"
                        descText: "Enable Vulkan validation layers."
                        checked: launcherBridge.vulkanValidationEnabled
                        onToggled: function(v) { launcherBridge.vulkanValidationEnabled = v }
                    }

                    ToggleRow {
                        titleText: "Shader validation"
                        descText: "Validate SPIR-V binary."
                        checked: launcherBridge.shaderValidationEnabled
                        onToggled: function(v) { launcherBridge.shaderValidationEnabled = v }
                    }

                    ToggleRow {
                        titleText: "Command buffer dump"
                        descText: "Dump command buffers to the folder below."
                        checked: launcherBridge.commandBufferDumpEnabled
                        onToggled: function(v) { launcherBridge.commandBufferDumpEnabled = v }
                    }

                    ToggleRow {
                        accent: window.accentAmber
                        titleText: "RenderDoc capture"
                        descText: "Enable RenderDoc capture on launch (--rd)."
                        checked: launcherBridge.renderDocEnabled
                        onToggled: function(v) { launcherBridge.renderDocEnabled = v }
                    }

                    ToggleRow {
                        titleText: "Use NGG rect-list draw"
                        descText: "Use the NGG 4-vertex path for rect-list DrawIndexAuto primitive 7."
                        checked: launcherBridge.nggRectlistDrawEnabled
                        onToggled: function(v) { launcherBridge.nggRectlistDrawEnabled = v }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: window.borderSubtle
                        Layout.topMargin: 4
                        Layout.bottomMargin: 4
                    }

                    // --- Display & frame pacing ---
                    Text {
                        text: "DISPLAY & FRAME PACING"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: window.textSecondary
                        font.letterSpacing: 1.5
                    }

                    ComboRow {
                        titleText: "Screen resolution"
                        descText: "Window resolution."
                        comboModel: ["1280x720", "1920x1080"]
                        comboIndex: launcherBridge.screenResolution
                        onComboChanged: function(idx) { launcherBridge.screenResolution = idx }
                    }

                    // Vblank frequency: SpinBox equivalent (30..360 Hz).
                    SpinRow {
                        titleText: "Vblank frequency"
                        descText: "Virtual display refresh rate used for frame pacing."
                        spinFrom: 30
                        spinTo: 360
                        spinValue: launcherBridge.vblankFrequency
                        onSpinChanged: function(v) { launcherBridge.vblankFrequency = v }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: window.borderSubtle
                        Layout.topMargin: 4
                        Layout.bottomMargin: 4
                    }

                    // --- Shaders ---
                    Text {
                        text: "SHADERS"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: window.textSecondary
                        font.letterSpacing: 1.5
                    }

                    ComboRow {
                        titleText: "Shader optimization type"
                        descText: "Optimize shaders for code size or performance."
                        comboModel: ["None", "Size", "Performance"]
                        comboIndex: launcherBridge.shaderOptimizationType
                        onComboChanged: function(idx) { launcherBridge.shaderOptimizationType = idx }
                    }

                    ComboRow {
                        titleText: "Shader log direction"
                        descText: "Dump shaders to file or console window. If enabled may decrease emulator performance."
                        comboModel: ["Silent", "Console", "File"]
                        comboIndex: launcherBridge.shaderLogDirection
                        onComboChanged: function(idx) { launcherBridge.shaderLogDirection = idx }
                    }

                    PathRow {
                        titleText: "Shader log folder"
                        descText: "Specify directory to dump shaders."
                        pathValue: launcherBridge.shaderLogFolder
                        pathPlaceholder: "_Shaders"
                        visible: launcherBridge.shaderLogDirection === 2
                        onPathEdited: function(newPath) { launcherBridge.shaderLogFolder = newPath }
                    }

                    PathRow {
                        titleText: "Command buffer dump folder"
                        descText: "Specify directory to dump command buffers."
                        pathValue: launcherBridge.commandBufferDumpFolder
                        pathPlaceholder: "_Buffers"
                        onPathEdited: function(newPath) { launcherBridge.commandBufferDumpFolder = newPath }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: window.borderSubtle
                        Layout.topMargin: 4
                        Layout.bottomMargin: 4
                    }

                    // --- Output routing ---
                    Text {
                        text: "OUTPUT ROUTING"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: window.textSecondary
                        font.letterSpacing: 1.5
                    }

                    ComboRow {
                        titleText: "Printf direction"
                        descText: "Print logs to file or console window. If enabled may decrease emulator performance."
                        comboModel: ["Silent", "Console", "File"]
                        comboIndex: launcherBridge.printfDirection
                        onComboChanged: function(idx) { launcherBridge.printfDirection = idx }
                    }

                    PathRow {
                        titleText: "Printf output file"
                        descText: "Specify file to dump logs."
                        pathValue: launcherBridge.printfOutputFile
                        pathPlaceholder: "_kyty.txt"
                        visible: launcherBridge.printfDirection === 2
                        onPathEdited: function(newPath) { launcherBridge.printfOutputFile = newPath }
                    }

                    ComboRow {
                        titleText: "Profiler direction"
                        descText: "Enable or disable profiler. If enabled may decrease emulator performance."
                        comboModel: ["None", "Network"]
                        comboIndex: launcherBridge.profilerDirection
                        onComboChanged: function(idx) { launcherBridge.profilerDirection = idx }
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }
    }
}
