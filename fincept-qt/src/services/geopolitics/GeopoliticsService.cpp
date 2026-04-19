// src/services/geopolitics/GeopoliticsService.cpp
#include "services/geopolitics/GeopoliticsService.h"

#include "core/logging/Logger.h"
#include "python/PythonRunner.h"
#include "services/news/NewsService.h"
#include "storage/cache/CacheManager.h"

#    include "datahub/DataHub.h"
#    include "datahub/DataHubMetaTypes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QDateTime>
#include <QUrlQuery>
#include <QVariant>

#include <limits>

namespace fincept::services::geo {

namespace {
inline void publish_to_hub(const QString& topic, const QVariant& value) {
    fincept::datahub::DataHub::instance().publish(topic, value);
}

struct GeoHint {
    const char* keyword;
    const char* country;
    const char* city;
    double latitude;
    double longitude;
};

const GeoHint* lookup_geo_hint(const QString& text) {
    static const GeoHint hints[] = {
        {"ukraine", "Ukraine", "Kyiv", 50.4501, 30.5234},
        {"kyiv", "Ukraine", "Kyiv", 50.4501, 30.5234},
        {"russia", "Russia", "Moscow", 55.7558, 37.6173},
        {"moscow", "Russia", "Moscow", 55.7558, 37.6173},
        {"gaza", "Palestine", "Gaza", 31.5018, 34.4668},
        {"israel", "Israel", "Tel Aviv", 32.0853, 34.7818},
        {"iran", "Iran", "Tehran", 35.6892, 51.3890},
        {"china", "China", "Beijing", 39.9042, 116.4074},
        {"taiwan", "Taiwan", "Taipei", 25.0330, 121.5654},
        {"north korea", "North Korea", "Pyongyang", 39.0392, 125.7625},
        {"south korea", "South Korea", "Seoul", 37.5665, 126.9780},
        {"syria", "Syria", "Damascus", 33.5138, 36.2765},
        {"lebanon", "Lebanon", "Beirut", 33.8938, 35.5018},
        {"sudan", "Sudan", "Khartoum", 15.5007, 32.5599},
        {"yemen", "Yemen", "Sanaa", 15.3694, 44.1910},
        {"afghanistan", "Afghanistan", "Kabul", 34.5553, 69.2075},
    };
    for (const auto& hint : hints) {
        if (text.contains(hint.keyword))
            return &hint;
    }
    return nullptr;
}

QString classify_event_category(const fincept::services::NewsArticle& article, const QString& text) {
    if (text.contains("protest") || text.contains("demonstration"))
        return "protests";
    if (text.contains("terror") || text.contains("bombing"))
        return "terrorism";
    if (text.contains("riot") || text.contains("clash"))
        return "riots";
    if (text.contains("explosion") || text.contains("blast"))
        return "explosions";
    if (text.contains("sanction") || text.contains("summit") || text.contains("diplomatic"))
        return "strategic";
    if (article.threat.category == "conflict" || text.contains("war") || text.contains("missile")
        || text.contains("airstrike") || text.contains("military"))
        return "armed_conflict";
    return "crisis";
}

QString keyword_summary(const fincept::services::NewsArticle& article) {
    const QString headline = article.headline.simplified().left(140);
    QStringList keywords = article.tickers;
    const QString text = (article.headline + " " + article.summary).toLower();
    if (text.contains("war"))
        keywords << "war";
    if (text.contains("sanction"))
        keywords << "sanction";
    if (text.contains("military"))
        keywords << "military";
    if (text.contains("protest"))
        keywords << "protest";
    if (text.contains("election"))
        keywords << "election";
    keywords.removeDuplicates();
    const QString keyword_text = keywords.mid(0, 4).join(", ");
    if (!headline.isEmpty() && !keyword_text.isEmpty())
        return headline + " | " + keyword_text;
    if (!headline.isEmpty())
        return headline;
    return keyword_text.isEmpty() ? article.summary.simplified().left(140) : keyword_text;
}

QString event_cache_key(const QString& country, const QString& city, const QString& category, int limit) {
    return QString("geo:events:%1:%2:%3:%4").arg(country, city, category).arg(limit);
}

QByteArray cached_bytes(const QVariant& cached) {
    QByteArray raw = cached.toByteArray();
    if (raw.isEmpty())
        raw = cached.toString().toUtf8();
    return raw;
}

QVector<NewsEvent> events_from_json(const QJsonArray& arr) {
    QVector<NewsEvent> events;
    events.reserve(arr.size());
    for (const auto& value : arr) {
        const QJsonObject o = value.toObject();
        NewsEvent ev;
        ev.url = o["url"].toString();
        ev.domain = o["domain"].toString();
        ev.event_category = o["event_category"].toString();
        ev.matched_keywords = o["matched_keywords"].toString();
        ev.city = o["city"].toString();
        ev.country = o["country"].toString();
        ev.latitude = o["latitude"].toDouble(std::numeric_limits<double>::quiet_NaN());
        ev.longitude = o["longitude"].toDouble(std::numeric_limits<double>::quiet_NaN());
        ev.extracted_date = o["extracted_date"].toString();
        ev.created_at = o["created_at"].toString();
        events.append(ev);
    }
    return events;
}

QJsonArray events_to_json(const QVector<NewsEvent>& events) {
    QJsonArray cached_arr;
    for (const auto& ev : events) {
        QJsonObject o;
        o["url"] = ev.url;
        o["domain"] = ev.domain;
        o["event_category"] = ev.event_category;
        o["matched_keywords"] = ev.matched_keywords;
        o["city"] = ev.city;
        o["country"] = ev.country;
        o["latitude"] = ev.latitude;
        o["longitude"] = ev.longitude;
        o["extracted_date"] = ev.extracted_date;
        o["created_at"] = ev.created_at;
        cached_arr.append(o);
    }
    return cached_arr;
}

QVector<fincept::services::NewsArticle> parse_live_search_articles(const QString& payload) {
    QVector<fincept::services::NewsArticle> articles;
    const QJsonObject obj = QJsonDocument::fromJson(payload.toUtf8()).object();
    if (!obj["success"].toBool())
        return articles;

    const QJsonArray arr = obj["articles"].toArray();
    articles.reserve(arr.size());
    for (const auto& value : arr) {
        const QJsonObject item = value.toObject();
        fincept::services::NewsArticle article;
        article.id = item["id"].toString();
        article.headline = item["headline"].toString();
        article.summary = item["summary"].toString();
        article.source = item["source"].toString();
        article.region = item["region"].toString("GLOBAL");
        article.category = item["category"].toString("GEOPOLITICS");
        article.link = item["link"].toString();
        article.sort_ts = item["sort_ts"].toVariant().toLongLong();
        article.time = QDateTime::fromSecsSinceEpoch(article.sort_ts).toString(Qt::ISODate);
        article.tier = item["tier"].toInt(2);
        article.lang = item["lang"].toString("en");
        articles.append(article);
    }
    return articles;
}

QVector<NewsEvent> merge_events(const QVector<NewsEvent>& primary, const QVector<NewsEvent>& extra, int limit) {
    QVector<NewsEvent> merged = primary;
    QSet<QString> seen;

    auto key_for = [](const NewsEvent& event) {
        if (!event.url.isEmpty())
            return event.url;
        return event.event_category + "|" + event.country + "|" + event.city + "|" + event.matched_keywords.left(120);
    };

    for (const auto& event : primary)
        seen.insert(key_for(event));

    for (const auto& event : extra) {
        const QString key = key_for(event);
        if (seen.contains(key))
            continue;
        seen.insert(key);
        merged.append(event);
    }

    std::sort(merged.begin(), merged.end(), [](const NewsEvent& a, const NewsEvent& b) {
        return a.created_at > b.created_at;
    });
    if (limit > 0 && merged.size() > limit)
        merged.resize(limit);
    return merged;
}

QVector<NewsEvent> build_local_events(const QVector<fincept::services::NewsArticle>& articles, const QString& country_filter,
                                      const QString& city_filter, const QString& category_filter, int limit) {
    QVector<NewsEvent> events;
    const QString country_term = country_filter.trimmed().toLower();
    const QString city_term = city_filter.trimmed().toLower();
    const QString category_term = category_filter.trimmed().toLower();

    for (const auto& article : articles) {
        const QString text = (article.headline + " " + article.summary).toLower();
        if (!(article.category == "GEOPOLITICS" || article.category == "DEFENSE" || article.threat.category == "conflict"
              || article.threat.category == "regulatory"))
            continue;

        const auto* hint = lookup_geo_hint(text);
        const QString country = hint ? QString::fromUtf8(hint->country) : QString();
        const QString city = hint ? QString::fromUtf8(hint->city) : QString();
        const QString category = classify_event_category(article, text);

        if (!country_term.isEmpty() && !country.toLower().contains(country_term))
            continue;
        if (!city_term.isEmpty() && !city.toLower().contains(city_term))
            continue;
        if (!category_term.isEmpty() && !category.toLower().contains(category_term))
            continue;

        NewsEvent ev;
        ev.url = article.link;
        ev.domain = article.source;
        ev.event_category = category;
        ev.matched_keywords = keyword_summary(article);
        ev.city = city;
        ev.country = country;
        ev.latitude = hint ? hint->latitude : std::numeric_limits<double>::quiet_NaN();
        ev.longitude = hint ? hint->longitude : std::numeric_limits<double>::quiet_NaN();
        ev.extracted_date = QDateTime::fromSecsSinceEpoch(article.sort_ts).date().toString(Qt::ISODate);
        ev.created_at = QDateTime::fromSecsSinceEpoch(article.sort_ts).toString(Qt::ISODate);
        events.append(ev);
        if (limit > 0 && events.size() >= limit)
            break;
    }

    return events;
}
} // namespace

// ── Singleton ────────────────────────────────────────────────────────────────
GeopoliticsService& GeopoliticsService::instance() {
    static GeopoliticsService inst;
    return inst;
}

GeopoliticsService::GeopoliticsService(QObject* parent) : QObject(parent) {}

// ── Python helper ────────────────────────────────────────────────────────────
void GeopoliticsService::run_python(const QString& script, const QStringList& args, const QString& context,
                                    std::function<void(bool, const QString&)> cb) {
    QPointer<GeopoliticsService> self = this;
    python::PythonRunner::instance().run(script, args, [self, context, cb](python::PythonResult result) {
        if (!self)
            return;
        cb(result.success, result.success ? result.output : result.error);
    });
}

// ═══════════════════════════════════════════════════════════════════════════════
// CONFLICT MONITOR — HTTP API
// ═══════════════════════════════════════════════════════════════════════════════

void GeopoliticsService::fetch_events(const QString& country, const QString& city, const QString& category, int limit) {
    const QString cache_key = event_cache_key(country, city, category, limit);
    const QVariant cached = fincept::CacheManager::instance().get(cache_key);
    if (!cached.isNull()) {
        const QJsonObject root = QJsonDocument::fromJson(cached_bytes(cached)).object();
        const QVector<NewsEvent> cached_events = events_from_json(root["events"].toArray());
        emit events_loaded(cached_events, root["total"].toInt(cached_events.size()));
        if (hub_registered_)
            publish_to_hub(QStringLiteral("geopolitics:events"), QVariant::fromValue(cached_events));
        return;
    }

    QPointer<GeopoliticsService> self = this;
    fincept::services::NewsService::instance().fetch_all_news(
        false, [self, country, city, category, limit](bool ok, QVector<fincept::services::NewsArticle> articles) {
        if (!self)
            return;
        QVector<NewsEvent> local_events;
        if (ok)
            local_events = build_local_events(articles, country, city, category, limit > 0 ? limit : 200);
        else
            LOG_WARN("Geopolitics", "Local events fetch failed, trying live search fallback");

        auto publish_events = [self, country, city, category, limit](const QVector<NewsEvent>& events) {
            if (!self)
                return;
            if (events.isEmpty()) {
                emit self->error_occurred("events", "No local or live geopolitics events available");
                return;
            }

            QJsonObject cached_root;
            cached_root["events"] = events_to_json(events);
            cached_root["total"] = events.size();
            fincept::CacheManager::instance().put(
                event_cache_key(country, city, category, limit),
                QVariant(QJsonDocument(cached_root).toJson(QJsonDocument::Compact)), kEventsTtlSec, "geopolitics");
            LOG_INFO("Geopolitics", QString("Loaded %1 events").arg(events.size()));
            emit self->events_loaded(events, events.size());
            if (self->hub_registered_)
                publish_to_hub(QStringLiteral("geopolitics:events"), QVariant::fromValue(events));
        };

        if (!python::PythonRunner::instance().is_available()) {
            publish_events(local_events);
            return;
        }

        const int search_limit = limit > 0 ? qBound(5, limit, 40) : 20;
        self->run_python(
            "news_search_rss.py", {"geopolitics", country, city, category, QString::number(search_limit)},
            "live_geopolitics_search",
            [self, country, city, category, limit, local_events, publish_events](bool search_ok, const QString& output) {
                if (!self)
                    return;

                QVector<NewsEvent> merged = local_events;
                if (search_ok) {
                    const QVector<fincept::services::NewsArticle> live_articles = parse_live_search_articles(output);
                    const QVector<NewsEvent> live_events =
                        build_local_events(live_articles, country, city, category, limit > 0 ? limit : 100);
                    merged = merge_events(local_events, live_events, limit);
                } else if (local_events.isEmpty()) {
                    LOG_ERROR("Geopolitics", "Live geopolitics search failed: " + output.left(200));
                }

                publish_events(merged);
            });
    });
}

void GeopoliticsService::fetch_unique_countries() {
    // Cache hit — deserialize and emit, otherwise go to network.
    const QVariant cached = fincept::CacheManager::instance().get("geo:countries");
    if (!cached.isNull()) {
        const QJsonArray arr = QJsonDocument::fromJson(cached.toString().toUtf8()).array();
        QVector<UniqueCountry> countries;
        countries.reserve(arr.size());
        for (const auto& v : arr) {
            const auto o = v.toObject();
            countries.append({o["country"].toString(), o["event_count"].toInt()});
        }
        emit countries_loaded(countries);
        return;
    }

    QPointer<GeopoliticsService> self = this;
    fincept::services::NewsService::instance().fetch_all_news(
        false, [self](bool ok, QVector<fincept::services::NewsArticle> articles) {
            if (!self)
                return;
            if (!ok) {
                emit self->error_occurred("countries", "Local geopolitics feed unavailable");
                return;
            }
            const auto events = build_local_events(articles, {}, {}, {}, 250);
            QMap<QString, int> counts;
            for (const auto& ev : events) {
                if (!ev.country.isEmpty())
                    counts[ev.country] += 1;
            }
            QVector<UniqueCountry> countries;
            countries.reserve(counts.size());
            QJsonArray to_cache;
            for (auto it = counts.begin(); it != counts.end(); ++it) {
                const QString name = it.key();
                const int count = it.value();
                countries.append({name, count});
                QJsonObject entry;
                entry["country"] = name;
                entry["event_count"] = count;
                to_cache.append(entry);
            }
            fincept::CacheManager::instance().put(
                "geo:countries",
                QVariant(QString::fromUtf8(QJsonDocument(to_cache).toJson(QJsonDocument::Compact))), kRefDataTtlSec,
                "geopolitics");
            emit self->countries_loaded(countries);
            if (self->hub_registered_)
                publish_to_hub(QStringLiteral("geopolitics:countries"), QVariant::fromValue(countries));
    });
}

void GeopoliticsService::fetch_unique_categories() {
    const QVariant cached = fincept::CacheManager::instance().get("geo:categories");
    if (!cached.isNull()) {
        const QJsonArray arr = QJsonDocument::fromJson(cached.toString().toUtf8()).array();
        QVector<UniqueCategory> cats;
        cats.reserve(arr.size());
        for (const auto& v : arr) {
            const auto o = v.toObject();
            cats.append({o["event_category"].toString(), o["event_count"].toInt()});
        }
        emit categories_loaded(cats);
        return;
    }

    QPointer<GeopoliticsService> self = this;
    fincept::services::NewsService::instance().fetch_all_news(
        false, [self](bool ok, QVector<fincept::services::NewsArticle> articles) {
        if (!self)
            return;
        if (!ok) {
            emit self->error_occurred("categories", "Local geopolitics feed unavailable");
            return;
        }
        const auto events = build_local_events(articles, {}, {}, {}, 250);
        QMap<QString, int> counts;
        for (const auto& ev : events)
            counts[ev.event_category] += 1;
        QVector<UniqueCategory> cats;
        cats.reserve(counts.size());
        QJsonArray to_cache;
        for (auto it = counts.begin(); it != counts.end(); ++it) {
            const QString name = it.key();
            const int count = it.value();
            cats.append({name, count});
            QJsonObject entry;
            entry["event_category"] = name;
            entry["event_count"] = count;
            to_cache.append(entry);
        }
        fincept::CacheManager::instance().put(
            "geo:categories",
            QVariant(QString::fromUtf8(QJsonDocument(to_cache).toJson(QJsonDocument::Compact))), kRefDataTtlSec,
            "geopolitics");
        emit self->categories_loaded(cats);
        if (self->hub_registered_)
            publish_to_hub(QStringLiteral("geopolitics:categories"), QVariant::fromValue(cats));
    });
}

void GeopoliticsService::fetch_unique_cities() {
    QPointer<GeopoliticsService> self = this;
    fincept::services::NewsService::instance().fetch_all_news(
        false, [self](bool ok, QVector<fincept::services::NewsArticle> articles) {
        if (!self)
            return;
        if (!ok) {
            emit self->error_occurred("cities", "Local geopolitics feed unavailable");
            return;
        }
        const auto events = build_local_events(articles, {}, {}, {}, 250);
        QStringList cities;
        QSet<QString> unique;
        for (const auto& ev : events) {
            if (!ev.city.isEmpty())
                unique.insert(ev.city);
        }
        cities = unique.values();
        std::sort(cities.begin(), cities.end());
        emit self->cities_loaded(cities);
        if (self->hub_registered_)
            publish_to_hub(QStringLiteral("geopolitics:cities"), QVariant::fromValue(cities));
    });
}

// ═══════════════════════════════════════════════════════════════════════════════
// HDX HUMANITARIAN DATA — Python
// ═══════════════════════════════════════════════════════════════════════════════

static QVector<HDXDataset> parse_hdx_results(const QString& output) {
    auto json_str = python::extract_json(output);
    auto doc = QJsonDocument::fromJson(json_str.toUtf8());
    QVector<HDXDataset> datasets;

    // Script returns: {"success": true, "data": {"datasets": [...]}}
    // Try unwrapping the data envelope first, then fall back to bare array/object
    auto root = doc.object();
    QJsonArray arr;
    if (root.contains("data")) {
        auto data = root["data"].toObject();
        arr = data["datasets"].toArray();
    } else if (root.contains("datasets")) {
        arr = root["datasets"].toArray();
    } else {
        arr = doc.array();
    }

    datasets.reserve(arr.size());
    for (const auto& v : arr) {
        auto o = v.toObject();
        HDXDataset d;
        d.id = o["id"].toString();
        d.title = o["title"].toString();
        d.organization = o["organization"].toString();
        d.notes = o["notes"].toString();
        d.date = o["dataset_date"].toString();
        d.num_resources = o["num_resources"].toInt();
        // Tags in summary are plain strings; in full detail they are objects with "name"
        for (const auto& t : o["tags"].toArray()) {
            if (t.isString())
                d.tags.append(t.toString());
            else
                d.tags.append(t.toObject()["name"].toString());
        }
        d.raw = o;
        datasets.append(d);
    }
    return datasets;
}

static inline void publish_hdx(GeopoliticsService* self, const QString& context, const QVector<HDXDataset>& datasets);

void GeopoliticsService::search_hdx_conflicts() {
    run_python("hdx_data.py", {"search_conflict", "", "20"}, "hdx_conflicts", [this](bool ok, const QString& out) {
        if (!ok) {
            emit error_occurred("hdx_conflicts", out);
            return;
        }
        const auto datasets = parse_hdx_results(out);
        emit hdx_results_loaded("conflicts", datasets);
        publish_hdx_result(QStringLiteral("conflicts"), datasets);
    });
}

void GeopoliticsService::search_hdx_humanitarian() {
    run_python("hdx_data.py", {"search_humanitarian", "", "20"}, "hdx_humanitarian",
               [this](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("hdx_humanitarian", out);
                       return;
                   }
                   const auto datasets = parse_hdx_results(out);
                   emit hdx_results_loaded("humanitarian", datasets);
                   publish_hdx_result(QStringLiteral("humanitarian"), datasets);
               });
}

