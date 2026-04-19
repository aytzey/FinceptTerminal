// src/services/maritime/MaritimeService.cpp
#include "services/maritime/MaritimeService.h"

#include "auth/AuthManager.h"
#include "core/logging/Logger.h"
#include "network/http/HttpClient.h"
#include "python/PythonRunner.h"
#include "storage/cache/CacheManager.h"

#    include "datahub/DataHub.h"
#    include "datahub/DataHubMetaTypes.h"

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcessEnvironment>

namespace fincept::services::maritime {

namespace {
inline void publish_to_hub(const QString& topic, const QVariant& value) {
    fincept::datahub::DataHub::instance().publish(topic, value);
}

bool local_maritime_enabled() {
    const auto& auth = auth::AuthManager::instance();
    return auth.has_local_runtime() || !auth.has_fincept_api_key();
}

QString local_unavailable_message() {
    return QStringLiteral("Local maritime AIS provider is not configured. Fincept Cloud fallback is disabled; "
                          "use cached vessel data or set AISSTREAM_API_KEY, MARINETRAFFIC_API_KEY, or GFW_API_KEY.");
}

bool has_env(const QString& key) {
    return !QProcessEnvironment::systemEnvironment().value(key).trimmed().isEmpty();
}

double json_number(const QJsonObject& obj, std::initializer_list<const char*> keys, double fallback = 0.0) {
    for (const char* key : keys) {
        const auto value = obj.value(QString::fromLatin1(key));
        if (value.isDouble())
            return value.toDouble();
        if (value.isString()) {
            bool ok = false;
            const double parsed = value.toString().toDouble(&ok);
            if (ok)
                return parsed;
        }
    }
    return fallback;
}

QString json_string(const QJsonObject& obj, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const auto value = obj.value(QString::fromLatin1(key));
        if (value.isString() && !value.toString().trimmed().isEmpty())
            return value.toString().trimmed();
        if (value.isDouble())
            return QString::number(value.toDouble(), 'f', 0);
    }
    return {};
}

QJsonDocument parse_python_json(const fincept::python::PythonResult& result, QString* error = nullptr) {
    if (!result.success) {
        if (error)
            *error = result.error.isEmpty() ? QStringLiteral("Python provider failed") : result.error;
        return {};
    }
    QJsonParseError parse_error;
    const QString json = fincept::python::extract_json(result.output);
    const auto doc = QJsonDocument::fromJson(json.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError && error)
        *error = QStringLiteral("Provider returned invalid JSON: ") + parse_error.errorString();
    return doc;
}

QJsonArray first_array_payload(const QJsonValue& value) {
    if (value.isArray())
        return value.toArray();
    if (!value.isObject())
        return {};
    const QJsonObject obj = value.toObject();
    for (const QString& key : {"vessels", "results", "data", "tracks", "positions", "history", "features"}) {
        if (obj.value(key).isArray())
            return obj.value(key).toArray();
    }
    return {};
}

QJsonObject first_object_payload(const QJsonValue& value) {
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        if (obj.contains("error"))
            return {};
        for (const QString& key : {"vessel", "data", "result"}) {
            if (obj.value(key).isObject())
                return obj.value(key).toObject();
        }
        const QJsonArray arr = first_array_payload(obj);
        if (!arr.isEmpty() && arr.first().isObject())
            return arr.first().toObject();
        return obj;
    }
    if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        if (!arr.isEmpty() && arr.first().isObject())
            return arr.first().toObject();
    }
    return {};
}
}  // namespace

static constexpr const char* kMarineBase = "https://api.fincept.in/marine";
static constexpr int kVesselTtlSec = 60;      // position data: 1 min
static constexpr int kHistoryTtlSec = 5 * 60; // history: 5 min

// ── Singleton ────────────────────────────────────────────────────────────────
MaritimeService& MaritimeService::instance() {
    static MaritimeService inst;
    return inst;
}

MaritimeService::MaritimeService(QObject* parent) : QObject(parent) {}

