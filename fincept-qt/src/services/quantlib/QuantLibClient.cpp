// QuantLibClient.cpp — Shared QuantLib API HTTP client.

#include "services/quantlib/QuantLibClient.h"

#include "auth/AuthManager.h"
#include "core/logging/Logger.h"
#include "storage/cache/CacheManager.h"

#include <QEventLoop>
#include <QDate>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <limits>

static constexpr int kRefDataTtlSec = 60 * 60; // 1 hour — static reference data

namespace fincept::services {

static constexpr const char* kQuantLibClientTag = "QuantLibClient";

const QString QuantLibClient::API_BASE = QStringLiteral("https://api.fincept.in");

// Endpoints that use GET (no request body).
static const QStringList GET_ENDPOINTS = {
    "core/types/currencies",           "core/types/frequencies",        "scheduling/calendar/list",
    "scheduling/daycount/conventions", "scheduling/adjustment/methods",
};

// Endpoints that take body fields as URL query params (POST with empty body).
static const QStringList QUERY_PARAM_ENDPOINTS = {
    "core/types/spread/from-bps",
};

bool QuantLibClient::is_get_endpoint(const QString& endpoint) {
    return GET_ENDPOINTS.contains(endpoint);
}

bool QuantLibClient::is_query_param_endpoint(const QString& endpoint) {
    return QUERY_PARAM_ENDPOINTS.contains(endpoint);
}

namespace {

bool local_quantlib_enabled() {
    auto& auth_mgr = auth::AuthManager::instance();
    return auth_mgr.has_local_runtime() || !auth_mgr.has_fincept_api_key();
}

double num(const QJsonObject& body, const QString& key, double fallback = 0.0) {
    const auto value = body.value(key);
    if (value.isDouble())
        return value.toDouble();
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        if (ok)
            return parsed;
    }
    return fallback;
}

QString str(const QJsonObject& body, const QString& key, const QString& fallback = {}) {
    const auto value = body.value(key);
    if (value.isString())
        return value.toString();
    return fallback;
}

QVector<double> nums(const QJsonObject& body, const QString& key) {
    QVector<double> out;
    const QJsonArray arr = body.value(key).toArray();
    out.reserve(arr.size());
    for (const auto& v : arr)
        out.append(v.toDouble());
    return out;
}

double norm_pdf(double x) {
    static constexpr double inv_sqrt_2pi = 0.39894228040143267794;
    return inv_sqrt_2pi * std::exp(-0.5 * x * x);
}

double norm_cdf(double x) {
    return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

double norm_inv(double p) {
    if (p <= 0.0)
        return -std::numeric_limits<double>::infinity();
    if (p >= 1.0)
        return std::numeric_limits<double>::infinity();

    static constexpr double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                                   1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    static constexpr double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                                   6.680131188771972e+01, -1.328068155288572e+01};
    static constexpr double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                                   -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    static constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                                   3.754408661907416e+00};
    static constexpr double plow = 0.02425;
    static constexpr double phigh = 1.0 - plow;

