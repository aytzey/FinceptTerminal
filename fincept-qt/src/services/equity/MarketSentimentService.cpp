#include "services/equity/MarketSentimentService.h"

#include "core/logging/Logger.h"
#include "services/equity/MarketSentimentSupport.h"
#include "services/news/NewsService.h"
#include "storage/cache/CacheManager.h"
#include "storage/repositories/DataSourceRepository.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QMap>
#include <QNetworkReply>
#include <QSet>
#include <QUrlQuery>

#include <algorithm>
#include <memory>

namespace fincept::services::equity {

namespace {

constexpr auto kApiBase = "https://api.adanos.org";

struct PendingSnapshotRequest {
    quint64 request_id = 0;
    QString symbol;
    int pending = 0;
    MarketSentimentSnapshot snapshot;
    QMap<QString, SentimentSourceSnapshot> sources;
};

QString cache_key_for_symbol(const QString& symbol, int days) {
    return QString("equity:adanos:sentiment:%1:%2").arg(symbol.toUpper(), QString::number(days));
}

QString local_cache_key_for_symbol(const QString& symbol, int days) {
    return QString("equity:local:sentiment:%1:%2").arg(symbol.toUpper(), QString::number(days));
}

QByteArray variant_to_json_bytes(const QVariant& value) {
    if (value.canConvert<QByteArray>()) {
        return value.toByteArray();
    }
    return value.toString().toUtf8();
}

} // namespace

MarketSentimentService& MarketSentimentService::instance() {
    static MarketSentimentService service;
    return service;
}

MarketSentimentService::MarketSentimentService(QObject* parent) : QObject(parent) {
    nam_ = new QNetworkAccessManager(this);
}

bool MarketSentimentService::is_configured() const {
    return true;
}

MarketSentimentService::ConnectionConfig MarketSentimentService::load_connection() const {
    const auto result = DataSourceRepository::instance().list_all();
    if (result.is_err()) {
        return {};
    }

    for (const auto& source : result.value()) {
        if (source.provider != sentiment::kProviderId || !source.enabled) {
            continue;
        }
        const auto config = QJsonDocument::fromJson(source.config.toUtf8()).object();
        const auto api_key = config.value("apiKey").toString().trimmed();
        if (!api_key.isEmpty()) {
            return {true, api_key};
        }
    }

    return {};
}

QNetworkRequest MarketSentimentService::build_request(const QUrl& url, const QString& api_key) const {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, "FinceptTerminal/4.0");
    request.setRawHeader("X-API-Key", api_key.toUtf8());
    return request;
}

void MarketSentimentService::fetch_snapshot(const QString& symbol, int days, bool force) {
    const QString normalized_symbol = symbol.trimmed().toUpper();
    if (normalized_symbol.isEmpty()) {
        return;
    }

    const auto connection = load_connection();
    if (!connection.configured) {
        fetch_local_snapshot(normalized_symbol, days, force);
        return;
    }

    const QString key = cache_key_for_symbol(normalized_symbol, days);
    if (!force && CacheManager::instance().has(key)) {
        const auto cached = variant_to_json_bytes(CacheManager::instance().get(key));
        const auto doc = QJsonDocument::fromJson(cached);
        if (doc.isObject()) {
            auto snapshot = sentiment::snapshot_from_json(doc.object());
            snapshot.configured = true;
            emit snapshot_loaded(normalized_symbol, snapshot);
            return;
        }
    }

    active_request_id_ += 1;
    const auto request_id = active_request_id_;

    auto pending = std::make_shared<PendingSnapshotRequest>();
    pending->request_id = request_id;
    pending->symbol = normalized_symbol;
    pending->pending = sentiment::source_ids().size();
    pending->snapshot.symbol = normalized_symbol;
    pending->snapshot.configured = true;
    pending->snapshot.status = "loading";

    auto finalize = [this, pending, key]() {
        if (pending->request_id != active_request_id_) {
            return;
        }

        const auto ordered_sources = sentiment::source_ids();
        pending->snapshot.sources.clear();
        pending->snapshot.sources.reserve(ordered_sources.size());

        double total_buzz = 0.0;
        double total_bullish = 0.0;
        int coverage = 0;

        for (const auto& source_id : ordered_sources) {
            const auto source = pending->sources.value(source_id, SentimentSourceSnapshot{source_id, sentiment::source_label(source_id)});
            pending->snapshot.sources.append(source);
            if (source.available) {
                total_buzz += source.buzz_score;
                total_bullish += source.bullish_pct;
                coverage += 1;
            }
        }

        pending->snapshot.coverage = coverage;
        pending->snapshot.source_alignment = sentiment::compute_source_alignment(pending->snapshot.sources);
        pending->snapshot.fetched_at = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

        if (coverage > 0) {
            pending->snapshot.available = true;
            pending->snapshot.status = "ok";
            pending->snapshot.average_buzz = total_buzz / coverage;
            pending->snapshot.average_bullish_pct = total_bullish / coverage;
        } else {
            pending->snapshot.available = false;
            pending->snapshot.status = "unavailable";
            pending->snapshot.message = "No Adanos market sentiment snapshot is available for this symbol yet.";
        }

        CacheManager::instance().put(
            key,
            QVariant(QJsonDocument(sentiment::snapshot_to_json(pending->snapshot)).toJson(QJsonDocument::Compact)),
            kCacheTtlSec,
            "equity");

        emit snapshot_loaded(pending->symbol, pending->snapshot);
    };

    for (const auto& source_id : sentiment::source_ids()) {
        QUrl url(QString("%1/%2/stocks/v1/compare").arg(kApiBase, source_id));
        QUrlQuery query;
        query.addQueryItem("tickers", normalized_symbol);
        query.addQueryItem("days", QString::number(days));
        url.setQuery(query);

        auto* reply = nam_->get(build_request(url, connection.api_key));
        connect(reply, &QNetworkReply::finished, this, [this, reply, pending, source_id, finalize]() {
            const QUrl request_url = reply->request().url();
            const int status_code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll();
            reply->deleteLater();

            SentimentSourceSnapshot snapshot;
            if (reply->error() == QNetworkReply::NoError) {
                const auto doc = QJsonDocument::fromJson(body);
                if (doc.isObject()) {
                    snapshot = sentiment::parse_compare_payload(source_id, doc.object());
                }
            } else {
                LOG_WARN(
                    "MarketSentiment",
                    QString("Adanos request failed for %1 (%2): %3")
                        .arg(source_id, request_url.toString(QUrl::RemoveQuery), reply->errorString()));
                if (pending->request_id == active_request_id_) {
                    emit error_occurred(
                        "market_sentiment",
                        QString("Adanos source %1 returned HTTP %2.").arg(source_id, QString::number(status_code)));
                }
            }

            if (snapshot.source_id.isEmpty()) {
                snapshot.source_id = source_id;
                snapshot.label = sentiment::source_label(source_id);
            }

            pending->sources.insert(source_id, snapshot);
            pending->pending -= 1;
            if (pending->pending == 0) {
                finalize();
            }
        });
    }
}