// ── Parse vessel from JSON ───────────────────────────────────────────────────
VesselData MaritimeService::parse_vessel(const QJsonObject& obj) const {
    VesselData v;
    v.id = obj["id"].toInt();
    v.imo = obj["imo"].toString();
    v.name = obj["name"].toString();
    v.latitude = obj["last_pos_latitude"].toString().toDouble();
    v.longitude = obj["last_pos_longitude"].toString().toDouble();
    v.speed = obj["last_pos_speed"].toString().toDouble();
    v.angle = obj["last_pos_angle"].toString().toDouble();
    v.from_port = obj["route_from_port_name"].toString();
    v.to_port = obj["route_to_port_name"].toString();
    v.from_date = obj["route_from_date"].toString();
    v.to_date = obj["route_to_date"].toString();
    v.route_progress = obj["route_progress"].toString().toDouble();
    v.draught = obj["current_draught"].toString().toDouble();
    v.last_updated = obj["last_pos_updated_at"].toString();
    v.fetched_at = obj["fetched_at"].toString();
    return v;
}

VesselData MaritimeService::parse_local_vessel(const QJsonObject& obj) const {
    QJsonObject src = obj;
    if (obj.value("properties").isObject())
        src = obj.value("properties").toObject();
    if (obj.value("registryInfo").isObject()) {
        QJsonObject merged = obj.value("registryInfo").toObject();
        for (auto it = obj.begin(); it != obj.end(); ++it)
            merged.insert(it.key(), it.value());
        src = merged;
    }

    VesselData v;
    v.id = static_cast<int>(json_number(src, {"id", "vesselId"}, 0.0));
    v.imo = json_string(src, {"imo", "IMO", "imoNumber", "ssvid", "mmsi", "MMSI"});
    v.name = json_string(src, {"name", "vessel_name", "shipname", "SHIPNAME", "vesselName", "callsign"});
    v.latitude = json_number(src, {"lat", "latitude", "LAT", "last_pos_latitude"});
    v.longitude = json_number(src, {"lon", "lng", "longitude", "LON", "last_pos_longitude"});
    v.speed = json_number(src, {"speed", "sog", "SOG", "last_pos_speed"});
    v.angle = json_number(src, {"course", "cog", "COG", "heading", "last_pos_angle"});
    v.from_port = json_string(src, {"from_port", "route_from_port_name", "origin"});
    v.to_port = json_string(src, {"to_port", "route_to_port_name", "destination"});
    v.from_date = json_string(src, {"from_date", "route_from_date", "startDate"});
    v.to_date = json_string(src, {"to_date", "route_to_date", "endDate"});
    v.draught = json_number(src, {"draught", "DRAUGHT", "current_draught"});
    v.last_updated = json_string(src, {"timestamp", "last_timestamp", "last_pos_updated_at", "date"});
    v.fetched_at = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    return v;
}

bool MaritimeService::try_local_area_provider(const AreaSearchParams& params) {
    if (!has_env("AISSTREAM_API_KEY"))
        return false;

    QPointer<MaritimeService> self = this;
    const QStringList args = {"area",
                              QString::number(params.min_lat, 'f', 6),
                              QString::number(params.min_lng, 'f', 6),
                              QString::number(params.max_lat, 'f', 6),
                              QString::number(params.max_lng, 'f', 6)};
    python::PythonRunner::instance().run("aisstream_data.py", args, [self](python::PythonResult result) {
        if (!self)
            return;
        QString error;
        const QJsonDocument doc = parse_python_json(result, &error);
        if (!error.isEmpty()) {
            emit self->error_occurred("area_search", error);
            return;
        }
        const QJsonArray arr = first_array_payload(doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()));
        QVector<VesselData> vessels;
        QJsonArray cache_arr;
        vessels.reserve(arr.size());
        for (const auto& value : arr) {
            if (!value.isObject())
                continue;
            const VesselData vessel = self->parse_local_vessel(value.toObject());
            if (!vessel.imo.isEmpty() || !vessel.name.isEmpty()) {
                vessels.append(vessel);
                cache_arr.append(value.toObject());
            }
        }
        if (vessels.isEmpty()) {
            emit self->error_occurred("area_search", "AISStream returned no vessels for this area.");
            return;
        }
        fincept::CacheManager::instance().put(
            "maritime:local:area:last",
            QVariant(QString::fromUtf8(QJsonDocument(cache_arr).toJson(QJsonDocument::Compact))),
            kVesselTtlSec, "maritime");
        emit self->vessels_loaded(vessels, vessels.size());
        if (self->hub_registered_)
            publish_to_hub(QStringLiteral("maritime:vessels:multi"), QVariant::fromValue(vessels));
    });
    return true;
}

