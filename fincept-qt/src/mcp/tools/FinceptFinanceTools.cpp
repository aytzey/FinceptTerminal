// FinceptFinanceTools.cpp — Local finance/search/macro/news MCP tools

#include "mcp/tools/FinceptFinanceTools.h"

#include "python/PythonRunner.h"
#include "services/geopolitics/GeopoliticsService.h"
#include "services/news/NewsService.h"

#include <QDate>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>

namespace fincept::mcp::tools {
namespace {

static constexpr int kAsyncTimeoutMs = 90000;

ToolResult run_script_sync(const QString& script, const QStringList& args) {
    if (!python::PythonRunner::instance().is_available())
        return ToolResult::fail("Python is not available");

    ToolResult out = ToolResult::fail("Python script did not return");
    bool done = false;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        out = ToolResult::fail("Python script timed out");
        done = true;
        loop.quit();
    });

    python::PythonRunner::instance().run(script, args, [&](python::PythonResult result) {
        if (done)
            return;

        if (!result.success) {
            out = ToolResult::fail("Script failed: " + result.error);
        } else {
            QString json_text = python::extract_json(result.output).trimmed();
            if (json_text.isEmpty())
                json_text = result.output.trimmed();

            QJsonParseError parse_error;
            QJsonDocument doc = QJsonDocument::fromJson(json_text.toUtf8(), &parse_error);
            if (parse_error.error == QJsonParseError::NoError && !doc.isNull()) {
                if (doc.isObject())
                    out = ToolResult::ok_data(doc.object());
                else if (doc.isArray())
                    out = ToolResult::ok_data(doc.array());
                else
                    out = ToolResult::ok(result.output);
            } else {
                out = ToolResult::ok(result.output);
            }
        }
        done = true;
        loop.quit();
    });

    timer.start(kAsyncTimeoutMs);
    if (!done)
        loop.exec();
    timer.stop();
    return out;
}

QJsonArray extract_events_array(const QJsonValue& value) {
    if (value.isArray())
        return value.toArray();
    if (!value.isObject())
        return {};

    const auto obj = value.toObject();
    if (obj.contains("data")) {
        const auto data = obj.value("data");
        if (data.isArray())
            return data.toArray();
        if (data.isObject()) {
            const auto data_obj = data.toObject();
            if (data_obj.contains("events") && data_obj.value("events").isArray())
                return data_obj.value("events").toArray();
        }
    }
    if (obj.contains("events") && obj.value("events").isArray())
        return obj.value("events").toArray();
    if (obj.contains("result") && obj.value("result").isArray())
        return obj.value("result").toArray();
    return {};
}

QJsonObject signal_to_json(const services::RiskSignal& signal) {
    return QJsonObject{{"level", signal.level}, {"details", signal.details}};
}

QJsonObject analysis_to_json(const services::NewsAnalysis& analysis) {
    QJsonArray keywords;
    for (const auto& keyword : analysis.keywords)
        keywords.append(keyword);
    QJsonArray topics;
    for (const auto& topic : analysis.topics)
        topics.append(topic);
    QJsonArray key_points;
    for (const auto& point : analysis.key_points)
        key_points.append(point);

    return QJsonObject{
        {"sentiment",
         QJsonObject{{"score", analysis.sentiment.score},
                     {"intensity", analysis.sentiment.intensity},
                     {"confidence", analysis.sentiment.confidence}}},
        {"market_impact",
         QJsonObject{{"urgency", analysis.market_impact.urgency}, {"prediction", analysis.market_impact.prediction}}},
        {"keywords", keywords},
        {"topics", topics},
        {"key_points", key_points},
        {"summary", analysis.summary},
        {"risk_signals",
         QJsonObject{{"regulatory", signal_to_json(analysis.regulatory)},
                     {"geopolitical", signal_to_json(analysis.geopolitical)},
                     {"operational", signal_to_json(analysis.operational)},
                     {"market", signal_to_json(analysis.market)}}},
        {"credits_used", analysis.credits_used},
        {"credits_remaining", analysis.credits_remaining},
    };
}

QJsonObject model_result_object(const ToolResult& result) {
    const QJsonObject obj = result.data.toObject();
    if (obj.contains("data") && obj.value("data").isObject())
        return obj.value("data").toObject();
    return obj;
}

