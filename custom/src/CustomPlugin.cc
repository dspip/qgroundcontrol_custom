#include "CustomPlugin.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "PlanMasterController.h"
#include "QmlComponentInfo.h"
#include "QmlObjectListModel.h"
#include "QGCLoggingCategory.h"
#include "QGCPalette.h"
#include "QGCMAVLink.h"
#include "AppSettings.h"
#include "Vehicle.h"

#include <algorithm>

#include <QtCore/QApplicationStatic>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoCoordinate>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlFile>

QGC_LOGGING_CATEGORY(CustomLog, "Custom.CustomPlugin")

Q_APPLICATION_STATIC(CustomPlugin, _customPluginInstance);

CustomFlyViewOptions::CustomFlyViewOptions(CustomOptions* options, QObject* parent)
    : QGCFlyViewOptions(options, parent)
{
    qCDebug(CustomLog) << this;
}

CustomOptions::CustomOptions(CustomPlugin *plugin, QObject *parent)
    : QGCOptions(parent)
    , _plugin(plugin)
    , _flyViewOptions(new CustomFlyViewOptions(this, this))
{
    Q_CHECK_PTR(_plugin);
}

/*===========================================================================*/

CustomPlugin::CustomPlugin(QObject *parent)
    : QGCCorePlugin(parent)
    , _options(new CustomOptions(this, this))
{
    qCDebug(CustomLog) << this;

    _showAdvancedUI = false;
    (void) connect(this, &QGCCorePlugin::showAdvancedUIChanged, this, &CustomPlugin::_advancedChanged);
}

QGCCorePlugin *CustomPlugin::instance()
{
    return _customPluginInstance();
}

QJsonArray CustomPlugin::_variantListToVertexJson(const QVariantList &list)
{
    QJsonArray out;
    for (const QVariant &v : list) {
        QGeoCoordinate c;
        if (v.canConvert<QGeoCoordinate>()) {
            c = v.value<QGeoCoordinate>();
        } else if (v.canConvert<QVariantMap>()) {
            const QVariantMap m = v.toMap();
            c.setLatitude(m.value(QStringLiteral("latitude")).toDouble());
            c.setLongitude(m.value(QStringLiteral("longitude")).toDouble());
        } else if (v.canConvert<QVariantList>()) {
            const QVariantList pair = v.toList();
            if (pair.size() >= 2) {
                c.setLatitude(pair.at(0).toDouble());
                c.setLongitude(pair.at(1).toDouble());
            }
        }
        if (!c.isValid()) {
            continue;
        }
        QJsonArray vertex;
        vertex.append(c.latitude());
        vertex.append(c.longitude());
        out.append(vertex);
    }
    return out;
}

QString CustomPlugin::_dayaMissionsAutosendDir() const
{
    const QByteArray env = qgetenv("DAYA_MISSIONS_AUTOSEND_DIR");
    if (!env.isEmpty()) {
        return QDir(QString::fromUtf8(env)).absolutePath();
    }
    const QFileInfo areaFi(areaScanPlanSavePath());
    QDir parentDir(areaFi.absolutePath());
    if (!parentDir.cdUp()) {
        return QString();
    }
    return parentDir.absoluteFilePath(QStringLiteral("missions"));
}