bool MaritimeService::try_local_vessel_provider(const QString& id) {
    QString script;
    QStringList args;
    QString provider;
    if (has_env("AISSTREAM_API_KEY")) {
        script = "aisstream_data.py";
        args = {"vessel", id.trimmed()};
        provider = "aisstream";
    } else if (has_env("MARINETRAFFIC_API_KEY")) {
        script = "marinetraffic_data.py";
        args = {"positions", id.trimmed()};
        provider = "marinetraffic";
    } else {
        return false;
    }

    QPointer<MaritimeService> self = this;
    const QString cache_key = "maritime:vessel:" + id.trimmed();
    python::PythonRunner::instance().run(script, args, [self, cache_key, provider, id](python::PythonResult result) {
        if (!self)
            return;
        QString error;
        const QJsonDocument doc = parse_python_json(result, &error);
        if (!error.isEmpty()) {
            emit self->error_occurred("vessel_position", provider + ": " + error);
            return;
        }
        const QJsonObject obj = first_object_payload(doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()));
        if (obj.isEmpty()) {
            emit self->error_occurred("vessel_position", provider + " returned no vessel for " + id);
            return;
        }
        fincept::CacheManager::instance().put(
            cache_key, QVariant(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact))),
            kVesselTtlSec, "maritime");
        const VesselData vessel = self->parse_local_vessel(obj);
        emit self->vessel_found(vessel);
        if (self->hub_registered_)
            publish_to_hub(QStringLiteral("maritime:vessel:") + id.trimmed(), QVariant::fromValue(vessel));
    });
    return true;
}

bool MaritimeService::try_local_history_provider(const QString& id) {
    if (!has_env("AISSTREAM_API_KEY"))
        return false;
    const QDate end = QDate::currentDate();
    const QDate start = end.addDays(-30);
    QPointer<MaritimeService> self = this;
    const QString cache_key = "maritime:history:" + id.trimmed();
    python::PythonRunner::instance().run(
        "aisstream_data.py", {"track", id.trimmed(), start.toString(Qt::ISODate), end.toString(Qt::ISODate)},
        [self, cache_key, id](python::PythonResult result) {
            if (!self)
                return;
            QString error;
            const QJsonDocument doc = parse_python_json(result, &error);
            if (!error.isEmpty()) {
                emit self->error_occurred("vessel_history", "aisstream: " + error);
                return;
            }
            const QJsonArray arr = first_array_payload(doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()));
            QVector<VesselData> history;
            QJsonArray cache_arr;
            history.reserve(arr.size());
            for (const auto& value : arr) {
                if (!value.isObject())
                    continue;
                history.append(self->parse_local_vessel(value.toObject()));
                cache_arr.append(value.toObject());
            }
            if (history.isEmpty()) {
                emit self->error_occurred("vessel_history", "AISStream returned no track points for " + id);
                return;
            }
            fincept::CacheManager::instance().put(
                cache_key, QVariant(QString::fromUtf8(QJsonDocument(cache_arr).toJson(QJsonDocument::Compact))),
                kHistoryTtlSec, "maritime");
            emit self->vessel_history_loaded(history);
            if (self->hub_registered_)
                publish_to_hub(QStringLiteral("maritime:history:") + id.trimmed(), QVariant::fromValue(history));
        });
    return true;
}

