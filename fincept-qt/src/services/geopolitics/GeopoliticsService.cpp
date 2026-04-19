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

#include <algorithm>
#include <cmath>
#include <limits>

namespace fincept::services::geo {

namespace {
constexpr int kEventArchiveTtlSec = 365 * 24 * 60 * 60;
constexpr int kEventArchiveMaxRows = 2500;

inline void publish_to_hub(const QString& topic, const QVariant& value) {
    fincept::datahub::DataHub::instance().publish(topic, value);
}

QString event_cache_key(const QString& country, const QString& city, const QString& category, int limit) {
    return QString("geo:events:%1:%2:%3:%4").arg(country, city, category).arg(limit);
}

QString event_archive_key() {
    return QStringLiteral("geo:events:archive:v1");
}

QByteArray cached_bytes(const QVariant& cached) {
    QByteArray raw = cached.toByteArray();
    if (raw.isEmpty())
        raw = cached.toString().toUtf8();
    return raw;
}

QString json_text(const QJsonValue& value) {
    if (value.isString())
        return value.toString();
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (!std::isfinite(number))
            return {};
        const qint64 as_int = static_cast<qint64>(number);
        if (std::fabs(number - static_cast<double>(as_int)) < 1e-6)
            return QString::number(as_int);
        return QString::number(number, 'g', 15);
    }
    return {};
}

double json_number_or_nan(const QJsonValue& value) {
    if (value.isDouble())
        return value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        if (ok)
            return parsed;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

void set_json_number(QJsonObject& object, const QString& key, double value) {
    object[key] = std::isfinite(value) ? QJsonValue(value) : QJsonValue();
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
        ev.latitude = json_number_or_nan(o["latitude"]);
        ev.longitude = json_number_or_nan(o["longitude"]);
        ev.extracted_date = json_text(o["extracted_date"]);
        ev.created_at = json_text(o["created_at"]);
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
        set_json_number(o, QStringLiteral("latitude"), ev.latitude);
        set_json_number(o, QStringLiteral("longitude"), ev.longitude);
        o["extracted_date"] = ev.extracted_date;
        o["created_at"] = ev.created_at;
        cached_arr.append(o);
    }
    return cached_arr;
}

QDateTime parse_event_time(const NewsEvent& ev) {
    const QStringList candidates = {ev.created_at.trimmed(), ev.extracted_date.trimmed()};
    for (const QString& raw : candidates) {
        if (raw.isEmpty())
            continue;

        bool seconds_ok = false;
        const qint64 seconds = raw.toLongLong(&seconds_ok);
        if (seconds_ok && seconds > 0)
            return QDateTime::fromSecsSinceEpoch(seconds, Qt::UTC);

        QDateTime dt = QDateTime::fromString(raw, Qt::ISODateWithMs);
        if (!dt.isValid())
            dt = QDateTime::fromString(raw, Qt::ISODate);
        if (dt.isValid())
            return dt.toUTC();

        const QDate date = QDate::fromString(raw.left(10), Qt::ISODate);
        if (date.isValid())
            return QDateTime(date, QTime(0, 0), Qt::UTC);
    }
    return {};
}

QString normalized_event_key(const NewsEvent& ev) {
    const QString url = ev.url.trimmed().toLower();
    if (!url.isEmpty())
        return QStringLiteral("url:") + url;

    const QString date_key = parse_event_time(ev).date().toString(Qt::ISODate);
    const QStringList parts = {
        ev.domain.trimmed(),
        ev.event_category.trimmed(),
        ev.country.trimmed(),
        ev.city.trimmed(),
        date_key,
        std::isfinite(ev.latitude) ? QString::number(ev.latitude, 'f', 4) : QString(),
        std::isfinite(ev.longitude) ? QString::number(ev.longitude, 'f', 4) : QString(),
        ev.matched_keywords.trimmed().left(180),
    };
    return QStringLiteral("fields:") + parts.join('|').toLower();
}

bool event_has_identity(const NewsEvent& ev) {
    return !ev.url.trimmed().isEmpty() || !ev.domain.trimmed().isEmpty() || !ev.event_category.trimmed().isEmpty()
           || !ev.country.trimmed().isEmpty() || !ev.city.trimmed().isEmpty()
           || !ev.matched_keywords.trimmed().isEmpty() || parse_event_time(ev).isValid();
}

bool event_matches_filters(const NewsEvent& ev, const QString& country, const QString& city, const QString& category) {
    if (!country.trimmed().isEmpty() && ev.country.compare(country.trimmed(), Qt::CaseInsensitive) != 0)
        return false;
    if (!city.trimmed().isEmpty() && ev.city.compare(city.trimmed(), Qt::CaseInsensitive) != 0)
        return false;
    if (!category.trimmed().isEmpty() && ev.event_category.compare(category.trimmed(), Qt::CaseInsensitive) != 0)
        return false;
    return true;
}

void sort_events_newest_first(QVector<NewsEvent>& events) {
    std::sort(events.begin(), events.end(), [](const NewsEvent& a, const NewsEvent& b) {
        const QDateTime at = parse_event_time(a);
        const QDateTime bt = parse_event_time(b);
        if (at.isValid() || bt.isValid()) {
            if (at != bt)
                return at > bt;
        }
        return normalized_event_key(a) < normalized_event_key(b);
    });
}

QVector<NewsEvent> limit_events(QVector<NewsEvent> events, int limit) {
    if (limit > 0 && events.size() > limit)
        events.resize(limit);
    return events;
}

QVector<NewsEvent> filter_events(const QVector<NewsEvent>& events, const QString& country, const QString& city,
                                 const QString& category) {
    QVector<NewsEvent> filtered;
    filtered.reserve(events.size());
    for (const auto& ev : events) {
        if (event_matches_filters(ev, country, city, category))
            filtered.append(ev);
    }
    sort_events_newest_first(filtered);
    return filtered;
}

NewsEvent merge_duplicate_event(const NewsEvent& old_event, const NewsEvent& new_event) {
    const bool prefer_new = parse_event_time(new_event) >= parse_event_time(old_event);
    NewsEvent merged = prefer_new ? new_event : old_event;
    const NewsEvent& fallback = prefer_new ? old_event : new_event;

    auto fill = [](QString& target, const QString& source) {
        if (target.trimmed().isEmpty() && !source.trimmed().isEmpty())
            target = source;
    };

    fill(merged.url, fallback.url);
    fill(merged.domain, fallback.domain);
    fill(merged.event_category, fallback.event_category);
    fill(merged.matched_keywords, fallback.matched_keywords);
    fill(merged.city, fallback.city);
    fill(merged.country, fallback.country);
    fill(merged.extracted_date, fallback.extracted_date);
    fill(merged.created_at, fallback.created_at);

    if (!std::isfinite(merged.latitude) && std::isfinite(fallback.latitude))
        merged.latitude = fallback.latitude;
    if (!std::isfinite(merged.longitude) && std::isfinite(fallback.longitude))
        merged.longitude = fallback.longitude;

    return merged;
}

QVector<NewsEvent> merge_events(const QVector<NewsEvent>& existing, const QVector<NewsEvent>& incoming) {
    QHash<QString, NewsEvent> by_key;
    by_key.reserve(existing.size() + incoming.size());

    auto add_event = [&by_key](const NewsEvent& ev) {
        if (!event_has_identity(ev))
            return;
        const QString key = normalized_event_key(ev);
        auto it = by_key.find(key);
        if (it == by_key.end()) {
            by_key.insert(key, ev);
        } else {
            it.value() = merge_duplicate_event(it.value(), ev);
        }
    };

    for (const auto& ev : existing)
        add_event(ev);
    for (const auto& ev : incoming)
        add_event(ev);

    QVector<NewsEvent> merged;
    merged.reserve(by_key.size());
    for (auto it = by_key.cbegin(); it != by_key.cend(); ++it)
        merged.append(it.value());

    sort_events_newest_first(merged);
    if (merged.size() > kEventArchiveMaxRows)
        merged.resize(kEventArchiveMaxRows);
    return merged;
}

QVector<NewsEvent> load_event_archive() {
    const QVariant cached = fincept::CacheManager::instance().get(event_archive_key());
    if (cached.isNull())
        return {};
    const QJsonObject root = QJsonDocument::fromJson(cached_bytes(cached)).object();
    QVector<NewsEvent> events = events_from_json(root["events"].toArray());
    sort_events_newest_first(events);
    return events;
}

void save_event_archive(const QVector<NewsEvent>& events) {
    if (events.isEmpty())
        return;

    QVector<NewsEvent> archived = events;
    sort_events_newest_first(archived);
    if (archived.size() > kEventArchiveMaxRows)
        archived.resize(kEventArchiveMaxRows);

    QJsonObject root;
    root["events"] = events_to_json(archived);
    root["total"] = archived.size();
    root["updated_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    auto& cache = fincept::CacheManager::instance();
    cache.put(event_archive_key(), QVariant(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact))),
              kEventArchiveTtlSec, "geopolitics");
    cache.remove(QStringLiteral("geo:countries"));
    cache.remove(QStringLiteral("geo:categories"));
}