void CustomPlugin::_setupMissionsDirectoryWatcher()
{
    if (!qEnvironmentVariableIsEmpty("DAYA_QGC_DISABLE_MISSION_AUTOSEND")) {
        return;
    }
    const QString dirPath = _dayaMissionsAutosendDir();
    if (dirPath.isEmpty()) {
        qCWarning(CustomLog) << "Daya mission autosend: could not resolve missions directory";
        return;
    }
    if (!QDir().mkpath(dirPath)) {
        qCWarning(CustomLog) << "Daya mission autosend: failed to create" << dirPath;
        return;
    }

    _missionsAutosendWatcher = new QFileSystemWatcher(this);
    if (!_missionsAutosendWatcher->addPath(dirPath)) {
        qCWarning(CustomLog) << "Daya mission autosend: failed to watch" << dirPath;
        delete _missionsAutosendWatcher;
        _missionsAutosendWatcher = nullptr;
        return;
    }
    (void) connect(_missionsAutosendWatcher, &QFileSystemWatcher::directoryChanged, this, &CustomPlugin::_queueMissionsAutosendRescan);

    _missionsAutosendDebounce = new QTimer(this);
    _missionsAutosendDebounce->setSingleShot(true);
    int debounceMs = 800;
    const QByteArray debounceEnv = qgetenv("DAYA_MISSIONS_AUTOSEND_DEBOUNCE_MS");
    if (!debounceEnv.isEmpty()) {
        bool ok = false;
        const int v = debounceEnv.toInt(&ok);
        if (ok && v >= 100 && v <= 60000) {
            debounceMs = v;
        }
    }
    _missionsAutosendDebounce->setInterval(debounceMs);
    (void) connect(_missionsAutosendDebounce, &QTimer::timeout, this, &CustomPlugin::_processMissionsAutosendDirectory);

    qCInfo(CustomLog) << "Daya mission autosend: watching" << dirPath << "debounce_ms" << debounceMs;
    QTimer::singleShot(0, this, &CustomPlugin::_ensureMvmVehicleAddedHook);
    QTimer::singleShot(1500, this, &CustomPlugin::_queueMissionsAutosendRescan);
}

void CustomPlugin::_ensureMvmVehicleAddedHook()
{
    if (!qEnvironmentVariableIsEmpty("DAYA_QGC_DISABLE_MISSION_AUTOSEND")) {
        return;
    }
    if (_missionsAutosendVehicleAddedHooked) {
        return;
    }
    MultiVehicleManager *const mvm = MultiVehicleManager::instance();
    if (mvm == nullptr) {
        QTimer::singleShot(500, this, &CustomPlugin::_ensureMvmVehicleAddedHook);
        return;
    }
    _missionsAutosendVehicleAddedConnection = connect(mvm, &MultiVehicleManager::vehicleAdded, this, &CustomPlugin::_queueMissionsAutosendRescan);
    _missionsAutosendVehicleAddedHooked = true;
}

void CustomPlugin::_scheduleMissionsAutosendRetryIfNeeded(bool needed)
{
    if (!needed) {
        return;
    }
    if (_missionsAutosendRetry == nullptr) {
        _missionsAutosendRetry = new QTimer(this);
        _missionsAutosendRetry->setSingleShot(true);
        (void) connect(_missionsAutosendRetry, &QTimer::timeout, this, &CustomPlugin::_queueMissionsAutosendRescan);
    }
    int retryMs = 2500;
    const QByteArray retryEnv = qgetenv("DAYA_MISSIONS_AUTOSEND_RETRY_MS");
    if (!retryEnv.isEmpty()) {
        bool ok = false;
        const int v = retryEnv.toInt(&ok);
        if (ok && v >= 500 && v <= 120000) {
            retryMs = v;
        }
    }
    if (!_missionsAutosendRetry->isActive()) {
        _missionsAutosendRetry->start(retryMs);
    }
}

void CustomPlugin::_queueMissionsAutosendRescan()
{
    // Do not clear the full sent-mtime cache here — Execute writes drone-1/2/3.plan in a burst and
    // wiping the cache forces redundant uploads while MAVLink may still be busy on vehicle 1.
    if (_missionsAutosendDebounce != nullptr) {
        _missionsAutosendDebounce->start();
    }
}

void CustomPlugin::_clearMissionsAutosendSentCache()
{
    if (!_missionsAutosendLastSentMtimeMs.isEmpty()) {
        _missionsAutosendLastSentMtimeMs.clear();
        qCInfo(CustomLog) << "Daya mission autosend: cleared sent-mtime cache";
    }
}