// ── Helper: unwrap {success, data: {...}} envelope ──────────────────────────
static QJsonObject unwrap(const QJsonObject& root) {
    if (root.contains("data") && root["data"].isObject())
        return root["data"].toObject();
    return root;
}

// ── Area search (uses multi-vessel with well-known port IMOs) ────────────────
void MaritimeService::search_vessels_by_area(const AreaSearchParams& params) {
    if (local_maritime_enabled()) {
        if (try_local_area_provider(params))
            return;
        emit error_occurred("area_search", local_unavailable_message());
        return;
    }

    // The API doesn't have an area-search endpoint.
    // Use multi-vessel with a set of well-known container ship IMOs instead.
    static const QStringList known_imos = {
        "9893890", // EVER ACE
        "9461867", // APL CHONGQING
        "9811000", // MSC GULSUN
        "9839430", // MSC TESSA
        "9795622", // HMM ALGECIRAS
        "9484525", // COSCO SHIPPING UNIVERSE
        "9706891", // ONE APRICOT
        "9732319", // MAERSK EDINBURGH
        "9758098", // CMA CGM ANTOINE
        "9778791", // EVER GOLDEN
    };
    get_multi_vessel_positions(known_imos);
}

// ── Single vessel position ───────────────────────────────────────────────────
void MaritimeService::get_vessel_position(const QString& imo) {
    if (imo.trimmed().isEmpty())
        return;

    const QString cache_key = "maritime:vessel:" + imo.trimmed();
    const QVariant cached = fincept::CacheManager::instance().get(cache_key);
    if (!cached.isNull()) {
        auto vessel = parse_vessel(QJsonDocument::fromJson(cached.toString().toUtf8()).object());
        emit vessel_found(vessel);
        return;
    }

    if (local_maritime_enabled()) {
        if (try_local_vessel_provider(imo.trimmed()))
            return;
        emit error_occurred("vessel_position", local_unavailable_message());
        return;
    }

    QJsonObject body;
    body["imo"] = imo.trimmed();

    QPointer<MaritimeService> self = this;
    const QString imo_trimmed = imo.trimmed();
    HttpClient::instance().post(
        QString(kMarineBase) + "/vessel/position", body,
        [self, cache_key, imo_trimmed](Result<QJsonDocument> result) {
            if (!self)
                return;
            if (!result.is_ok()) {
                LOG_ERROR("Maritime", "Vessel position failed: " + QString::fromStdString(result.error()));
                emit self->error_occurred("vessel_position", QString::fromStdString(result.error()));
                return;
            }
            auto data = unwrap(result.value().object());
            auto vessel_obj = data["vessel"].toObject();
            fincept::CacheManager::instance().put(
                cache_key, QVariant(QString::fromUtf8(QJsonDocument(vessel_obj).toJson(QJsonDocument::Compact))),
                kVesselTtlSec, "maritime");
            auto vessel = self->parse_vessel(vessel_obj);
            LOG_INFO("Maritime", QString("Found vessel: %1 [%2]").arg(vessel.name, vessel.imo));
            emit self->vessel_found(vessel);
            if (self->hub_registered_)
                publish_to_hub(QStringLiteral("maritime:vessel:") + imo_trimmed, QVariant::fromValue(vessel));
        });
}

