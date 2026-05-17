import QtQuick
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FlightMap
import QGroundControl.PlanView

// Adds visual items associated with the Flight Plan to the map.
// Currently only used by Fly View even though it's called PlanMapItems!
Item {
    id: _root

    property var    map                     ///< Map control to show items on
    property bool   largeMapView            ///< true: map takes up entire view, false: map is in small window
    property var    planMasterController    ///< Reference to PlanMasterController for vehicle
    property var    vehicle                 ///< Vehicle associated with these items

    property var    _map:                       map
    property var    _vehicle:                   vehicle
    property var    _missionController:         planMasterController.missionController
    property var    _geoFenceController:        planMasterController.geoFenceController
    property var    _rallyPointController:      planMasterController.rallyPointController
    property var    _guidedController:          globals.guidedControllerFlyView
    property var    _missionLineViewComponent

    // When missions are synced from the vehicle, ``dayaPlanVisualColor`` from the JSON plan is not preserved.
    // Fall back to MAVLink vehicle id (1 / 2 / 3) so multi-UAV paths match Daya Station colors.
    readonly property string _dayaPathColorFallback: {
        const v = _vehicle
        const vid = v ? v.id : 0
        if (vid === 1) return "#1976D2"
        if (vid === 2) return "#388E3C"
        if (vid === 3) return "#F9A825"
        return ""
    }

    readonly property string _effectiveMissionPathColor: {
        const c = _missionController.dayaPlanVisualColor
        return (c && c.length > 0) ? c : _dayaPathColorFallback
    }

    property string fmode: vehicle.flightMode

    // Add the mission item visuals to the map
    Repeater {
        model: largeMapView ? _missionController.visualItems : 0

        delegate: MissionItemMapVisual {
            map:        _map
            vehicle:    _vehicle
            onClicked:  _guidedController.confirmAction(_guidedController.actionSetWaypoint, Math.max(object.sequenceNumber, 1))
        }
    }

    Component.onCompleted: {
        _missionLineViewComponent = missionLineViewComponent.createObject(map)
        if (_missionLineViewComponent.status === Component.Error)
            console.log(_missionLineViewComponent.errorString())
        map.addMapItemGroup(_missionLineViewComponent)
    }

    Component.onDestruction: {
        if (_missionLineViewComponent) {
            // Must remove MapItemGroup before destruction, otherwise we crash on quit
            map.removeMapItemGroup(_missionLineViewComponent)
            _missionLineViewComponent.destroy()
        }
    }

    Component {
        id: missionLineViewComponent

        MapItemGroup {
            MissionLineView {
                missionPathColor: _root._effectiveMissionPathColor
                model: _missionController.simpleFlightPathSegments
            }

            MapItemView {
                model: _missionController.directionArrows

                delegate: MapLineArrow {
                    fromCoord:      object ? object.coordinate1 : undefined
                    toCoord:        object ? object.coordinate2 : undefined
                    arrowPosition:  3
                    z:              QGroundControl.zOrderWaypointLines + 1
                    arrowColor:     (_root._effectiveMissionPathColor && _root._effectiveMissionPathColor.length > 0)
                                    ? _root._effectiveMissionPathColor : "white"
                }
            }
        }
    }
}