void GeopoliticsService::search_hdx_by_country(const QString& country) {
    run_python("hdx_data.py", {"search_by_country", country}, "hdx_country", [this, country](bool ok, const QString& out) {
        if (!ok) {
            emit error_occurred("hdx_country", out);
            return;
        }
        const auto datasets = parse_hdx_results(out);
        emit hdx_results_loaded("country", datasets);
        publish_hdx_result(QStringLiteral("country:") + country, datasets);
    });
}

void GeopoliticsService::search_hdx_by_topic(const QString& topic) {
    run_python("hdx_data.py", {"search_by_topic", topic}, "hdx_topic", [this, topic](bool ok, const QString& out) {
        if (!ok) {
            emit error_occurred("hdx_topic", out);
            return;
        }
        const auto datasets = parse_hdx_results(out);
        emit hdx_results_loaded("topic", datasets);
        publish_hdx_result(QStringLiteral("topic:") + topic, datasets);
    });
}

void GeopoliticsService::search_hdx_advanced(const QString& query) {
    // Use search_datasets for free-text queries (advanced_search expects key:value pairs)
    run_python("hdx_data.py", {"search_datasets", query, "20"}, "hdx_search",
               [this, query](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("hdx_search", out);
                       return;
                   }
                   const auto datasets = parse_hdx_results(out);
                   emit hdx_results_loaded("search", datasets);
                   publish_hdx_result(QStringLiteral("search:") + query, datasets);
               });
}