void CustomPlugin::_processMissionsAutosendDirectory()
{
    if (!qEnvironmentVariableIsEmpty("DAYA_QGC_DISABLE_MISSION_AUTOSEND")) {
        return;
    }
    MultiVehicleManager *const mvm = MultiVehicleManager::instance();
    const QString dirPath = _dayaMissionsAutosendDir();
    if (dirPath.isEmpty()) {
        return;
    }
    const QDir dir(dirPath);
    if (!dir.exists()) {
        return;
    }
    QStringList names = dir.entryList(QStringList{QStringLiteral("drone-*.plan")}, QDir::Files, QDir::Name);
    static const QRegularExpression re(QStringLiteral(R"(^drone-(\d+)\.plan$)"));
    struct PendingPlan {
        int mavId = 0;
        QString path;
        qint64 mtimeMs = 0;
    };
    QList<PendingPlan> pending;
    for (const QString &name : names) {
        const QRegularExpressionMatch m = re.match(name);
        if (!m.hasMatch()) {
            continue;
        }
        const int mavId = m.captured(1).toInt();
        if (mavId <= 0 || mavId > 255) {
            continue;
        }
        const QString path = dir.absoluteFilePath(name);
        const QFileInfo fi(path);
        if (!fi.exists() || fi.size() < 32) {
            continue;
        }
        const qint64 mt = fi.lastModified().toMSecsSinceEpoch();
        if (_missionsAutosendLastSentMtimeMs.value(mavId) == mt) {
            continue;
        }
        pending.append(PendingPlan{mavId, path, mt});
    }
    std::sort(pending.begin(), pending.end(), [](const PendingPlan &a, const PendingPlan &b) {
        return a.mavId < b.mavId; // drone-1 / MAV 1 first (UAV1 often connects last in SITL)
    });

    bool scheduleRetry = false;
    bool startedUpload = false;
    for (const PendingPlan &plan : pending) {
        const int mavId = plan.mavId;
        if (mvm == nullptr) {
            scheduleRetry = true;
            continue;
        }
        Vehicle *const v = mvm->getVehicleById(mavId);
        if (v == nullptr) {
            qCInfo(CustomLog) << "Daya mission autosend: vehicle id" << mavId << "not connected — retry";
            scheduleRetry = true;
            continue;
        }
        if (!v->parameterManager()->parametersReady()) {
            qCInfo(CustomLog) << "Daya mission autosend: vehicle" << mavId << "parameters not ready — retry";
            scheduleRetry = true;
            continue;
        }
        if (v->missionManager()->inProgress()) {
            scheduleRetry = true;
            continue;
        }
        // One MAVLink mission transfer at a time (PX4 SITL + relay); finish UAV1 before UAV2/3.
        if (startedUpload) {
            scheduleRetry = true;
            continue;
        }
        qCInfo(CustomLog) << "Daya mission autosend: uploading" << plan.path << "to vehicle id" << mavId;
        PlanMasterController::sendPlanToVehicle(v, plan.path);
        _missionsAutosendLastSentMtimeMs.insert(mavId, plan.mtimeMs);
        startedUpload = true;
    }
    _scheduleMissionsAutosendRetryIfNeeded(scheduleRetry);
}

QString CustomPlugin::areaScanPlanSavePath() const
{
    const QByteArray fullPath = qgetenv("DAYA_AREA_SCAN_PLAN");
    if (!fullPath.isEmpty()) {
        return QString::fromUtf8(fullPath);
    }
    const QByteArray dir = qgetenv("DAYA_AREA_PLAN_DIR");
    if (!dir.isEmpty()) {
        return QDir(QString::fromUtf8(dir)).filePath(QStringLiteral("area.plan"));
    }
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return docs + QStringLiteral("/Daya/area_to_scan/area.plan");
}

