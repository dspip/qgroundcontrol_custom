#pragma once

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaObject>
#include <QtCore/QTranslator>
#include <QtQml/QQmlAbstractUrlInterceptor>

#include "QGCCorePlugin.h"
#include "QGCOptions.h"

class CustomOptions;
class CustomPlugin;
class CustomSettings;
class QFileSystemWatcher;
class QQmlApplicationEngine;
class QTimer;

Q_DECLARE_LOGGING_CATEGORY(CustomLog)

class CustomFlyViewOptions : public QGCFlyViewOptions
{
    Q_OBJECT

public:
    explicit CustomFlyViewOptions(CustomOptions *options, QObject *parent = nullptr);

    // Overrides from CustomFlyViewOptions

    /// This custom build has it's own custom instrument panel. Don't show regular one.
    bool showInstrumentPanel() const final { return false; }
    /// This custom build does not support conecting multiple vehicles to it.
    /// This in turn simplifies various parts of the QGC ui.
    bool showMultiVehicleList() const final { return false; }
};

/*===========================================================================*/

class CustomOptions : public QGCOptions
{
    Q_OBJECT

public:
    explicit CustomOptions(CustomPlugin *plugin, QObject *parent = nullptr);

    // Overrides from QGCOptions

    /// Firmware upgrade page is only shown in Advanced Mode.
    bool showFirmwareUpgrade() const final { return _plugin->showAdvancedUI(); }
    QGCFlyViewOptions *flyViewOptions() const final { return _flyViewOptions; }

private:
    QGCCorePlugin *_plugin = nullptr;
    CustomFlyViewOptions *_flyViewOptions = nullptr;
};

/*===========================================================================*/

class CustomPlugin : public QGCCorePlugin
{
    Q_OBJECT

public:
    explicit CustomPlugin(QObject *parent = nullptr);

    static QGCCorePlugin *instance();

    /// Save current Fly view region polygon into a QGC .plan file (adds top-level `dayaRegionPolygon` for downstream tools).
    Q_INVOKABLE bool saveFlyViewRegionPlan(QObject *planMaster, const QString &filePath, const QVariantList &polygonCoordinates);

    /// Target path for Daya "Execute" (env `DAYA_AREA_SCAN_PLAN` full path, else `DAYA_AREA_PLAN_DIR`/area.plan, else Documents/…).
    Q_INVOKABLE QString areaScanPlanSavePath() const;

    /// ``daya_station_params.json`` next to ``area.plan`` (survey altitude, revisit interval, camera HFOV for the Station app).
    Q_INVOKABLE QString dayaStationParamsSavePath(void) const;
    Q_INVOKABLE QVariantMap loadDayaStationParams(void) const;
    Q_INVOKABLE bool saveDayaStationParams(double surveyAltM, double revisitS, double cameraHfovDeg);

    /// Pull mission (and follow-on geo/rally chain) from the active vehicle into QGC. Safe from Fly view.
    Q_INVOKABLE bool dayaRefreshMissionFromVehicle(void);

    /// Load ``drone-{mavId}.plan`` from the missions directory into Plan View (does not upload to the vehicle).
    Q_INVOKABLE bool dayaLoadDronePlanInPlanView(QObject *planMaster, int mavId);

    // Overrides from QGCCorePlugin

    void cleanup() final;
    QGCOptions *options() final { return _options; }
    /// This allows you to override/hide QGC Application settings
    void adjustSettingMetaData(const QString &settingsGroup, FactMetaData &metaData, bool &userVisible) final;
    /// This modifies QGC colors palette to match possible custom corporate branding
    void paletteOverride(const QString &colorName, QGCPalette::PaletteColorInfo_t &colorInfo) final;
    /// We override this so we can get access to QQmlApplicationEngine and use it to register our qml module
    QQmlApplicationEngine *createQmlApplicationEngine(QObject *parent) final;

    void postSaveToJson(PlanMasterController *pController, QJsonObject &json) final;

private slots:
    void _advancedChanged(bool advanced);
    void _queueMissionsAutosendRescan();
    void _processMissionsAutosendDirectory();
    void _ensureMvmVehicleAddedHook();

private:
    QString _dayaMissionsAutosendDir() const;
    void _setupMissionsDirectoryWatcher();
    void _clearMissionsAutosendSentCache();
    void _scheduleMissionsAutosendRetryIfNeeded(bool needed);
    void _addSettingsEntry(const QString& title, const char* qmlFile, const char* iconFile = nullptr);

    CustomOptions *_options = nullptr;
    QQmlApplicationEngine *_qmlEngine = nullptr;
    class CustomOverrideInterceptor *_selector = nullptr;
    QVariantList _customSettingsList; // Not to be mixed up with QGCCorePlugin implementation

    QJsonArray _pendingDayaRegionVertices;
    static QJsonArray _variantListToVertexJson(const QVariantList &list);

    QFileSystemWatcher *_missionsAutosendWatcher = nullptr;
    QTimer *_missionsAutosendDebounce = nullptr;
    QHash<int, qint64> _missionsAutosendLastSentMtimeMs;
    QMetaObject::Connection _missionsAutosendVehicleAddedConnection;
    QTimer *_missionsAutosendRetry = nullptr;
    bool _missionsAutosendVehicleAddedHooked = false;
};

/*===========================================================================*/

class CustomOverrideInterceptor : public QQmlAbstractUrlInterceptor
{
public:
    CustomOverrideInterceptor();

    QUrl intercept(const QUrl &url, QQmlAbstractUrlInterceptor::DataType type) final;
};