    double q = 0.0;
    if (p < plow) {
        q = std::sqrt(-2.0 * std::log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    if (p > phigh) {
        q = std::sqrt(-2.0 * std::log(1.0 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    q = p - 0.5;
    const double r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

QJsonObject bs_values(const QJsonObject& body) {
    const double S = num(body, "S", num(body, "spot", num(body, "S0", 100.0)));
    const double K = num(body, "K", num(body, "strike", 100.0));
    const double r = num(body, "r", num(body, "rate", 0.05));
    const double q = num(body, "q", num(body, "dividend_yield", 0.0));
    const double sigma = std::max(1e-12, num(body, "sigma", num(body, "vol", num(body, "volatility", 0.2))));
    const double T = std::max(1e-12, num(body, "T", num(body, "time", 1.0)));
    const QString option_type = str(body, "option_type", str(body, "type", "call")).toLower();
    const bool call = option_type != "put";

    const double sqrtT = std::sqrt(T);
    const double d1 = (std::log(S / K) + (r - q + 0.5 * sigma * sigma) * T) / (sigma * sqrtT);
    const double d2 = d1 - sigma * sqrtT;
    const double df_r = std::exp(-r * T);
    const double df_q = std::exp(-q * T);
    const double price =
        call ? S * df_q * norm_cdf(d1) - K * df_r * norm_cdf(d2)
             : K * df_r * norm_cdf(-d2) - S * df_q * norm_cdf(-d1);
    const double delta = call ? df_q * norm_cdf(d1) : df_q * (norm_cdf(d1) - 1.0);
    const double gamma = df_q * norm_pdf(d1) / (S * sigma * sqrtT);
    const double vega = S * df_q * norm_pdf(d1) * sqrtT;
    const double theta_call =
        -(S * df_q * norm_pdf(d1) * sigma) / (2.0 * sqrtT) - r * K * df_r * norm_cdf(d2) +
        q * S * df_q * norm_cdf(d1);
    const double theta_put =
        -(S * df_q * norm_pdf(d1) * sigma) / (2.0 * sqrtT) + r * K * df_r * norm_cdf(-d2) -
        q * S * df_q * norm_cdf(-d1);
    const double rho = call ? K * T * df_r * norm_cdf(d2) : -K * T * df_r * norm_cdf(-d2);

    return QJsonObject{{"engine", "local"},
                       {"model", "black_scholes"},
                       {"option_type", call ? "call" : "put"},
                       {"price", price},
                       {"delta", delta},
                       {"gamma", gamma},
                       {"vega", vega},
                       {"theta", call ? theta_call : theta_put},
                       {"rho", rho},
                       {"d1", d1},
                       {"d2", d2}};
}

QJsonObject black76_values(const QJsonObject& body) {
    const double F = num(body, "F", num(body, "forward", 100.0));
    const double K = num(body, "K", num(body, "strike", 100.0));
    const double r = num(body, "r", num(body, "rate", 0.05));
    const double sigma = std::max(1e-12, num(body, "sigma", num(body, "volatility", 0.2)));
    const double T = std::max(1e-12, num(body, "T", 1.0));
    const bool call = str(body, "option_type", str(body, "type", "call")).toLower() != "put";
    const double sqrtT = std::sqrt(T);
    const double d1 = (std::log(F / K) + 0.5 * sigma * sigma * T) / (sigma * sqrtT);
    const double d2 = d1 - sigma * sqrtT;
    const double df = std::exp(-r * T);
    const double price = call ? df * (F * norm_cdf(d1) - K * norm_cdf(d2))
                              : df * (K * norm_cdf(-d2) - F * norm_cdf(-d1));
    return QJsonObject{{"engine", "local"},
                       {"model", "black76"},
                       {"option_type", call ? "call" : "put"},
                       {"price", price},
                       {"delta", call ? df * norm_cdf(d1) : df * (norm_cdf(d1) - 1.0)},
                       {"gamma", df * norm_pdf(d1) / (F * sigma * sqrtT)},
                       {"vega", df * F * norm_pdf(d1) * sqrtT},
                       {"d1", d1},
                       {"d2", d2}};
}

double percentile(QVector<double> values, double p) {
    if (values.isEmpty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const double pos = std::clamp(p, 0.0, 1.0) * (values.size() - 1);
    const int lo = static_cast<int>(std::floor(pos));
    const int hi = static_cast<int>(std::ceil(pos));
    if (lo == hi)
        return values.at(lo);
    return values.at(lo) + (values.at(hi) - values.at(lo)) * (pos - lo);
}

mcp::ToolResult local_quantlib_call(const QString& endpoint, const QJsonObject& body) {
    if (endpoint == "core/types/currencies")
        return mcp::ToolResult::ok_data(QJsonArray{"USD", "EUR", "GBP", "JPY", "CHF", "CAD", "AUD", "TRY"});
    if (endpoint == "core/types/frequencies")
        return mcp::ToolResult::ok_data(QJsonArray{"annual", "semiannual", "quarterly", "monthly", "continuous"});
    if (endpoint == "scheduling/daycount/conventions")
        return mcp::ToolResult::ok_data(QJsonArray{"ACT/360", "ACT/365", "30/360"});
    if (endpoint == "scheduling/adjustment/methods")
        return mcp::ToolResult::ok_data(QJsonArray{"following", "modified_following", "preceding", "unadjusted"});
    if (endpoint == "scheduling/calendar/list")
        return mcp::ToolResult::ok_data(QJsonArray{"TARGET", "US", "UK", "NYSE", "Turkey"});

    if (endpoint == "core/types/money/create")
        return mcp::ToolResult::ok_data(QJsonObject{{"amount", num(body, "amount")}, {"currency", str(body, "currency", "USD")}});
    if (endpoint == "core/types/money/convert") {
        const double amount = num(body, "amount");
        const double rate = num(body, "rate", 1.0);
        return mcp::ToolResult::ok_data(QJsonObject{{"amount", amount * rate},
                                                    {"from_currency", str(body, "from_currency")},
                                                    {"to_currency", str(body, "to_currency")},
                                                    {"rate", rate},
                                                    {"engine", "local"}});
    }
    if (endpoint == "core/types/spread/from-bps")
        return mcp::ToolResult::ok_data(QJsonObject{{"bps", num(body, "bps")}, {"decimal", num(body, "bps") / 10000.0}});
    if (endpoint == "core/types/rate/convert" || endpoint == "core/ops/zero-rate-convert") {
        const double value = num(body, "value", num(body, "rate", 0.0));
        const QString from = str(body, "from_type", str(body, "from", "annual")).toLower();
        const QString to = str(body, "to_type", str(body, "to", "continuous")).toLower();
        const double continuous = from == "continuous" ? value : std::log1p(value);
        const double converted = to == "continuous" ? continuous : std::expm1(continuous);
        return mcp::ToolResult::ok_data(QJsonObject{{"input", value}, {"output", converted}, {"from", from}, {"to", to}});
    }
    if (endpoint == "core/types/notional-schedule") {
        const double notional = num(body, "notional", 1.0);
        const int periods = std::max(1, static_cast<int>(num(body, "periods", 1)));
        QJsonArray schedule;
        for (int i = 0; i < periods; ++i)
            schedule.append(QJsonObject{{"period", i + 1}, {"notional", notional}});
        return mcp::ToolResult::ok_data(schedule);
    }

    if (endpoint == "core/distributions/normal/pdf" || endpoint == "statistics/distributions/normal/pdf") {
        const double mean = num(body, "mean", 0.0);
        const double std = std::max(1e-12, num(body, "std", num(body, "sigma", 1.0)));
        const double x = num(body, "x");
        return mcp::ToolResult::ok_data(QJsonObject{{"pdf", norm_pdf((x - mean) / std) / std}, {"x", x}});
    }
    if (endpoint == "core/distributions/normal/cdf" || endpoint == "statistics/distributions/normal/cdf") {
        const double mean = num(body, "mean", 0.0);
        const double std = std::max(1e-12, num(body, "std", num(body, "sigma", 1.0)));
        const double x = num(body, "x");
        return mcp::ToolResult::ok_data(QJsonObject{{"cdf", norm_cdf((x - mean) / std)}, {"x", x}});
    }
    if (endpoint == "core/distributions/normal/ppf" || endpoint == "statistics/distributions/normal/ppf") {
        const double mean = num(body, "mean", 0.0);
        const double std = std::max(1e-12, num(body, "std", num(body, "sigma", 1.0)));
        const double p = num(body, "p", 0.5);
        return mcp::ToolResult::ok_data(QJsonObject{{"ppf", mean + std * norm_inv(p)}, {"p", p}});
    }

    if (endpoint == "pricing/bs/price" || endpoint == "pricing/bs/greeks" ||
        endpoint == "pricing/bs/greeks-full" || endpoint == "core/ops/black-scholes")
        return mcp::ToolResult::ok_data(bs_values(body));
    if (endpoint == "pricing/black76/price" || endpoint == "pricing/black76/greeks" ||
        endpoint == "pricing/black76/greeks-full" || endpoint == "core/ops/black76")
        return mcp::ToolResult::ok_data(black76_values(body));
    if (endpoint == "pricing/bs/implied-vol" || endpoint == "solver/finance/implied-vol") {
        const double target = num(body, "price", num(body, "target_price", 10.0));
        double lo = 1e-4;
        double hi = 5.0;
        QJsonObject work = body;
        for (int i = 0; i < 80; ++i) {
            const double mid = 0.5 * (lo + hi);
            work["sigma"] = mid;
            const double price = bs_values(work).value("price").toDouble();
            if (price > target)
                hi = mid;
            else
                lo = mid;
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"implied_vol", 0.5 * (lo + hi)}, {"target_price", target}, {"engine", "local"}});
    }

    if (endpoint == "core/ops/forward-rate" || endpoint == "solver/finance/forward-rate") {
        const double r1 = num(body, "r1", num(body, "short_rate", 0.03));
        const double r2 = num(body, "r2", num(body, "long_rate", 0.04));
        const double t1 = num(body, "t1", 1.0);
        const double t2 = std::max(t1 + 1e-12, num(body, "t2", 2.0));
        return mcp::ToolResult::ok_data(QJsonObject{{"forward_rate", (r2 * t2 - r1 * t1) / (t2 - t1)}, {"engine", "local"}});
    }
    if (endpoint == "solver/finance/discount-factor") {
        const double r = num(body, "rate", num(body, "r", 0.05));
        const double t = num(body, "time", num(body, "T", 1.0));
        return mcp::ToolResult::ok_data(QJsonObject{{"discount_factor", std::exp(-r * t)}});
    }
    if (endpoint == "core/ops/statistics") {
        const QVector<double> xs = nums(body, "values");
        if (xs.isEmpty())
            return mcp::ToolResult::fail("values must be a non-empty numeric array");
        double sum = 0.0;
        for (double x : xs)
            sum += x;
        const double mean = sum / xs.size();
        double ss = 0.0;
        for (double x : xs)
            ss += (x - mean) * (x - mean);
        return mcp::ToolResult::ok_data(QJsonObject{{"count", xs.size()}, {"mean", mean}, {"variance", ss / xs.size()}, {"std", std::sqrt(ss / xs.size())}});
    }
    if (endpoint == "core/ops/percentile") {
        const double p = num(body, "p", num(body, "percentile", 0.95));
        return mcp::ToolResult::ok_data(QJsonObject{{"percentile", percentile(nums(body, "values"), p > 1.0 ? p / 100.0 : p)}});
    }
    if (endpoint == "core/ops/var" || endpoint == "risk/var/historical" || endpoint == "portfolio/risk/var") {
        const double confidence = num(body, "confidence", 0.95);
        const QVector<double> returns = nums(body, "returns");
        if (returns.isEmpty())
            return mcp::ToolResult::fail("returns must be a non-empty numeric array");
        const double q = percentile(returns, 1.0 - confidence);
        return mcp::ToolResult::ok_data(QJsonObject{{"var", -q}, {"confidence", confidence}, {"method", "historical"}, {"engine", "local"}});
    }
    if (endpoint == "risk/var/parametric") {
        const double mu = num(body, "mean", num(body, "mu", 0.0));
        const double sigma = num(body, "std", num(body, "sigma", 0.02));
        const double confidence = num(body, "confidence", 0.95);
        return mcp::ToolResult::ok_data(QJsonObject{{"var", -(mu + sigma * norm_inv(1.0 - confidence))}, {"confidence", confidence}, {"method", "parametric"}});
    }

    if (endpoint == "scheduling/calendar/is-business-day") {
        const QDate d = QDate::fromString(str(body, "date"), Qt::ISODate);
        const bool business = d.isValid() && d.dayOfWeek() < 6;
        return mcp::ToolResult::ok_data(QJsonObject{{"date", d.toString(Qt::ISODate)}, {"is_business_day", business}, {"calendar", str(body, "calendar", "weekend-only")}});
    }
    if (endpoint == "scheduling/calendar/next-business-day" || endpoint == "scheduling/calendar/previous-business-day") {
        QDate d = QDate::fromString(str(body, "date"), Qt::ISODate);
        if (!d.isValid())
            return mcp::ToolResult::fail("date must be ISO format YYYY-MM-DD");
        const int step = endpoint.contains("previous") ? -1 : 1;
        do {
            d = d.addDays(step);
        } while (d.dayOfWeek() >= 6);
        return mcp::ToolResult::ok_data(QJsonObject{{"date", d.toString(Qt::ISODate)}, {"calendar", "weekend-only"}});
    }
    if (endpoint == "core/conventions/days-to-years") {
        const QString dc = str(body, "day_count", "ACT/365").toUpper();
        const double denom = dc.contains("360") ? 360.0 : 365.0;
        return mcp::ToolResult::ok_data(QJsonObject{{"years", num(body, "value") / denom}, {"day_count", dc}});
    }
    if (endpoint == "core/conventions/years-to-days") {
        const QString dc = str(body, "day_count", "ACT/365").toUpper();
        const double denom = dc.contains("360") ? 360.0 : 365.0;
        return mcp::ToolResult::ok_data(QJsonObject{{"days", num(body, "value") * denom}, {"day_count", dc}});
    }

    if (endpoint == "numerical/linalg/dot") {
        const QVector<double> a = nums(body, "a");
        const QVector<double> b = nums(body, "b");
        if (a.size() != b.size())
            return mcp::ToolResult::fail("a and b must have the same length");
        double dot = 0.0;
        for (int i = 0; i < a.size(); ++i)
            dot += a.at(i) * b.at(i);
        return mcp::ToolResult::ok_data(QJsonObject{{"dot", dot}});
    }
    if (endpoint == "numerical/linalg/norm") {
        double ss = 0.0;
        for (double x : nums(body, "values"))
            ss += x * x;
        return mcp::ToolResult::ok_data(QJsonObject{{"norm", std::sqrt(ss)}});
    }
    if (endpoint == "physics/entropy/shannon") {
        QVector<double> p = nums(body, "probabilities");
        if (p.isEmpty())
            p = nums(body, "p");
        double h = 0.0;
        for (double x : p) {
            if (x > 0.0)
                h -= x * std::log2(x);
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"entropy", h}, {"base", 2}, {"engine", "local"}});
    }

    return mcp::ToolResult::fail(
        "Local QuantLib endpoint is not implemented yet: " + endpoint +
        ". Fincept Cloud fallback is disabled in local runtime.");
}

} // namespace

// ── Singleton ────────────────────────────────────────────────────────────────

QuantLibClient::QuantLibClient(QObject* parent) : QObject(parent) {}

QuantLibClient& QuantLibClient::instance() {
    static QuantLibClient inst;
    return inst;
}

// ── Request builder ──────────────────────────────────────────────────────────

static QNetworkRequest build_request(const QString& endpoint, const QJsonObject& query_params = {}) {
    QString url = QuantLibClient::API_BASE + "/quantlib/" + endpoint;

    if (!query_params.isEmpty()) {
        QStringList parts;
        for (auto it = query_params.begin(); it != query_params.end(); ++it)
            parts << (it.key() + "=" + it.value().toVariant().toString());
        url += "?" + parts.join("&");
    }

    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("User-Agent", "FinceptTerminal/4.0.0");

    auto& auth_mgr = auth::AuthManager::instance();
    const QString fincept_api_key = auth_mgr.effective_api_key();
    if (!fincept_api_key.isEmpty())
        req.setRawHeader("X-API-Key", fincept_api_key.toUtf8());

    return req;
}

// ── Response parser ──────────────────────────────────────────────────────────

mcp::ToolResult QuantLibClient::parse_response(int http_status, const QByteArray& raw) {
    QJsonParseError parse_err;
    auto doc = QJsonDocument::fromJson(raw, &parse_err);

    if (parse_err.error != QJsonParseError::NoError) {
        return mcp::ToolResult::fail(
            QString("JSON parse error (HTTP %1): %2").arg(http_status).arg(parse_err.errorString()));
    }

    // FastAPI 422 validation error: {"detail": [...]}
    if (http_status == 422 && doc.isObject()) {
        auto obj = doc.object();
        if (obj.contains("detail")) {
            QString msg;
            auto detail = obj["detail"];
            if (detail.isArray()) {
                for (const auto& item : detail.toArray()) {
                    auto d = item.toObject();
                    QString loc = d["loc"].toArray().last().toString();
                    msg += loc + ": " + d["msg"].toString() + "\n";
                }
            } else {
                msg = detail.toString();
            }
            return mcp::ToolResult::fail("Validation error: " + msg.trimmed());
        }
    }

    if (!doc.isObject()) {
        // Top-level array or scalar — return as-is in data
        if (doc.isArray())
            return mcp::ToolResult::ok_data(doc.array());
        return mcp::ToolResult::fail(QString("Unexpected response (HTTP %1)").arg(http_status));
    }

    auto obj = doc.object();

    // API envelope: {"success": bool, "message": "...", "data": <payload>}
    if (obj.contains("success")) {
        if (!obj["success"].toBool()) {
            QString msg = obj["message"].toString();
            if (msg.isEmpty())
                msg = QString("API error (HTTP %1)").arg(http_status);
            return mcp::ToolResult::fail(msg);
        }
        // Unwrap data payload
        auto payload = obj.value("data");
        QString message = obj.value("message").toString();
        return mcp::ToolResult::ok(message, payload);
    }

    // No envelope — return raw object
    return mcp::ToolResult::ok_data(obj);
}

// ── Async call ───────────────────────────────────────────────────────────────

void QuantLibClient::call(const QString& endpoint, const QJsonObject& body, QuantLibCallback callback) {
    if (local_quantlib_enabled()) {
        callback(local_quantlib_call(endpoint, body));
        return;
    }

    // Cache GET endpoints (static reference data) and query-param endpoints
    const bool cacheable = is_get_endpoint(endpoint) || is_query_param_endpoint(endpoint);
    if (cacheable) {
        QString cache_key = "quantlib:" + endpoint;
        if (!body.isEmpty())
            cache_key += ":" + QString::fromUtf8(QJsonDocument(body).toJson(QJsonDocument::Compact));

        const QVariant cached = fincept::CacheManager::instance().get(cache_key);
        if (!cached.isNull()) {
            auto doc = QJsonDocument::fromJson(cached.toString().toUtf8());
            mcp::ToolResult result =
                doc.isObject() ? mcp::ToolResult::ok_data(doc.object()) : mcp::ToolResult::ok_data(doc.array());
            callback(result);
            return;
        }

        auto* nam = new QNetworkAccessManager(this);
        QNetworkReply* reply = nullptr;
        if (is_get_endpoint(endpoint)) {
            reply = nam->get(build_request(endpoint));
        } else {
            reply = nam->post(build_request(endpoint, body), QByteArray("{}"));
        }

        QPointer<QuantLibClient> self = this;
        connect(reply, &QNetworkReply::finished, this, [self, reply, nam, endpoint, cache_key, callback]() {
            reply->deleteLater();
            nam->deleteLater();
            if (!self)
                return;
            if (reply->error() != QNetworkReply::NoError) {
                LOG_ERROR(kQuantLibClientTag, "Network error on " + endpoint + ": " + reply->errorString());
                callback(mcp::ToolResult::fail(reply->errorString()));
                return;
            }
            int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            auto raw = reply->readAll();
            auto result = parse_response(http_status, raw);
            if (result.success) {
                // Serialize the data payload for caching
                QString to_cache;
                if (result.data.isObject())
                    to_cache = QString::fromUtf8(QJsonDocument(result.data.toObject()).toJson(QJsonDocument::Compact));
                else if (result.data.isArray())
                    to_cache = QString::fromUtf8(QJsonDocument(result.data.toArray()).toJson(QJsonDocument::Compact));
                if (!to_cache.isEmpty())
                    fincept::CacheManager::instance().put(cache_key, QVariant(to_cache), kRefDataTtlSec, "quantlib");
            }
            LOG_DEBUG(
                kQuantLibClientTag,
                QString("HTTP %1 — %2 — %3").arg(http_status).arg(endpoint).arg(result.success ? "OK" : result.error));
            callback(result);
        });
        return;
    }

    // Non-cacheable POST endpoints
    auto* nam = new QNetworkAccessManager(this);
    auto req = build_request(endpoint);
    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto* reply = nam->post(req, data);

    QPointer<QuantLibClient> self = this;
    connect(reply, &QNetworkReply::finished, this, [self, reply, nam, endpoint, callback]() {
        reply->deleteLater();
        nam->deleteLater();

        if (!self)
            return;

        if (reply->error() != QNetworkReply::NoError) {
            LOG_ERROR(kQuantLibClientTag, "Network error on " + endpoint + ": " + reply->errorString());
            callback(mcp::ToolResult::fail(reply->errorString()));
            return;
        }

        int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        auto raw = reply->readAll();
        auto result = parse_response(http_status, raw);

        LOG_DEBUG(
            kQuantLibClientTag,
            QString("HTTP %1 — %2 — %3").arg(http_status).arg(endpoint).arg(result.success ? "OK" : result.error));

        callback(result);
    });
}

// ── Sync call (MCP tool handlers only — never call from UI thread) ───────────

mcp::ToolResult QuantLibClient::call_sync(const QString& endpoint, const QJsonObject& body) {
    mcp::ToolResult result = mcp::ToolResult::fail("Timeout");

    QEventLoop loop;
    call(endpoint, body, [&](mcp::ToolResult r) {
        result = std::move(r);
        loop.quit();
    });
    loop.exec();

    return result;
}

} // namespace fincept::services