// ═══════════════════════════════════════════════════════════════════════════════
// TRADE ANALYSIS — Python
// ═══════════════════════════════════════════════════════════════════════════════

void GeopoliticsService::analyze_trade_benefits(const QJsonObject& params) {
    auto json_str = QJsonDocument(params).toJson(QJsonDocument::Compact);
    run_python("Analytics/economics/trade_geopolitics.py", {"benefits_costs", json_str}, "trade_benefits",
               [this](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("trade_benefits", out);
                       return;
                   }
                   auto doc = QJsonDocument::fromJson(python::extract_json(out).toUtf8());
                   const auto obj = doc.object();
                   emit trade_result_ready("trade_benefits", obj);
                   if (hub_registered_)
                       publish_to_hub(QStringLiteral("geopolitics:trade:benefits"), QVariant(obj));
               });
}

void GeopoliticsService::analyze_trade_restrictions(const QJsonObject& params) {
    auto json_str = QJsonDocument(params).toJson(QJsonDocument::Compact);
    run_python("Analytics/economics/trade_geopolitics.py", {"restrictions", json_str}, "trade_restrictions",
               [this](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("trade_restrictions", out);
                       return;
                   }
                   auto doc = QJsonDocument::fromJson(python::extract_json(out).toUtf8());
                   const auto obj = doc.object();
                   emit trade_result_ready("trade_restrictions", obj);
                   if (hub_registered_)
                       publish_to_hub(QStringLiteral("geopolitics:trade:restrictions"), QVariant(obj));
               });
}

