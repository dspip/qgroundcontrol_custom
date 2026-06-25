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
        _dayaBeaconDrawActive = false
        _dayaNavaidDrawActive = false
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

    // --- Daya "beacons": star markers on the Fly map, saved to beacons_locations.plan ---
    property var  _dayaBeaconCoords:      []
    property bool _dayaBeaconDrawActive:  false

    function _dayaAddBeacon(coord) {
        if (!coord || !coord.isValid)
            return
        _dayaBeaconCoords = _dayaBeaconCoords.concat([coord])
    }

    function _dayaClearBeacons() {
        _dayaBeaconCoords = []
        _dayaBeaconDrawActive = false
    }

    function _dayaToggleBeaconDraw() {
        // Only one draw mode active at a time so a map tap is unambiguous.
        _dayaRegionDrawActive = false
        _dayaNavaidDrawActive = false
        _dayaBeaconDrawActive = !_dayaBeaconDrawActive
    }

    function _dayaBeaconsVariantListForSave() {
        var out = []
        for (var i = 0; i < _dayaBeaconCoords.length; i++) {
            var c = _dayaBeaconCoords[i]
            out.push({ latitude: c.latitude, longitude: c.longitude })
        }
        return out
    }

    // --- Daya "navaid zone": a second polygon (red), saved to navaid_zone.plan ---
    property var  _dayaNavaidCoords:       []
    property bool _dayaNavaidDrawActive:   false
    property bool _dayaNavaidFinished:     false

    function _dayaAppendNavaidVertex(coord) {
        if (_dayaNavaidFinished || !coord || !coord.isValid)
            return
        _dayaNavaidCoords = _dayaNavaidCoords.concat([coord])
    }

    function _dayaClearNavaid() {
        _dayaNavaidCoords = []
        _dayaNavaidFinished = false
        _dayaNavaidDrawActive = false
    }

    function _dayaToggleNavaidDraw() {
        // Only one draw mode active at a time so a map tap is unambiguous.
        _dayaRegionDrawActive = false
        _dayaBeaconDrawActive = false
        if (_dayaNavaidFinished)
            _dayaNavaidCoords = []
        _dayaNavaidFinished = false
        _dayaNavaidDrawActive = !_dayaNavaidDrawActive
    }

    function _dayaFinishNavaid() {
        if (_dayaNavaidCoords.length < 3)
            return
        _dayaNavaidFinished = true
        _dayaNavaidDrawActive = false
    }

    function _dayaNavaidVariantListForSave() {
        var out = []
        for (var i = 0; i < _dayaNavaidCoords.length; i++) {
            var c = _dayaNavaidCoords[i]
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

    Connections {
        target: mapControl
        enabled: _dayaBeaconDrawActive && mapControl !== undefined && mapControl !== null

        function onMapClicked(position) {
            var c = mapControl.toCoordinate(position, false)
            _dayaAddBeacon(c)
        }
    }

    Connections {
        target: mapControl
        enabled: _dayaNavaidDrawActive && mapControl !== undefined && mapControl !== null

        function onMapClicked(position) {
            var c = mapControl.toCoordinate(position, false)
            _dayaAppendNavaidVertex(c)
        }
    }

    MapPolyline {
        id:                     _dayaNavaidOpenLine
        parent:                 mapControl
        z:                      QGroundControl.zOrderMapItems + 1
        visible:                mapControl && _dayaNavaidCoords.length >= 2 && !_dayaNavaidFinished
        line.color:             "#e03131"
        line.width:             3
        path:                   _dayaNavaidCoords
    }

    MapPolygon {
        id:                     _dayaNavaidClosedPoly
        parent:                 mapControl
        z:                      QGroundControl.zOrderMapItems + 1
        visible:                mapControl && _dayaNavaidFinished && _dayaNavaidCoords.length >= 3
        color:                  Qt.rgba(0.88, 0.11, 0.11, 0.22)
        border.color:           "#c0392b"
        border.width:           2
        path:                   _dayaNavaidCoords
    }

    // Star marker template. Declaring a MapQuickItem directly as a Repeater delegate does NOT
    // register it with the map (QtLocation requires map.addMapItem). So we create + add each
    // marker imperatively, mirroring QGC's RallyPointMapVisuals.
    Component {
        id: _dayaBeaconMarkerComponent
        MapQuickItem {
            z:              QGroundControl.zOrderMapItems + 2
            anchorPoint.x:  sourceItem.width  / 2
            anchorPoint.y:  sourceItem.height / 2
            property var beaconCoordinate
            coordinate:     beaconCoordinate
            sourceItem: Item {
                width:  ScreenTools.defaultFontPixelHeight * 1.6
                height: width
                Text {
                    anchors.centerIn:   parent
                    text:               "\u2605"   // ★
                    color:              "#FFD43B"
                    style:              Text.Outline
                    styleColor:         "#1a1c1f"
                    font.pointSize:     ScreenTools.largeFontPointSize
                }
            }
        }
    }

    // One star per dropped beacon. The array is rebuilt on each add, so delegates (and their
    // map items) are recreated; we add on completion and remove on destruction to avoid leaks.
    Repeater {
        model: _dayaBeaconCoords
        delegate: Item {
            property var _marker: null
            Component.onCompleted: {
                if (!mapControl)
                    return
                _marker = _dayaBeaconMarkerComponent.createObject(mapControl, { "beaconCoordinate": modelData })
                if (_marker)
                    mapControl.addMapItem(_marker)
            }
            Component.onDestruction: {
                if (_marker) {
                    mapControl.removeMapItem(_marker)
                    _marker.destroy()
                    _marker = null
                }
            }
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
                        var requested = Math.max(1, Math.floor(Number(droneCountField.text) || 1))
                        var connected = DayaCustom.dayaConnectedVehicleCount()
                        var splitN = connected > 0 ? connected : requested
                        DayaCustom.saveDayaStationParams(
                            Number(surveyAltField.text),
                            Number(revisitField.text),
                            Number(hfovField.text),
                            requested,
                            splitN)
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

    Rectangle {
        id:                     dayaStationParamsPanel
        readonly property real pad: ScreenTools.defaultFontPixelWidth * 0.5
        anchors.left:           parent.left
        anchors.top:            dayaRegionPanel.bottom
        anchors.margins:        _toolsMargin
        anchors.topMargin:      _toolsMargin
        width:                  dayaStationParamsCol.implicitWidth + pad * 2
        height:                 dayaStationParamsCol.implicitHeight + pad * 2
        radius:                 4
        color:                  qgcPal.windowShade
        opacity:                0.94
        border.width:           1
        border.color:           qgcPal.buttonBorder

        function _dayaReloadStationParams() {
            var p = DayaCustom.loadDayaStationParams()
            var sa = (p["survey_alt_m"] !== undefined) ? p["survey_alt_m"] : 30
            var rv = (p["revisit_s"] !== undefined) ? p["revisit_s"] : 8
            var hf = (p["camera_hfov_deg"] !== undefined) ? p["camera_hfov_deg"] : 78
            var dc = (p["drone_count"] !== undefined) ? p["drone_count"] : 3
            surveyAltField.text = Number(sa).toFixed(1)
            revisitField.text = Number(rv).toFixed(1)
            hfovField.text = Number(hf).toFixed(1)
            droneCountField.text = String(Math.max(1, Math.floor(Number(dc) || 1)))
        }

        function _dayaEffectiveSplitCount() {
            var requested = Math.max(1, Math.floor(Number(droneCountField.text) || 1))
            var connected = DayaCustom.dayaConnectedVehicleCount()
            return connected > 0 ? connected : requested
        }

        Column {
            id:                     dayaStationParamsCol
            anchors.centerIn:       parent
            spacing:                dayaStationParamsPanel.pad

            QGCLabel {
                width:                  ScreenTools.defaultFontPixelWidth * 28
                wrapMode:               Text.WordWrap
                font.pointSize:         ScreenTools.smallFontPointSize
                color:                  qgcPal.text
                text:                   qsTr("Station scan params (saved on Execute and to daya_station_params.json). Survey alt sets waypoint height and line spacing.")
            }

            Row {
                spacing: dayaStationParamsPanel.pad
                QGCLabel {
                    text:                   qsTr("Survey alt (m):")
                    font.pointSize:         ScreenTools.smallFontPointSize
                    color:                  qgcPal.text
                }
                QGCTextField {
                    id:                     surveyAltField
                    width:                  ScreenTools.defaultFontPixelWidth * 10
                    numericValuesOnly:      true
                    font.pointSize:         ScreenTools.smallFontPointSize
                }
            }
            Row {
                spacing: dayaStationParamsPanel.pad
                QGCLabel {
                    text:                   qsTr("Drones (split):")
                    font.pointSize:         ScreenTools.smallFontPointSize
                    color:                  qgcPal.text
                }
                QGCTextField {
                    id:                     droneCountField
                    width:                  ScreenTools.defaultFontPixelWidth * 6
                    numericValuesOnly:      true
                    font.pointSize:         ScreenTools.smallFontPointSize
                }
                QGCLabel {
                    text: {
                        var c = DayaCustom.dayaConnectedVehicleCount()
                        return c > 0
                            ? qsTr("(%1 connected — Execute uses %1)").arg(c)
                            : qsTr("(no vehicles — uses count above)")
                    }
                    font.pointSize:         ScreenTools.smallFontPointSize
                    color:                  qgcPal.warningText
                    wrapMode:               Text.WordWrap
                    width:                  ScreenTools.defaultFontPixelWidth * 18
                }
            }
            Row {
                spacing: dayaStationParamsPanel.pad
                QGCLabel {
                    text:                   qsTr("Revisit (s):")
                    font.pointSize:         ScreenTools.smallFontPointSize
                    color:                  qgcPal.text
                }
                QGCTextField {
                    id:                     revisitField
                    width:                  ScreenTools.defaultFontPixelWidth * 10
                    numericValuesOnly:      true
                    font.pointSize:         ScreenTools.smallFontPointSize
                }
            }
            Row {
                spacing: dayaStationParamsPanel.pad
                QGCLabel {
                    text:                   qsTr("Camera HFOV (°):")
                    font.pointSize:         ScreenTools.smallFontPointSize
                    color:                  qgcPal.text
                }
                QGCTextField {
                    id:                     hfovField
                    width:                  ScreenTools.defaultFontPixelWidth * 10
                    numericValuesOnly:      true
                    font.pointSize:         ScreenTools.smallFontPointSize
                }
            }
            QGCButton {
                text:       qsTr("Save station params")
                onClicked:  {
                    var requested = Math.max(1, Math.floor(Number(droneCountField.text) || 1))
                    var ok = DayaCustom.saveDayaStationParams(
                        Number(surveyAltField.text),
                        Number(revisitField.text),
                        Number(hfovField.text),
                        requested,
                        -1)
                    if (ok) {
                        dayaStationParamsPanel._dayaReloadStationParams()
                    }
                }
            }

            Component.onCompleted: dayaStationParamsPanel._dayaReloadStationParams()
        }
    }

    Rectangle {
        id:                     dayaBeaconsPanel
        readonly property real pad: ScreenTools.defaultFontPixelWidth * 0.5
        anchors.left:           parent.left
        anchors.top:            dayaStationParamsPanel.bottom
        anchors.margins:        _toolsMargin
        anchors.topMargin:      _toolsMargin
        width:                  dayaBeaconsCol.implicitWidth + pad * 2
        height:                 dayaBeaconsCol.implicitHeight + pad * 2
        radius:                 4
        color:                  qgcPal.windowShade
        opacity:                0.94
        border.width:           1
        border.color:           qgcPal.buttonBorder

        Column {
            id:                     dayaBeaconsCol
            anchors.centerIn:       parent
            spacing:                dayaBeaconsPanel.pad

            QGCLabel {
                width:                  ScreenTools.defaultFontPixelWidth * 28
                wrapMode:               Text.WordWrap
                text:                   qsTr("Beacons (map markers — saved to beacons_locations.plan)")
                font.pointSize:         ScreenTools.smallFontPointSize
                color:                  qgcPal.text
            }

            QGCLabel {
                visible:                _dayaBeaconDrawActive
                width:                  ScreenTools.defaultFontPixelWidth * 28
                text:                   qsTr("Tap map to drop beacon stars. Press Execute to save beacons_locations.plan. Tap Add beacons again to stop placing.")
                font.pointSize:         ScreenTools.smallFontPointSize
                color:                  qgcPal.warningText
                wrapMode:               Text.WordWrap
            }

            QGCLabel {
                text:                   qsTr("Beacons placed: %1").arg(_dayaBeaconCoords.length)
                font.pointSize:         ScreenTools.smallFontPointSize
                color:                  qgcPal.text
            }

            Row {
                spacing: dayaBeaconsPanel.pad
                QGCButton {
                    text:       _dayaBeaconDrawActive ? qsTr("Stop adding") : qsTr("Add beacons")
                    onClicked:  _dayaToggleBeaconDraw()
                }
                QGCButton {
                    text:       qsTr("Execute")
                    enabled:    _dayaBeaconCoords.length >= 1 && _planMaster !== null
                    onClicked:  {
                        if (!_planMaster)
                            return
                        _dayaBeaconDrawActive = false
                        var path = DayaCustom.beaconsPlanSavePath()
                        DayaCustom.saveBeaconsPlan(_planMaster, path, _dayaBeaconsVariantListForSave())
                    }
                }
                QGCButton {
                    text:       qsTr("Clear")
                    onClicked:  _dayaClearBeacons()
                }
            }
        }
    }

    Rectangle {
        id:                     dayaNavaidPanel
        readonly property real pad: ScreenTools.defaultFontPixelWidth * 0.5
        anchors.left:           parent.left
        anchors.top:            dayaBeaconsPanel.bottom
        anchors.margins:        _toolsMargin
        anchors.topMargin:      _toolsMargin
        width:                  dayaNavaidCol.implicitWidth + pad * 2
        height:                 dayaNavaidCol.implicitHeight + pad * 2
        radius:                 4
        color:                  qgcPal.windowShade
        opacity:                0.94
        border.width:           1
        border.color:           qgcPal.buttonBorder

        Column {
            id:                     dayaNavaidCol
            anchors.centerIn:       parent
            spacing:                dayaNavaidPanel.pad

            QGCLabel {
                width:                  ScreenTools.defaultFontPixelWidth * 28
                wrapMode:               Text.WordWrap
                text:                   qsTr("Navaid zone (red polygon — map only, saved to navaid_zone.plan)")
                font.pointSize:         ScreenTools.smallFontPointSize
                color:                  qgcPal.text
            }

            QGCLabel {
                visible:                _dayaNavaidDrawActive
                width:                  ScreenTools.defaultFontPixelWidth * 28
                text:                   qsTr("Tap map to add corners (≥3), then Execute to close the zone and save navaid_zone.plan. Tap Draw zone again to stop outlining.")
                font.pointSize:         ScreenTools.smallFontPointSize
                color:                  qgcPal.warningText
                wrapMode:               Text.WordWrap
            }

            Row {
                spacing: dayaNavaidPanel.pad
                QGCButton {
                    text:       _dayaNavaidDrawActive ? qsTr("Stop drawing") : qsTr("Draw zone")
                    onClicked:  _dayaToggleNavaidDraw()
                }
                QGCButton {
                    text:       qsTr("Execute")
                    enabled:    _dayaNavaidCoords.length >= 3 && _planMaster !== null
                    onClicked:  {
                        if (!_planMaster)
                            return
                        _dayaFinishNavaid()
                        var path = DayaCustom.navaidZonePlanSavePath()
                        DayaCustom.saveNavaidZonePlan(_planMaster, path, _dayaNavaidVariantListForSave())
                    }
                }
                QGCButton {
                    text:       qsTr("Clear")
                    onClicked:  _dayaClearNavaid()
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