void MarketSentimentService::fetch_local_snapshot(const QString& symbol, int days, bool force) {
    const QString key = local_cache_key_for_symbol(symbol, days);
    if (!force && CacheManager::instance().has(key)) {
        const auto cached = variant_to_json_bytes(CacheManager::instance().get(key));
        const auto doc = QJsonDocument::fromJson(cached);
        if (doc.isObject()) {
            auto snapshot = sentiment::snapshot_from_json(doc.object());
            snapshot.configured = true;
            emit snapshot_loaded(symbol, snapshot);
            return;
        }
    }

    fincept::services::NewsService::instance().fetch_all_news(
        force, [this, symbol, days, key](bool ok, QVector<fincept::services::NewsArticle> articles) {
        MarketSentimentSnapshot snapshot;
        snapshot.symbol = symbol;
        snapshot.configured = true;
        snapshot.fetched_at = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

        QMap<QString, SentimentSourceSnapshot> sources;
        for (const auto& source_id : sentiment::source_ids())
            sources.insert(source_id, SentimentSourceSnapshot{source_id, sentiment::source_label(source_id)});

        if (!ok) {
            snapshot.status = "unavailable";
            snapshot.message = "Local news sentiment could not refresh right now.";
            snapshot.sources = sources.values().toVector();
            emit snapshot_loaded(symbol, snapshot);
            return;
        }

        const int64_t cutoff = QDateTime::currentSecsSinceEpoch() - static_cast<int64_t>(std::max(1, days)) * 86400;
        int matches = 0;
        int bullish = 0;
        int bearish = 0;
        int neutral = 0;
        double weighted_score = 0.0;
        QSet<QString> unique_sources;

        for (const auto& article : articles) {
            if (article.sort_ts > 0 && article.sort_ts < cutoff)
                continue;
            const QString text = (article.headline + " " + article.summary + " " + article.tickers.join(" ")).toUpper();
            if (!article.tickers.contains(symbol, Qt::CaseInsensitive) && !text.contains(symbol))
                continue;

            ++matches;
            unique_sources.insert(article.source);
            const double impact_weight = article.impact == fincept::services::Impact::HIGH
                                             ? 1.5
                                             : article.impact == fincept::services::Impact::MEDIUM ? 1.15 : 1.0;
            if (article.sentiment == fincept::services::Sentiment::BULLISH) {
                ++bullish;
                weighted_score += impact_weight;
            } else if (article.sentiment == fincept::services::Sentiment::BEARISH) {
                ++bearish;
                weighted_score -= impact_weight;
            } else {
                ++neutral;
            }
        }

        auto news = sources.value("news");
        news.available = matches > 0;
        news.activity_count = matches;
        news.buzz_score = std::min(100.0, matches * 8.0 + unique_sources.size() * 6.0);
        news.bullish_pct = matches > 0 ? 100.0 * bullish / matches : 0.0;
        news.sentiment_score = matches > 0 ? weighted_score / matches : 0.0;
        sources.insert("news", news);

        snapshot.sources = sources.values().toVector();
        snapshot.coverage = news.available ? 1 : 0;
        snapshot.source_alignment = sentiment::compute_source_alignment(snapshot.sources);
        snapshot.average_buzz = news.available ? news.buzz_score : 0.0;
        snapshot.average_bullish_pct = news.available ? news.bullish_pct : 0.0;
        snapshot.available = news.available;
        snapshot.status = news.available ? "ok" : "unavailable";
        snapshot.message = news.available
                               ? QString("Local news sentiment from %1 matching articles (%2 bullish, %3 bearish, %4 neutral).")
                                     .arg(matches)
                                     .arg(bullish)
                                     .arg(bearish)
                                     .arg(neutral)
                               : "No local news sentiment snapshot is available for this symbol yet.";

        CacheManager::instance().put(
            key,
            QVariant(QJsonDocument(sentiment::snapshot_to_json(snapshot)).toJson(QJsonDocument::Compact)),
            kCacheTtlSec,
            "equity");
        emit snapshot_loaded(symbol, snapshot);
    });
}

} // namespace fincept::services::equity