// ═══════════════════════════════════════════════════════════════════════════════
// GEOLOCATION — Python
// ═══════════════════════════════════════════════════════════════════════════════

void GeopoliticsService::extract_geolocations(const QStringList& headlines) {
    auto json = QJsonDocument(QJsonArray::fromStringList(headlines)).toJson(QJsonDocument::Compact);
    run_python("news_geolocation.py", {"extract_and_geocode", json}, "geolocation",
               [this](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("geolocation", out);
                       return;
                   }
                   auto doc = QJsonDocument::fromJson(python::extract_json(out).toUtf8());
                   const auto obj = doc.object();
                   emit geolocation_ready(obj);
                   if (hub_registered_)
                       publish_to_hub(QStringLiteral("geopolitics:geolocation"), QVariant(obj));
               });
}

// ═══════════════════════════════════════════════════════════════════════════════
// DATAHUB PRODUCER — geopolitics:*
// ═══════════════════════════════════════════════════════════════════════════════

void GeopoliticsService::publish_hdx_result(const QString& context, const QVector<HDXDataset>& datasets) {
    if (!hub_registered_) return;
    publish_to_hub(QStringLiteral("geopolitics:hdx:") + context, QVariant::fromValue(datasets));
}

QStringList GeopoliticsService::topic_patterns() const {
    return {QStringLiteral("geopolitics:*")};
}