QString CustomPlugin::dayaStationParamsSavePath(void) const
{
    const QFileInfo fi(areaScanPlanSavePath());
    return QFileInfo(fi.absoluteDir(), QStringLiteral("daya_station_params.json")).absoluteFilePath();
}

QString CustomPlugin::navaidZonePlanSavePath() const
{
    // One level above the area-scan dir (area.plan lives in e.g. data/area_to_scan, navaid_zone.plan in data/).
    const QFileInfo areaFi(areaScanPlanSavePath());
    QDir dir(areaFi.absolutePath());
    if (!dir.cdUp()) {
        dir = QDir(areaFi.absolutePath());
    }
    return dir.absoluteFilePath(QStringLiteral("navaid_zone.plan"));
}

QString CustomPlugin::beaconsPlanSavePath() const
{
    const QByteArray fullPath = qgetenv("DAYA_BEACONS_PLAN");
    if (!fullPath.isEmpty()) {
        return QString::fromUtf8(fullPath);
    }
    // Default: one level above the area-scan dir (area.plan lives in e.g. data/area_to_scan, beacons in data/).
    const QFileInfo areaFi(areaScanPlanSavePath());
    QDir dir(areaFi.absolutePath());
    if (!dir.cdUp()) {
        dir = QDir(areaFi.absolutePath());
    }
    return dir.absoluteFilePath(QStringLiteral("beacons_locations.plan"));
}

int CustomPlugin::dayaConnectedVehicleCount(void) const
{
    MultiVehicleManager *const mvm = MultiVehicleManager::instance();
    if (mvm == nullptr) {
        return 0;
    }
    QmlObjectListModel *const vehicles = mvm->vehicles();
    if (vehicles == nullptr) {
        return 0;
    }
    return vehicles->count();
}

QVariantMap CustomPlugin::loadDayaStationParams(void) const
{
    QVariantMap m;
    m.insert(QStringLiteral("survey_alt_m"), 30.0);
    m.insert(QStringLiteral("revisit_s"), 8.0);
    m.insert(QStringLiteral("camera_hfov_deg"), 78.0);
    m.insert(QStringLiteral("drone_count"), 3);
    const QString path = dayaStationParamsSavePath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return m;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return m;
    }
    const QJsonObject o = doc.object();
    if (const QJsonValue v = o.value(QStringLiteral("survey_alt_m")); v.isDouble()) {
        m.insert(QStringLiteral("survey_alt_m"), v.toDouble());
    }
    if (const QJsonValue v = o.value(QStringLiteral("revisit_s")); v.isDouble()) {
        m.insert(QStringLiteral("revisit_s"), v.toDouble());
    }
    if (const QJsonValue v = o.value(QStringLiteral("camera_hfov_deg")); v.isDouble()) {
        m.insert(QStringLiteral("camera_hfov_deg"), v.toDouble());
    }
    if (const QJsonValue dc = o.value(QStringLiteral("drone_count")); dc.isDouble()) {
        m.insert(QStringLiteral("drone_count"), dc.toInt());
    }
    if (const QJsonValue sc = o.value(QStringLiteral("split_drone_count")); sc.isDouble()) {
        m.insert(QStringLiteral("split_drone_count"), sc.toInt());
    }
    return m;
}