// ── Multi vessel positions ───────────────────────────────────────────────────
void MaritimeService::get_multi_vessel_positions(const QStringList& imos) {
    QStringList sorted = imos;
    std::sort(sorted.begin(), sorted.end());
    const QString cache_key = "maritime:multi:" + sorted.join(",");
    const QVariant cached = fincept::CacheManager::instance().get(cache_key);
    if (!cached.isNull()) {
        QJsonArray vessels_arr = QJsonDocument::fromJson(cached.toString().toUtf8()).array();
        QVector<VesselData> vessels;
        vessels.reserve(vessels_arr.size());
        for (const auto& v : vessels_arr)
            vessels.append(parse_vessel(v.toObject()));
        emit vessels_loaded(vessels, vessels.size());
        return;
    }

    if (local_maritime_enabled()) {
        QVector<VesselData> vessels;
        for (const auto& imo : imos) {
            const QVariant single_cached = fincept::CacheManager::instance().get("maritime:vessel:" + imo.trimmed());
            if (!single_cached.isNull())
                vessels.append(parse_vessel(QJsonDocument::fromJson(single_cached.toString().toUtf8()).object()));
        }
        if (!vessels.isEmpty()) {
            emit vessels_loaded(vessels, vessels.size());
            if (hub_registered_)
                publish_to_hub(QStringLiteral("maritime:vessels:multi"), QVariant::fromValue(vessels));
            return;
        }
        if (has_env("AISSTREAM_API_KEY")) {
            AreaSearchParams params;
            if (try_local_area_provider(params))
                return;
        }
        emit error_occurred("multi_vessel", local_unavailable_message());
        return;
    }

    QJsonObject body;
    QJsonArray arr;
    for (const auto& imo : imos)
        arr.append(imo.trimmed());
    body["imos"] = arr;

    QPointer<MaritimeService> self = this;
    HttpClient::instance().post(
        QString(kMarineBase) + "/vessel/multi", body, [self, cache_key](Result<QJsonDocument> result) {
            if (!self)
                return;
            if (!result.is_ok()) {
                LOG_ERROR("Maritime", "Multi vessel failed: " + QString::fromStdString(result.error()));
                emit self->error_occurred("multi_vessel", QString::fromStdString(result.error()));
                return;
            }
            auto data = unwrap(result.value().object());
            auto vessels_arr = data["vessels"].toArray();
            fincept::CacheManager::instance().put(
                cache_key, QVariant(QString::fromUtf8(QJsonDocument(vessels_arr).toJson(QJsonDocument::Compact))),
                kVesselTtlSec, "maritime");
            QVector<VesselData> vessels;
            vessels.reserve(vessels_arr.size());
            for (const auto& v : vessels_arr)
                vessels.append(self->parse_vessel(v.toObject()));
            int total = data["found_count"].toInt(vessels.size());
            LOG_INFO("Maritime", QString("Multi vessel: %1 found").arg(vessels.size()));
            emit self->vessels_loaded(vessels, total);
            if (self->hub_registered_)
                publish_to_hub(QStringLiteral("maritime:vessels:multi"), QVariant::fromValue(vessels));
        });
}

// ── Vessel history ───────────────────────────────────────────────────────────
void MaritimeService::get_vessel_history(const QString& imo) {
    const QString cache_key = "maritime:history:" + imo.trimmed();
    const QVariant cached = fincept::CacheManager::instance().get(cache_key);
    if (!cached.isNull()) {
        QJsonArray history_arr = QJsonDocument::fromJson(cached.toString().toUtf8()).array();
        QVector<VesselData> history;
        history.reserve(history_arr.size());
        for (const auto& v : history_arr)
            history.append(parse_vessel(v.toObject()));
        emit vessel_history_loaded(history);
        return;
    }

    if (local_maritime_enabled()) {
        if (try_local_history_provider(imo.trimmed()))
            return;
        emit error_occurred("vessel_history", local_unavailable_message());
        return;
    }

    QJsonObject body;
    body["imo"] = imo.trimmed();

    QPointer<MaritimeService> self = this;
    const QString imo_trimmed = imo.trimmed();
    HttpClient::instance().post(
        QString(kMarineBase) + "/vessel/history", body,
        [self, cache_key, imo_trimmed](Result<QJsonDocument> result) {
            if (!self)
                return;
            if (!result.is_ok()) {
                emit self->error_occurred("vessel_history", QString::fromStdString(result.error()));
                return;
            }
            auto data = unwrap(result.value().object());
            auto arr = data["positions"].toArray();
            if (arr.isEmpty())
                arr = data["history"].toArray();
            fincept::CacheManager::instance().put(
                cache_key, QVariant(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact))),
                kHistoryTtlSec, "maritime");
            QVector<VesselData> history;
            history.reserve(arr.size());
            for (const auto& v : arr)
                history.append(self->parse_vessel(v.toObject()));
            emit self->vessel_history_loaded(history);
            if (self->hub_registered_)
                publish_to_hub(QStringLiteral("maritime:history:") + imo_trimmed,
                               QVariant::fromValue(history));
        });
}