ToolResult run_news_analysis_sync(const QString& url) {
    ToolResult out = ToolResult::fail("Article analysis did not complete");
    bool done = false;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        out = ToolResult::fail("Article analysis timed out");
        done = true;
        loop.quit();
    });

    services::NewsService::instance().analyze_article(url, [&](bool ok, services::NewsAnalysis analysis) {
        if (!ok)
            out = ToolResult::fail("Local article analysis failed");
        else
            out = ToolResult::ok_data(analysis_to_json(analysis));
        done = true;
        loop.quit();
    });

    timer.start(kAsyncTimeoutMs);
    if (!done)
        loop.exec();
    timer.stop();
    return out;
}

ToolResult fetch_news_sync() {
    ToolResult out = ToolResult::fail("News fetch did not complete");
    bool done = false;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        out = ToolResult::fail("News fetch timed out");
        done = true;
        loop.quit();
    });

    services::NewsService::instance().fetch_all_news(false, [&](bool ok, QVector<services::NewsArticle> articles) {
        if (!ok) {
            out = ToolResult::fail("Unable to fetch local news");
        } else {
            QJsonArray arr;
            for (const auto& article : articles) {
                QJsonObject obj;
                obj["id"] = article.id;
                obj["headline"] = article.headline;
                obj["summary"] = article.summary;
                obj["source"] = article.source;
                obj["region"] = article.region;
                obj["category"] = article.category;
                obj["link"] = article.link;
                obj["time"] = article.time;
                obj["sort_ts"] = static_cast<qint64>(article.sort_ts);
                obj["priority"] = services::priority_string(article.priority);
                obj["sentiment"] = services::sentiment_string(article.sentiment);
                obj["impact"] = services::impact_string(article.impact);
                QJsonArray tickers;
                for (const auto& ticker : article.tickers)
                    tickers.append(ticker);
                obj["tickers"] = tickers;
                arr.append(obj);
            }
            out = ToolResult::ok_data(arr);
        }
        done = true;
        loop.quit();
    });

    timer.start(kAsyncTimeoutMs);
    if (!done)
        loop.exec();
    timer.stop();
    return out;
}

ToolResult fetch_geopolitics_sync(const QString& country, const QString& city, const QString& category, int limit) {
    ToolResult out = ToolResult::fail("Geopolitical event fetch did not complete");
    bool done = false;
    QEventLoop loop;
    QTimer timer;
    auto* geo_service = &services::geo::GeopoliticsService::instance();

    QMetaObject::Connection loaded_connection;
    QMetaObject::Connection error_connection;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        out = ToolResult::fail("Geopolitical event fetch timed out");
        done = true;
        loop.quit();
    });

    loaded_connection = QObject::connect(
        geo_service, &services::geo::GeopoliticsService::events_loaded, &loop,
        [&](QVector<services::geo::NewsEvent> events, int total) {
            QJsonArray arr;
            for (const auto& event : events) {
                arr.append(QJsonObject{{"url", event.url},
                                       {"domain", event.domain},
                                       {"event_category", event.event_category},
                                       {"matched_keywords", event.matched_keywords},
                                       {"city", event.city},
                                       {"country", event.country},
                                       {"latitude", event.latitude},
                                       {"longitude", event.longitude},
                                       {"extracted_date", event.extracted_date},
                                       {"created_at", event.created_at}});
            }
            out = ToolResult::ok(QString("Found %1 geopolitical events").arg(arr.size()),
                                 QJsonObject{{"count", arr.size()},
                                             {"total", total},
                                             {"country", country},
                                             {"city", city},
                                             {"category", category},
                                             {"events", arr},
                                             {"source", "local_news_pipeline"}});
            done = true;
            loop.quit();
        });
    error_connection = QObject::connect(
        geo_service, &services::geo::GeopoliticsService::error_occurred, &loop,
        [&](const QString& context_name, const QString& message) {
            if (context_name == "events") {
                out = ToolResult::fail(message);
                done = true;
                loop.quit();
            }
        });

    services::geo::GeopoliticsService::instance().fetch_events(country, city, category, limit);
    timer.start(kAsyncTimeoutMs);
    if (!done)
        loop.exec();
    timer.stop();

    QObject::disconnect(loaded_connection);
    QObject::disconnect(error_connection);
    return out;
}

