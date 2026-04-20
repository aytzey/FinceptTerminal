#include "services/news/NewsService.h"

#include "ai_chat/LlmService.h"
#include "core/logging/Logger.h"
#include "python/PythonRunner.h"
#include "storage/cache/CacheManager.h"

#    include "datahub/DataHub.h"
#    include "datahub/DataHubMetaTypes.h"

#include <QAtomicInt>
#include <QDateTime>
#include <QEventLoop>
#include <QHash>
#include <QJsonDocument>
#include <QMap>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>
#include <QtConcurrent>

#ifdef HAS_QT_WEBSOCKETS
#    include <QtWebSockets/QWebSocket>
#endif

#include <algorithm>
#include <memory>

namespace fincept::services {

static constexpr int kFeedTransferTimeoutMs = 5000;   // 5s per RSS feed request
static constexpr int kWsReconnectDelayMs    = 10000;  // 10s before WebSocket reconnect
static constexpr int kSummaryMaxChars       = 300;    // max chars for article summary
static constexpr int kNewsArticlesCacheTtlSec = 600;  // 10 min
static constexpr const char* kNewsArticlesCacheKey = "news:articles:v2";

namespace {

struct ArticleSnapshot {
    bool success = false;
    QString title;
    QString summary;
    QString body;
    QString source;
    QString error;
};

using ArticlesTransformCallback = std::function<void(QVector<NewsArticle>)>;

QString decode_html_entities(QString text) {
    static const QMap<QString, QString> replacements = {
        {"&amp;", "&"}, {"&lt;", "<"},   {"&gt;", ">"},   {"&quot;", "\""},
        {"&#39;", "'"}, {"&nbsp;", " "}, {"&#8217;", "'"}, {"&#8211;", "-"},
        {"&#8212;", "-"},
    };
    for (auto it = replacements.begin(); it != replacements.end(); ++it)
        text.replace(it.key(), it.value(), Qt::CaseInsensitive);
    return text;
}

QString extract_first_match(const QString& text, const QRegularExpression& re) {
    const auto match = re.match(text);
    if (!match.hasMatch())
        return {};
    return decode_html_entities(match.captured(1).trimmed()).simplified();
}

QString strip_html_blocks(QString html) {
    static const QRegularExpression script_re("<script[^>]*>.*?</script>",
                                              QRegularExpression::CaseInsensitiveOption
                                                  | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression style_re("<style[^>]*>.*?</style>",
                                             QRegularExpression::CaseInsensitiveOption
                                                 | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression tag_re("<[^>]+>");
    html.remove(script_re);
    html.remove(style_re);
    html.replace(tag_re, " ");
    return decode_html_entities(html).simplified();
}

QString normalized_article_url(const QString& raw_url) {
    QUrl url(raw_url.trimmed());
    if (!url.isValid() || url.host().isEmpty()) {
        QString fallback = raw_url.trimmed();
        fallback.remove(QRegularExpression(R"([?#].*$)"));
        fallback.remove(QRegularExpression(R"(/+$)"));
        return fallback.toLower();
    }

    QString host = url.host().toLower();
    if (host.startsWith(QStringLiteral("www.")))
        host = host.mid(4);

    QString path = url.path();
    path.remove(QRegularExpression(R"(/+$)"));
    return host + path.toLower();
}

QString normalized_article_headline(QString headline) {
    headline = strip_html_blocks(headline).toLower().simplified();
    headline.remove(QRegularExpression(R"(\s+-\s+[^-]{2,80}$)"));
    headline.remove(QRegularExpression(R"([^\p{L}\p{N}\s])"));
    return headline.simplified();
}

QVector<NewsArticle> dedupe_articles(const QVector<NewsArticle>& articles) {
    QVector<NewsArticle> out;
    QSet<QString> seen_urls;
    QSet<QString> seen_headlines;
    out.reserve(articles.size());

    for (const auto& article : articles) {
        const QString headline_key = normalized_article_headline(article.headline);
        if (headline_key.isEmpty())
            continue;

        const QString url_key = normalized_article_url(article.link);
        if (!url_key.isEmpty() && seen_urls.contains(url_key))
            continue;
        if (seen_headlines.contains(headline_key))
            continue;

        if (!url_key.isEmpty())
            seen_urls.insert(url_key);
        seen_headlines.insert(headline_key);
        out.append(article);
    }

    return out;
}

ArticleSnapshot fetch_article_snapshot(QNetworkAccessManager* nam, const QString& url) {
    ArticleSnapshot snapshot;
    if (!nam) {
        snapshot.error = "Network manager unavailable";
        return snapshot;
    }

    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, "FinceptTerminal/4.0");
    req.setRawHeader("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    req.setTransferTimeout(10000);

    auto* reply = nam->get(req);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        if (!reply->isFinished())
            reply->abort();
        loop.quit();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(10000);
    loop.exec();
    timer.stop();

    if (reply->error() != QNetworkReply::NoError) {
        snapshot.error = reply->errorString();
        reply->deleteLater();
        return snapshot;
    }

    const QString html = QString::fromUtf8(reply->readAll());
    reply->deleteLater();

    static const QRegularExpression og_title_re(
        R"(<meta[^>]+property=["']og:title["'][^>]+content=["']([^"']+)["'])",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression title_re(R"(<title[^>]*>(.*?)</title>)",
                                             QRegularExpression::CaseInsensitiveOption
                                                 | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression og_desc_re(
        R"(<meta[^>]+property=["']og:description["'][^>]+content=["']([^"']+)["'])",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression desc_re(
        R"(<meta[^>]+name=["']description["'][^>]+content=["']([^"']+)["'])",
        QRegularExpression::CaseInsensitiveOption);

    snapshot.title = extract_first_match(html, og_title_re);
    if (snapshot.title.isEmpty())
        snapshot.title = extract_first_match(html, title_re);
    snapshot.summary = extract_first_match(html, og_desc_re);
    if (snapshot.summary.isEmpty())
        snapshot.summary = extract_first_match(html, desc_re);
    snapshot.body = strip_html_blocks(html);

    if (snapshot.summary.isEmpty())
        snapshot.summary = snapshot.body.left(kSummaryMaxChars);
    if (snapshot.title.isEmpty())
        snapshot.title = snapshot.summary.left(120);

    snapshot.success = !snapshot.title.isEmpty() || !snapshot.summary.isEmpty() || !snapshot.body.isEmpty();
    if (!snapshot.success)
        snapshot.error = "Could not extract readable article text";
    return snapshot;
}

QString clean_llm_text(QString text) {
    text = text.trimmed();
    if (text.startsWith("```")) {
        text.remove(QRegularExpression(R"(^```[a-zA-Z0-9_-]*\s*)"));
        text.remove(QRegularExpression(R"(\s*```$)"));
    }
    return text.trimmed();
}

QString article_lines_for_prompt(const QVector<NewsArticle>& articles, int count) {
    QStringList lines;
    const int take = std::min(count, static_cast<int>(articles.size()));
    lines.reserve(take);
    for (int i = 0; i < take; ++i) {
        const auto& a = articles[i];
        lines << QString("%1. [%2/%3/%4] %5 - %6")
                     .arg(i + 1)
                     .arg(a.source.left(40), a.category.left(24), sentiment_string(a.sentiment))
                     .arg(a.headline.left(220), a.summary.left(260));
    }
    return lines.join('\n');
}

QString build_headline_brief_prompt(const QVector<NewsArticle>& articles, int count) {
    return QString(
               "Write a concise professional market-news brief from these headlines.\n"
               "Use only the supplied items. Do not invent prices, facts, or causality.\n"
               "Focus on market drivers, cross-asset implications, and watch items.\n"
               "Return 4 short bullets, no markdown heading, no preamble.\n\n"
               "%1")
        .arg(article_lines_for_prompt(articles, count));
}

QString build_article_analysis_prompt(const ArticleSnapshot& snapshot, const QString& url) {
    const QString body = snapshot.body.left(6000);
    return QString(
               "Analyze this financial news article for a trading terminal.\n"
               "Use only the article text. If evidence is weak, say so in the relevant details.\n"
               "Return JSON only with this schema:\n"
               "{"
               "\"summary\":\"2-3 sentence market interpretation\","
               "\"sentiment_score\":0,"
               "\"sentiment_intensity\":0,"
               "\"sentiment_confidence\":0,"
               "\"market_urgency\":\"LOW|MEDIUM|HIGH\","
               "\"market_prediction\":\"negative|neutral|moderate_positive|positive\","
               "\"keywords\":[\"...\"],"
               "\"topics\":[\"...\"],"
               "\"key_points\":[\"...\"],"
               "\"regulatory\":{\"level\":\"LOW|MEDIUM|HIGH\",\"details\":\"...\"},"
               "\"geopolitical\":{\"level\":\"LOW|MEDIUM|HIGH\",\"details\":\"...\"},"
               "\"operational\":{\"level\":\"LOW|MEDIUM|HIGH\",\"details\":\"...\"},"
               "\"market\":{\"level\":\"LOW|MEDIUM|HIGH\",\"details\":\"...\"}"
               "}.\n\n"
               "URL: %1\n"
               "Source: %2\n"
               "Title: %3\n"
               "Summary: %4\n"
               "Body: %5")
        .arg(url.left(500), snapshot.source.left(120), snapshot.title.left(300), snapshot.summary.left(800), body);
}