// ── Health check ─────────────────────────────────────────────────────────────
void MaritimeService::check_health() {
    if (local_maritime_enabled()) {
        QJsonObject obj{{"status", "local"},
                        {"provider", "cache_or_open_ais"},
                        {"aisstream_configured", has_env("AISSTREAM_API_KEY")},
                        {"marinetraffic_configured", has_env("MARINETRAFFIC_API_KEY")},
                        {"gfw_configured", has_env("GFW_API_KEY")},
                        {"fincept_cloud_fallback", false},
                        {"message", local_unavailable_message()}};
        emit health_loaded(obj);
        if (hub_registered_)
            publish_to_hub(QStringLiteral("maritime:health"), QVariant(obj));
        return;
    }

    QPointer<MaritimeService> self = this;
    HttpClient::instance().get(QString(kMarineBase) + "/health", [self](Result<QJsonDocument> result) {
        if (!self)
            return;
        if (!result.is_ok()) {
            emit self->error_occurred("health", QString::fromStdString(result.error()));
            return;
        }
        const auto obj = result.value().object();
        emit self->health_loaded(obj);
        if (self->hub_registered_)
            publish_to_hub(QStringLiteral("maritime:health"), QVariant(obj));
    });
}

// ═══════════════════════════════════════════════════════════════════════════════
// DATAHUB PRODUCER — maritime:*
// ═══════════════════════════════════════════════════════════════════════════════

QStringList MaritimeService::topic_patterns() const {
    return {QStringLiteral("maritime:*")};
}

void MaritimeService::refresh(const QStringList& topics) {
    for (const auto& topic : topics) {
        const QStringList parts = topic.split(QLatin1Char(':'));
        if (parts.size() >= 3 && parts[1] == QStringLiteral("vessel")) {
            get_vessel_position(parts[2]);
        } else if (parts.size() >= 3 && parts[1] == QStringLiteral("history")) {
            get_vessel_history(parts[2]);
        } else if (parts.size() == 3 && parts[1] == QStringLiteral("vessels") &&
                   parts[2] == QStringLiteral("multi")) {
            search_vessels_by_area({});
        } else if (topic == QStringLiteral("maritime:health")) {
            check_health();
        }
    }
}

int MaritimeService::max_requests_per_sec() const {
    return 2;  // External AIS provider cap — conservative.
}

void MaritimeService::ensure_registered_with_hub() {
    if (hub_registered_) return;
    auto& hub = fincept::datahub::DataHub::instance();
    hub.register_producer(this);

    // Position data: 1 min TTL matches kVesselTtlSec; min 30s between refreshes.
    fincept::datahub::TopicPolicy vessel_policy;
    vessel_policy.ttl_ms = kVesselTtlSec * 1000;
    vessel_policy.min_interval_ms = 30 * 1000;
    hub.set_policy_pattern(QStringLiteral("maritime:vessel:*"), vessel_policy);
    hub.set_policy_pattern(QStringLiteral("maritime:vessels:*"), vessel_policy);

    // History: 5 min TTL.
    fincept::datahub::TopicPolicy history_policy;
    history_policy.ttl_ms = kHistoryTtlSec * 1000;
    history_policy.min_interval_ms = 60 * 1000;
    hub.set_policy_pattern(QStringLiteral("maritime:history:*"), history_policy);

    // Health: 5 min TTL, non-urgent.
    fincept::datahub::TopicPolicy health_policy;
    health_policy.ttl_ms = 5 * 60 * 1000;
    health_policy.min_interval_ms = 60 * 1000;
    hub.set_policy_pattern(QStringLiteral("maritime:health"), health_policy);

    hub_registered_ = true;
    LOG_INFO("MaritimeService", "Registered with DataHub (maritime:*)");
}

} // namespace fincept::services::maritime