bool CustomPlugin::saveDayaStationParams(
    double surveyAltM,
    double revisitS,
    double cameraHfovDeg,
    int requestedDroneCount,
    int splitDroneCount)
{
    const QString path = dayaStationParamsSavePath();
    const QFileInfo fi(path);
    if (!QDir().mkpath(fi.absolutePath())) {
        qCWarning(CustomLog) << "saveDayaStationParams: failed to create directory" << fi.absolutePath();
        return false;
    }
    QJsonObject o;
    o.insert(QStringLiteral("version"), 1);
    o.insert(QStringLiteral("survey_alt_m"), surveyAltM);
    o.insert(QStringLiteral("revisit_s"), revisitS);
    o.insert(QStringLiteral("camera_hfov_deg"), cameraHfovDeg);
    if (requestedDroneCount >= 1) {
        o.insert(QStringLiteral("drone_count"), requestedDroneCount);
    }
    if (splitDroneCount >= 1) {
        o.insert(QStringLiteral("split_drone_count"), splitDroneCount);
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(CustomLog) << "saveDayaStationParams: open failed" << path;
        return false;
    }
    const QByteArray bytes = QJsonDocument(o).toJson(QJsonDocument::Indented);
    if (f.write(bytes) != bytes.size()) {
        qCWarning(CustomLog) << "saveDayaStationParams: short write" << path;
        return false;
    }
    f.close();
    qCInfo(CustomLog) << "Daya station params saved" << path;
    _clearMissionsAutosendSentCache();
    _queueMissionsAutosendRescan();
    return true;
}

bool CustomPlugin::dayaRefreshMissionFromVehicle(void)
{
    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    if (vehicle == nullptr) {
        qCWarning(CustomLog) << "dayaRefreshMissionFromVehicle: no active vehicle";
        return false;
    }
    vehicle->missionManager()->loadFromVehicle();
    return true;
}

bool CustomPlugin::dayaLoadDronePlanInPlanView(QObject *planMaster, int mavId)
{
    if (planMaster == nullptr || mavId <= 0) {
        return false;
    }
    auto *const pmc = qobject_cast<PlanMasterController *>(planMaster);
    if (pmc == nullptr) {
        qCWarning(CustomLog) << "dayaLoadDronePlanInPlanView: planMaster is not a PlanMasterController";
        return false;
    }
    const QString dirPath = _dayaMissionsAutosendDir();
    if (dirPath.isEmpty()) {
        return false;
    }
    const QString path = QDir(dirPath).absoluteFilePath(QStringLiteral("drone-%1.plan").arg(mavId));
    const QFileInfo fi(path);
    if (!fi.exists() || fi.size() < 32) {
        qCInfo(CustomLog) << "dayaLoadDronePlanInPlanView: no plan file" << path;
        return false;
    }
    pmc->loadFromFile(path);
    qCInfo(CustomLog) << "Daya Plan View: loaded" << path;
    return true;
}

bool CustomPlugin::saveFlyViewRegionPlan(QObject *planMaster, const QString &filePath, const QVariantList &polygonCoordinates)
{
    auto *pm = qobject_cast<PlanMasterController *>(planMaster);
    if (!pm) {
        qCWarning(CustomLog) << "saveFlyViewRegionPlan: planMaster is not a PlanMasterController";
        return false;
    }
    const QJsonArray verts = _variantListToVertexJson(polygonCoordinates);
    if (verts.size() < 3) {
        qCWarning(CustomLog) << "saveFlyViewRegionPlan: need at least 3 vertices";
        return false;
    }
    const QFileInfo fi(filePath);
    if (!QDir().mkpath(fi.absolutePath())) {
        qCWarning(CustomLog) << "saveFlyViewRegionPlan: failed to create directory" << fi.absolutePath();
        return false;
    }
    _pendingDayaRegionVertices = verts;
    const bool ok = pm->saveToFile(filePath);
    _pendingDayaRegionVertices = QJsonArray();
    return ok;
}

bool CustomPlugin::saveBeaconsPlan(QObject *planMaster, const QString &filePath, const QVariantList &beaconCoordinates)
{
    auto *pm = qobject_cast<PlanMasterController *>(planMaster);
    if (!pm) {
        qCWarning(CustomLog) << "saveBeaconsPlan: planMaster is not a PlanMasterController";
        return false;
    }
    const QJsonArray beacons = _variantListToVertexJson(beaconCoordinates);
    if (beacons.isEmpty()) {
        qCWarning(CustomLog) << "saveBeaconsPlan: need at least one beacon";
        return false;
    }
    const QFileInfo fi(filePath);
    if (!QDir().mkpath(fi.absolutePath())) {
        qCWarning(CustomLog) << "saveBeaconsPlan: failed to create directory" << fi.absolutePath();
        return false;
    }
    _pendingDayaBeacons = beacons;
    const bool ok = pm->saveToFile(filePath);
    _pendingDayaBeacons = QJsonArray();
    if (ok) {
        qCInfo(CustomLog) << "Daya beacons saved" << filePath << "count" << beacons.size();
    }
    return ok;
}