QStringList string_array_from_json(const QJsonValue& value, int limit) {
    QStringList out;
    for (const auto& item : value.toArray()) {
        const QString text = item.toString().trimmed();
        if (!text.isEmpty())
            out << text.left(180);
        if (out.size() >= limit)
            break;
    }
    return out;
}

QString normalized_choice(QString value, const QStringList& allowed, const QString& fallback) {
    value = value.trimmed();
    const QString upper = value.toUpper();
    for (const auto& item : allowed) {
        if (upper == item.toUpper())
            return item;
    }
    return fallback;
}

RiskSignal risk_signal_from_json(const QJsonValue& value) {
    const auto obj = value.toObject();
    RiskSignal signal;
    signal.level = normalized_choice(obj.value("level").toString(), {"LOW", "MEDIUM", "HIGH"}, "LOW");
    signal.details = obj.value("details").toString().trimmed().left(260);
    if (signal.details.isEmpty())
        signal.details = "No material signal identified in the article text.";
    return signal;
}

bool analysis_from_llm_json(const QString& content, NewsAnalysis& analysis) {
    QJsonParseError parse_error;
    const QString json_text = python::extract_json(clean_llm_text(content));
    const auto doc = QJsonDocument::fromJson(json_text.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject())
        return false;

    const auto obj = doc.object();
    analysis.summary = obj.value("summary").toString().trimmed().left(1000);
    analysis.sentiment.score = std::clamp(obj.value("sentiment_score").toDouble(0.0), -1.0, 1.0);
    analysis.sentiment.intensity = std::clamp(obj.value("sentiment_intensity").toDouble(0.0), 0.0, 1.0);
    analysis.sentiment.confidence = std::clamp(obj.value("sentiment_confidence").toDouble(0.0), 0.0, 1.0);
    analysis.market_impact.urgency =
        normalized_choice(obj.value("market_urgency").toString(), {"LOW", "MEDIUM", "HIGH"}, "LOW");
    analysis.market_impact.prediction = normalized_choice(
        obj.value("market_prediction").toString(),
        {"negative", "neutral", "moderate_positive", "positive"}, "neutral");
    analysis.keywords = string_array_from_json(obj.value("keywords"), 10);
    analysis.topics = string_array_from_json(obj.value("topics"), 8);
    analysis.key_points = string_array_from_json(obj.value("key_points"), 6);
    analysis.regulatory = risk_signal_from_json(obj.value("regulatory"));
    analysis.geopolitical = risk_signal_from_json(obj.value("geopolitical"));
    analysis.operational = risk_signal_from_json(obj.value("operational"));
    analysis.market = risk_signal_from_json(obj.value("market"));
    analysis.credits_used = 0;
    analysis.credits_remaining = 0;
    return !analysis.summary.isEmpty() || !analysis.key_points.isEmpty();
}

QJsonArray articles_to_nlp_json(const QVector<NewsArticle>& articles) {
    QJsonArray arr;
    for (const auto& article : articles) {
        QJsonObject obj;
        obj["id"] = article.id;
        obj["headline"] = article.headline;
        obj["summary"] = article.summary;
        obj["source"] = article.source;
        obj["category"] = article.category;
        obj["region"] = article.region;
        obj["sort_ts"] = static_cast<qint64>(article.sort_ts);
        arr.append(obj);
    }
    return arr;
}

Sentiment model_sentiment_from_string(const QString& value) {
    const QString normalized = value.trimmed().toUpper();
    if (normalized == "BULLISH")
        return Sentiment::BULLISH;
    if (normalized == "BEARISH")
        return Sentiment::BEARISH;
    return Sentiment::NEUTRAL;
}

void recompute_threats(QVector<NewsArticle>& articles) {
    for (auto& article : articles)
        article.threat = NewsService::classify_threat(article);
}

void apply_finbert_sentiment(QVector<NewsArticle> articles, ArticlesTransformCallback cb) {
    for (auto& article : articles)
        article.sentiment = Sentiment::NEUTRAL;

    if (articles.isEmpty() || !python::PythonRunner::instance().is_available()) {
        recompute_threats(articles);
        cb(std::move(articles));
        return;
    }

    const QString json = QString::fromUtf8(QJsonDocument(articles_to_nlp_json(articles)).toJson(QJsonDocument::Compact));
    python::PythonRunner::instance().run(
        "news_nlp.py", {"analyze_sentiment_batch", json},
        [articles = std::move(articles), cb = std::move(cb)](python::PythonResult result) mutable {
            if (!result.success) {
                LOG_WARN("NewsService", "FinBERT sentiment unavailable: " + result.error.left(180));
                recompute_threats(articles);
                cb(std::move(articles));
                return;
            }

            const auto doc = QJsonDocument::fromJson(python::extract_json(result.output).toUtf8());
            const auto obj = doc.object();
            if (!obj.value("success").toBool()) {
                LOG_WARN("NewsService", "FinBERT sentiment failed: " + obj.value("error").toString().left(180));
                recompute_threats(articles);
                cb(std::move(articles));
                return;
            }

            QHash<QString, int> article_index;
            article_index.reserve(articles.size());
            for (int i = 0; i < articles.size(); ++i)
                article_index.insert(articles[i].id, i);

            for (const auto& value : obj.value("results").toArray()) {
                const auto item = value.toObject();
                const QString id = item.value("id").toString();
                const auto it = article_index.find(id);
                if (it == article_index.end())
                    continue;
                articles[it.value()].sentiment = model_sentiment_from_string(item.value("sentiment").toString());
            }

            recompute_threats(articles);
            cb(std::move(articles));
        });
}

QJsonObject article_to_cache_json(const NewsArticle& article) {
    QJsonObject obj;
    obj["id"] = article.id;
    obj["time"] = article.time;
    obj["headline"] = article.headline;
    obj["summary"] = article.summary;
    obj["source"] = article.source;
    obj["region"] = article.region;
    obj["category"] = article.category;
    obj["link"] = article.link;
    obj["sort_ts"] = static_cast<qint64>(article.sort_ts);
    obj["tier"] = article.tier;
    obj["priority"] = priority_string(article.priority);
    obj["sentiment"] = sentiment_string(article.sentiment);
    obj["impact"] = impact_string(article.impact);
    obj["lang"] = article.lang;
    QJsonArray tickers;
    for (const auto& ticker : article.tickers)
        tickers.append(ticker);
    obj["tickers"] = tickers;
    return obj;
}

void cache_news_articles(const QVector<NewsArticle>& articles) {
    if (articles.isEmpty())
        return;
    QJsonArray arr;
    for (const auto& article : articles)
        arr.append(article_to_cache_json(article));
    fincept::CacheManager::instance().put(
        kNewsArticlesCacheKey, QVariant(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact))),
        kNewsArticlesCacheTtlSec, "news");
}

} // namespace

// ── Singleton ───────────────────────────────────────────────────────────────

NewsService& NewsService::instance() {
    static NewsService s;
    return s;
}

NewsService::NewsService() {
    nam_ = new QNetworkAccessManager(this);
    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(kArticleCacheTtlSec * 1000);
    connect(refresh_timer_, &QTimer::timeout, this,
            [this]() { fetch_all_news(true, [](bool, QVector<NewsArticle>) {}); });
}

// ── Fetch all RSS feeds in parallel ─────────────────────────────────────────