QVector<NewsEvent> merge_into_event_archive(const QVector<NewsEvent>& events) {
    if (events.isEmpty())
        return load_event_archive();

    QVector<NewsEvent> merged = merge_events(load_event_archive(), events);
    save_event_archive(merged);
    return merged;
}

QJsonArray articles_to_json(const QVector<fincept::services::NewsArticle>& articles) {
    QJsonArray arr;
    for (const auto& article : articles) {
        QJsonObject item;
        item["id"] = article.id;
        item["headline"] = article.headline;
        item["summary"] = article.summary;
        item["source"] = article.source;
        item["category"] = article.category;
        item["region"] = article.region;
        item["link"] = article.link;
        item["sort_ts"] = static_cast<qint64>(article.sort_ts);
        arr.append(item);
    }
    return arr;
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

QVector<fincept::services::NewsArticle> merge_articles(const QVector<fincept::services::NewsArticle>& primary,
                                                       const QVector<fincept::services::NewsArticle>& extra) {
    QVector<fincept::services::NewsArticle> merged = primary;
    QSet<QString> seen;

    auto key_for = [](const fincept::services::NewsArticle& article) {
        if (!article.link.isEmpty())
            return article.link;
        return article.headline.simplified().toLower();
    };

    for (const auto& article : primary)
        seen.insert(key_for(article));

    for (const auto& article : extra) {
        const QString key = key_for(article);
        if (seen.contains(key))
            continue;
        seen.insert(key);
        merged.append(article);
    }

    std::sort(merged.begin(), merged.end(), [](const fincept::services::NewsArticle& a,
                                               const fincept::services::NewsArticle& b) {
        return a.sort_ts > b.sort_ts;
    });
    return merged;
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

void GeopoliticsService::extract_model_events(const QVector<fincept::services::NewsArticle>& articles,
                                              const QString& country, const QString& city, const QString& category,
                                              int limit, ModelEventsCallback cb) {
    if (!python::PythonRunner::instance().is_available()) {
        cb(false, {}, 0, "Python runtime unavailable for model-based geopolitics extraction");
        return;
    }
    if (articles.isEmpty()) {
        cb(true, {}, 0, {});
        return;
    }

    const QString payload =
        QString::fromUtf8(QJsonDocument(articles_to_json(articles)).toJson(QJsonDocument::Compact));

    run_python("news_nlp.py",
               {"extract_geopolitics_events", payload, country, city, category, QString::number(limit)},
               "model_geopolitics_events", [cb](bool ok, const QString& output) {
                   if (!ok) {
                       cb(false, {}, 0, output);
                       return;
                   }

                   const QString json_text = python::extract_json(output).trimmed();
                   const QJsonObject root = QJsonDocument::fromJson(json_text.toUtf8()).object();
                   if (!root["success"].toBool()) {
                       cb(false, {}, 0, root["error"].toString("Model geopolitics extraction failed"));
                       return;
                   }

                   const QVector<NewsEvent> events = events_from_json(root["events"].toArray());
                   cb(true, events, root["total"].toInt(events.size()), {});
               });
}

void GeopoliticsService::load_reference_events(ModelEventsCallback cb) {
    if (reference_events_cache_ready_) {
        cb(true, reference_events_cache_, reference_events_cache_.size(), {});
        return;
    }

    const QVector<NewsEvent> archived = load_event_archive();
    if (!archived.isEmpty()) {
        reference_events_cache_ = archived;
        reference_events_cache_ready_ = true;
        cb(true, reference_events_cache_, reference_events_cache_.size(), {});
        return;
    }

    const QString cache_key = event_cache_key({}, {}, {}, 250);
    const QVariant cached = fincept::CacheManager::instance().get(cache_key);
    if (!cached.isNull()) {
        const QJsonObject root = QJsonDocument::fromJson(cached_bytes(cached)).object();
        reference_events_cache_ = events_from_json(root["events"].toArray());
        reference_events_cache_ready_ = true;
        cb(true, reference_events_cache_, root["total"].toInt(reference_events_cache_.size()), {});
        return;
    }

    QPointer<GeopoliticsService> self = this;
    fincept::services::NewsService::instance().fetch_all_news(
        false, [self, cache_key, cb](bool ok, QVector<fincept::services::NewsArticle> articles) {
            if (!self)
                return;
            if (!ok || articles.isEmpty()) {
                cb(false, {}, 0, "Local geopolitics feed unavailable");
                return;
            }

            self->extract_model_events(articles, {}, {}, {}, 250,
                                       [self, cache_key, cb](bool extract_ok, QVector<NewsEvent> events, int,
                                                             QString error) {
                                           if (!self)
                                               return;
                                           if (!extract_ok) {
                                               cb(false, {}, 0, error);
                                               return;
                                           }

                                           const QVector<NewsEvent> archived_events = merge_into_event_archive(events);
                                           QJsonObject cached_root;
                                           cached_root["events"] = events_to_json(limit_events(archived_events, 250));
                                           cached_root["total"] = archived_events.size();
                                           fincept::CacheManager::instance().put(
                                               cache_key,
                                               QVariant(QJsonDocument(cached_root).toJson(QJsonDocument::Compact)),
                                               kRefDataTtlSec, "geopolitics");
                                           self->reference_events_cache_ = archived_events;
                                           self->reference_events_cache_ready_ = true;
                                           cb(true, archived_events, archived_events.size(), {});
                                       });
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

    auto serve_archive = [this, country, city, category, limit](const QString& reason) -> bool {
        const QVector<NewsEvent> matching = filter_events(load_event_archive(), country, city, category);
        if (matching.isEmpty())
            return false;

        const int effective_limit = limit > 0 ? limit : 100;
        const QVector<NewsEvent> response = limit_events(matching, effective_limit);
        LOG_WARN("Geopolitics", QString("Serving %1 archived events after live pipeline miss: %2")
                                    .arg(response.size())
                                    .arg(reason.left(180)));
        emit events_loaded(response, matching.size());
        if (hub_registered_)
            publish_to_hub(QStringLiteral("geopolitics:events"), QVariant::fromValue(response));
        return true;
    };

    QPointer<GeopoliticsService> self = this;
    fincept::services::NewsService::instance().fetch_all_news(
        false, [self, country, city, category, limit, serve_archive](bool ok,
                                                                     QVector<fincept::services::NewsArticle> articles) {
            if (!self)
                return;
            if (!python::PythonRunner::instance().is_available()) {
                if (serve_archive(QStringLiteral("Python runtime unavailable")))
                    return;
                emit self->error_occurred("events", "Python runtime unavailable for model-based geopolitics extraction");
                return;
            }

            const QVector<fincept::services::NewsArticle> base_articles = ok ? articles : QVector<fincept::services::NewsArticle>{};
            const int search_limit = limit > 0 ? qBound(5, limit, 40) : 20;

            auto run_model_pipeline = [self, country, city, category, limit,
                                       serve_archive](QVector<fincept::services::NewsArticle> merged_articles) {
                if (!self)
                    return;
                if (merged_articles.isEmpty()) {
                    if (serve_archive(QStringLiteral("No geopolitics articles available")))
                        return;
                    emit self->error_occurred("events", "No geopolitics articles available for model extraction");
                    return;
                }

                self->extract_model_events(
                    merged_articles, country, city, category, limit > 0 ? limit : 200,
                    [self, country, city, category, limit,
                     serve_archive](bool extract_ok, QVector<NewsEvent> events, int, QString error) {
                        if (!self)
                            return;
                        if (!extract_ok) {
                            LOG_ERROR("Geopolitics", "Model geopolitics extraction failed: " + error.left(240));
                            if (serve_archive(error.isEmpty() ? QStringLiteral("Model extraction failed") : error))
                                return;
                            emit self->error_occurred("events", error.isEmpty()
                                                                    ? "Model geopolitics extraction failed"
                                                                    : error);
                            return;
                        }
                        if (events.isEmpty()) {
                            if (serve_archive(QStringLiteral("Model pipeline returned no geopolitics events")))
                                return;
                            emit self->error_occurred("events", "No geopolitics events produced by the model pipeline");
                            return;
                        }

                        const QVector<NewsEvent> archive = merge_into_event_archive(events);
                        self->reference_events_cache_ = archive;
                        self->reference_events_cache_ready_ = true;

                        QVector<NewsEvent> response_events = filter_events(archive, country, city, category);
                        if (response_events.isEmpty()) {
                            response_events = events;
                            sort_events_newest_first(response_events);
                        }
                        const int response_total = response_events.size();
                        response_events = limit_events(response_events, limit > 0 ? limit : 100);

                        QJsonObject cached_root;
                        cached_root["events"] = events_to_json(response_events);
                        cached_root["total"] = response_total;
                        fincept::CacheManager::instance().put(
                            event_cache_key(country, city, category, limit),
                            QVariant(QJsonDocument(cached_root).toJson(QJsonDocument::Compact)), kEventsTtlSec,
                            "geopolitics");
                        LOG_INFO("Geopolitics", QString("Loaded %1 events (%2 archived)")
                                                   .arg(response_events.size())
                                                   .arg(archive.size()));
                        emit self->events_loaded(response_events, response_total);
                        if (self->hub_registered_)
                            publish_to_hub(QStringLiteral("geopolitics:events"), QVariant::fromValue(response_events));
                    });
            };

            self->run_python(
                "news_search_rss.py", {"geopolitics", country, city, category, QString::number(search_limit)},
                "live_geopolitics_search",
                [self, base_articles, run_model_pipeline](bool search_ok, const QString& output) mutable {
                    if (!self)
                        return;

                    QVector<fincept::services::NewsArticle> merged_articles = base_articles;
                    if (search_ok) {
                        merged_articles = merge_articles(base_articles, parse_live_search_articles(output));
                    } else {
                        LOG_WARN("Geopolitics", "Live geopolitics search failed: " + output.left(240));
                    }

                    run_model_pipeline(merged_articles);
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
    load_reference_events([self](bool ok, QVector<NewsEvent> events, int, QString error) {
        if (!self)
            return;
        if (!ok) {
            emit self->error_occurred("countries", error);
            return;
        }
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
    load_reference_events([self](bool ok, QVector<NewsEvent> events, int, QString error) {
        if (!self)
            return;
        if (!ok) {
            emit self->error_occurred("categories", error);
            return;
        }
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
    load_reference_events([self](bool ok, QVector<NewsEvent> events, int, QString error) {
        if (!self)
            return;
        if (!ok) {
            emit self->error_occurred("cities", error);
            return;
        }
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
    return 2;  // Local news/model pipeline + HDX Python — conservative
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