QStringList topic_tokens(const QString& topic) {
    const QString normalized = topic.toLower().trimmed();
    QStringList raw = normalized.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    QStringList filtered;
    for (const auto& token : raw) {
        if (token.size() >= 3)
            filtered.append(token);
    }
    return filtered;
}

int article_match_score(const QJsonObject& article, const QString& topic, const QStringList& tokens) {
    const QString haystack =
        (article.value("headline").toString() + " " + article.value("summary").toString() + " "
         + article.value("source").toString() + " " + article.value("category").toString() + " "
         + article.value("region").toString())
            .toLower();

    int score = 0;
    if (haystack.contains(topic))
        score += 5;
    for (const auto& token : tokens) {
        if (haystack.contains(token))
            score += 1;
    }
    return score;
}

} // namespace

std::vector<ToolDef> get_fincept_finance_tools() {
    std::vector<ToolDef> tools;

    // ── search_market_assets ────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "search_market_assets";
        t.description = "Search market assets locally via Yahoo Finance search. Supports stocks, funds, indexes, "
                        "forex, and crypto by symbol or company name.";
        t.category = "markets";
        t.input_schema.properties = QJsonObject{
            {"query", QJsonObject{{"type", "string"}, {"description", "Ticker or company search query"}}},
            {"type", QJsonObject{{"type", "string"},
                                 {"description", "Optional asset type filter: stock, fund, index, forex, crypto"}}},
            {"limit", QJsonObject{{"type", "integer"}, {"description", "Max results (default: 10, max: 50)"}}}};
        t.input_schema.required = {"query"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            const QString query = args.value("query").toString().trimmed();
            if (query.isEmpty())
                return ToolResult::fail("Missing 'query'");

            const int limit = qBound(1, args.value("limit").toInt(10), 50);
            ToolResult result = run_script_sync("yfinance_data.py", {"search", query, QString::number(limit)});
            if (!result.success)
                return result;

            QJsonObject payload = result.data.toObject();
            QJsonArray filtered;
            const QString requested_type = args.value("type").toString().trimmed().toLower();
            for (const auto& value : payload.value("results").toArray()) {
                if (!value.isObject())
                    continue;
                const auto obj = value.toObject();
                const QString quote_type = obj.value("type").toString().toLower();
                if (!requested_type.isEmpty() && !quote_type.contains(requested_type)) {
                    if (!(requested_type == "stock" && quote_type.contains("equity")))
                        continue;
                }
                filtered.append(obj);
                if (filtered.size() >= limit)
                    break;
            }

            return ToolResult::ok(QString("Found %1 market assets").arg(filtered.size()),
                                  QJsonObject{{"query", query},
                                              {"type", requested_type},
                                              {"count", filtered.size()},
                                              {"results", filtered},
                                              {"source", "yahoo_finance_search"}});
        };
        tools.push_back(std::move(t));
    }

    // ── get_macro_upcoming_events ──────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_macro_upcoming_events";
        t.description = "Fetch upcoming macro and economic calendar events from the local Investing calendar feed. "
                        "Supports local filtering by country, keyword, and minimum importance.";
        t.category = "macro";
        t.input_schema.properties = QJsonObject{
            {"limit", QJsonObject{{"type", "integer"}, {"description", "Max events after filtering (default: 25)"}}},
            {"country", QJsonObject{{"type", "string"}, {"description", "Optional country code or name filter"}}},
            {"query", QJsonObject{{"type", "string"}, {"description", "Optional event/category keyword filter"}}},
            {"min_importance",
             QJsonObject{{"type", "integer"}, {"description", "Optional minimum importance 0-3 (default: 0)"}}}};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            const int limit = qBound(1, args.value("limit").toInt(25), 100);
            const int min_importance = qBound(0, args.value("min_importance").toInt(0), 3);
            const QString country = args.value("country").toString().trimmed();
            const QString query = args.value("query").toString().trimmed().toLower();
            const QString start = QDate::currentDate().toString(Qt::ISODate);
            const QString end = QDate::currentDate().addDays(7).toString(Qt::ISODate);
            const QString country_arg = country;

            ToolResult result = run_script_sync("investing_calendar_data.py", {"economic", country_arg, start, end});
            if (!result.success)
                return result;

            QJsonArray raw_events = extract_events_array(result.data);
            QJsonArray filtered;
            for (const auto& value : raw_events) {
                if (!value.isObject())
                    continue;
                const auto event = value.toObject();
                const QString event_country =
                    (event.value("country").toString() + " " + event.value("country_code").toString()).toLower();
                const QString haystack =
                    (event.value("event").toString() + " " + event.value("category").toString()).toLower();
                if (!country.isEmpty() && !event_country.contains(country.toLower()))
                    continue;
                if (!query.isEmpty() && !haystack.contains(query))
                    continue;
                if (event.value("importance").toInt(0) < min_importance)
                    continue;
                filtered.append(event);
                if (filtered.size() >= limit)
                    break;
            }

            return ToolResult::ok(QString("Found %1 macro events").arg(filtered.size()),
                                  QJsonObject{{"count", filtered.size()},
                                              {"country", country},
                                              {"query", query},
                                              {"min_importance", min_importance},
                                              {"events", filtered},
                                              {"source", "investing_calendar"}});
        };
        tools.push_back(std::move(t));
    }

    // ── get_geopolitical_events ────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_geopolitical_events";
        t.description = "Fetch locally derived geopolitical incidents from the terminal's news pipeline. "
                        "Supports country, city, category, and limit filters.";
        t.category = "geopolitics";
        t.input_schema.properties = QJsonObject{
            {"country", QJsonObject{{"type", "string"}, {"description", "Optional country filter"}}},
            {"city", QJsonObject{{"type", "string"}, {"description", "Optional city filter"}}},
            {"category", QJsonObject{{"type", "string"}, {"description", "Optional event category filter"}}},
            {"limit", QJsonObject{{"type", "integer"}, {"description", "Max results (default: 20, max: 100)"}}}};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            const QString country = args.value("country").toString().trimmed();
            const QString city = args.value("city").toString().trimmed();
            const QString category = args.value("category").toString().trimmed();
            const int limit = qBound(1, args.value("limit").toInt(20), 100);
            return fetch_geopolitics_sync(country, city, category, limit);
        };
        tools.push_back(std::move(t));
    }

    // ── analyze_news_article ────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "analyze_news_article";
        t.description = "Run local article NLP analysis for a public article URL. Returns sentiment, market impact, "
                        "risk signals, summary, topics, keywords, and entities without paid backends.";
        t.category = "news";
        t.input_schema.properties =
            QJsonObject{{"url", QJsonObject{{"type", "string"}, {"description", "Public article URL to analyze"}}}};
        t.input_schema.required = {"url"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            const QString url = args.value("url").toString().trimmed();
            if (url.isEmpty())
                return ToolResult::fail("Missing 'url'");

            ToolResult analysis = run_news_analysis_sync(url);
            if (!analysis.success)
                return analysis;

            QJsonObject data = analysis.data.toObject();
            const QJsonArray key_points = data.value("key_points").toArray();
            const QString headline = key_points.isEmpty() ? QString() : key_points.at(0).toString();
            if (!headline.isEmpty()) {
                QJsonArray article_array{
                    QJsonObject{{"id", url}, {"headline", headline}, {"summary", data.value("summary").toString()}}};
                ToolResult entities =
                    run_script_sync("news_nlp.py", {"extract_entities",
                                                   QString::fromUtf8(QJsonDocument(article_array).toJson(
                                                       QJsonDocument::Compact))});
                if (entities.success)
                    data["entities"] = entities.data.toObject();
            }

            data["url"] = url;
            data["source"] = "local_news_pipeline";
            return ToolResult::ok("Article analysis completed", data);
        };
        tools.push_back(std::move(t));
    }

    // ── build_local_research_brief ─────────────────────────────────────
    {
        ToolDef t;
        t.name = "build_local_research_brief";
        t.description = "Build a deterministic local research brief from live RSS news. Returns matched headlines, "
                        "category counts, tone, top tickers, and key observations without using external paid LLMs.";
        t.category = "research";
        t.input_schema.properties = QJsonObject{
            {"topic", QJsonObject{{"type", "string"}, {"description", "Topic, company, sector, or theme to scan"}}},
            {"limit", QJsonObject{{"type", "integer"}, {"description", "Maximum matched headlines (default: 8)"}}}};
        t.input_schema.required = {"topic"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            const QString topic = args.value("topic").toString().trimmed();
            if (topic.isEmpty())
                return ToolResult::fail("Missing 'topic'");

            const int limit = qBound(1, args.value("limit").toInt(8), 20);
            ToolResult news_result = fetch_news_sync();
            if (!news_result.success)
                return news_result;

            const QJsonArray news = news_result.data.toArray();
            const QString normalized_topic = topic.toLower();
            const QStringList tokens = topic_tokens(topic);

            QList<QPair<int, QJsonObject>> scored;
            for (const auto& value : news) {
                if (!value.isObject())
                    continue;
                const QJsonObject article = value.toObject();
                const int score = article_match_score(article, normalized_topic, tokens);
                if (score > 0)
                    scored.append({score, article});
            }

            std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
                if (a.first != b.first)
                    return a.first > b.first;
                return a.second.value("sort_ts").toVariant().toLongLong()
                       > b.second.value("sort_ts").toVariant().toLongLong();
            });

            QJsonArray matches;
            QMap<QString, int> category_counts;
            for (int i = 0; i < std::min(limit, static_cast<int>(scored.size())); ++i) {
                const auto article = scored[i].second;
                matches.append(article);
                category_counts[article.value("category").toString()] += 1;
            }

            const QString model_payload =
                QString::fromUtf8(QJsonDocument(matches).toJson(QJsonDocument::Compact));
            ToolResult model_sentiment =
                run_script_sync("news_nlp.py", {"analyze_sentiment_batch", model_payload});
            if (!model_sentiment.success)
                return ToolResult::fail("Model sentiment failed: " + model_sentiment.error);

            ToolResult entities = run_script_sync("news_nlp.py", {"extract_entities", model_payload});
            if (!entities.success)
                return ToolResult::fail("Model entity extraction failed: " + entities.error);

            ToolResult clusters = run_script_sync("news_nlp.py", {"cluster_semantic", model_payload});
            if (!clusters.success)
                return ToolResult::fail("Semantic clustering failed: " + clusters.error);

            const QJsonObject sentiment_obj = model_result_object(model_sentiment);
            const QJsonObject entities_obj = model_result_object(entities);
            const QJsonObject clusters_obj = model_result_object(clusters);

            QJsonArray categories;
            for (auto it = category_counts.begin(); it != category_counts.end(); ++it)
                categories.append(QJsonObject{{"category", it.key()}, {"count", it.value()}});

            QJsonArray top_tickers;
            const QJsonArray entity_tickers = entities_obj.value("top_tickers").toArray();
            for (int i = 0; i < std::min(5, static_cast<int>(entity_tickers.size())); ++i)
                top_tickers.append(entity_tickers.at(i).toObject());

            const QJsonObject aggregate = sentiment_obj.value("aggregate").toObject();
            const int bullish = aggregate.value("bullish").toInt();
            const int bearish = aggregate.value("bearish").toInt();
            const int neutral = aggregate.value("neutral").toInt();

            const QString summary =
                QString("%1 local headline matched '%2'. FinBERT tone: %3 bullish, %4 bearish, %5 neutral.")
                    .arg(matches.size())
                    .arg(topic)
                    .arg(bullish)
                    .arg(bearish)
                    .arg(neutral);

            return ToolResult::ok(summary,
                                  QJsonObject{{"topic", topic},
                                              {"count", matches.size()},
                                              {"matches", matches},
                                              {"categories", categories},
                                              {"top_tickers", top_tickers},
                                              {"tone", QJsonObject{{"bullish", bullish},
                                                                   {"bearish", bearish},
                                                                   {"neutral", neutral},
                                                                   {"overall_score",
                                                                    sentiment_obj.value("overall_score").toDouble()}}},
                                              {"model_sentiment", sentiment_obj},
                                              {"entities", entities_obj},
                                              {"semantic_clusters", clusters_obj},
                                              {"source", "local_news_pipeline"},
                                              {"model_source", "FinBERT + GLiNER + sentence-transformers"}});
        };
        tools.push_back(std::move(t));
    }

    return tools;
}

} // namespace fincept::mcp::tools