void NewsService::fetch_all_news(bool force, ArticlesCallback cb) {
    if (!force) {
        const QVariant cached = fincept::CacheManager::instance().get(kNewsArticlesCacheKey);
        if (!cached.isNull()) {
            const QJsonArray arr = QJsonDocument::fromJson(cached.toString().toUtf8()).array();
            QVector<NewsArticle> articles;
            articles.reserve(arr.size());
            for (const auto& v : arr) {
                const QJsonObject o = v.toObject();
                NewsArticle a;
                a.id = o["id"].toString();
                a.time = o["time"].toString();
                a.headline = o["headline"].toString();
                a.summary = o["summary"].toString();
                a.source = o["source"].toString();
                a.region = o["region"].toString();
                a.category = o["category"].toString();
                a.link = o["link"].toString();
                a.sort_ts = o["sort_ts"].toVariant().toLongLong();
                a.tier = o["tier"].toInt(4);
                a.priority = priority_from_string(o["priority"].toString());
                a.sentiment = sentiment_from_string(o["sentiment"].toString());
                a.impact = impact_from_string(o["impact"].toString());
                a.lang = o["lang"].toString();
                for (const auto& t : o["tickers"].toArray())
                    a.tickers << t.toString();
                articles.append(a);
            }
            if (!articles.isEmpty()) {
                articles = dedupe_articles(articles);
                cb(true, articles);
                publish_articles_to_hub(articles);
                return;
            }
        }
    }

    auto feeds = default_feeds();
    feed_count_ = feeds.size();

    // Shared state for collecting results from parallel requests
    struct FetchState {
        QMutex mutex;
        QVector<NewsArticle> all_articles;
        QAtomicInt remaining{0};
        ArticlesCallback callback;
        NewsService* service = nullptr;
    };

    auto state = std::make_shared<FetchState>();
    state->remaining.storeRelaxed(feeds.size());
    state->callback = std::move(cb);
    state->service = this;

    for (const auto& feed : feeds) {
        QNetworkRequest req(QUrl(feed.url));
        req.setHeader(QNetworkRequest::UserAgentHeader, "FinceptTerminal/4.0");
        req.setRawHeader("Accept", "application/rss+xml, application/xml, text/xml, */*");
        req.setTransferTimeout(kFeedTransferTimeoutMs);

        auto* reply = nam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, feed, state]() {
            reply->deleteLater();

            QVector<NewsArticle> articles;
            if (reply->error() == QNetworkReply::NoError) {
                QByteArray data = reply->readAll();
                if (data.trimmed().startsWith('<')) {
                    articles = parse_rss_xml(data, feed);
                }
            }

            {
                QMutexLocker lock(&state->mutex);
                state->all_articles.append(articles);
            }

            if (state->remaining.fetchAndSubRelaxed(1) == 1) {
                // Last feed done — sort by time descending
                auto all = dedupe_articles(state->all_articles);
                std::sort(all.begin(), all.end(),
                          [](const NewsArticle& a, const NewsArticle& b) { return a.sort_ts > b.sort_ts; });

                apply_finbert_sentiment(std::move(all), [state](QVector<NewsArticle> refined) {
                    QSet<QString> sources;
                    for (const auto& a : refined)
                        sources.insert(a.source);
                    state->service->active_sources_ = sources.values();

                    cache_news_articles(refined);

                    LOG_INFO("NewsService",
                             QString("Fetched %1 articles from %2 sources with FinBERT sentiment")
                                 .arg(refined.size())
                                 .arg(sources.size()));

                    state->callback(true, refined);
                    emit state->service->articles_updated(refined);
                    state->service->publish_articles_to_hub(refined);
                });
            }
        });
    }
}

// ── Progressive fetch — emits partial batches as each feed arrives ───────────
// First few fast feeds (Reuters, BBC) typically arrive in <300ms giving the
// screen something to render immediately while slower feeds trickle in.

void NewsService::fetch_all_news_progressive(bool force, ArticlesCallback final_cb) {
    if (!force) {
        const QVariant cached = fincept::CacheManager::instance().get(kNewsArticlesCacheKey);
        if (!cached.isNull()) {
            const QJsonArray arr = QJsonDocument::fromJson(cached.toString().toUtf8()).array();
            QVector<NewsArticle> articles;
            articles.reserve(arr.size());
            for (const auto& v : arr) {
                const QJsonObject o = v.toObject();
                NewsArticle a;
                a.id = o["id"].toString();
                a.time = o["time"].toString();
                a.headline = o["headline"].toString();
                a.summary = o["summary"].toString();
                a.source = o["source"].toString();
                a.region = o["region"].toString();
                a.category = o["category"].toString();
                a.link = o["link"].toString();
                a.sort_ts = o["sort_ts"].toVariant().toLongLong();
                a.tier = o["tier"].toInt(4);
                a.priority = priority_from_string(o["priority"].toString());
                a.sentiment = sentiment_from_string(o["sentiment"].toString());
                a.impact = impact_from_string(o["impact"].toString());
                a.lang = o["lang"].toString();
                for (const auto& t : o["tickers"].toArray())
                    a.tickers << t.toString();
                articles.append(a);
            }
            if (!articles.isEmpty()) {
                articles = dedupe_articles(articles);
                final_cb(true, articles);
                emit articles_partial(articles, feed_count_, feed_count_);
                publish_articles_to_hub(articles);
                return;
            }
        }
    }

    auto feeds = default_feeds();
    feed_count_ = feeds.size();
    const int total = feeds.size();

    struct FetchState {
        QMutex mutex;
        QVector<NewsArticle> all_articles;
        QAtomicInt remaining{0};
        QAtomicInt done{0};
        ArticlesCallback callback;
        NewsService* service = nullptr;
    };

    auto state = std::make_shared<FetchState>();
    state->remaining.storeRelaxed(total);
    state->callback = std::move(final_cb);
    state->service = this;

    for (const auto& feed : feeds) {
        QNetworkRequest req(QUrl(feed.url));
        req.setHeader(QNetworkRequest::UserAgentHeader, "FinceptTerminal/4.0");
        req.setRawHeader("Accept", "application/rss+xml, application/xml, text/xml, */*");
        req.setTransferTimeout(kFeedTransferTimeoutMs);

        auto* reply = nam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, feed, state, total, this]() {
            reply->deleteLater();

            QVector<NewsArticle> batch;
            if (reply->error() == QNetworkReply::NoError) {
                QByteArray data = reply->readAll();
                if (data.trimmed().startsWith('<'))
                    batch = parse_rss_xml(data, feed);
            }

            QVector<NewsArticle> snapshot;
            int feeds_done = 0;
            {
                QMutexLocker lock(&state->mutex);
                state->all_articles.append(batch);
                feeds_done = state->done.fetchAndAddRelaxed(1) + 1;
                // Partial snapshot sorted by time for progressive display
                snapshot = dedupe_articles(state->all_articles);
            }
            std::sort(snapshot.begin(), snapshot.end(),
                      [](const NewsArticle& a, const NewsArticle& b) { return a.sort_ts > b.sort_ts; });
            emit articles_partial(snapshot, feeds_done, total);
            // Progressive publish — each chunk fans out the accumulated
            // list. Hub's per-topic coalescing (news:general at 250ms)
            // throttles the UI repaint storm on cold-cache fills.
            publish_articles_to_hub(snapshot);

            if (state->remaining.fetchAndSubRelaxed(1) == 1) {
                // All feeds done — finalize cache
                auto all = dedupe_articles(state->all_articles);
                std::sort(all.begin(), all.end(),
                          [](const NewsArticle& a, const NewsArticle& b) { return a.sort_ts > b.sort_ts; });

                apply_finbert_sentiment(std::move(all), [state, total](QVector<NewsArticle> refined) {
                    QSet<QString> sources;
                    for (const auto& a : refined)
                        sources.insert(a.source);
                    state->service->active_sources_ = sources.values();

                    cache_news_articles(refined);

                    LOG_INFO("NewsService",
                             QString("Progressive fetch complete: %1 articles, %2 sources with FinBERT sentiment")
                                 .arg(refined.size())
                                 .arg(sources.size()));

                    emit state->service->articles_partial(refined, total, total);
                    state->callback(true, refined);
                    emit state->service->articles_updated(refined);
                    state->service->publish_articles_to_hub(refined);
                });
            }
        });
    }
}

// ── Codex Article Analysis ─────────────────────────────────────────────────

void NewsService::analyze_article(const QString& url, AnalysisCallback cb) {
    const QString trimmed_url = url.trimmed();
    auto finish_with_snapshot = [this, cb, trimmed_url](const ArticleSnapshot& snapshot) {
        if (!snapshot.success) {
            LOG_ERROR("NewsService", "Local article analysis failed: " + snapshot.error);
            cb(false, {});
            return;
        }

        if (!ai_chat::LlmService::instance().is_configured()) {
            LOG_WARN("NewsService", "Article analysis requires Codex OAuth or another configured LLM provider");
            cb(false, {});
            return;
        }

        const QString prompt = build_article_analysis_prompt(snapshot, trimmed_url);
        QPointer<NewsService> self = this;
        auto future = QtConcurrent::run([self, cb, prompt]() {
            const auto response = ai_chat::LlmService::instance().chat(prompt, {}, false);
            NewsAnalysis analysis;
            const bool ok = response.success && analysis_from_llm_json(response.content, analysis);
            const QString error = response.error;

            if (!self)
                return;
            QMetaObject::invokeMethod(
                self,
                [self, cb, ok, analysis, error]() {
                    if (!self)
                        return;
                    if (!ok) {
                        LOG_WARN("NewsService", "Codex article analysis failed: " + error.left(180));
                        cb(false, {});
                        return;
                    }
                    cb(true, analysis);
                    emit self->analysis_ready(analysis);
                },
                Qt::QueuedConnection);
        });
        Q_UNUSED(future);
    };

    if (!python::PythonRunner::instance().is_available()) {
        finish_with_snapshot(fetch_article_snapshot(nam_, trimmed_url));
        return;
    }

    QPointer<NewsService> self = this;
    python::PythonRunner::instance().run(
        "news_article_extract.py", {trimmed_url}, [self, finish_with_snapshot, trimmed_url](python::PythonResult result) {
            if (!self)
                return;

            if (result.success) {
                const QJsonObject obj = QJsonDocument::fromJson(result.output.toUtf8()).object();
                if (obj["success"].toBool()) {
                    ArticleSnapshot snapshot;
                    snapshot.success = true;
                    snapshot.title = obj["title"].toString().trimmed();
                    snapshot.summary = obj["summary"].toString().trimmed();
                    snapshot.body = obj["body"].toString().trimmed();
                    snapshot.source = obj["source"].toString().trimmed().toUpper();
                    if (snapshot.summary.isEmpty())
                        snapshot.summary = snapshot.body.left(kSummaryMaxChars);
                    if (snapshot.title.isEmpty())
                        snapshot.title = snapshot.summary.left(120);
                    if (!snapshot.title.isEmpty() || !snapshot.summary.isEmpty() || !snapshot.body.isEmpty()) {
                        finish_with_snapshot(snapshot);
                        return;
                    }
                }
                LOG_WARN("NewsService", "Python extractor returned empty article content, falling back to Qt fetch");
            } else {
                LOG_WARN("NewsService", "Python extractor failed, falling back to Qt fetch: " + result.error.left(160));
            }

            finish_with_snapshot(fetch_article_snapshot(self->nam_, trimmed_url));
        });
}