void GeopoliticsService::refresh(const QStringList& topics) {
    // Hub-driven refresh for the lightweight reference endpoints. Anything
    // parameterised (events:<filters>, hdx:*, trade:*, geolocation) is
    // user-invoked and not driven through the hub scheduler.
    for (const auto& topic : topics) {
        if (topic == QStringLiteral("geopolitics:events")) {
            fetch_events();
        } else if (topic == QStringLiteral("geopolitics:countries")) {
            fetch_unique_countries();
        } else if (topic == QStringLiteral("geopolitics:categories")) {
            fetch_unique_categories();
        } else if (topic == QStringLiteral("geopolitics:cities")) {
            fetch_unique_cities();
        }
    }
}

int GeopoliticsService::max_requests_per_sec() const {
    return 2;  // Fincept research API + HDX Python — conservative
}

void GeopoliticsService::ensure_registered_with_hub() {
    if (hub_registered_) return;
    auto& hub = fincept::datahub::DataHub::instance();
    hub.register_producer(this);

    // Events — 2 min TTL (matches kEventsTtlSec), 30s min_interval.
    fincept::datahub::TopicPolicy events_policy;
    events_policy.ttl_ms = kEventsTtlSec * 1000;
    events_policy.min_interval_ms = 30 * 1000;
    hub.set_policy_pattern(QStringLiteral("geopolitics:events"), events_policy);

    // Reference data (countries/categories/cities) — 10 min TTL.
    fincept::datahub::TopicPolicy ref_policy;
    ref_policy.ttl_ms = kRefDataTtlSec * 1000;
    ref_policy.min_interval_ms = 60 * 1000;
    hub.set_policy_pattern(QStringLiteral("geopolitics:countries"), ref_policy);
    hub.set_policy_pattern(QStringLiteral("geopolitics:categories"), ref_policy);
    hub.set_policy_pattern(QStringLiteral("geopolitics:cities"), ref_policy);

    // HDX datasets — 1 hour TTL (humanitarian data refresh cadence).
    fincept::datahub::TopicPolicy hdx_policy;
    hdx_policy.ttl_ms = 60 * 60 * 1000;
    hdx_policy.min_interval_ms = 60 * 1000;
    hub.set_policy_pattern(QStringLiteral("geopolitics:hdx:*"), hdx_policy);

    // Trade analysis + geolocation — user-invoked, treat as push-only so the
    // hub caches the most recent result without scheduling a refresh.
    fincept::datahub::TopicPolicy push_policy;
    push_policy.push_only = true;
    push_policy.ttl_ms = 15 * 60 * 1000;
    hub.set_policy_pattern(QStringLiteral("geopolitics:trade:*"), push_policy);
    hub.set_policy_pattern(QStringLiteral("geopolitics:geolocation"), push_policy);

    hub_registered_ = true;
    LOG_INFO("GeopoliticsService", "Registered with DataHub (geopolitics:*)");
}

} // namespace fincept::services::geo
