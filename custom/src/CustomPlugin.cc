#include "CustomPlugin.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "PlanMasterController.h"
#include "QmlComponentInfo.h"
#include "QGCLoggingCategory.h"
#include "QGCPalette.h"
#include "QGCMAVLink.h"
#include "AppSettings.h"
#include "Vehicle.h"

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
    if (!_missionsAutosendRetry->isActive()) {
        _missionsAutosendRetry->start(2500);
    }
}

void CustomPlugin::_queueMissionsAutosendRescan()
{
    if (_missionsAutosendDebounce != nullptr) {
        _missionsAutosendDebounce->start();
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
    const QStringList names = dir.entryList(QStringList{QStringLiteral("drone-*.plan")}, QDir::Files, QDir::Name);
    static const QRegularExpression re(QStringLiteral(R"(^drone-(\d+)\.plan$)"));
    bool scheduleRetry = false;
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
        if (mvm == nullptr) {
            scheduleRetry = true;
            continue;
        }
        Vehicle *const v = mvm->getVehicleById(mavId);
        if (v == nullptr) {
            scheduleRetry = true;
            continue;
        }
        if (!v->parameterManager()->parametersReady()) {
            scheduleRetry = true;
            continue;
        }
        if (!v->initialPlanRequestComplete()) {
            scheduleRetry = true;
            continue;
        }
        if (v->missionManager()->inProgress()) {
            scheduleRetry = true;
            continue;
        }
        qCInfo(CustomLog) << "Daya mission autosend: uploading" << path << "to vehicle id" << mavId;
        PlanMasterController::sendPlanToVehicle(v, path);
        _missionsAutosendLastSentMtimeMs.insert(mavId, mt);
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

QVariantMap CustomPlugin::loadDayaStationParams(void) const
{
    QVariantMap m;
    m.insert(QStringLiteral("survey_alt_m"), 30.0);
    m.insert(QStringLiteral("revisit_s"), 8.0);
    m.insert(QStringLiteral("camera_hfov_deg"), 78.0);
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
    return m;
}

bool CustomPlugin::saveDayaStationParams(double surveyAltM, double revisitS, double cameraHfovDeg)
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

void CustomPlugin::postSaveToJson(PlanMasterController *pController, QJsonObject &json)
{
    QGCCorePlugin::postSaveToJson(pController, json);
    if (_pendingDayaRegionVertices.isEmpty()) {
        return;
    }
    QJsonObject region;
    region[QStringLiteral("version")] = 1;
    region[QStringLiteral("vertices")] = _pendingDayaRegionVertices;
    json[QStringLiteral("dayaRegionPolygon")] = region;
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