// ── Codex Headline Summarization ────────────────────────────────────────────

void NewsService::summarize_headlines(const QVector<NewsArticle>& articles, int count, SummaryCallback cb) {
    if (articles.isEmpty()) {
        cb(false, {});
        return;
    }

    // Build headline signature for cache check
    QStringList headlines;
    for (int i = 0; i < std::min(count, static_cast<int>(articles.size())); ++i)
        headlines.append(articles[i].headline);
    std::sort(headlines.begin(), headlines.end());
    QString sig = headlines.join("|").left(500);
    const QString sum_key = "news:summary:" + sig.left(200);

    {
        const QVariant cached = fincept::CacheManager::instance().get(sum_key);
        if (!cached.isNull()) {
            cb(true, cached.toString());
            return;
        }
    }

    if (!ai_chat::LlmService::instance().is_configured()) {
        LOG_WARN("NewsService", "Headline brief requires Codex OAuth or another configured LLM provider");
        cb(false, {});
        return;
    }

    const QString prompt = build_headline_brief_prompt(articles, count);
    QPointer<NewsService> self = this;
    auto future = QtConcurrent::run([self, cb, prompt, sum_key]() {
        const auto response = ai_chat::LlmService::instance().chat(prompt, {}, false);
        const QString summary = clean_llm_text(response.content).left(1800);
        const bool ok = response.success && !summary.isEmpty();
        const QString error = response.error;

        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, cb, ok, summary, sum_key, error]() {
                if (!self)
                    return;
                if (!ok) {
                    LOG_WARN("NewsService", "Codex headline brief failed: " + error.left(180));
                    cb(false, {});
                    return;
                }
                fincept::CacheManager::instance().put(sum_key, QVariant(summary), kSummaryCacheTtlSec, "news");
                cb(true, summary);
            },
            Qt::QueuedConnection);
    });
    Q_UNUSED(future);
}

// ── WebSocket live feed ──────────────────────────────────────────────────────

#ifdef HAS_QT_WEBSOCKETS
void NewsService::connect_live_feed(const QString& ws_url) {
    if (ws_url.trimmed().isEmpty()) {
        LOG_INFO("NewsService", "Live news feed disabled: no local default WebSocket configured");
        return;
    }

    if (live_ws_)
        return; // already connected

    live_ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);

    connect(live_ws_, &QWebSocket::connected, this, [this]() {
        live_connected_ = true;
        LOG_INFO("NewsService", "WebSocket live feed connected");
    });

    connect(live_ws_, &QWebSocket::disconnected, this, [this]() {
        live_connected_ = false;
        LOG_WARN("NewsService", "WebSocket live feed disconnected");
        // Auto-reconnect after delay
        QTimer::singleShot(kWsReconnectDelayMs, this, [this]() {
            if (live_ws_ && !live_connected_)
                live_ws_->open(live_ws_->requestUrl());
        });
    });

    connect(live_ws_, &QWebSocket::textMessageReceived, this, [this](const QString& msg) {
        // Parse incoming JSON article
        auto doc = QJsonDocument::fromJson(msg.toUtf8());
        if (!doc.isObject())
            return;

        auto obj = doc.object();
        NewsArticle article;
        article.id = obj["id"].toString();
        article.headline = obj["headline"].toString(obj["title"].toString());
        article.summary = obj["summary"].toString(obj["description"].toString());
        article.source = obj["source"].toString();
        article.link = obj["link"].toString(obj["url"].toString());
        article.category = obj["category"].toString("MARKETS");
        article.sort_ts = obj["timestamp"].toInteger(QDateTime::currentSecsSinceEpoch());
        article.time = QDateTime::fromSecsSinceEpoch(article.sort_ts).toString("MMM dd, HH:mm");
        article.tier = obj["tier"].toInt(2);

        if (article.headline.isEmpty())
            return;

        enrich_article(article);

        QVector<NewsArticle> updated;
        {
            const QVariant cv = fincept::CacheManager::instance().get(kNewsArticlesCacheKey);
            if (!cv.isNull()) {
                const QJsonArray existing = QJsonDocument::fromJson(cv.toString().toUtf8()).array();
                updated.reserve(existing.size() + 1);
                for (const auto& v : existing) {
                    const QJsonObject o = v.toObject();
                    NewsArticle a;
                    a.id = o["id"].toString();
                    a.time = o["time"].toString();
                    a.headline = o["headline"].toString();
                    a.summary = o["summary"].toString();
                    a.source = o["source"].toString();
                    a.region = o["region"].toString();
                    a.category = o["category"].toString();
                    a.link = o["link"].toString();
                    a.sort_ts = o["sort_ts"].toVariant().toLongLong();
                    a.tier = o["tier"].toInt(4);
                    a.priority = priority_from_string(o["priority"].toString());
                    a.sentiment = sentiment_from_string(o["sentiment"].toString());
                    a.impact = impact_from_string(o["impact"].toString());
                    a.lang = o["lang"].toString();
                    for (const auto& t : o["tickers"].toArray())
                        a.tickers << t.toString();
                    updated.append(a);
                }
            }
        }
        updated.prepend(article);
        updated = dedupe_articles(updated);

        QPointer<NewsService> self = this;
        const QString headline = article.headline;
        apply_finbert_sentiment(std::move(updated), [self, headline](QVector<NewsArticle> refined) {
            if (!self)
                return;
            cache_news_articles(refined);
            emit self->articles_partial(refined, 1, 1);
            self->publish_articles_to_hub(refined);
            LOG_INFO("NewsService", "Live article: " + headline.left(50));
        });
    });

    QString url = ws_url.trimmed();
    live_ws_->open(QUrl(url));
}

void NewsService::disconnect_live_feed() {
    if (!live_ws_)
        return;
    live_ws_->close();
    live_ws_->deleteLater();
    live_ws_ = nullptr;
    live_connected_ = false;
}

bool NewsService::is_live_connected() const {
    return live_connected_;
}
#else
// No WebSocket support — stubs
void NewsService::connect_live_feed(const QString&) {}
void NewsService::disconnect_live_feed() {}
bool NewsService::is_live_connected() const {
    return false;
}
#endif

// ── Auto-refresh ────────────────────────────────────────────────────────────

void NewsService::set_refresh_interval(int minutes) {
    refresh_timer_->setInterval(minutes * 60 * 1000);
}

void NewsService::start_auto_refresh() {
    refresh_timer_->start();
}
void NewsService::stop_auto_refresh() {
    refresh_timer_->stop();
}

// ── RSS XML parser ──────────────────────────────────────────────────────────

QVector<NewsArticle> NewsService::parse_rss_xml(const QByteArray& xml, const RSSFeed& feed) {
    QVector<NewsArticle> articles;
    QXmlStreamReader reader(xml);

    bool in_item = false;
    NewsArticle current;
    QString current_tag;
    int item_idx = 0;

    while (!reader.atEnd()) {
        auto token = reader.readNext();

        if (token == QXmlStreamReader::StartElement) {
            current_tag = reader.name().toString();

            if (current_tag == "item" || current_tag == "entry") {
                in_item = true;
                item_idx++;
                current = {};
                current.category = feed.category;
                current.source = feed.source;
                current.region = feed.region;
                current.tier = feed.tier;
                current.id = QString("%1-%2-%3").arg(feed.id).arg(QDateTime::currentMSecsSinceEpoch()).arg(item_idx);
            }

            // Atom <link href="..."/> or <link rel="alternate" href="..."/>
            if (in_item && current_tag == "link") {
                auto href = reader.attributes().value("href").toString();
                auto rel = reader.attributes().value("rel").toString();
                if (!href.isEmpty() && (rel.isEmpty() || rel == "alternate")) {
                    if (current.link.isEmpty())
                        current.link = href;
                }
            }
        } else if (token == QXmlStreamReader::Characters && in_item) {
            QString text = reader.text().toString().trimmed();
            if (text.isEmpty())
                continue;

            if (current_tag == "title" && current.headline.isEmpty()) {
                current.headline = text.left(200);
            } else if ((current_tag == "description" || current_tag == "summary" || current_tag == "encoded") &&
                       current.summary.isEmpty()) {
                current.summary = strip_html(text).left(kSummaryMaxChars);
            } else if (current_tag == "link" && current.link.isEmpty()) {
                current.link = text.trimmed();
            } else if ((current_tag == "guid" || current_tag == "id") && current.link.isEmpty()) {
                // guid/id often contains the article URL as fallback
                if (text.startsWith("http"))
                    current.link = text.trimmed();
            } else if (current_tag == "source" && feed.source == QStringLiteral("GOOGLE NEWS")) {
                current.source = text.left(80).toUpper();
            } else if (current_tag == "pubDate" || current_tag == "published" || current_tag == "updated" ||
                       current_tag == "date") {
                if (current.sort_ts == 0) {
                    QDateTime dt = QDateTime::fromString(text, Qt::RFC2822Date);
                    if (!dt.isValid())
                        dt = QDateTime::fromString(text, Qt::ISODate);
                    if (!dt.isValid())
                        dt = QDateTime::fromString(text, "ddd, dd MMM yyyy HH:mm:ss");
                    if (dt.isValid()) {
                        current.sort_ts = dt.toSecsSinceEpoch();
                        current.time = dt.toString("MMM dd, HH:mm");
                    } else {
                        current.time = text.left(22);
                    }
                }
            }
        } else if (token == QXmlStreamReader::EndElement) {
            QString tag = reader.name().toString();
            if ((tag == "item" || tag == "entry") && in_item) {
                in_item = false;
                if (current.headline.isEmpty())
                    continue;

                if (current.time.isEmpty())
                    current.time = QDateTime::currentDateTime().toString("MMM dd, HH:mm");
                if (current.sort_ts == 0)
                    current.sort_ts = QDateTime::currentSecsSinceEpoch();

                enrich_article(current);
                articles.append(std::move(current));
            }
        }
    }

    return articles;
}

