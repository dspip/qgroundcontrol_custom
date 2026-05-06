import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls
import Custom.Widgets

Item {
    property var parentToolInsets                       // These insets tell you what screen real estate is available for positioning the controls in your overlay
    property var totalToolInsets:   _totalToolInsets    // The insets updated for the custom overlay additions
    property var mapControl

    /// FlyView root (`FlyView.qml`) exposes `planController` (PlanMasterController).
    readonly property var _planMaster: (parent && parent.parent) ? parent.parent.planController : null
    readonly property string _dayaRunStamp: Qt.formatDateTime(new Date(), "yyyy-MM-dd HH:mm:ss")

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // --- Daya "region" polygon: visual only on Fly map (not mission path, not geofence) ---
    property var  _dayaRegionCoords:       []
    property bool _dayaRegionDrawActive:   false
    property bool _dayaRegionFinished:      false

    function _dayaAppendVertex(coord) {
        if (_dayaRegionFinished || !coord || !coord.isValid)
            return
        _dayaRegionCoords = _dayaRegionCoords.concat([coord])
    }

    function _dayaClearRegion() {
        _dayaRegionCoords = []
        _dayaRegionFinished = false
        _dayaRegionDrawActive = false
    }

    function _dayaToggleDraw() {
        if (_dayaRegionFinished)
            _dayaRegionCoords = []
        _dayaRegionFinished = false
        _dayaRegionDrawActive = !_dayaRegionDrawActive
    }

    function _dayaFinishRegion() {
        if (_dayaRegionCoords.length < 3)
            return
        _dayaRegionFinished = true
        _dayaRegionDrawActive = false
    }

    function _dayaCoordsVariantListForSave() {
        var out = []
        for (var i = 0; i < _dayaRegionCoords.length; i++) {
            var c = _dayaRegionCoords[i]
            out.push({ latitude: c.latitude, longitude: c.longitude })
        }
        return out
    }

    Connections {
        target: mapControl
        enabled: _dayaRegionDrawActive && mapControl !== undefined && mapControl !== null

        function onMapClicked(position) {
            var c = mapControl.toCoordinate(position, false)
            _dayaAppendVertex(c)
        }
    }

    MapPolyline {
        id:                     _dayaOpenLine
        parent:                 mapControl
        z:                      QGroundControl.zOrderMapItems + 1
        visible:                mapControl && _dayaRegionCoords.length >= 2 && !_dayaRegionFinished
        line.color:             "#1abc9c"
        line.width:             3
        path:                   _dayaRegionCoords
    }

    MapPolygon {
        id:                     _dayaClosedPoly
        parent:                 mapControl
        z:                      QGroundControl.zOrderMapItems + 1
        visible:                mapControl && _dayaRegionFinished && _dayaRegionCoords.length >= 3
        color:                  Qt.rgba(0.1, 0.75, 0.55, 0.22)
        border.color:           "#16a085"
        border.width:           2
        path:                   _dayaRegionCoords
    }

    Rectangle {
        id:                     dayaRegionPanel
        // Declare before any binding references `pad` (QML evaluates child bindings in a scope where `pad` must already exist).
        readonly property real pad: ScreenTools.defaultFontPixelWidth * 0.5
        anchors.left:           parent.left
        anchors.top:            parent.top
        anchors.margins:        _toolsMargin
        anchors.topMargin:      parentToolInsets.topEdgeLeftInset + _toolsMargin
        width:                  dayaRegionCol.implicitWidth + pad * 2
        height:                 dayaRegionCol.implicitHeight + pad * 2
        radius:                 4
        color:                    qgcPal.windowShade
        opacity:                0.94
        border.width:           1
        border.color:           qgcPal.buttonBorder

        Column {
            id:                     dayaRegionCol
            anchors.centerIn:       parent
            spacing:                dayaRegionPanel.pad

            QGCLabel {
                width:                  ScreenTools.defaultFontPixelWidth * 28
                wrapMode:               Text.WordWrap
                text:                   qsTr("Daya region (map only — not mission, not geofence)")
                font.pointSize:         ScreenTools.smallFontPointSize
                color:                  qgcPal.text
            }

            QGCLabel {
                visible:                _dayaRegionDrawActive
                width:                  ScreenTools.defaultFontPixelWidth * 28
                text:                   qsTr("Tap map to add corners (≥3), then Execute to close the region and save area.plan. Tap Plan mission again to stop outlining.")
                font.pointSize:         ScreenTools.smallFontPointSize
                color:                  qgcPal.warningText
                wrapMode:               Text.WordWrap
            }

            Row {
                spacing: dayaRegionPanel.pad
                QGCButton {
                    text:       qsTr("Plan mission")
                    onClicked:  _dayaToggleDraw()
                }
                QGCButton {
                    text:       qsTr("Execute")
                    enabled:    _dayaRegionCoords.length >= 3 && _planMaster !== null
                    onClicked:  {
                        if (!_planMaster)
                            return
                        _dayaFinishRegion()
                        var path = DayaCustom.areaScanPlanSavePath()
                        DayaCustom.saveFlyViewRegionPlan(_planMaster, path, _dayaCoordsVariantListForSave())
                    }
                }
                QGCButton {
                    text:       qsTr("Refresh")
                    enabled:    QGroundControl.multiVehicleManager.activeVehicle !== null
                    onClicked:  DayaCustom.dayaRefreshMissionFromVehicle()
                }
                QGCButton {
                    text:       qsTr("Clear")
                    onClicked:  _dayaClearRegion()
                }
            }
        }
    }

    // Top-right run stamp so it's obvious which QGC run is active.
    Rectangle {
        id:                     _dayaRunStampPanel
        anchors.right:          parent.right
        anchors.top:            parent.top
        anchors.margins:        _toolsMargin
        anchors.topMargin:      parentToolInsets.topEdgeRightInset + _toolsMargin
        radius:                 4
        color:                  qgcPal.windowShade
        opacity:                0.94
        border.width:           1
        border.color:           qgcPal.buttonBorder
        width:                  _dayaRunStampLabel.implicitWidth + ScreenTools.defaultFontPixelWidth * 1.2
        height:                 _dayaRunStampLabel.implicitHeight + ScreenTools.defaultFontPixelHeight * 0.6

        QGCLabel {
            id:                     _dayaRunStampLabel
            anchors.centerIn:       parent
            text:                   qsTr("Run: %1").arg(_dayaRunStamp)
            font.pointSize:         ScreenTools.smallFontPointSize
            color:                  qgcPal.text
        }
    }

    readonly property string noGPS:         qsTr("NO GPS")
    readonly property real   indicatorValueWidth:   ScreenTools.defaultFontPixelWidth * 7

    property var    _activeVehicle:         QGroundControl.multiVehicleManager.activeVehicle
    property real   _indicatorDiameter:     ScreenTools.defaultFontPixelWidth * 18
    property real   _indicatorsHeight:      ScreenTools.defaultFontPixelHeight
    property var    _sepColor:              qgcPal.globalTheme === QGCPalette.Light ? Qt.rgba(0,0,0,0.5) : Qt.rgba(1,1,1,0.5)
    property color  _indicatorsColor:       qgcPal.text
    property bool   _isVehicleGps:          _activeVehicle ? _activeVehicle.gps.count.rawValue > 1 && _activeVehicle.gps.hdop.rawValue < 1.4 : false
    property string _altitude:              _activeVehicle ? (isNaN(_activeVehicle.altitudeRelative.value) ? "0.0" : _activeVehicle.altitudeRelative.value.toFixed(1)) + ' ' + _activeVehicle.altitudeRelative.units : "0.0"
    property string _distanceStr:           isNaN(_distance) ? "0" : _distance.toFixed(0) + ' ' + QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsString
    property real   _heading:               _activeVehicle   ? _activeVehicle.heading.rawValue : 0
    property real   _distance:              _activeVehicle ? _activeVehicle.distanceToHome.rawValue : 0
    property string _messageTitle:          ""
    property string _messageText:           ""
    property real   _toolsMargin:           ScreenTools.defaultFontPixelWidth * 0.75

    function secondsToHHMMSS(timeS) {
        var sec_num = parseInt(timeS, 10);
        var hours   = Math.floor(sec_num / 3600);
        var minutes = Math.floor((sec_num - (hours * 3600)) / 60);
        var seconds = sec_num - (hours * 3600) - (minutes * 60);
        if (hours   < 10) {hours   = "0"+hours;}
        if (minutes < 10) {minutes = "0"+minutes;}
        if (seconds < 10) {seconds = "0"+seconds;}
        return hours+':'+minutes+':'+seconds;
    }

    QGCToolInsets {
        id:                     _totalToolInsets
        leftEdgeTopInset:       parentToolInsets.leftEdgeTopInset
        leftEdgeCenterInset:    exampleRectangle.leftEdgeCenterInset
        leftEdgeBottomInset:    parentToolInsets.leftEdgeBottomInset
        rightEdgeTopInset:      parentToolInsets.rightEdgeTopInset
        rightEdgeCenterInset:   parentToolInsets.rightEdgeCenterInset
        rightEdgeBottomInset:   parent.width - compassBackground.x
        topEdgeLeftInset:       parentToolInsets.topEdgeLeftInset
        topEdgeCenterInset:     compassArrowIndicator.y + compassArrowIndicator.height
        topEdgeRightInset:      parentToolInsets.topEdgeRightInset
        bottomEdgeLeftInset:    parentToolInsets.bottomEdgeLeftInset
        bottomEdgeCenterInset:  parentToolInsets.bottomEdgeCenterInset
        bottomEdgeRightInset:   parent.height - attitudeIndicator.y
    }

    // This is an example of how you can use parent tool insets to position an element on the custom fly view layer
    // - we use parent topEdgeLeftInset to position the widget below the toolstrip
    // - we use parent bottomEdgeLeftInset to dodge the virtual joystick if enabled
    // - we use the parent leftEdgeTopInset to size our element to the same width as the ToolStripAction
    // - we export the width of this element as the leftEdgeCenterInset so that the map will recenter if the vehicle flys behind this element
    Rectangle {
        id: exampleRectangle
        visible: false // to see this example, set this to true. To view insets, enable the insets viewer FlyView.qml
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: parentToolInsets.topEdgeLeftInset + _toolsMargin
        anchors.bottomMargin: parentToolInsets.bottomEdgeLeftInset + _toolsMargin
        anchors.leftMargin: _toolsMargin
        width: parentToolInsets.leftEdgeTopInset - _toolsMargin
        color: 'red'

        property real leftEdgeCenterInset: visible ? x + width : 0
    }

    //-------------------------------------------------------------------------
    //-- Heading Indicator
    Rectangle {
        id:                         compassBar
        height:                     ScreenTools.defaultFontPixelHeight * 1.5
        width:                      ScreenTools.defaultFontPixelWidth  * 50
        anchors.bottom:             parent.bottom
        anchors.bottomMargin:       _toolsMargin
        color:                      "#DEDEDE"
        radius:                     2
        clip:                       true
        anchors.horizontalCenter:   parent.horizontalCenter
        Repeater {
            model: 720
            QGCLabel {
                function _normalize(degrees) {
                    var a = degrees % 360
                    if (a < 0) a += 360
                    return a
                }
                property int _startAngle: modelData + 180 + _heading
                property int _angle: _normalize(_startAngle)
                anchors.verticalCenter: parent.verticalCenter
                x:              visible ? ((modelData * (compassBar.width / 360)) - (width * 0.5)) : 0
                visible:        _angle % 45 == 0
                color:          "#75505565"
                font.pointSize: ScreenTools.smallFontPointSize
                text: {
                    switch(_angle) {
                    case 0:     return "N"
                    case 45:    return "NE"
                    case 90:    return "E"
                    case 135:   return "SE"
                    case 180:   return "S"
                    case 225:   return "SW"
                    case 270:   return "W"
                    case 315:   return "NW"
                    }
                    return ""
                }
            }
        }
    }
    Rectangle {
        id:                         headingIndicator
        height:                     ScreenTools.defaultFontPixelHeight
        width:                      ScreenTools.defaultFontPixelWidth * 4
        color:                      qgcPal.windowShadeDark
        anchors.top:                compassBar.top
        anchors.topMargin:          -headingIndicator.height / 2
        anchors.horizontalCenter:   parent.horizontalCenter
        QGCLabel {
            text:                   _heading
            color:                  qgcPal.text
            font.pointSize:         ScreenTools.smallFontPointSize
            anchors.centerIn:       parent
        }
    }
    Image {
        id:                         compassArrowIndicator
        height:                     _indicatorsHeight
        width:                      height
        source:                     "/custom/img/compass_pointer.svg"
        fillMode:                   Image.PreserveAspectFit
        sourceSize.height:          height
        anchors.top:                compassBar.bottom
        anchors.topMargin:          -height / 2
        anchors.horizontalCenter:   parent.horizontalCenter
    }

    Rectangle {
        id:                     compassBackground
        anchors.bottom:         attitudeIndicator.bottom
        anchors.right:          attitudeIndicator.left
        anchors.rightMargin:    -attitudeIndicator.width / 2
        width:                  -anchors.rightMargin + compassBezel.width + (_toolsMargin * 2)
        height:                 attitudeIndicator.height * 0.75
        radius:                 2
        color:                  qgcPal.window

        Rectangle {
            id:                     compassBezel
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin:     _toolsMargin
            anchors.left:           parent.left
            width:                  height
            height:                 parent.height - (northLabelBackground.height / 2) - (headingLabelBackground.height / 2)
            radius:                 height / 2
            border.color:           qgcPal.text
            border.width:           1
            color:                  Qt.rgba(0,0,0,0)
        }

        Rectangle {
            id:                         northLabelBackground
            anchors.top:                compassBezel.top
            anchors.topMargin:          -height / 2
            anchors.horizontalCenter:   compassBezel.horizontalCenter
            width:                      northLabel.contentWidth * 1.5
            height:                     northLabel.contentHeight * 1.5
            radius:                     ScreenTools.defaultFontPixelWidth  * 0.25
            color:                      qgcPal.windowShade

            QGCLabel {
                id:                 northLabel
                anchors.centerIn:   parent
                text:               "N"
                color:              qgcPal.text
                font.pointSize:     ScreenTools.smallFontPointSize
            }
        }

        Image {
            id:                 headingNeedle
            anchors.centerIn:   compassBezel
            height:             compassBezel.height * 0.75
            width:              height
            source:             "/custom/img/compass_needle.svg"
            fillMode:           Image.PreserveAspectFit
            sourceSize.height:  height
            transform: [
                Rotation {
                    origin.x:   headingNeedle.width  / 2
                    origin.y:   headingNeedle.height / 2
                    angle:      _heading
                }]
        }

        Rectangle {
            id:                         headingLabelBackground
            anchors.top:                compassBezel.bottom
            anchors.topMargin:          -height / 2
            anchors.horizontalCenter:   compassBezel.horizontalCenter
            width:                      headingLabel.contentWidth * 1.5
            height:                     headingLabel.contentHeight * 1.5
            radius:                     ScreenTools.defaultFontPixelWidth  * 0.25
            color:                      qgcPal.windowShade

            QGCLabel {
                id:                 headingLabel
                anchors.centerIn:   parent
                text:               _heading
                color:              qgcPal.text
                font.pointSize:     ScreenTools.smallFontPointSize
            }
        }
    }

    Rectangle {
        id:                     attitudeIndicator
        anchors.bottomMargin:   _toolsMargin + parentToolInsets.bottomEdgeRightInset
        anchors.rightMargin:    _toolsMargin
        anchors.bottom:         parent.bottom
        anchors.right:          parent.right
        height:                 ScreenTools.defaultFontPixelHeight * 6
        width:                  height
        radius:                 height * 0.5
        color:                  qgcPal.windowShade

        CustomAttitudeWidget {
            size:               parent.height * 0.95
            vehicle:            _activeVehicle
            showHeading:        false
            anchors.centerIn:   parent
        }
    }
}