bool CustomPlugin::saveNavaidZonePlan(QObject *planMaster, const QString &filePath, const QVariantList &polygonCoordinates)
{
    auto *pm = qobject_cast<PlanMasterController *>(planMaster);
    if (!pm) {
        qCWarning(CustomLog) << "saveNavaidZonePlan: planMaster is not a PlanMasterController";
        return false;
    }
    const QJsonArray verts = _variantListToVertexJson(polygonCoordinates);
    if (verts.size() < 3) {
        qCWarning(CustomLog) << "saveNavaidZonePlan: need at least 3 vertices";
        return false;
    }
    const QFileInfo fi(filePath);
    if (!QDir().mkpath(fi.absolutePath())) {
        qCWarning(CustomLog) << "saveNavaidZonePlan: failed to create directory" << fi.absolutePath();
        return false;
    }
    _pendingDayaNavaidVertices = verts;
    const bool ok = pm->saveToFile(filePath);
    _pendingDayaNavaidVertices = QJsonArray();
    if (ok) {
        qCInfo(CustomLog) << "Daya navaid zone saved" << filePath << "vertices" << verts.size();
    }
    return ok;
}

void CustomPlugin::postSaveToJson(PlanMasterController *pController, QJsonObject &json)
{
    QGCCorePlugin::postSaveToJson(pController, json);
    if (!_pendingDayaBeacons.isEmpty()) {
        QJsonObject beacons;
        beacons[QStringLiteral("version")] = 1;
        beacons[QStringLiteral("points")] = _pendingDayaBeacons;
        json[QStringLiteral("dayaBeacons")] = beacons;
    }
    if (!_pendingDayaNavaidVertices.isEmpty()) {
        QJsonObject zone;
        zone[QStringLiteral("version")] = 1;
        zone[QStringLiteral("vertices")] = _pendingDayaNavaidVertices;
        json[QStringLiteral("dayaNavaidZonePolygon")] = zone;
    }
    if (_pendingDayaRegionVertices.isEmpty()) {
        return;
    }
    QJsonObject region;
    region[QStringLiteral("version")] = 1;
    region[QStringLiteral("vertices")] = _pendingDayaRegionVertices;
    json[QStringLiteral("dayaRegionPolygon")] = region;

    const QString paramsPath = dayaStationParamsSavePath();
    QFile pf(paramsPath);
    if (pf.open(QIODevice::ReadOnly)) {
        QJsonParseError perr{};
        const QJsonDocument pdoc = QJsonDocument::fromJson(pf.readAll(), &perr);
        if (perr.error == QJsonParseError::NoError && pdoc.isObject()) {
            const QJsonObject params = pdoc.object();
            json[QStringLiteral("dayaStationParams")] = params;
            const QJsonValue altV = params.value(QStringLiteral("survey_alt_m"));
            if (altV.isDouble()) {
                QJsonObject mission = json.value(QStringLiteral("mission")).toObject();
                QJsonArray home = mission.value(QStringLiteral("plannedHomePosition")).toArray();
                if (home.size() >= 3) {
                    home[2] = altV.toDouble();
                    mission.insert(QStringLiteral("plannedHomePosition"), home);
                    json.insert(QStringLiteral("mission"), mission);
                }
            }
        }
    }
}