// ── Strip HTML tags ─────────────────────────────────────────────────────────

QString NewsService::strip_html(const QString& html) {
    static QRegularExpression re("<[^>]*>");
    QString out = html;
    out.replace(re, "");
    return out.simplified();
}

// ── Enrich article: sentiment, priority, category, tickers ──────────────────

void NewsService::enrich_article(NewsArticle& article) {
    // Build once — reused for all keyword checks, ticker regex, and classify_threat
    const QString combined = article.headline + " " + article.summary;
    const QString text = combined.toLower();

    // Priority
    if (text.contains("breaking") || text.contains("alert"))
        article.priority = Priority::FLASH;
    else if (text.contains("urgent") || text.contains("emergency"))
        article.priority = Priority::URGENT;
    else if (text.contains("announce") || text.contains("report"))
        article.priority = Priority::BREAKING;

    // Weighted sentiment
    struct WordWeight {
        const char* word;
        int weight;
    };

    static const WordWeight positives[] = {
        {"surge", 3},       {"soar", 3},       {"skyrocket", 3}, {"breakthrough", 3}, {"boom", 3},
        {"record high", 3}, {"rally", 2},      {"gain", 2},      {"rise", 2},         {"jump", 2},
        {"climb", 2},       {"spike", 2},      {"rebound", 2},   {"boost", 2},        {"beat", 2},
        {"exceed", 2},      {"upgrade", 2},    {"profit", 2},    {"growth", 2},       {"expand", 2},
        {"recover", 2},     {"victory", 2},    {"ceasefire", 2}, {"treaty", 2},       {"reform", 2},
        {"optimism", 2},    {"milestone", 2},  {"strong", 1},    {"robust", 1},       {"stellar", 1},
        {"buy", 1},         {"positive", 1},   {"success", 1},   {"win", 1},          {"approval", 1},
        {"deal", 1},        {"confidence", 1}, {"dividend", 1},  {"progress", 1},     {"improve", 1},
        {"hope", 1},        {"support", 1},    {"bolster", 1},   {"outperform", 1},   {"bullish", 1},
        {"upside", 1},      {"favorable", 1},  {"momentum", 1},  {"launch", 1},       {"unveil", 1},
    };

    static const WordWeight negatives[] = {
        {"crash", 3},      {"plunge", 3},    {"collapse", 3},   {"devastat", 3},  {"catastroph", 3}, {"invasion", 3},
        {"war crime", 3},  {"nuclear", 3},   {"bankruptcy", 3}, {"meltdown", 3},  {"fall", 2},       {"drop", 2},
        {"decline", 2},    {"tumble", 2},    {"slide", 2},      {"slump", 2},     {"miss", 2},       {"disappoint", 2},
        {"fail", 2},       {"recession", 2}, {"crisis", 2},     {"conflict", 2},  {"attack", 2},     {"kill", 2},
        {"sanction", 2},   {"tariff", 2},    {"escalat", 2},    {"layoff", 2},    {"downgrade", 2},  {"default", 2},
        {"fraud", 2},      {"scandal", 2},   {"coup", 2},       {"protest", 2},   {"disaster", 2},   {"worst", 1},
        {"weak", 1},       {"loss", 1},      {"deficit", 1},    {"fear", 1},      {"risk", 1},       {"threat", 1},
        {"warning", 1},    {"sell", 1},      {"debt", 1},       {"inflation", 1}, {"slowdown", 1},   {"bearish", 1},
        {"negative", 1},   {"volatile", 1},  {"uncertain", 1},  {"reject", 1},    {"ban", 1},        {"suspend", 1},
        {"investigat", 1}, {"probe", 1},     {"hack", 1},       {"leak", 1},      {"shortage", 1},   {"disrupt", 1},
        {"shrink", 1},
    };

    int pos = 0, neg = 0;
    for (const auto& [w, wt] : positives) {
        if (text.contains(w))
            pos += wt;
    }
    for (const auto& [w, wt] : negatives) {
        if (text.contains(w))
            neg += wt;
    }

    int net = pos - neg;
    if (net >= 1)
        article.sentiment = Sentiment::BULLISH;
    else if (net <= -1)
        article.sentiment = Sentiment::BEARISH;
    // else stays NEUTRAL

    // Impact
    int strength = std::abs(net);
    if (article.priority == Priority::FLASH || article.priority == Priority::URGENT || strength >= 6)
        article.impact = Impact::HIGH;
    else if (article.priority == Priority::BREAKING || strength >= 3)
        article.impact = Impact::MEDIUM;

    // Category refinement
    if (text.contains("earnings") || text.contains("quarterly results") || text.contains("eps") ||
        text.contains("guidance"))
        article.category = "EARNINGS";
    else if (text.contains("crypto") || text.contains("bitcoin") || text.contains("ethereum") ||
             text.contains("blockchain"))
        article.category = "CRYPTO";
    else if (text.contains("missile") || text.contains("troops") || text.contains("pentagon") ||
             text.contains("military"))
        article.category = "DEFENSE";
    else if (text.contains("fed ") || text.contains("federal reserve") || text.contains("inflation") ||
             text.contains("gdp") || text.contains("interest rate") || text.contains("central bank"))
        article.category = "ECONOMIC";
    else if (text.contains("s&p 500") || text.contains("nasdaq") || text.contains("dow jones") ||
             text.contains("stock market"))
        article.category = "MARKETS";
    else if (text.contains("energy") || text.contains("crude") || text.contains("opec") ||
             text.contains("natural gas") || text.contains("oil price"))
        article.category = "ENERGY";
    else if (text.contains("tech") || text.contains(" ai ") || text.contains("artificial intelligence") ||
             text.contains("semiconductor") || text.contains("startup"))
        article.category = "TECH";
    else if (text.contains("nato") || text.contains("ukraine") || text.contains("russia") || text.contains("china") ||
             text.contains("gaza") || text.contains("sanctions") || text.contains("geopolit"))
        article.category = "GEOPOLITICS";

    // Extract tickers: uppercase 2-5 letter words
    static QRegularExpression ticker_re("\\b[A-Z]{2,5}\\b");
    static QSet<QString> common_words = {"THE",  "FOR",  "AND",  "BUT",  "NOT",  "FROM", "WITH", "THIS", "THAT", "HAVE",
                                         "WILL", "BEEN", "THEY", "WERE", "SAID", "HAS",  "ITS",  "NEW",  "ARE",  "WAS"};
    auto it = ticker_re.globalMatch(combined); // reuse already-built string
    QSet<QString> found;
    while (it.hasNext() && found.size() < 5) {
        auto m = it.next();
        QString t = m.captured();
        if (!common_words.contains(t))
            found.insert(t);
    }
    article.tickers = found.values();

    // Language detection — check for CJK, Cyrillic, Arabic, Devanagari characters
    auto detect_lang = [](const QString& s) -> QString {
        int cjk = 0, cyrillic = 0, arabic = 0, devanagari = 0, latin = 0;
        for (const auto& ch : s) {
            ushort u = ch.unicode();
            if (u >= 0x4e00 && u <= 0x9fff)
                cjk++;
            else if (u >= 0x3040 && u <= 0x30ff)
                return "ja"; // kana = definitely Japanese
            else if (u >= 0xac00 && u <= 0xd7af)
                return "ko"; // hangul = Korean
            else if (u >= 0x0400 && u <= 0x04ff)
                cyrillic++;
            else if (u >= 0x0600 && u <= 0x06ff)
                arabic++;
            else if (u >= 0x0900 && u <= 0x097f)
                devanagari++;
            else if ((u >= 0x41 && u <= 0x5a) || (u >= 0x61 && u <= 0x7a))
                latin++;
        }
        int total = s.size();
        if (total == 0)
            return "en";
        if (cjk * 10 > total)
            return "zh";
        if (cyrillic * 10 > total)
            return "ru";
        if (arabic * 10 > total)
            return "ar";
        if (devanagari * 10 > total)
            return "hi";
        return "en";
    };
    article.lang = detect_lang(article.headline);

    // Threat classification — pass pre-built text to avoid a 3rd toLower()
    article.threat = classify_threat(article, text);

    // Source credibility flag
    article.source_flag = source_flag_for(article.source);
}

