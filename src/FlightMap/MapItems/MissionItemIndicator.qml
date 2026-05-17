import QtQuick
import QtLocation

import QGroundControl
import QGroundControl.Controls
import QGroundControl.PlanView

/// Marker for displaying a mission item on the map
MapQuickItem {
    id: _item

    property var missionItem
    property int sequenceNumber
    /// When set (Fly view), waypoint tint uses this vehicle's MAV id if the loaded plan has no ``dayaPlanVisualColor``.
    property var mapVehicle: null

    signal clicked

    anchorPoint.x:  sourceItem.anchorPointX
    anchorPoint.y:  sourceItem.anchorPointY

    readonly property string _planPathColor: {
        if (!missionItem || !missionItem.masterController || !missionItem.masterController.missionController) {
            return ""
        }
        const c = missionItem.masterController.missionController.dayaPlanVisualColor
        if (c && c.length > 0) {
            return c
        }
        const v = mapVehicle
        const vid = v ? v.id : 0
        if (vid === 1) return "#1976D2"
        if (vid === 2) return "#388E3C"
        if (vid === 3) return "#F9A825"
        return ""
    }

    sourceItem:
        MissionItemIndexLabel {
            id:                 _label
            mapVehicle:         _item.mapVehicle
            planVisualColor:    _item._planPathColor
            checked:            _isCurrentItem
            label:              missionItem.abbreviation
            index:              missionItem.abbreviation.charAt(0) > 'A' && missionItem.abbreviation.charAt(0) < 'z' ? -1 : missionItem.sequenceNumber
            gimbalYaw:          missionItem.missionGimbalYaw
            vehicleYaw:         missionItem.missionVehicleYaw
            showGimbalYaw:      !isNaN(missionItem.missionGimbalYaw)
            highlightSelected:  true
            onClicked:          _item.clicked()
            opacity:            _item.opacity

            property bool _isCurrentItem:   missionItem ? missionItem.isCurrentItem || missionItem.hasCurrentChildItem : false
        }
}