void CustomPlugin::cleanup()
{
    (void) disconnect(_missionsAutosendVehicleAddedConnection);
    _missionsAutosendVehicleAddedConnection = QMetaObject::Connection();
    _missionsAutosendVehicleAddedHooked = false;
    if (_missionsAutosendRetry != nullptr) {
        _missionsAutosendRetry->stop();
    }
    if (_missionsAutosendDebounce != nullptr) {
        _missionsAutosendDebounce->stop();
    }
    if (_missionsAutosendWatcher != nullptr) {
        _missionsAutosendWatcher->removePaths(_missionsAutosendWatcher->directories());
    }

    if (_qmlEngine) {
        _qmlEngine->removeUrlInterceptor(_selector);
    }

    delete _selector;
}

void CustomPlugin::_advancedChanged(bool changed)
{
    // Firmware Upgrade page is only show in Advanced mode
    emit _options->showFirmwareUpgradeChanged(changed);
}

void CustomPlugin::_addSettingsEntry(const QString &title, const char *qmlFile, const char *iconFile)
{
    Q_CHECK_PTR(qmlFile);
    // 'this' instance will take ownership on the QmlComponentInfo instance
    _customSettingsList.append(QVariant::fromValue(
        new QmlComponentInfo(
            title,
            QUrl::fromUserInput(qmlFile),
            !iconFile ? QUrl() : QUrl::fromUserInput(iconFile),
            this)
        )
    );
}

void CustomPlugin::adjustSettingMetaData(const QString& settingsGroup, FactMetaData& metaData, bool &userVisible)
{
    QGCCorePlugin::adjustSettingMetaData(settingsGroup, metaData, userVisible);

    if (settingsGroup == AppSettings::settingsGroup) {
        // This tells QGC than when you are creating Plans while not connected to a vehicle
        // the specific firmware/vehicle the plan is for.
        if (metaData.name() == AppSettings::offlineEditingFirmwareClassName) {
            metaData.setRawDefaultValue(QGCMAVLink::FirmwareClassPX4);
            userVisible = false;
            return;
        } else if (metaData.name() == AppSettings::offlineEditingVehicleClassName) {
            metaData.setRawDefaultValue(QGCMAVLink::VehicleClassMultiRotor);
            userVisible = false;
            return;
        }
    }
}