// ── Threat classification with confidence ───────────────────────────────────

ThreatClassification NewsService::classify_threat(const NewsArticle& article) {
    // Convenience overload — builds text itself (used only outside enrich_article)
    return classify_threat(article, (article.headline + " " + article.summary).toLower());
}

ThreatClassification NewsService::classify_threat(const NewsArticle& article, const QString& text) {
    ThreatClassification tc;
    tc.category = "general";
    tc.confidence = 0.3; // base confidence from keyword matching

    // Critical — immediate, high-impact events
    struct PatternScore {
        const char* pattern;
        const char* category;
        ThreatLevel level;
        double conf;
    };
    static const PatternScore critical_patterns[] = {
        {"nuclear strike", "conflict", ThreatLevel::CRITICAL, 0.95},
        {"nuclear attack", "conflict", ThreatLevel::CRITICAL, 0.95},
        {"war declared", "conflict", ThreatLevel::CRITICAL, 0.95},
        {"market crash", "market", ThreatLevel::CRITICAL, 0.9},
        {"flash crash", "market", ThreatLevel::CRITICAL, 0.9},
        {"circuit breaker", "market", ThreatLevel::CRITICAL, 0.85},
        {"trading halt", "market", ThreatLevel::CRITICAL, 0.85},
        {"bank run", "market", ThreatLevel::CRITICAL, 0.9},
        {"sovereign default", "market", ThreatLevel::CRITICAL, 0.9},
        {"cyberattack", "cyber", ThreatLevel::HIGH, 0.8},
        {"data breach", "cyber", ThreatLevel::HIGH, 0.75},
        {"ransomware", "cyber", ThreatLevel::HIGH, 0.8},
    };

    // High — significant events
    static const PatternScore high_patterns[] = {
        {"invasion", "conflict", ThreatLevel::HIGH, 0.85},
        {"airstrike", "conflict", ThreatLevel::HIGH, 0.85},
        {"missile launch", "conflict", ThreatLevel::HIGH, 0.85},
        {"military deploy", "conflict", ThreatLevel::HIGH, 0.8},
        {"coup attempt", "conflict", ThreatLevel::HIGH, 0.85},
        {"martial law", "conflict", ThreatLevel::HIGH, 0.85},
        {"bankruptcy fil", "market", ThreatLevel::HIGH, 0.8},
        {"rate hike", "market", ThreatLevel::HIGH, 0.7},
        {"rate cut", "market", ThreatLevel::HIGH, 0.7},
        {"earnings miss", "market", ThreatLevel::HIGH, 0.75},
        {"profit warning", "market", ThreatLevel::HIGH, 0.75},
        {"downgrad", "market", ThreatLevel::HIGH, 0.7},
        {"sanction", "regulatory", ThreatLevel::HIGH, 0.7},
        {"embargo", "regulatory", ThreatLevel::HIGH, 0.75},
        {"earthquake", "natural", ThreatLevel::HIGH, 0.8},
        {"tsunami", "natural", ThreatLevel::HIGH, 0.85},
        {"hurricane", "natural", ThreatLevel::HIGH, 0.75},
        {"pandemic", "natural", ThreatLevel::HIGH, 0.8},
    };

    // Medium patterns
    static const PatternScore medium_patterns[] = {
        {"protest", "conflict", ThreatLevel::MEDIUM, 0.6},     {"riot", "conflict", ThreatLevel::MEDIUM, 0.7},
        {"tension", "conflict", ThreatLevel::MEDIUM, 0.5},     {"escalat", "conflict", ThreatLevel::MEDIUM, 0.65},
        {"tariff", "regulatory", ThreatLevel::MEDIUM, 0.65},   {"regulation", "regulatory", ThreatLevel::MEDIUM, 0.5},
        {"antitrust", "regulatory", ThreatLevel::MEDIUM, 0.6}, {"investigat", "regulatory", ThreatLevel::MEDIUM, 0.55},
        {"layoff", "market", ThreatLevel::MEDIUM, 0.6},        {"recession", "market", ThreatLevel::MEDIUM, 0.65},
        {"inflation", "market", ThreatLevel::MEDIUM, 0.55},    {"selloff", "market", ThreatLevel::MEDIUM, 0.6},
        {"sell-off", "market", ThreatLevel::MEDIUM, 0.6},      {"volatil", "market", ThreatLevel::MEDIUM, 0.5},
        {"wildfire", "natural", ThreatLevel::MEDIUM, 0.6},     {"flood", "natural", ThreatLevel::MEDIUM, 0.6},
    };

    // Check patterns in priority order — first critical, then high, then medium
    for (const auto& p : critical_patterns) {
        if (text.contains(p.pattern)) {
            tc.level = p.level;
            tc.category = p.category;
            tc.confidence = p.conf;
            return tc;
        }
    }
    for (const auto& p : high_patterns) {
        if (text.contains(p.pattern)) {
            tc.level = p.level;
            tc.category = p.category;
            tc.confidence = p.conf;
            return tc;
        }
    }
    for (const auto& p : medium_patterns) {
        if (text.contains(p.pattern)) {
            tc.level = p.level;
            tc.category = p.category;
            tc.confidence = p.conf;
            return tc;
        }
    }

    // Low: any negative sentiment article
    if (article.sentiment == Sentiment::BEARISH) {
        tc.level = ThreatLevel::LOW;
        tc.confidence = 0.4;
    }

    return tc;
}

// ── Source credibility ──────────────────────────────────────────────────────

SourceFlag NewsService::source_flag_for(const QString& source) {
    static const QMap<QString, SourceFlag> flags = {
        // State media
        {"XINHUA", SourceFlag::STATE_MEDIA},
        {"CGTN", SourceFlag::STATE_MEDIA},
        {"GLOBAL TIMES", SourceFlag::STATE_MEDIA},
        {"RT", SourceFlag::STATE_MEDIA},
        {"TASS", SourceFlag::STATE_MEDIA},
        {"SPUTNIK", SourceFlag::STATE_MEDIA},
        {"PRESS TV", SourceFlag::STATE_MEDIA},
        {"KCNA", SourceFlag::STATE_MEDIA},
        {"TRT WORLD", SourceFlag::STATE_MEDIA},
        {"AL ARABIYA", SourceFlag::STATE_MEDIA},
        // Caution — sensationalism or low editorial standards
        {"ZEROHEDGE", SourceFlag::CAUTION},
        {"INFOWARS", SourceFlag::CAUTION},
        {"DAILY MAIL", SourceFlag::CAUTION},
        {"NY POST", SourceFlag::CAUTION},
    };
    auto it = flags.find(source.toUpper());
    return it != flags.end() ? it.value() : SourceFlag::NONE;
}

QString NewsService::source_flag_label(SourceFlag flag) {
    switch (flag) {
        case SourceFlag::STATE_MEDIA:
            return "STATE MEDIA";
        case SourceFlag::CAUTION:
            return "CAUTION";
        default:
            return {};
    }
}

// ── Default RSS feeds ──────────────────────────────────────────────────────

QVector<RSSFeed> NewsService::default_feeds() {
    return {
        // Free fallback feeds — Google News RSS stays useful when vendor RSS
        // endpoints move, rate-limit, or block desktop clients.
        {"google-market", "Google Market News",
         "https://news.google.com/rss/search?q=stock%20market%20OR%20S%26P%20500%20OR%20Nasdaq&hl=en-US&gl=US&ceid=US:en",
         "MARKETS", "GLOBAL", "GOOGLE NEWS", 2},
        {"google-business", "Google Business News",
         "https://news.google.com/rss/search?q=business%20OR%20earnings%20OR%20Federal%20Reserve&hl=en-US&gl=US&ceid=US:en",
         "MARKETS", "GLOBAL", "GOOGLE NEWS", 2},
        {"google-crypto", "Google Crypto News",
         "https://news.google.com/rss/search?q=bitcoin%20OR%20ethereum%20OR%20crypto%20market&hl=en-US&gl=US&ceid=US:en",
         "CRYPTO", "GLOBAL", "GOOGLE NEWS", 2},

        // Tier 1 — Wire Services & Regulators
        {"reuters-world", "Reuters World", "https://feeds.reuters.com/Reuters/worldNews", "GEOPOLITICS", "GLOBAL",
         "REUTERS", 1},
        {"reuters-biz", "Reuters Business", "https://feeds.reuters.com/reuters/businessNews", "MARKETS", "GLOBAL",
         "REUTERS", 1},
        {"reuters-mkts", "Reuters Markets", "https://feeds.reuters.com/reuters/financialsNews", "MARKETS", "GLOBAL",
         "REUTERS", 1},
        {"ap-top", "AP Top News", "https://rsshub.app/apnews/topics/ap-top-news", "GEOPOLITICS", "GLOBAL", "AP", 1},
        {"sec-press", "SEC Press Releases", "https://www.sec.gov/news/pressreleases.rss", "REGULATORY", "US", "SEC", 1},
        {"fed-press", "Federal Reserve", "https://www.federalreserve.gov/feeds/press_all.xml", "REGULATORY", "US",
         "FEDERAL RESERVE", 1},
        {"un-news", "UN News", "https://news.un.org/feed/subscribe/en/news/all/rss.xml", "GEOPOLITICS", "GLOBAL", "UN",
         1},
        {"imf-news", "IMF News", "https://www.imf.org/en/News/rss?language=eng", "ECONOMIC", "GLOBAL", "IMF", 1},

        // Tier 2 — Major Financial Media
        {"bloomberg-mkts", "Bloomberg Markets", "https://feeds.bloomberg.com/markets/news.rss", "MARKETS", "GLOBAL",
         "BLOOMBERG", 2},
        {"wsj-markets", "WSJ Markets", "https://feeds.a.dj.com/rss/RSSMarketsMain.xml", "MARKETS", "US", "WSJ", 2},
        {"wsj-world", "WSJ World", "https://feeds.a.dj.com/rss/RSSWorldNews.xml", "GEOPOLITICS", "GLOBAL", "WSJ", 2},
        {"marketwatch", "MarketWatch", "https://feeds.marketwatch.com/marketwatch/topstories/", "MARKETS", "US",
         "MARKETWATCH", 2},
        {"cnbc-finance", "CNBC Finance",
         "https://search.cnbc.com/rs/search/combinedcms/view.xml?partnerId=wrss01&id=100003114", "MARKETS", "US",
         "CNBC", 2},
        {"seekingalpha", "Seeking Alpha", "https://seekingalpha.com/market_currents.xml", "MARKETS", "US",
         "SEEKING ALPHA", 2},

        // Tier 2 — Global News
        {"bbc-world", "BBC World", "http://feeds.bbci.co.uk/news/world/rss.xml", "GEOPOLITICS", "GLOBAL", "BBC", 2},
        {"bbc-business", "BBC Business", "http://feeds.bbci.co.uk/news/business/rss.xml", "MARKETS", "GLOBAL", "BBC",
         2},
        {"aljazeera", "Al Jazeera", "https://www.aljazeera.com/xml/rss/all.xml", "GEOPOLITICS", "GLOBAL", "AL JAZEERA",
         2},
        {"nyt-world", "NYT World", "https://rss.nytimes.com/services/xml/rss/nyt/World.xml", "GEOPOLITICS", "GLOBAL",
         "NYT", 2},
        {"guardian-world", "Guardian World", "https://www.theguardian.com/world/rss", "GEOPOLITICS", "GLOBAL",
         "GUARDIAN", 2},
        {"france24", "France 24", "https://www.france24.com/en/rss", "GEOPOLITICS", "EU", "FRANCE 24", 2},

        // Tier 2 — Geopolitics & Defense
        {"foreignpolicy", "Foreign Policy", "https://foreignpolicy.com/feed/", "GEOPOLITICS", "GLOBAL",
         "FOREIGN POLICY", 2},
        {"defensenews", "Defense News", "https://www.defensenews.com/rss/", "GEOPOLITICS", "GLOBAL", "DEFENSE NEWS", 2},

        // Tier 2 — Energy & Commodities
        {"oilprice", "OilPrice.com", "https://oilprice.com/rss/main", "ENERGY", "GLOBAL", "OILPRICE", 2},
        {"kitco", "Kitco Gold", "https://www.kitco.com/rss/news/", "MARKETS", "GLOBAL", "KITCO", 2},

        // Tier 2 — Tech
        {"techcrunch", "TechCrunch", "https://techcrunch.com/feed/", "TECH", "GLOBAL", "TECHCRUNCH", 2},
        {"wired", "Wired", "https://www.wired.com/feed/rss", "TECH", "US", "WIRED", 2},

        // Tier 2 — Forex
        {"fxstreet", "FXStreet", "https://www.fxstreet.com/rss/news", "MARKETS", "GLOBAL", "FXSTREET", 2},

        // Tier 2 — Asia
        {"scmp", "South China Morning Post", "https://www.scmp.com/rss/91/feed", "GEOPOLITICS", "ASIA", "SCMP", 2},
        {"nikkei-asia", "Nikkei Asia", "https://asia.nikkei.com/rss/feed/nar", "MARKETS", "ASIA", "NIKKEI ASIA", 2},
        {"hindu-biz", "The Hindu Business", "https://www.thehindu.com/business/feeder/default.rss", "MARKETS", "INDIA",
         "THE HINDU", 2},

        // Tier 2 — MENA
        {"middle-east-eye", "Middle East Eye", "https://www.middleeasteye.net/rss", "GEOPOLITICS", "MENA",
         "MIDDLE EAST EYE", 2},

        // ── Additional feeds (29→80+) ──────────────────────────────────────

        // Tier 1 — Wire (additional)
        {"reuters-tech", "Reuters Technology", "https://feeds.reuters.com/reuters/technologyNews", "TECH", "GLOBAL",
         "REUTERS", 1},

        // Tier 2 — Major Financial (additional)
        {"cnbc-world", "CNBC World",
         "https://search.cnbc.com/rs/search/combinedcms/view.xml?partnerId=wrss01&id=100727362", "MARKETS", "GLOBAL",
         "CNBC", 2},
        {"cnbc-tech", "CNBC Technology",
         "https://search.cnbc.com/rs/search/combinedcms/view.xml?partnerId=wrss01&id=19854910", "TECH", "US", "CNBC",
         2},
        {"investing-news", "Investing.com", "https://www.investing.com/rss/news.rss", "MARKETS", "GLOBAL",
         "INVESTING.COM", 2},
        {"economist", "The Economist", "https://www.economist.com/finance-and-economics/rss.xml", "ECONOMIC", "GLOBAL",
         "ECONOMIST", 2},

        // Tier 2 — Crypto
        {"coindesk", "CoinDesk", "https://www.coindesk.com/arc/outboundfeeds/rss/", "CRYPTO", "GLOBAL", "COINDESK", 2},
        {"cointelegraph", "CoinTelegraph", "https://cointelegraph.com/rss", "CRYPTO", "GLOBAL", "COINTELEGRAPH", 2},
        {"theblock", "The Block", "https://www.theblock.co/rss.xml", "CRYPTO", "GLOBAL", "THE BLOCK", 2},
        {"decrypt", "Decrypt", "https://decrypt.co/feed", "CRYPTO", "GLOBAL", "DECRYPT", 2},

        // Tier 1 — Central Banks & Regulators
        {"ecb-press", "ECB Press", "https://www.ecb.europa.eu/rss/press.html", "REGULATORY", "EU", "ECB", 1},
        {"boe-news", "Bank of England", "https://www.bankofengland.co.uk/rss/news", "REGULATORY", "UK", "BOE", 1},

        // Tier 2 — Commodities (additional)
        {"mining-com", "Mining.com", "https://www.mining.com/feed/", "MARKETS", "GLOBAL", "MINING.COM", 2},

        // Tier 2 — US Markets (additional)
        {"benzinga", "Benzinga", "https://www.benzinga.com/feed", "MARKETS", "US", "BENZINGA", 2},

        // Tier 2 — Europe
        {"dw-world", "Deutsche Welle", "https://rss.dw.com/rdf/rss-en-all", "GEOPOLITICS", "EU", "DW", 2},

        // Tier 2 — Asia & India (additional)
        {"livemint", "LiveMint", "https://www.livemint.com/rss/markets", "MARKETS", "INDIA", "LIVEMINT", 2},
        {"et-markets", "Economic Times", "https://economictimes.indiatimes.com/rssfeedstopstories.cms", "MARKETS",
         "INDIA", "ECONOMIC TIMES", 2},
        {"moneycontrol", "MoneyControl", "https://www.moneycontrol.com/rss/latestnews.xml", "MARKETS", "INDIA",
         "MONEYCONTROL", 2},
        {"channel-news-asia", "CNA", "https://www.channelnewsasia.com/rssfeeds/8395986", "MARKETS", "ASIA", "CNA", 2},

        // Tier 2 — Fintech
        {"finextra", "Finextra", "https://www.finextra.com/rss/headlines.aspx", "TECH", "GLOBAL", "FINEXTRA", 2},

        // Tier 3 — Economic / Macro
        {"zero-hedge", "ZeroHedge", "https://feeds.feedburner.com/zerohedge/feed", "ECONOMIC", "GLOBAL", "ZEROHEDGE",
         3},
        {"calculated-risk", "Calculated Risk", "https://feeds.feedburner.com/CalculatedRisk", "ECONOMIC", "US",
         "CALCULATED RISK", 3},
        {"wolfstreet", "Wolf Street", "https://wolfstreet.com/feed/", "ECONOMIC", "US", "WOLF STREET", 3},

        // Tier 3 — Defense & Security
        {"defense-one", "Defense One", "https://www.defenseone.com/rss/", "DEFENSE", "US", "DEFENSE ONE", 3},
        {"bellingcat", "Bellingcat", "https://www.bellingcat.com/feed/", "GEOPOLITICS", "GLOBAL", "BELLINGCAT", 3},

        // Tier 3 — Tech (additional)
        {"arstechnica", "Ars Technica", "https://feeds.arstechnica.com/arstechnica/index", "TECH", "GLOBAL",
         "ARS TECHNICA", 3},
        {"theverge", "The Verge", "https://www.theverge.com/rss/index.xml", "TECH", "GLOBAL", "THE VERGE", 3},
        {"mit-tech", "MIT Tech Review", "https://www.technologyreview.com/feed/", "TECH", "GLOBAL", "MIT TECH REVIEW",
         3},

        // Tier 3 — ESG
        {"carbon-brief", "Carbon Brief", "https://www.carbonbrief.org/feed/", "ENERGY", "GLOBAL", "CARBON BRIEF", 3},

        // Tier 4 — Blogs & Aggregators
        {"hackernews", "Hacker News", "https://hnrss.org/frontpage", "TECH", "GLOBAL", "HACKER NEWS", 4},
        {"abnormal-returns", "Abnormal Returns", "https://abnormalreturns.com/feed/", "MARKETS", "US",
         "ABNORMAL RETURNS", 4},
        {"marginal-rev", "Marginal Revolution", "https://marginalrevolution.com/feed", "ECONOMIC", "GLOBAL",
         "MARGINAL REVOLUTION", 4},
    };
}