void CustomPlugin::paletteOverride(const QString &colorName, QGCPalette::PaletteColorInfo_t& colorInfo)
{
    if (colorName == QStringLiteral("window")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#212529");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#f8f9fa");
    } else if (colorName == QStringLiteral("windowShade")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#343a40");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#343a40");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#f1f3f5");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#d9d9d9");
    } else if (colorName == QStringLiteral("windowShadeDark")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#1a1c1f");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#1a1c1f");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#e9ecef");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#bdbdbd");
    } else if (colorName == QStringLiteral("text")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#ffffff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#777c89");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#9d9d9d");
    } else if (colorName == QStringLiteral("warningText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#e03131");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#e03131");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#cc0808");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#cc0808");
    } else if (colorName == QStringLiteral("button")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#495057");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#495057");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#ffffff");
    } else if (colorName == QStringLiteral("buttonText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#ffffff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#777c89");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#9d9d9d");
    } else if (colorName == QStringLiteral("buttonHighlight")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#07916d");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#495057");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#aeebd0");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#e4e4e4");
    } else if (colorName == QStringLiteral("buttonHighlightText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#ffffff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#777c89");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#2c2c2c");
    } else if (colorName == QStringLiteral("primaryButton")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#12b886");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#495057");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#aeebd0");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#585858");
    } else if (colorName == QStringLiteral("primaryButtonText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#ffffff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#cad0d0");
    } else if (colorName == QStringLiteral("textField")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#212529");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#495057");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#f1f3f5");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#ffffff");
    } else if (colorName == QStringLiteral("textFieldText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#ffffff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#777c89");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#808080");
    } else if (colorName == QStringLiteral("mapButton")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#000000");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#585858");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#585858");
    } else if (colorName == QStringLiteral("mapButtonHighlight")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#07916d");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#585858");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#be781c");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#585858");
    } else if (colorName == QStringLiteral("mapIndicator")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#9dda4f");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#585858");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#be781c");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#585858");
    } else if (colorName == QStringLiteral("mapIndicatorChild")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#527942");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#585858");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#766043");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#585858");
    } else if (colorName == QStringLiteral("colorGreen")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#27bf89");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#0ca678");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#009431");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#009431");
    } else if (colorName == QStringLiteral("colorOrange")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#f7b24a");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#f6921e");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#b95604");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#b95604");
    } else if (colorName == QStringLiteral("colorRed")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#e1544c");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#e03131");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#ed3939");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#ed3939");
    } else if (colorName == QStringLiteral("colorGrey")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#8b90a0");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#8b90a0");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#808080");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#808080");
    } else if (colorName == QStringLiteral("colorBlue")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#228be6");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#228be6");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#1a72ff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#1a72ff");
    } else if (colorName == QStringLiteral("alertBackground")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#d4b106");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#d4b106");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#fffb8f");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#b45d48");
    } else if (colorName == QStringLiteral("alertBorder")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#876800");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#876800");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#808080");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#808080");
    } else if (colorName == QStringLiteral("alertText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#000000");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#fff9ed");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#fff9ed");
    } else if (colorName == QStringLiteral("missionItemEditor")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#212529");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#0b1420");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#585858");
    } else if (colorName == QStringLiteral("hoverColor")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#07916d");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#33c494");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#aeebd0");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#464f5a");
    } else if (colorName == QStringLiteral("mapWidgetBorderLight")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#ffffff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#f1f3f5");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#ffffff");
    } else if (colorName == QStringLiteral("mapWidgetBorderDark")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#000000");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#000000");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#212529");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#000000");
    } else if (colorName == QStringLiteral("brandingPurple")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#4a2c6d");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#4a2c6d");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#4a2c6d");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#4a2c6d");
    } else if (colorName == QStringLiteral("brandingBlue")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#6045c5");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#48d6ff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#6045c5");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#48d6ff");
    }
}

QQmlApplicationEngine* CustomPlugin::createQmlApplicationEngine(QObject* parent)
{
    _qmlEngine = QGCCorePlugin::createQmlApplicationEngine(parent);
    _qmlEngine->addImportPath("qrc:/qml/Custom/Widgets");
    // TODO: Investigate _qmlEngine->setExtraSelectors({"custom"})

    _selector = new CustomOverrideInterceptor();
    _qmlEngine->addUrlInterceptor(_selector);

    // Expose custom invokables to all QML (including nested JS handlers where `import Daya` can fail).
    _qmlEngine->rootContext()->setContextProperty(QStringLiteral("DayaCustom"), this);

    _setupMissionsDirectoryWatcher();

    return _qmlEngine;
}

/*===========================================================================*/

CustomOverrideInterceptor::CustomOverrideInterceptor()
    : QQmlAbstractUrlInterceptor()
{

}

QUrl CustomOverrideInterceptor::intercept(const QUrl &url, QQmlAbstractUrlInterceptor::DataType type)
{
    switch (type) {
    case QQmlAbstractUrlInterceptor::QmlFile:
    case QQmlAbstractUrlInterceptor::UrlString:
        if (url.scheme() == QStringLiteral("qrc")) {
            const QString origPath = url.path();
            const QString overrideRes = QStringLiteral(":/Custom%1").arg(origPath);
            if (QFile::exists(overrideRes)) {
                const QString relPath = overrideRes.mid(2);
                QUrl result;
                result.setScheme(QStringLiteral("qrc"));
                result.setPath('/' + relPath);
                return result;
            }
        }
        break;
    default:
        break;
    }

    return url;
}