// ── Free helpers ────────────────────────────────────────────────────────────

QString priority_string(Priority p) {
    switch (p) {
        case Priority::FLASH:
            return "FLASH";
        case Priority::URGENT:
            return "URGENT";
        case Priority::BREAKING:
            return "BREAKING";
        case Priority::ROUTINE:
            return "ROUTINE";
    }
    return "ROUTINE";
}

QString sentiment_string(Sentiment s) {
    switch (s) {
        case Sentiment::BULLISH:
            return "BULLISH";
        case Sentiment::BEARISH:
            return "BEARISH";
        case Sentiment::NEUTRAL:
            return "NEUTRAL";
    }
    return "NEUTRAL";
}

QString impact_string(Impact i) {
    switch (i) {
        case Impact::HIGH:
            return "HIGH";
        case Impact::MEDIUM:
            return "MEDIUM";
        case Impact::LOW:
            return "LOW";
    }
    return "LOW";
}

QString priority_color(Priority p) {
    switch (p) {
        case Priority::FLASH:
            return "#dc2626";
        case Priority::URGENT:
            return "#d97706";
        case Priority::BREAKING:
            return "#ca8a04";
        case Priority::ROUTINE:
            return "#525252";
    }
    return "#525252";
}

QString sentiment_color(Sentiment s) {
    switch (s) {
        case Sentiment::BULLISH:
            return "#16a34a";
        case Sentiment::BEARISH:
            return "#dc2626";
        case Sentiment::NEUTRAL:
            return "#ca8a04";
    }
    return "#ca8a04";
}

QString relative_time(int64_t unix_ts) {
    if (unix_ts <= 0)
        return {};
    auto now = QDateTime::currentSecsSinceEpoch();
    auto d = now - unix_ts;
    if (d < 0)
        return "now";
    if (d < 60)
        return QString("%1s").arg(d);
    if (d < 3600)
        return QString("%1m").arg(d / 60);
    if (d < 86400)
        return QString("%1h").arg(d / 3600);
    return QString("%1d").arg(d / 86400);
}

QString threat_level_string(ThreatLevel t) {
    switch (t) {
        case ThreatLevel::CRITICAL:
            return "CRITICAL";
        case ThreatLevel::HIGH:
            return "HIGH";
        case ThreatLevel::MEDIUM:
            return "MEDIUM";
        case ThreatLevel::LOW:
            return "LOW";
        case ThreatLevel::INFO:
            return "INFO";
    }
    return "INFO";
}

QString threat_level_color(ThreatLevel t) {
    switch (t) {
        case ThreatLevel::CRITICAL:
            return "#dc2626";
        case ThreatLevel::HIGH:
            return "#f97316";
        case ThreatLevel::MEDIUM:
            return "#eab308";
        case ThreatLevel::LOW:
            return "#22c55e";
        case ThreatLevel::INFO:
            return "#525252";
    }
    return "#525252";
}

Priority priority_from_string(const QString& s) {
    if (s == "FLASH")
        return Priority::FLASH;
    if (s == "URGENT")
        return Priority::URGENT;
    if (s == "BREAKING")
        return Priority::BREAKING;
    return Priority::ROUTINE;
}

Sentiment sentiment_from_string(const QString& s) {
    if (s == "BULLISH")
        return Sentiment::BULLISH;
    if (s == "BEARISH")
        return Sentiment::BEARISH;
    return Sentiment::NEUTRAL;
}

Impact impact_from_string(const QString& s) {
    if (s == "HIGH")
        return Impact::HIGH;
    if (s == "MEDIUM")
        return Impact::MEDIUM;
    return Impact::LOW;
}

// ── DataHub producer wiring ─────────────────────────────────────────────────

QStringList NewsService::topic_patterns() const {
    return {QStringLiteral("news:general"),
            QStringLiteral("news:symbol:*"),
            QStringLiteral("news:category:*"),
            QStringLiteral("news:cluster:*")};
}

void NewsService::refresh(const QStringList& topics) {
    // Cluster topics are push-only — producer never pulls them.
    bool needs_general = false;
    for (const auto& t : topics) {
        if (t == QLatin1String("news:general") ||
            t.startsWith(QLatin1String("news:symbol:")) ||
            t.startsWith(QLatin1String("news:category:"))) {
            needs_general = true;
            break;
        }
    }
    if (!needs_general) return;

    // All non-cluster topics derive from the general feed; one fetch
    // fans out via publish_articles_to_hub.
    fetch_all_news_progressive(/*force=*/true, [](bool, QVector<NewsArticle>) {});
}

int NewsService::max_requests_per_sec() const {
    return 2;  // RSS aggregator pacing — generous but avoids request storms
}

void NewsService::ensure_registered_with_hub() {
    if (hub_registered_) return;
    auto& hub = fincept::datahub::DataHub::instance();
    hub.register_producer(this);

    // General feed — cache 5m, min refresh interval 30s, coalesce
    // progressive chunks to 250ms so cold-cache fills don't repaint in
    // a tight loop. `coalesce_within_ms` field arrived in Phase 4.
    fincept::datahub::TopicPolicy general;
    general.ttl_ms = 5 * 60 * 1000;
    general.min_interval_ms = 30 * 1000;
    general.coalesce_within_ms = 250;
    general.push_only = false;
    hub.set_policy_pattern(QStringLiteral("news:general"), general);

    // Per-symbol / per-category slices share the same TTL; they derive
    // from the same fetch so min_interval keeps producer pacing sane.
    fincept::datahub::TopicPolicy derived = general;
    hub.set_policy_pattern(QStringLiteral("news:symbol:*"), derived);
    hub.set_policy_pattern(QStringLiteral("news:category:*"), derived);

    // Server-assigned clusters — push-only, no scheduled refresh.
    fincept::datahub::TopicPolicy cluster_policy;
    cluster_policy.push_only = true;
    cluster_policy.ttl_ms = 0;
    cluster_policy.min_interval_ms = 0;
    hub.set_policy_pattern(QStringLiteral("news:cluster:*"), cluster_policy);

    hub_registered_ = true;
    LOG_INFO("NewsService",
             "Registered with DataHub (news:general, news:symbol:*, "
             "news:category:*, news:cluster:*)");
}

void NewsService::publish_articles_to_hub(const QVector<NewsArticle>& accumulated) {
    if (!hub_registered_) return;
    auto& hub = fincept::datahub::DataHub::instance();

    // Single canonical publish — the whole accumulated list on news:general.
    hub.publish(QStringLiteral("news:general"), QVariant::fromValue(accumulated));

    // Fan out per-symbol and per-category slices, but only for topics
    // that currently have subscribers — the hub is the authority on
    // who's listening. For now publish unconditionally; the hub's
    // push_only policy on symbol/category patterns caches last-known-
    // good even with no live subscribers, so future mounts get the
    // snapshot via peek(). This is cheap: lists are small and the
    // string splits are linear in article count.
    QHash<QString, QVector<NewsArticle>> by_symbol;
    QHash<QString, QVector<NewsArticle>> by_category;
    for (const auto& a : accumulated) {
        for (const auto& sym : a.tickers) {
            if (!sym.isEmpty())
                by_symbol[sym].append(a);
        }
        if (!a.category.isEmpty())
            by_category[a.category].append(a);
    }
    for (auto it = by_symbol.constBegin(); it != by_symbol.constEnd(); ++it) {
        hub.publish(QStringLiteral("news:symbol:") + it.key(),
                    QVariant::fromValue(it.value()));
    }
    for (auto it = by_category.constBegin(); it != by_category.constEnd(); ++it) {
        hub.publish(QStringLiteral("news:category:") + it.key(),
                    QVariant::fromValue(it.value()));
    }
}

} // namespace fincept::services
