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
#include <functional>
#include <limits>
#include <numeric>
#include <random>

static constexpr int kRefDataTtlSec = 60 * 60; // 1 hour — static reference data

namespace fincept::services {

static constexpr const char* kQuantLibClientTag = "QuantLibClient";
static constexpr double kPi = 3.14159265358979323846;

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

QVector<double> nums_any(const QJsonObject& body, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        QVector<double> values = nums(body, QString::fromLatin1(key));
        if (!values.isEmpty())
            return values;
    }
    return {};
}

QJsonArray to_json(const QVector<double>& values) {
    QJsonArray out;
    for (double value : values)
        out.append(value);
    return out;
}

QJsonArray to_json(const QVector<QVector<double>>& matrix) {
    QJsonArray out;
    for (const auto& row : matrix)
        out.append(to_json(row));
    return out;
}

QVector<QVector<double>> matrix(const QJsonObject& body, const QString& key) {
    QVector<QVector<double>> out;
    const QJsonArray arr = body.value(key).toArray();
    out.reserve(arr.size());
    for (const auto& row_value : arr) {
        QVector<double> row;
        const QJsonArray row_arr = row_value.toArray();
        row.reserve(row_arr.size());
        for (const auto& value : row_arr)
            row.append(value.toDouble());
        if (!row.isEmpty())
            out.append(row);
    }
    return out;
}

QVector<QVector<double>> matrix_any(const QJsonObject& body, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        QVector<QVector<double>> values = matrix(body, QString::fromLatin1(key));
        if (!values.isEmpty())
            return values;
    }
    return {};
}

double safe_div(double numerator, double denominator, double fallback = 0.0) {
    return std::abs(denominator) < 1e-12 ? fallback : numerator / denominator;
}

double sum(const QVector<double>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0);
}

double mean(const QVector<double>& values) {
    return values.isEmpty() ? 0.0 : sum(values) / values.size();
}

double variance(const QVector<double>& values, bool sample = false) {
    if (values.size() < (sample ? 2 : 1))
        return 0.0;
    const double m = mean(values);
    double ss = 0.0;
    for (double value : values)
        ss += (value - m) * (value - m);
    return ss / (values.size() - (sample ? 1 : 0));
}

double covariance(const QVector<double>& a, const QVector<double>& b) {
    const int n = std::min(a.size(), b.size());
    if (n < 2)
        return 0.0;
    const double ma = mean(a);
    const double mb = mean(b);
    double cov = 0.0;
    for (int i = 0; i < n; ++i)
        cov += (a.at(i) - ma) * (b.at(i) - mb);
    return cov / (n - 1);
}

QVector<QVector<double>> covariance_matrix(const QVector<QVector<double>>& observations) {
    if (observations.isEmpty())
        return {};
    const int rows = observations.size();
    const int cols = observations.first().size();
    QVector<QVector<double>> columns(cols);
    for (int c = 0; c < cols; ++c) {
        columns[c].reserve(rows);
        for (const auto& row : observations) {
            if (row.size() == cols)
                columns[c].append(row.at(c));
        }
    }
    QVector<QVector<double>> cov(cols, QVector<double>(cols, 0.0));
    for (int i = 0; i < cols; ++i) {
        for (int j = i; j < cols; ++j) {
            const double value = covariance(columns.at(i), columns.at(j));
            cov[i][j] = value;
            cov[j][i] = value;
        }
    }
    return cov;
}

double portfolio_return(const QVector<double>& weights, const QVector<double>& returns) {
    const int n = std::min(weights.size(), returns.size());
    double out = 0.0;
    for (int i = 0; i < n; ++i)
        out += weights.at(i) * returns.at(i);
    return out;
}

double portfolio_variance(const QVector<double>& weights, const QVector<QVector<double>>& cov) {
    const int n = std::min(weights.size(), cov.size());
    double out = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n && j < cov.at(i).size(); ++j)
            out += weights.at(i) * weights.at(j) * cov.at(i).at(j);
    }
    return out;
}

QVector<double> normalize_weights(QVector<double> weights) {
    double total = sum(weights);
    if (std::abs(total) < 1e-12 && !weights.isEmpty()) {
        const double equal = 1.0 / weights.size();
        std::fill(weights.begin(), weights.end(), equal);
        return weights;
    }
    for (double& weight : weights)
        weight /= total;
    return weights;
}

QVector<double> inverse_vol_weights(const QVector<QVector<double>>& cov) {
    QVector<double> weights;
    weights.reserve(cov.size());
    for (int i = 0; i < cov.size(); ++i) {
        const double vol = std::sqrt(std::max(0.0, cov.at(i).value(i)));
        weights.append(vol > 1e-12 ? 1.0 / vol : 0.0);
    }
    return normalize_weights(weights);
}

QVector<double> inverse_variance_weights(const QVector<QVector<double>>& cov) {
    QVector<double> weights;
    weights.reserve(cov.size());
    for (int i = 0; i < cov.size(); ++i) {
        const double var = std::max(0.0, cov.at(i).value(i));
        weights.append(var > 1e-12 ? 1.0 / var : 0.0);
    }
    return normalize_weights(weights);
}

QDate iso_date(const QString& value) {
    return QDate::fromString(value, Qt::ISODate);
}

QString qt_date_format(QString fmt) {
    if (fmt.isEmpty())
        return QStringLiteral("yyyy-MM-dd");
    return fmt.replace("%Y", "yyyy").replace("%m", "MM").replace("%d", "dd");
}

int months_from_tenor(QString tenor) {
    tenor = tenor.trimmed().toUpper();
    if (tenor == "ANNUAL" || tenor == "YEARLY")
        return 12;
    if (tenor == "SEMIANNUAL" || tenor == "SEMI-ANNUAL")
        return 6;
    if (tenor == "QUARTERLY")
        return 3;
    if (tenor == "MONTHLY")
        return 1;
    bool ok = false;
    const int n = tenor.left(tenor.size() - 1).toInt(&ok);
    if (!ok || n <= 0)
        return 0;
    const QChar unit = tenor.back();
    if (unit == 'Y')
        return n * 12;
    if (unit == 'M')
        return n;
    if (unit == 'W')
        return std::max(1, static_cast<int>(std::round(n * 12.0 / 52.0)));
    if (unit == 'D')
        return std::max(1, static_cast<int>(std::round(n / 30.0)));
    return 0;
}

double year_fraction(const QDate& start, const QDate& end, QString convention = QStringLiteral("ACT/365")) {
    if (!start.isValid() || !end.isValid())
        return 0.0;
    convention = convention.toUpper();
    if (convention.contains("30/360")) {
        const int d1 = std::min(start.day(), 30);
        const int d2 = std::min(end.day(), 30);
        return ((end.year() - start.year()) * 360.0 + (end.month() - start.month()) * 30.0 + (d2 - d1)) / 360.0;
    }
    return start.daysTo(end) / (convention.contains("360") ? 360.0 : 365.0);
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

double integrate_simpson(const std::function<double(double)>& f, double a, double b, int n = 512) {
    if (b < a)
        return -integrate_simpson(f, b, a, n);
    if (std::abs(b - a) < 1e-12)
        return 0.0;
    if (n % 2 != 0)
        ++n;
    const double h = (b - a) / n;
    double s = f(a) + f(b);
    for (int i = 1; i < n; ++i)
        s += f(a + i * h) * (i % 2 == 0 ? 2.0 : 4.0);
    return s * h / 3.0;
}

double regularized_gamma_p(double a, double x) {
    if (a <= 0.0 || x <= 0.0)
        return 0.0;
    static constexpr int kMaxIter = 200;
    static constexpr double kEps = 3e-14;
    if (x < a + 1.0) {
        double ap = a;
        double del = 1.0 / a;
        double sum = del;
        for (int n = 1; n <= kMaxIter; ++n) {
            ++ap;
            del *= x / ap;
            sum += del;
            if (std::abs(del) < std::abs(sum) * kEps)
                break;
        }
        return sum * std::exp(-x + a * std::log(x) - std::lgamma(a));
    }

    double b = x + 1.0 - a;
    double c = 1.0 / std::numeric_limits<double>::min();
    double d = 1.0 / b;
    double h = d;
    for (int i = 1; i <= kMaxIter; ++i) {
        const double an = -i * (i - a);
        b += 2.0;
        d = an * d + b;
        if (std::abs(d) < std::numeric_limits<double>::min())
            d = std::numeric_limits<double>::min();
        c = b + an / c;
        if (std::abs(c) < std::numeric_limits<double>::min())
            c = std::numeric_limits<double>::min();
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::abs(del - 1.0) < kEps)
            break;
    }
    return 1.0 - std::exp(-x + a * std::log(x) - std::lgamma(a)) * h;
}

double gamma_pdf_value(double x, double alpha, double beta) {
    if (x < 0.0 || alpha <= 0.0 || beta <= 0.0)
        return 0.0;
    return std::pow(beta, alpha) * std::pow(x, alpha - 1.0) * std::exp(-beta * x) / std::tgamma(alpha);
}

double student_t_pdf(double x, double df) {
    if (df <= 0.0)
        return 0.0;
    return std::tgamma((df + 1.0) / 2.0) /
           (std::sqrt(df * kPi) * std::tgamma(df / 2.0)) *
           std::pow(1.0 + x * x / df, -(df + 1.0) / 2.0);
}

double student_t_cdf(double x, double df) {
    if (x == 0.0)
        return 0.5;
    const double area = integrate_simpson([df](double t) { return student_t_pdf(t, df); }, 0.0, std::abs(x), 800);
    return x > 0.0 ? 0.5 + area : 0.5 - area;
}

double beta_func(double a, double b) {
    return std::exp(std::lgamma(a) + std::lgamma(b) - std::lgamma(a + b));
}

double beta_pdf_value(double x, double a, double b) {
    if (x <= 0.0 || x >= 1.0 || a <= 0.0 || b <= 0.0)
        return 0.0;
    return std::pow(x, a - 1.0) * std::pow(1.0 - x, b - 1.0) / beta_func(a, b);
}

double beta_cdf_value(double x, double a, double b) {
    if (x <= 0.0)
        return 0.0;
    if (x >= 1.0)
        return 1.0;
    return integrate_simpson([a, b](double t) { return beta_pdf_value(t, a, b); }, 0.0, x, 600);
}

double linear_interpolate(QVector<double> xs, QVector<double> ys, double x) {
    if (xs.isEmpty() || ys.isEmpty() || xs.size() != ys.size())
        return 0.0;
    QVector<int> order(xs.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return xs.at(a) < xs.at(b); });
    QVector<double> sx;
    QVector<double> sy;
    for (int i : order) {
        sx.append(xs.at(i));
        sy.append(ys.at(i));
    }
    if (x <= sx.first())
        return sy.first();
    if (x >= sx.last())
        return sy.last();
    for (int i = 1; i < sx.size(); ++i) {
        if (x <= sx.at(i)) {
            const double w = (x - sx.at(i - 1)) / (sx.at(i) - sx.at(i - 1));
            return sy.at(i - 1) + w * (sy.at(i) - sy.at(i - 1));
        }
    }
    return sy.last();
}

double unary_function(QString name, double x) {
    name = name.trimmed().toLower();
    if (name == "sin")
        return std::sin(x);
    if (name == "cos")
        return std::cos(x);
    if (name == "tan")
        return std::tan(x);
    if (name == "exp")
        return std::exp(x);
    if (name == "log")
        return std::log(x);
    if (name == "sqrt")
        return std::sqrt(x);
    if (name == "abs")
        return std::abs(x);
    if (name == "square")
        return x * x;
    return x;
}

double binary_function(QString name, double x, double y) {
    name = name.trimmed().toLower();
    if (name == "power" || name == "pow")
        return std::pow(x, y);
    if (name == "maximum" || name == "max")
        return std::max(x, y);
    if (name == "minimum" || name == "min")
        return std::min(x, y);
    if (name == "add")
        return x + y;
    if (name == "subtract")
        return x - y;
    if (name == "multiply")
        return x * y;
    if (name == "divide")
        return safe_div(x, y);
    return std::numeric_limits<double>::quiet_NaN();
}

QJsonObject bs_values(const QJsonObject& body) {
    const double S = num(body, "S", num(body, "spot", num(body, "S0", 100.0)));
    const double K = num(body, "K", num(body, "strike", 100.0));
    const double r = num(body, "r", num(body, "rate", num(body, "risk_free_rate", 0.05)));
    const double q = num(body, "q", num(body, "dividend_yield", 0.0));
    const double sigma = std::max(1e-12, num(body, "sigma", num(body, "vol", num(body, "volatility", 0.2))));
    const double T = std::max(1e-12, num(body, "T", num(body, "time", num(body, "time_to_maturity", 1.0))));
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
    const double T = std::max(1e-12, num(body, "T", num(body, "time", num(body, "time_to_maturity", 1.0))));
    const double df_input = num(body, "discount_factor", -1.0);
    const double r = df_input > 0.0 ? -std::log(df_input) / T
                                    : num(body, "r", num(body, "rate", num(body, "risk_free_rate", 0.05)));
    const double sigma = std::max(1e-12, num(body, "sigma", num(body, "volatility", 0.2)));
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

QJsonObject bachelier_values(const QJsonObject& body) {
    const double F = num(body, "F", num(body, "forward", 100.0));
    const double K = num(body, "K", num(body, "strike", 100.0));
    const double r = num(body, "r", num(body, "rate", num(body, "risk_free_rate", 0.05)));
    const double sigma_n = std::max(1e-12, num(body, "normal_volatility", num(body, "normal_vol", num(body, "sigma", 5.0))));
    const double T = std::max(1e-12, num(body, "T", num(body, "time", num(body, "time_to_maturity", 1.0))));
    const bool call = str(body, "option_type", str(body, "type", "call")).toLower() != "put";
    const double sqrtT = std::sqrt(T);
    const double d = (F - K) / (sigma_n * sqrtT);
    const double df = std::exp(-r * T);
    const double intrinsic = F - K;
    const double price = call ? df * (intrinsic * norm_cdf(d) + sigma_n * sqrtT * norm_pdf(d))
                              : df * (-intrinsic * norm_cdf(-d) + sigma_n * sqrtT * norm_pdf(d));
    return QJsonObject{{"engine", "local"},
                       {"model", "bachelier"},
                       {"option_type", call ? "call" : "put"},
                       {"price", price},
                       {"delta", call ? df * norm_cdf(d) : -df * norm_cdf(-d)},
                       {"gamma", df * norm_pdf(d) / (sigma_n * sqrtT)},
                       {"vega", df * sqrtT * norm_pdf(d)},
                       {"d", d}};
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

QVector<double> historical_portfolio_returns(const QJsonObject& body) {
    QVector<double> flat = nums(body, "returns");
    const QVector<double> weights = nums(body, "weights");
    const QVector<QVector<double>> observations = matrix(body, "returns");
    if (!observations.isEmpty() && !weights.isEmpty()) {
        QVector<double> out;
        out.reserve(observations.size());
        for (const auto& row : observations)
            out.append(portfolio_return(weights, row));
        return out;
    }
    return flat;
}

QJsonObject var_result(QVector<double> returns, double confidence) {
    if (returns.isEmpty())
        return QJsonObject{{"error", "returns must be a non-empty numeric array"}};
    const double q = percentile(returns, 1.0 - confidence);
    double tail_sum = 0.0;
    int tail_count = 0;
    for (double r : returns) {
        if (r <= q) {
            tail_sum += r;
            ++tail_count;
        }
    }
    const double es = tail_count > 0 ? -tail_sum / tail_count : -q;
    return QJsonObject{{"var", -q}, {"expected_shortfall", es}, {"confidence", confidence}, {"method", "historical"}, {"engine", "local"}};
}

QVector<double> discount_factors_from_curve(const QJsonObject& body, QVector<double>* times_out = nullptr) {
    QVector<double> times = nums_any(body, {"times", "tenors", "maturities"});
    QVector<double> dfs = nums_any(body, {"discount_factors", "dfs"});
    QVector<double> rates = nums_any(body, {"zero_rates", "rates", "yields"});
    if (times.isEmpty()) {
        const int n = std::max(dfs.size(), rates.size());
        for (int i = 0; i < n; ++i)
            times.append(i + 1.0);
    }
    if (dfs.isEmpty()) {
        for (int i = 0; i < rates.size() && i < times.size(); ++i)
            dfs.append(std::exp(-rates.at(i) * times.at(i)));
    }
    if (times_out)
        *times_out = times;
    return dfs;
}

QJsonArray curve_points(const QVector<double>& times, const QVector<double>& dfs) {
    QJsonArray points;
    const int n = std::min(times.size(), dfs.size());
    for (int i = 0; i < n; ++i) {
        const double t = std::max(1e-12, times.at(i));
        points.append(QJsonObject{{"time", t}, {"discount_factor", dfs.at(i)}, {"zero_rate", -std::log(dfs.at(i)) / t}});
    }
    return points;
}

QJsonArray cashflow_leg(const QJsonObject& body, bool floating) {
    const double notional = num(body, "notional", 1000000.0);
    const double rate = floating ? num(body, "fixing_rate", num(body, "index_rate", num(body, "rate", 0.0))) + num(body, "spread", 0.0)
                                 : num(body, "rate", 0.05);
    QDate start = iso_date(str(body, "start_date", "2024-01-01"));
    const QDate end = iso_date(str(body, "end_date", start.addYears(1).toString(Qt::ISODate)));
    const int months = std::max(1, months_from_tenor(str(body, "frequency", "6M")));
    const QString day_count = str(body, "day_count", str(body, "convention", "ACT/365"));
    QJsonArray cashflows;
    while (start.isValid() && end.isValid() && start < end) {
        const QDate next = std::min(start.addMonths(months), end);
        const double yf = year_fraction(start, next, day_count);
        cashflows.append(QJsonObject{{"start_date", start.toString(Qt::ISODate)},
                                     {"end_date", next.toString(Qt::ISODate)},
                                     {"year_fraction", yf},
                                     {"rate", rate},
                                     {"amount", notional * rate * yf}});
        start = next;
    }
    return cashflows;
}

QJsonObject bond_cashflows(const QJsonObject& body) {
    const double face = num(body, "face", num(body, "notional", 100.0));
    const double coupon = num(body, "coupon_rate", num(body, "coupon", 0.05));
    const int frequency = std::max(1, static_cast<int>(num(body, "frequency", 2.0)));
    const double maturity = std::max(1e-12, num(body, "maturity", num(body, "T", 5.0)));
    const int periods = std::max(1, static_cast<int>(std::round(maturity * frequency)));
    QJsonArray flows;
    for (int i = 1; i <= periods; ++i) {
        const bool last = i == periods;
        flows.append(QJsonObject{{"time", i / static_cast<double>(frequency)},
                                 {"amount", face * coupon / frequency + (last ? face : 0.0)}});
    }
    return QJsonObject{{"cashflows", flows}, {"periods", periods}, {"frequency", frequency}};
}

double bond_price_from_yield(const QJsonObject& body, double y) {
    const double face = num(body, "face", num(body, "notional", 100.0));
    const double coupon = num(body, "coupon_rate", num(body, "coupon", 0.05));
    const int frequency = std::max(1, static_cast<int>(num(body, "frequency", 2.0)));
    const double maturity = std::max(1e-12, num(body, "maturity", num(body, "T", 5.0)));
    const int periods = std::max(1, static_cast<int>(std::round(maturity * frequency)));
    double price = 0.0;
    for (int i = 1; i <= periods; ++i) {
        const double cf = face * coupon / frequency + (i == periods ? face : 0.0);
        price += cf / std::pow(1.0 + y / frequency, i);
    }
    return price;
}

double solve_bond_yield(const QJsonObject& body) {
    const double target = num(body, "price", 100.0);
    double lo = -0.95;
    double hi = 2.0;
    for (int i = 0; i < 100; ++i) {
        const double mid = 0.5 * (lo + hi);
        const double price = bond_price_from_yield(body, mid);
        if (price > target)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5 * (lo + hi);
}

QJsonObject ratio_result(const QString& name, double numerator, double denominator) {
    return QJsonObject{{"ratio", name}, {"value", safe_div(numerator, denominator)}, {"numerator", numerator}, {"denominator", denominator}, {"engine", "local"}};
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
        QString from = str(body, "from_type", str(body, "from", "annual")).toLower();
        QString to = str(body, "to_type", str(body, "to", "continuous")).toLower();
        const QString direction = str(body, "direction").toLower();
        if (direction.contains("continuous_to_annual")) {
            from = "continuous";
            to = "annual";
        } else if (direction.contains("annual_to_continuous")) {
            from = "annual";
            to = "continuous";
        }
        const double t = std::max(1e-12, num(body, "t", 1.0));
        const double continuous = from == "continuous" ? value : std::log1p(value);
        const double converted = to == "continuous" ? continuous : std::expm1(continuous);
        return mcp::ToolResult::ok_data(QJsonObject{{"input", value},
                                                    {"output", converted},
                                                    {"from", from},
                                                    {"to", to},
                                                    {"continuous_rate", continuous},
                                                    {"annual_rate", std::expm1(continuous)},
                                                    {"discount_factor", std::exp(-continuous * t)}});
    }
    if (endpoint == "core/types/notional-schedule") {
        const double notional = num(body, "notional", 1.0);
        const int periods = std::max(1, static_cast<int>(num(body, "periods", 1)));
        const QString type = str(body, "schedule_type", "constant").toLower();
        QJsonArray schedule;
        for (int i = 0; i < periods; ++i) {
            double current = notional;
            if (type.contains("linear") || type.contains("amort") || type.contains("declin"))
                current = notional * (periods - i) / periods;
            else if (type.contains("bullet"))
                current = i == periods - 1 ? notional : 0.0;
            else if (type.contains("accret"))
                current = notional * (i + 1) / periods;
            schedule.append(QJsonObject{{"period", i + 1}, {"notional", current}});
        }
        return mcp::ToolResult::ok_data(schedule);
    }
    if (endpoint == "core/types/tenor/add-to-date") {
        const QDate start = iso_date(str(body, "start_date", str(body, "date")));
        const QString tenor = str(body, "tenor", "3M");
        const int months = months_from_tenor(tenor);
        if (!start.isValid() || months <= 0)
            return mcp::ToolResult::fail("start_date must be YYYY-MM-DD and tenor must look like 3M or 1Y");
        const QDate result = start.addMonths(months);
        return mcp::ToolResult::ok_data(QJsonObject{{"start_date", start.toString(Qt::ISODate)}, {"tenor", tenor}, {"result_date", result.toString(Qt::ISODate)}});
    }
    if (endpoint == "core/conventions/parse-date") {
        const QString fmt = qt_date_format(str(body, "format", "%Y-%m-%d"));
        const QDate date = QDate::fromString(str(body, "date_string", str(body, "date")), fmt);
        if (!date.isValid())
            return mcp::ToolResult::fail("date_string does not match format");
        return mcp::ToolResult::ok_data(QJsonObject{{"date", date.toString(Qt::ISODate)}, {"year", date.year()}, {"month", date.month()}, {"day", date.day()}});
    }
    if (endpoint == "core/conventions/format-date") {
        const QDate date = iso_date(str(body, "date_str", str(body, "date", "2024-01-01")));
        if (!date.isValid())
            return mcp::ToolResult::fail("date_str must be YYYY-MM-DD");
        return mcp::ToolResult::ok_data(QJsonObject{{"formatted", date.toString(qt_date_format(str(body, "format", "%Y-%m-%d")))}, {"date", date.toString(Qt::ISODate)}});
    }
    if (endpoint == "core/conventions/normalize-rate") {
        double value = num(body, "value", num(body, "rate", 0.0));
        const QString comp = str(body, "compounding", str(body, "unit", "decimal")).toLower();
        if (comp.contains("percent") || std::abs(value) > 1.0)
            value /= 100.0;
        if (comp.contains("bp"))
            value /= 10000.0;
        return mcp::ToolResult::ok_data(QJsonObject{{"normalized", value}, {"decimal", value}, {"percentage", value * 100.0}, {"bps", value * 10000.0}});
    }
    if (endpoint == "core/conventions/normalize-volatility") {
        double value = num(body, "value", num(body, "volatility", 0.2));
        if (value > 1.0)
            value /= 100.0;
        return mcp::ToolResult::ok_data(QJsonObject{{"normalized", value}, {"annualized", value}, {"variance", value * value}});
    }
    if (endpoint == "core/autodiff/dual-eval") {
        const QString fn = str(body, "func_name", "sin");
        const double x = num(body, "x", 0.0);
        const double h = 1e-5;
        return mcp::ToolResult::ok_data(QJsonObject{{"x", x}, {"f_x", unary_function(fn, x)}, {"f_prime_x", (unary_function(fn, x + h) - unary_function(fn, x - h)) / (2.0 * h)}, {"function", fn}});
    }
    if (endpoint == "core/autodiff/gradient") {
        const QString fn = str(body, "func_name", "sin");
        QVector<double> xs = nums(body, "x");
        if (xs.isEmpty())
            xs.append(num(body, "x", 0.0));
        QJsonArray gradient;
        const double h = 1e-5;
        for (double x : xs)
            gradient.append((unary_function(fn, x + h) - unary_function(fn, x - h)) / (2.0 * h));
        return mcp::ToolResult::ok_data(QJsonObject{{"function", fn}, {"gradient", gradient}});
    }
    if (endpoint == "core/autodiff/taylor-expand") {
        const QString fn = str(body, "func_name", "sin");
        const double x0 = num(body, "x0", 0.0);
        const int order = std::max(1, static_cast<int>(num(body, "order", 3)));
        const double f0 = unary_function(fn, x0);
        const double h = 1e-4;
        const double f1 = (unary_function(fn, x0 + h) - unary_function(fn, x0 - h)) / (2.0 * h);
        const double f2 = (unary_function(fn, x0 + h) - 2.0 * f0 + unary_function(fn, x0 - h)) / (h * h);
        QJsonArray coefficients{f0};
        if (order >= 1)
            coefficients.append(f1);
        if (order >= 2)
            coefficients.append(f2 / 2.0);
        for (int i = 3; i <= order; ++i)
            coefficients.append(0.0);
        return mcp::ToolResult::ok_data(QJsonObject{{"function", fn}, {"x0", x0}, {"order", order}, {"coefficients", coefficients}});
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
    if (endpoint == "core/distributions/t/pdf" || endpoint == "statistics/distributions/student-t/pdf") {
        const double x = num(body, "x");
        const double df = num(body, "df", 10.0);
        return mcp::ToolResult::ok_data(QJsonObject{{"x", x}, {"df", df}, {"pdf", student_t_pdf(x, df)}});
    }
    if (endpoint == "core/distributions/t/cdf" || endpoint == "statistics/distributions/student-t/cdf") {
        const double x = num(body, "x");
        const double df = num(body, "df", 10.0);
        return mcp::ToolResult::ok_data(QJsonObject{{"x", x}, {"df", df}, {"cdf", student_t_cdf(x, df)}});
    }
    if (endpoint == "core/distributions/t/ppf") {
        const double p = num(body, "p", 0.5);
        const double df = num(body, "df", 10.0);
        double lo = -20.0;
        double hi = 20.0;
        for (int i = 0; i < 80; ++i) {
            const double mid = 0.5 * (lo + hi);
            if (student_t_cdf(mid, df) < p)
                lo = mid;
            else
                hi = mid;
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"p", p}, {"df", df}, {"ppf", 0.5 * (lo + hi)}});
    }
    if (endpoint == "core/distributions/chi2/pdf" || endpoint == "statistics/distributions/chi-squared/pdf") {
        const double x = num(body, "x");
        const double df = num(body, "df", 1.0);
        return mcp::ToolResult::ok_data(QJsonObject{{"x", x}, {"df", df}, {"pdf", gamma_pdf_value(x, df / 2.0, 0.5)}});
    }
    if (endpoint == "core/distributions/chi2/cdf" || endpoint == "statistics/distributions/chi-squared/cdf") {
        const double x = num(body, "x");
        const double df = num(body, "df", 1.0);
        return mcp::ToolResult::ok_data(QJsonObject{{"x", x}, {"df", df}, {"cdf", regularized_gamma_p(df / 2.0, x / 2.0)}});
    }
    if (endpoint == "core/distributions/gamma/pdf" || endpoint == "statistics/distributions/gamma/pdf") {
        const double x = num(body, "x");
        const double alpha = num(body, "alpha", num(body, "shape", 2.0));
        const double beta = num(body, "beta", num(body, "rate", 1.0));
        return mcp::ToolResult::ok_data(QJsonObject{{"x", x}, {"alpha", alpha}, {"beta", beta}, {"pdf", gamma_pdf_value(x, alpha, beta)}});
    }
    if (endpoint == "core/distributions/gamma/cdf" || endpoint == "statistics/distributions/gamma/cdf") {
        const double x = num(body, "x");
        const double alpha = num(body, "alpha", num(body, "shape", 2.0));
        const double beta = num(body, "beta", num(body, "rate", 1.0));
        return mcp::ToolResult::ok_data(QJsonObject{{"x", x}, {"alpha", alpha}, {"beta", beta}, {"cdf", regularized_gamma_p(alpha, beta * x)}});
    }
    if (endpoint == "core/distributions/exponential/pdf" || endpoint == "statistics/distributions/exponential/pdf") {
        const double x = num(body, "x");
        const double rate = num(body, "rate", num(body, "lambda", 1.0));
        return mcp::ToolResult::ok_data(QJsonObject{{"x", x}, {"rate", rate}, {"pdf", x < 0.0 ? 0.0 : rate * std::exp(-rate * x)}});
    }
    if (endpoint == "core/distributions/exponential/cdf" || endpoint == "statistics/distributions/exponential/cdf") {
        const double x = num(body, "x");
        const double rate = num(body, "rate", num(body, "lambda", 1.0));
        return mcp::ToolResult::ok_data(QJsonObject{{"x", x}, {"rate", rate}, {"cdf", x < 0.0 ? 0.0 : 1.0 - std::exp(-rate * x)}});
    }
    if (endpoint == "core/distributions/exponential/ppf" || endpoint == "statistics/distributions/exponential/ppf") {
        const double p = std::clamp(num(body, "p", 0.5), 0.0, 1.0 - 1e-15);
        const double rate = num(body, "rate", num(body, "lambda", 1.0));
        return mcp::ToolResult::ok_data(QJsonObject{{"p", p}, {"rate", rate}, {"ppf", -std::log(1.0 - p) / rate}});
    }
    if (endpoint == "core/distributions/bivariate-normal/cdf") {
        const double x = num(body, "x");
        const double y = num(body, "y");
        const double rho = std::clamp(num(body, "rho", 0.0), -0.999, 0.999);
        const double lower = -8.0;
        const double upper = std::min(x, 8.0);
        const double cdf = integrate_simpson([=](double u) {
            return norm_pdf(u) * norm_cdf((y - rho * u) / std::sqrt(1.0 - rho * rho));
        }, lower, upper, 700);
        return mcp::ToolResult::ok_data(QJsonObject{{"x", x}, {"y", y}, {"rho", rho}, {"cdf", std::clamp(cdf, 0.0, 1.0)}});
    }
    if (endpoint == "core/math/eval") {
        const QString fn = str(body, "func_name", str(body, "function", "sqrt"));
        const double x = num(body, "x", 0.0);
        return mcp::ToolResult::ok_data(QJsonObject{{"function", fn}, {"x", x}, {"result", unary_function(fn, x)}});
    }
    if (endpoint == "core/math/two-arg") {
        const QString fn = str(body, "func_name", str(body, "function", "power"));
        const double x = num(body, "x", 0.0);
        const double y = num(body, "y", 0.0);
        const double result = binary_function(fn, x, y);
        if (!std::isfinite(result))
            return mcp::ToolResult::fail("Unknown two-argument function");
        return mcp::ToolResult::ok_data(QJsonObject{{"function", fn}, {"x", x}, {"y", y}, {"result", result}});
    }
    if (endpoint == "core/ops/discount-cashflows") {
        const QVector<double> cashflows = nums(body, "cashflows");
        const QVector<double> dfs = nums(body, "discount_factors");
        const QVector<double> times = nums(body, "times");
        const double rate = num(body, "rate", num(body, "r", 0.0));
        double pv = 0.0;
        for (int i = 0; i < cashflows.size(); ++i) {
            const double df = i < dfs.size() ? dfs.at(i) : std::exp(-rate * (i < times.size() ? times.at(i) : i + 1.0));
            pv += cashflows.at(i) * df;
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"present_value", pv}, {"engine", "local"}});
    }
    if (endpoint == "core/ops/interpolate" || endpoint == "numerical/interpolation/evaluate") {
        const QVector<double> xs = nums_any(body, {"x_data", "xs", "x_points"});
        const QVector<double> ys = nums_any(body, {"y_data", "ys", "y_points"});
        const double x = num(body, "x", num(body, "query", 0.0));
        if (xs.size() != ys.size() || xs.isEmpty())
            return mcp::ToolResult::fail("x_data and y_data must be same-length numeric arrays");
        return mcp::ToolResult::ok_data(QJsonObject{{"method", "linear"}, {"x", x}, {"interpolated_value", linear_interpolate(xs, ys, x)}});
    }
    if (endpoint == "core/ops/covariance-matrix" || endpoint == "ml/covariance/estimate") {
        const QVector<QVector<double>> observations = matrix_any(body, {"returns", "values", "observations"});
        if (observations.isEmpty())
            return mcp::ToolResult::fail("returns must be a non-empty matrix");
        return mcp::ToolResult::ok_data(QJsonObject{{"covariance_matrix", to_json(covariance_matrix(observations))}, {"engine", "local"}});
    }
    if (endpoint == "core/ops/cholesky" || endpoint == "numerical/linalg/decompose") {
        const QVector<QVector<double>> a = matrix_any(body, {"matrix", "covariance", "cov"});
        if (a.isEmpty())
            return mcp::ToolResult::fail("matrix must be a square numeric matrix");
        const int n = a.size();
        QVector<QVector<double>> l(n, QVector<double>(n, 0.0));
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j <= i; ++j) {
                double s = 0.0;
                for (int k = 0; k < j; ++k)
                    s += l.at(i).at(k) * l.at(j).at(k);
                if (i == j)
                    l[i][j] = std::sqrt(std::max(0.0, a.at(i).value(i) - s));
                else
                    l[i][j] = safe_div(a.at(i).value(j) - s, l.at(j).at(j));
            }
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"cholesky", to_json(l)}, {"engine", "local"}});
    }
    if (endpoint == "core/ops/gbm-paths" || endpoint == "stochastic/gbm/simulate" ||
        endpoint == "stochastic/exact/gbm" || endpoint == "stochastic/sampling/antithetic") {
        const double s0 = num(body, "S0", num(body, "spot", 100.0));
        const double mu = num(body, "mu", num(body, "drift", 0.05));
        const double sigma = num(body, "sigma", num(body, "volatility", 0.2));
        const double T = std::max(1e-12, num(body, "T", num(body, "time", 1.0)));
        const int steps = std::max(1, static_cast<int>(num(body, "n_steps", endpoint.contains("exact") ? 1.0 : 52.0)));
        const int paths = std::max(1, static_cast<int>(num(body, "n_paths", num(body, "n", 3.0))));
        std::mt19937 rng(static_cast<unsigned>(num(body, "seed", 42.0)));
        std::normal_distribution<double> nd(0.0, 1.0);
        const double dt = T / steps;
        QJsonArray out;
        for (int p = 0; p < paths; ++p) {
            double s = s0;
            QJsonArray path{s};
            for (int i = 0; i < steps; ++i) {
                const double z = nd(rng);
                s *= std::exp((mu - 0.5 * sigma * sigma) * dt + sigma * std::sqrt(dt) * z);
                path.append(s);
            }
            out.append(path);
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"n_paths", paths}, {"n_steps", steps}, {"paths", out}, {"engine", "local"}});
    }

    if (endpoint == "pricing/bs/price" || endpoint == "pricing/bs/greeks" ||
        endpoint == "pricing/bs/greeks-full" || endpoint == "core/ops/black-scholes")
        return mcp::ToolResult::ok_data(bs_values(body));
    if (endpoint == "pricing/black76/price" || endpoint == "pricing/black76/greeks" ||
        endpoint == "pricing/black76/greeks-full" || endpoint == "core/ops/black76")
        return mcp::ToolResult::ok_data(black76_values(body));
    if (endpoint == "pricing/bs/digital-call" || endpoint == "pricing/bs/digital-put" ||
        endpoint == "pricing/bs/asset-or-nothing-call" || endpoint == "pricing/bs/asset-or-nothing-put") {
        QJsonObject work = body;
        work["option_type"] = endpoint.contains("put") ? "put" : "call";
        const QJsonObject values = bs_values(work);
        const double S = num(body, "S", num(body, "spot", num(body, "S0", 100.0)));
        const double r = num(body, "r", num(body, "rate", num(body, "risk_free_rate", 0.05)));
        const double q = num(body, "q", num(body, "dividend_yield", 0.0));
        const double T = std::max(1e-12, num(body, "T", num(body, "time", num(body, "time_to_maturity", 1.0))));
        const double d1 = values.value("d1").toDouble();
        const double d2 = values.value("d2").toDouble();
        const bool call = endpoint.contains("call");
        const bool asset = endpoint.contains("asset-or-nothing");
        const double price = asset ? S * std::exp(-q * T) * (call ? norm_cdf(d1) : norm_cdf(-d1))
                                   : std::exp(-r * T) * (call ? norm_cdf(d2) : norm_cdf(-d2));
        return mcp::ToolResult::ok_data(QJsonObject{{"engine", "local"}, {"price", price}, {"payoff", asset ? "asset_or_nothing" : "cash_or_nothing"}, {"option_type", call ? "call" : "put"}});
    }
    if (endpoint == "pricing/black76/caplet" || endpoint == "pricing/black76/floorlet") {
        const double forward = num(body, "forward_rate", num(body, "forward", 0.05));
        const double strike = num(body, "strike", 0.05);
        const double notional = num(body, "notional", 1000000.0);
        const double t0 = num(body, "t_start", 1.0);
        const double t1 = num(body, "t_end", t0 + 0.25);
        QJsonObject work{{"forward", forward}, {"strike", strike}, {"discount_factor", num(body, "discount_factor", 1.0)}, {"volatility", num(body, "volatility", 0.2)}, {"time_to_maturity", t0}, {"option_type", endpoint.contains("floorlet") ? "put" : "call"}};
        const double option = black76_values(work).value("price").toDouble();
        return mcp::ToolResult::ok_data(QJsonObject{{"price", notional * std::max(0.0, t1 - t0) * option}, {"option_unit_price", option}, {"engine", "local"}});
    }
    if (endpoint == "pricing/black76/swaption") {
        QJsonObject work{{"forward", num(body, "forward_swap_rate", num(body, "forward", 0.05))},
                         {"strike", num(body, "strike", 0.05)},
                         {"risk_free_rate", num(body, "risk_free_rate", 0.05)},
                         {"volatility", num(body, "volatility", 0.2)},
                         {"time_to_maturity", num(body, "t_expiry", num(body, "time_to_maturity", 1.0))},
                         {"option_type", str(body, "option_type", "call")}};
        const double unit = black76_values(work).value("price").toDouble();
        return mcp::ToolResult::ok_data(QJsonObject{{"price", unit * num(body, "annuity", 1.0) * num(body, "notional", 1000000.0)}, {"unit_price", unit}, {"engine", "local"}});
    }
    if (endpoint == "pricing/bachelier/price" || endpoint == "pricing/bachelier/greeks" ||
        endpoint == "pricing/bachelier/greeks-full")
        return mcp::ToolResult::ok_data(bachelier_values(body));
    if (endpoint == "pricing/bachelier/implied-vol") {
        const double target = num(body, "market_price", num(body, "price", 5.0));
        double lo = 1e-6;
        double hi = std::max(1.0, num(body, "forward", 100.0) * 2.0);
        QJsonObject work = body;
        for (int i = 0; i < 80; ++i) {
            const double mid = 0.5 * (lo + hi);
            work["normal_volatility"] = mid;
            if (bachelier_values(work).value("price").toDouble() > target)
                hi = mid;
            else
                lo = mid;
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"implied_normal_vol", 0.5 * (lo + hi)}, {"target_price", target}, {"engine", "local"}});
    }
    if (endpoint == "pricing/bachelier/shifted-lognormal") {
        QJsonObject work = body;
        work["forward"] = num(body, "forward", 100.0) + num(body, "shift", 0.0);
        work["strike"] = num(body, "strike", 100.0) + num(body, "shift", 0.0);
        return mcp::ToolResult::ok_data(bs_values(work));
    }
    if (endpoint == "pricing/bachelier/vol-conversion") {
        const double normal_vol = num(body, "normal_vol", num(body, "normal_volatility", 5.0));
        const double forward = num(body, "forward", 100.0);
        const double strike = num(body, "strike", forward);
        const double atm = std::max(1e-12, 0.5 * (forward + strike));
        return mcp::ToolResult::ok_data(QJsonObject{{"normal_vol", normal_vol}, {"lognormal_vol", normal_vol / atm}, {"engine", "local"}});
    }
    if (endpoint.startsWith("pricing/binomial/")) {
        const double S = num(body, "spot", num(body, "S", 100.0));
        const double K = num(body, "strike", num(body, "K", 100.0));
        const double r = num(body, "risk_free_rate", num(body, "r", 0.05));
        const double q = num(body, "dividend_yield", num(body, "q", 0.0));
        const double sigma = std::max(1e-12, num(body, "volatility", num(body, "sigma", 0.2)));
        const double T = std::max(1e-12, num(body, "time_to_maturity", num(body, "T", 1.0)));
        const int steps = std::clamp(static_cast<int>(num(body, "steps", 100.0)), 1, 1000);
        const bool call = str(body, "option_type", "call").toLower() != "put";
        const bool american = endpoint.contains("american");
        const bool bermudan = endpoint.contains("bermudan");
        const double barrier = num(body, "barrier", std::numeric_limits<double>::quiet_NaN());
        const bool barrier_endpoint = endpoint.contains("barrier");
        const bool knock_in = body.value("is_knock_in").toBool(false);
        const bool down = body.value("is_down").toBool(false);
        const double dt = T / steps;
        const double u = std::exp(sigma * std::sqrt(dt));
        const double d = 1.0 / u;
        const double disc = std::exp(-r * dt);
        const double p = std::clamp((std::exp((r - q) * dt) - d) / (u - d), 0.0, 1.0);
        QVector<double> values(steps + 1);
        for (int j = 0; j <= steps; ++j) {
            const double spot = S * std::pow(u, j) * std::pow(d, steps - j);
            double payoff = std::max(call ? spot - K : K - spot, 0.0);
            if (barrier_endpoint && std::isfinite(barrier)) {
                const bool touched = down ? spot <= barrier : spot >= barrier;
                payoff = knock_in == touched ? payoff : 0.0;
            }
            values[j] = payoff;
        }
        const QVector<double> exercise_dates = nums(body, "exercise_dates");
        for (int i = steps - 1; i >= 0; --i) {
            const double t = i * dt;
            bool exercise_allowed = american;
            if (bermudan) {
                for (double ex : exercise_dates) {
                    if (std::abs(ex - t) <= dt / 2.0) {
                        exercise_allowed = true;
                        break;
                    }
                }
            }
            for (int j = 0; j <= i; ++j) {
                const double continuation = disc * (p * values[j + 1] + (1.0 - p) * values[j]);
                const double spot = S * std::pow(u, j) * std::pow(d, i - j);
                const double exercise = std::max(call ? spot - K : K - spot, 0.0);
                values[j] = exercise_allowed ? std::max(continuation, exercise) : continuation;
            }
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"price", values.first()}, {"steps", steps}, {"engine", "local"}});
    }
    if (endpoint == "pricing/kirk/spread-price" || endpoint == "pricing/kirk/spread-greeks") {
        const double F1 = num(body, "F1", 100.0);
        const double F2 = num(body, "F2", 95.0);
        const double K = num(body, "strike", 5.0);
        const double sigma1 = num(body, "sigma1", 0.2);
        const double sigma2 = num(body, "sigma2", 0.18);
        const double rho = num(body, "rho", 0.0);
        const double denom = F2 + K;
        const double sigma = std::sqrt(std::max(0.0, sigma1 * sigma1 - 2.0 * rho * sigma1 * sigma2 * F2 / denom + sigma2 * sigma2 * F2 * F2 / (denom * denom)));
        QJsonObject work{{"forward", F1}, {"strike", denom}, {"volatility", sigma}, {"risk_free_rate", num(body, "risk_free_rate", 0.05)}, {"time_to_maturity", num(body, "time_to_maturity", 1.0)}, {"option_type", "call"}};
        QJsonObject result = black76_values(work);
        result["model"] = "kirk_spread";
        result["effective_volatility"] = sigma;
        return mcp::ToolResult::ok_data(result);
    }
    if (endpoint == "pricing/margrabe") {
        const double S1 = num(body, "S1", 100.0);
        const double S2 = num(body, "S2", 95.0);
        const double sigma1 = num(body, "sigma1", 0.2);
        const double sigma2 = num(body, "sigma2", 0.18);
        const double rho = num(body, "rho", 0.0);
        const double T = num(body, "time_to_maturity", num(body, "T", 1.0));
        const double q1 = num(body, "Q1", num(body, "q1", 0.0));
        const double q2 = num(body, "Q2", num(body, "q2", 0.0));
        const double sigma = std::sqrt(std::max(0.0, sigma1 * sigma1 + sigma2 * sigma2 - 2.0 * rho * sigma1 * sigma2));
        const double d1 = (std::log(S1 / S2) + (q2 - q1 + 0.5 * sigma * sigma) * T) / (sigma * std::sqrt(T));
        const double d2 = d1 - sigma * std::sqrt(T);
        const double price = S1 * std::exp(-q1 * T) * norm_cdf(d1) - S2 * std::exp(-q2 * T) * norm_cdf(d2);
        return mcp::ToolResult::ok_data(QJsonObject{{"price", price}, {"effective_volatility", sigma}, {"d1", d1}, {"d2", d2}, {"engine", "local"}});
    }
    if (endpoint == "pricing/basket-levy") {
        const QVector<double> forwards = nums(body, "forwards");
        QVector<double> weights = nums(body, "weights");
        const QVector<double> sigmas = nums(body, "sigmas");
        if (forwards.isEmpty() || sigmas.size() != forwards.size())
            return mcp::ToolResult::fail("forwards and sigmas must be same-length arrays");
        if (weights.size() != forwards.size())
            weights = QVector<double>(forwards.size(), 1.0 / forwards.size());
        const QVector<double> corrs_flat = nums(body, "correlations");
        double forward = 0.0;
        double variance_sum = 0.0;
        for (int i = 0; i < forwards.size(); ++i)
            forward += weights.at(i) * forwards.at(i);
        for (int i = 0; i < forwards.size(); ++i) {
            for (int j = 0; j < forwards.size(); ++j) {
                const double rho = corrs_flat.size() == forwards.size() * forwards.size() ? corrs_flat.at(i * forwards.size() + j) : (i == j ? 1.0 : 0.0);
                variance_sum += weights.at(i) * weights.at(j) * forwards.at(i) * forwards.at(j) * sigmas.at(i) * sigmas.at(j) * rho;
            }
        }
        const double vol = std::sqrt(std::max(0.0, variance_sum)) / std::max(1e-12, forward);
        QJsonObject work{{"forward", forward}, {"strike", num(body, "strike", forward)}, {"volatility", vol}, {"risk_free_rate", num(body, "risk_free_rate", 0.05)}, {"time_to_maturity", num(body, "time_to_maturity", 1.0)}, {"option_type", str(body, "option_type", "call")}};
        QJsonObject result = black76_values(work);
        result["model"] = "levy_moment_matching_basket";
        result["basket_forward"] = forward;
        result["basket_volatility"] = vol;
        return mcp::ToolResult::ok_data(result);
    }
    if (endpoint == "pricing/bs/implied-vol" || endpoint == "solver/finance/implied-vol") {
        const double target = num(body, "market_price", num(body, "price", num(body, "target_price", 10.0)));
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
    if (endpoint == "pricing/black76/implied-vol" || endpoint == "solver/finance/implied-vol-black76") {
        const double target = num(body, "market_price", num(body, "price", num(body, "target_price", 10.0)));
        double lo = 1e-4;
        double hi = 5.0;
        QJsonObject work = body;
        for (int i = 0; i < 80; ++i) {
            const double mid = 0.5 * (lo + hi);
            work["volatility"] = mid;
            const double price = black76_values(work).value("price").toDouble();
            if (price > target)
                hi = mid;
            else
                lo = mid;
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"implied_vol", 0.5 * (lo + hi)}, {"target_price", target}, {"engine", "local"}});
    }

    if (endpoint == "core/ops/forward-rate" || endpoint == "solver/finance/forward-rate") {
        const double df1 = num(body, "df1", -1.0);
        const double df2 = num(body, "df2", -1.0);
        const double t1 = num(body, "t1", 1.0);
        const double t2 = std::max(t1 + 1e-12, num(body, "t2", 2.0));
        double fwd = 0.0;
        if (df1 > 0.0 && df2 > 0.0)
            fwd = (df1 / df2 - 1.0) / (t2 - t1);
        else {
            const double r1 = num(body, "r1", num(body, "short_rate", 0.03));
            const double r2 = num(body, "r2", num(body, "long_rate", 0.04));
            fwd = (r2 * t2 - r1 * t1) / (t2 - t1);
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"forward_rate", fwd}, {"engine", "local"}});
    }
    if (endpoint == "solver/finance/discount-factor" || endpoint == "curves/discount-factor") {
        const double r = num(body, "rate", num(body, "r", 0.05));
        const double t = num(body, "time", num(body, "T", num(body, "maturity", 1.0)));
        return mcp::ToolResult::ok_data(QJsonObject{{"discount_factor", std::exp(-r * t)}});
    }
    if (endpoint == "solver/finance/zero-rate" || endpoint == "curves/zero-rate") {
        const double df = num(body, "discount_factor", num(body, "df", 0.95));
        const double t = std::max(1e-12, num(body, "time", num(body, "T", num(body, "maturity", 1.0))));
        return mcp::ToolResult::ok_data(QJsonObject{{"zero_rate", -std::log(df) / t}, {"time", t}});
    }
    if (endpoint == "curves/build" || endpoint == "curves/curve-points") {
        QVector<double> times;
        const QVector<double> dfs = discount_factors_from_curve(body, &times);
        if (dfs.isEmpty())
            return mcp::ToolResult::fail("curve requires discount_factors or zero_rates/rates");
        return mcp::ToolResult::ok_data(QJsonObject{{"points", curve_points(times, dfs)}, {"engine", "local"}});
    }
    if (endpoint == "curves/forward-rate" || endpoint == "curves/instantaneous-forward") {
        QVector<double> times;
        const QVector<double> dfs = discount_factors_from_curve(body, &times);
        const double t1 = num(body, "t1", num(body, "start", 1.0));
        const double t2 = num(body, "t2", num(body, "end", t1 + 1.0));
        if (!dfs.isEmpty()) {
            const double df1 = linear_interpolate(times, dfs, t1);
            const double df2 = linear_interpolate(times, dfs, t2);
            return mcp::ToolResult::ok_data(QJsonObject{{"forward_rate", (df1 / df2 - 1.0) / (t2 - t1)}, {"engine", "local"}});
        }
        return local_quantlib_call("core/ops/forward-rate", body);
    }
    if (endpoint == "curves/interpolate" || endpoint == "curves/interpolate-derivative") {
        QVector<double> times;
        const QVector<double> dfs = discount_factors_from_curve(body, &times);
        const double t = num(body, "time", num(body, "x", 1.0));
        if (dfs.isEmpty())
            return mcp::ToolResult::fail("curve requires discount_factors or zero_rates/rates");
        if (endpoint.endsWith("derivative")) {
            const double h = 1e-4;
            const double derivative = (linear_interpolate(times, dfs, t + h) - linear_interpolate(times, dfs, t - h)) / (2.0 * h);
            return mcp::ToolResult::ok_data(QJsonObject{{"time", t}, {"derivative", derivative}, {"engine", "local"}});
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"time", t}, {"discount_factor", linear_interpolate(times, dfs, t)}, {"engine", "local"}});
    }
    if (endpoint == "curves/parallel-shift" || endpoint == "curves/twist" || endpoint == "curves/butterfly" ||
        endpoint == "curves/key-rate-shift" || endpoint == "curves/roll" || endpoint == "curves/scale" ||
        endpoint == "curves/time-shift") {
        QVector<double> times;
        QVector<double> dfs = discount_factors_from_curve(body, &times);
        if (dfs.isEmpty())
            return mcp::ToolResult::fail("curve requires discount_factors or zero_rates/rates");
        const double shift = num(body, "shift", num(body, "bps", 0.0) / 10000.0);
        const double scale = num(body, "scale", 1.0);
        const double time_shift = num(body, "time_shift", 0.0);
        QJsonArray points;
        for (int i = 0; i < dfs.size() && i < times.size(); ++i) {
            double t = std::max(1e-12, times.at(i) + (endpoint == "curves/time-shift" || endpoint == "curves/roll" ? time_shift : 0.0));
            double rate = -std::log(dfs.at(i)) / std::max(1e-12, times.at(i));
            if (endpoint == "curves/scale")
                rate *= scale;
            else
                rate += shift;
            points.append(QJsonObject{{"time", t}, {"zero_rate", rate}, {"discount_factor", std::exp(-rate * t)}});
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"points", points}, {"engine", "local"}});
    }
    if (endpoint == "curves/nelson-siegel/evaluate" || endpoint == "curves/nss/evaluate") {
        const double t = std::max(1e-12, num(body, "time", num(body, "t", 1.0)));
        const double beta0 = num(body, "beta0", 0.03);
        const double beta1 = num(body, "beta1", -0.01);
        const double beta2 = num(body, "beta2", 0.01);
        const double beta3 = endpoint.contains("nss") ? num(body, "beta3", 0.0) : 0.0;
        const double tau1 = std::max(1e-12, num(body, "tau1", num(body, "tau", 2.0)));
        const double tau2 = std::max(1e-12, num(body, "tau2", 5.0));
        const double x1 = t / tau1;
        const double f1 = (1.0 - std::exp(-x1)) / x1;
        const double rate = beta0 + beta1 * f1 + beta2 * (f1 - std::exp(-x1)) +
                            beta3 * (((1.0 - std::exp(-t / tau2)) / (t / tau2)) - std::exp(-t / tau2));
        return mcp::ToolResult::ok_data(QJsonObject{{"time", t}, {"zero_rate", rate}, {"discount_factor", std::exp(-rate * t)}, {"engine", "local"}});
    }
    if (endpoint == "curves/monotonicity-check" || endpoint == "curves/smoothness-penalty") {
        QVector<double> times;
        const QVector<double> dfs = discount_factors_from_curve(body, &times);
        if (dfs.isEmpty())
            return mcp::ToolResult::fail("curve requires discount_factors or zero_rates/rates");
        bool monotone = true;
        double penalty = 0.0;
        for (int i = 1; i < dfs.size(); ++i) {
            if (dfs.at(i) > dfs.at(i - 1))
                monotone = false;
        }
        QVector<double> rates;
        for (int i = 0; i < dfs.size() && i < times.size(); ++i)
            rates.append(-std::log(dfs.at(i)) / std::max(1e-12, times.at(i)));
        for (int i = 2; i < rates.size(); ++i) {
            const double second = rates.at(i) - 2.0 * rates.at(i - 1) + rates.at(i - 2);
            penalty += second * second;
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"is_monotone", monotone}, {"smoothness_penalty", penalty}, {"engine", "local"}});
    }
    if (endpoint == "curves/real-rate") {
        const double nominal = num(body, "nominal_rate", num(body, "rate", 0.05));
        const double inflation = num(body, "inflation_rate", num(body, "inflation", 0.02));
        return mcp::ToolResult::ok_data(QJsonObject{{"real_rate", (1.0 + nominal) / (1.0 + inflation) - 1.0}, {"engine", "local"}});
    }
    if (endpoint == "curves/composite" || endpoint == "curves/proxy") {
        QVector<double> times;
        const QVector<double> dfs = discount_factors_from_curve(body, &times);
        return mcp::ToolResult::ok_data(QJsonObject{{"points", curve_points(times, dfs)}, {"method", endpoint.section('/', 1)}, {"engine", "local"}});
    }
    if (endpoint == "core/ops/statistics") {
        const QVector<double> xs = nums(body, "values");
        if (xs.isEmpty())
            return mcp::ToolResult::fail("values must be a non-empty numeric array");
        const double m = mean(xs);
        const double var = variance(xs);
        return mcp::ToolResult::ok_data(QJsonObject{{"count", xs.size()}, {"mean", m}, {"variance", var}, {"std", std::sqrt(var)}, {"min", *std::min_element(xs.begin(), xs.end())}, {"max", *std::max_element(xs.begin(), xs.end())}});
    }
    if (endpoint == "core/ops/percentile") {
        const double p = num(body, "p", num(body, "percentile", 0.95));
        const double prob = p > 1.0 ? p / 100.0 : p;
        return mcp::ToolResult::ok_data(QJsonObject{{"percentile", prob}, {"value", percentile(nums(body, "values"), prob)}});
    }
    if (endpoint == "core/ops/var" || endpoint == "risk/var/historical" || endpoint == "portfolio/risk/var") {
        const double confidence = num(body, "confidence", 0.95);
        const QVector<double> returns = historical_portfolio_returns(body);
        if (returns.isEmpty())
            return mcp::ToolResult::fail("returns must be a non-empty numeric array");
        return mcp::ToolResult::ok_data(var_result(returns, confidence));
    }
    if (endpoint == "risk/var/parametric") {
        const double mu = num(body, "mean", num(body, "mu", 0.0));
        const double sigma = num(body, "std", num(body, "sigma", 0.02));
        const double confidence = num(body, "confidence", 0.95);
        return mcp::ToolResult::ok_data(QJsonObject{{"var", -(mu + sigma * norm_inv(1.0 - confidence))}, {"confidence", confidence}, {"method", "parametric"}});
    }
    if (endpoint == "portfolio/risk/cvar" || endpoint == "risk/tail-risk/comprehensive") {
        const double confidence = num(body, "confidence", 0.95);
        const QJsonObject vr = var_result(historical_portfolio_returns(body), confidence);
        return mcp::ToolResult::ok_data(QJsonObject{{"cvar", vr.value("expected_shortfall").toDouble()}, {"var", vr.value("var").toDouble()}, {"confidence", confidence}, {"engine", "local"}});
    }
    if (endpoint == "risk/var/component" || endpoint == "risk/var/marginal" ||
        endpoint == "risk/var/incremental" || endpoint == "portfolio/risk/incremental-var" ||
        endpoint == "portfolio/risk/contribution") {
        QVector<double> weights = nums(body, "weights");
        QVector<QVector<double>> cov = matrix_any(body, {"covariance", "covariance_matrix", "cov"});
        if (cov.isEmpty())
            cov = covariance_matrix(matrix(body, "returns"));
        if (weights.isEmpty() || cov.isEmpty())
            return mcp::ToolResult::fail("weights and covariance/returns are required");
        const double port_var = std::max(0.0, portfolio_variance(weights, cov));
        const double port_vol = std::sqrt(port_var);
        QJsonArray contributions;
        for (int i = 0; i < weights.size(); ++i) {
            double marginal = 0.0;
            for (int j = 0; j < weights.size() && j < cov.size() && i < cov.at(j).size(); ++j)
                marginal += cov.at(i).value(j) * weights.at(j);
            marginal = safe_div(marginal, port_vol);
            contributions.append(QJsonObject{{"asset", i}, {"marginal_var", marginal}, {"component_var", weights.at(i) * marginal}});
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"portfolio_volatility", port_vol}, {"contributions", contributions}, {"engine", "local"}});
    }
    if (endpoint == "risk/backtest") {
        const QVector<double> returns = nums(body, "returns");
        const double var_value = num(body, "var", num(body, "var_value", 0.02));
        int exceptions = 0;
        for (double r : returns) {
            if (r < -var_value)
                ++exceptions;
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"observations", returns.size()}, {"exceptions", exceptions}, {"exception_rate", safe_div(exceptions, returns.size())}, {"engine", "local"}});
    }
    if (endpoint == "risk/stress/scenario" || endpoint == "risk/correlation-stress") {
        const QVector<double> weights = nums(body, "weights");
        const QVector<double> shocks = nums_any(body, {"shocks", "scenario_returns", "returns"});
        if (weights.isEmpty() || shocks.isEmpty())
            return mcp::ToolResult::fail("weights and shocks are required");
        return mcp::ToolResult::ok_data(QJsonObject{{"portfolio_pnl", portfolio_return(weights, shocks)}, {"engine", "local"}});
    }
    if (endpoint == "risk/sensitivities/greeks") {
        QJsonObject out = bs_values(body);
        out["source"] = "black_scholes";
        return mcp::ToolResult::ok_data(out);
    }
    if (endpoint == "risk/sensitivities/parallel-shift" || endpoint == "risk/sensitivities/twist" ||
        endpoint == "risk/sensitivities/key-rate-duration" || endpoint == "risk/sensitivities/bucket-delta") {
        const QVector<double> cashflows = nums(body, "cashflows");
        const QVector<double> times = nums(body, "times");
        const double rate = num(body, "rate", 0.05);
        const double shift = num(body, "shift", 0.0001);
        QJsonObject base{{"cashflows", to_json(cashflows)}, {"times", to_json(times)}, {"rate", rate}};
        QJsonObject bumped{{"cashflows", to_json(cashflows)}, {"times", to_json(times)}, {"rate", rate + shift}};
        const double pv0 = local_quantlib_call("core/ops/discount-cashflows", base).data.toObject().value("present_value").toDouble();
        const double pv1 = local_quantlib_call("core/ops/discount-cashflows", bumped).data.toObject().value("present_value").toDouble();
        return mcp::ToolResult::ok_data(QJsonObject{{"base_pv", pv0}, {"bumped_pv", pv1}, {"sensitivity", safe_div(pv1 - pv0, shift)}, {"engine", "local"}});
    }
    if (endpoint == "portfolio/optimize/min-variance" || endpoint == "portfolio/risk/inverse-volatility" ||
        endpoint == "portfolio/risk-parity" || endpoint == "portfolio/optimize/max-sharpe" ||
        endpoint == "portfolio/optimize/target-return") {
        QVector<QVector<double>> cov = matrix_any(body, {"covariance", "covariance_matrix", "cov"});
        if (cov.isEmpty())
            cov = covariance_matrix(matrix(body, "returns"));
        if (cov.isEmpty())
            return mcp::ToolResult::fail("covariance or returns matrix is required");
        QVector<double> weights;
        if (endpoint.contains("inverse-volatility") || endpoint.contains("risk-parity"))
            weights = inverse_vol_weights(cov);
        else
            weights = inverse_variance_weights(cov);
        const QVector<double> er = nums_any(body, {"expected_returns", "mu", "returns_mean"});
        if (endpoint.contains("max-sharpe") && er.size() == weights.size()) {
            weights.clear();
            const double rf = num(body, "risk_free_rate", 0.0);
            for (int i = 0; i < er.size(); ++i)
                weights.append(std::max(0.0, er.at(i) - rf) / std::max(1e-12, cov.at(i).value(i)));
            weights = normalize_weights(weights);
        }
        const double pret = er.size() == weights.size() ? portfolio_return(weights, er) : 0.0;
        const double pvol = std::sqrt(std::max(0.0, portfolio_variance(weights, cov)));
        return mcp::ToolResult::ok_data(QJsonObject{{"weights", to_json(weights)}, {"expected_return", pret}, {"volatility", pvol}, {"sharpe", safe_div(pret - num(body, "risk_free_rate", 0.0), pvol)}, {"engine", "local"}});
    }
    if (endpoint == "portfolio/optimize/efficient-frontier") {
        QVector<QVector<double>> cov = matrix_any(body, {"covariance", "covariance_matrix", "cov"});
        QVector<double> er = nums_any(body, {"expected_returns", "mu"});
        if (cov.isEmpty() || er.isEmpty())
            return mcp::ToolResult::fail("expected_returns and covariance are required");
        const QVector<double> base = inverse_variance_weights(cov);
        const QVector<double> aggressive = normalize_weights(er);
        QJsonArray points;
        for (int i = 0; i <= 10; ++i) {
            const double a = i / 10.0;
            QVector<double> w;
            for (int j = 0; j < base.size(); ++j)
                w.append((1.0 - a) * base.at(j) + a * aggressive.value(j));
            w = normalize_weights(w);
            points.append(QJsonObject{{"weights", to_json(w)}, {"expected_return", portfolio_return(w, er)}, {"volatility", std::sqrt(std::max(0.0, portfolio_variance(w, cov)))}});
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"frontier", points}, {"engine", "local"}});
    }
    if (endpoint == "portfolio/risk/ratios" || endpoint == "portfolio/risk/comprehensive" ||
        endpoint == "portfolio/risk/portfolio-comprehensive") {
        const QVector<double> returns = historical_portfolio_returns(body);
        if (returns.isEmpty())
            return mcp::ToolResult::fail("returns are required");
        const double rf = num(body, "risk_free_rate", 0.0);
        const double avg = mean(returns);
        const double vol = std::sqrt(variance(returns, true));
        QVector<double> downside;
        for (double r : returns)
            downside.append(std::min(0.0, r - rf));
        const double downside_vol = std::sqrt(variance(downside, false));
        const double max_loss = -*std::min_element(returns.begin(), returns.end());
        return mcp::ToolResult::ok_data(QJsonObject{{"mean_return", avg}, {"volatility", vol}, {"sharpe", safe_div(avg - rf, vol)}, {"sortino", safe_div(avg - rf, downside_vol)}, {"max_loss", max_loss}, {"engine", "local"}});
    }
    if (endpoint == "core/periods/day-count-fraction" || endpoint == "scheduling/daycount/year-fraction") {
        const QDate start = iso_date(str(body, "start_date", "2024-01-01"));
        const QDate end = iso_date(str(body, "end_date", "2024-07-01"));
        const QString convention = str(body, "convention", str(body, "day_count", "ACT/365"));
        return mcp::ToolResult::ok_data(QJsonObject{{"year_fraction", year_fraction(start, end, convention)}, {"convention", convention}, {"engine", "local"}});
    }
    if (endpoint == "scheduling/daycount/day-count") {
        const QDate start = iso_date(str(body, "start_date", "2024-01-01"));
        const QDate end = iso_date(str(body, "end_date", "2024-07-01"));
        return mcp::ToolResult::ok_data(QJsonObject{{"days", start.daysTo(end)}, {"engine", "local"}});
    }
    if (endpoint == "scheduling/daycount/batch-year-fraction") {
        const QJsonArray starts = body.value("start_dates").toArray();
        const QJsonArray ends = body.value("end_dates").toArray();
        const QString convention = str(body, "convention", str(body, "day_count", "ACT/365"));
        QJsonArray out;
        const int n = std::min(starts.size(), ends.size());
        for (int i = 0; i < n; ++i)
            out.append(year_fraction(iso_date(starts.at(i).toString()), iso_date(ends.at(i).toString()), convention));
        return mcp::ToolResult::ok_data(QJsonObject{{"year_fractions", out}, {"convention", convention}, {"engine", "local"}});
    }
    if (endpoint == "core/periods/fixed-coupon" || endpoint == "core/periods/float-coupon") {
        const QDate start = iso_date(str(body, "start_date", "2024-01-01"));
        const QDate end = iso_date(str(body, "end_date", "2024-07-01"));
        const double notional = num(body, "notional", 1000000.0);
        const double rate = endpoint.contains("float") ? num(body, "fixing_rate", num(body, "index_rate", 0.0)) + num(body, "spread", 0.0)
                                                       : num(body, "rate", 0.05);
        const double yf = year_fraction(start, end, str(body, "day_count", "ACT/365"));
        return mcp::ToolResult::ok_data(QJsonObject{{"start_date", start.toString(Qt::ISODate)}, {"end_date", end.toString(Qt::ISODate)}, {"year_fraction", yf}, {"rate", rate}, {"cashflow", notional * rate * yf}, {"engine", "local"}});
    }
    if (endpoint == "core/legs/fixed" || endpoint == "core/legs/float") {
        const bool floating = endpoint.endsWith("/float");
        const QJsonArray flows = cashflow_leg(body, floating);
        double total = 0.0;
        for (const auto& flow : flows)
            total += flow.toObject().value("amount").toDouble();
        return mcp::ToolResult::ok_data(QJsonObject{{"cashflows", flows}, {"total_cashflow", total}, {"engine", "local"}});
    }
    if (endpoint == "core/legs/zero-coupon") {
        const QDate start = iso_date(str(body, "start_date", "2024-01-01"));
        const QDate end = iso_date(str(body, "end_date", "2029-01-01"));
        const double notional = num(body, "notional", 1000000.0);
        const double rate = num(body, "rate", 0.05);
        const double yf = year_fraction(start, end, str(body, "day_count", "ACT/365"));
        const double amount = notional * (std::exp(rate * yf) - 1.0);
        return mcp::ToolResult::ok_data(QJsonObject{{"cashflows", QJsonArray{QJsonObject{{"date", end.toString(Qt::ISODate)}, {"amount", amount}}}}, {"compounded_rate", std::exp(rate * yf) - 1.0}, {"cashflow_amount", amount}, {"engine", "local"}});
    }
    if (endpoint == "instruments/bond/fixed/cashflows") {
        QJsonObject out = bond_cashflows(body);
        out["engine"] = "local";
        return mcp::ToolResult::ok_data(out);
    }
    if (endpoint == "instruments/bond/fixed/price" || endpoint == "instruments/bond/zero-coupon/price") {
        const double y = num(body, "yield", num(body, "yield_rate", num(body, "rate", 0.05)));
        QJsonObject work = body;
        if (endpoint.contains("zero-coupon"))
            work["coupon_rate"] = 0.0;
        return mcp::ToolResult::ok_data(QJsonObject{{"price", bond_price_from_yield(work, y)}, {"yield", y}, {"engine", "local"}});
    }
    if (endpoint == "instruments/bond/fixed/yield" || endpoint == "solver/finance/bond-yield") {
        const double y = solve_bond_yield(body);
        return mcp::ToolResult::ok_data(QJsonObject{{"yield", y}, {"price", num(body, "price", 100.0)}, {"engine", "local"}});
    }
    if (endpoint == "instruments/bond/fixed/analytics" || endpoint == "solver/finance/duration" ||
        endpoint == "solver/finance/modified-duration" || endpoint == "solver/finance/convexity" ||
        endpoint == "solver/finance/pv01" || endpoint == "solver/finance/dv01") {
        const double y = num(body, "yield", num(body, "yield_rate", 0.05));
        const double price = bond_price_from_yield(body, y);
        const double up = bond_price_from_yield(body, y + 0.0001);
        const double down = bond_price_from_yield(body, y - 0.0001);
        const double dv01 = (down - up) / 2.0;
        const double mod_duration = safe_div(dv01 / 0.0001, price);
        const double convexity = safe_div(up + down - 2.0 * price, price * 0.0001 * 0.0001);
        return mcp::ToolResult::ok_data(QJsonObject{{"price", price}, {"modified_duration", mod_duration}, {"duration", mod_duration * (1.0 + y / std::max(1.0, num(body, "frequency", 2.0)))}, {"convexity", convexity}, {"pv01", dv01}, {"dv01", dv01}, {"engine", "local"}});
    }
    if (endpoint == "instruments/fx/forward") {
        const double spot = num(body, "spot", 1.0);
        const double rd = num(body, "domestic_rate", num(body, "rd", 0.05));
        const double rf = num(body, "foreign_rate", num(body, "rf", 0.02));
        const double T = num(body, "time_to_maturity", num(body, "T", 1.0));
        return mcp::ToolResult::ok_data(QJsonObject{{"forward", spot * std::exp((rd - rf) * T)}, {"engine", "local"}});
    }
    if (endpoint == "instruments/fx/garman-kohlhagen") {
        QJsonObject work = body;
        work["q"] = num(body, "foreign_rate", num(body, "rf", 0.02));
        work["r"] = num(body, "domestic_rate", num(body, "rd", 0.05));
        return mcp::ToolResult::ok_data(bs_values(work));
    }
    if (endpoint == "instruments/money-market/deposit" || endpoint == "instruments/money-market/repo" ||
        endpoint == "instruments/money-market/tbill") {
        const double notional = num(body, "notional", num(body, "face", 1000000.0));
        const double rate = num(body, "rate", num(body, "discount_rate", 0.05));
        const double yf = num(body, "year_fraction", num(body, "days", 90.0) / 360.0);
        const double interest = notional * rate * yf;
        return mcp::ToolResult::ok_data(QJsonObject{{"present_value", endpoint.contains("tbill") ? notional * (1.0 - rate * yf) : notional}, {"interest", interest}, {"maturity_value", notional + interest}, {"engine", "local"}});
    }
    if (endpoint == "solver/finance/par-rate") {
        const QVector<double> dfs = nums_any(body, {"discount_factors", "dfs"});
        if (dfs.isEmpty())
            return mcp::ToolResult::fail("discount_factors are required");
        const double annuity = sum(dfs) * num(body, "accrual", 1.0);
        return mcp::ToolResult::ok_data(QJsonObject{{"par_rate", safe_div(1.0 - dfs.last(), annuity)}, {"engine", "local"}});
    }
    if (endpoint == "solver/finance/z-spread" || endpoint == "solver/finance/i-spread" ||
        endpoint == "solver/finance/g-spread" || endpoint == "solver/finance/oas" ||
        endpoint == "solver/finance/asw-spread" || endpoint == "solver/finance/basis") {
        const double instrument_yield = num(body, "yield", num(body, "instrument_yield", 0.05));
        const double benchmark = num(body, "benchmark_yield", num(body, "benchmark", num(body, "curve_rate", 0.03)));
        return mcp::ToolResult::ok_data(QJsonObject{{"spread", instrument_yield - benchmark}, {"spread_bps", (instrument_yield - benchmark) * 10000.0}, {"engine", "local"}});
    }
    if (endpoint == "solver/finance/irr" || endpoint == "solver/finance/xirr") {
        const QVector<double> cashflows = nums(body, "cashflows");
        if (cashflows.size() < 2)
            return mcp::ToolResult::fail("cashflows must contain at least two values");
        double lo = -0.999;
        double hi = 10.0;
        for (int iter = 0; iter < 100; ++iter) {
            const double mid = 0.5 * (lo + hi);
            double npv = 0.0;
            for (int i = 0; i < cashflows.size(); ++i)
                npv += cashflows.at(i) / std::pow(1.0 + mid, i);
            if (npv > 0.0)
                lo = mid;
            else
                hi = mid;
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"irr", 0.5 * (lo + hi)}, {"engine", "local"}});
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
    if (endpoint == "scheduling/calendar/business-days-between") {
        QDate d = iso_date(str(body, "start_date"));
        const QDate end = iso_date(str(body, "end_date"));
        if (!d.isValid() || !end.isValid())
            return mcp::ToolResult::fail("start_date and end_date must be YYYY-MM-DD");
        int count = 0;
        while (d < end) {
            if (d.dayOfWeek() < 6)
                ++count;
            d = d.addDays(1);
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"business_days", count}, {"calendar", "weekend-only"}});
    }
    if (endpoint == "scheduling/calendar/add-business-days") {
        QDate d = iso_date(str(body, "date", str(body, "start_date")));
        int days = static_cast<int>(num(body, "days", 1.0));
        if (!d.isValid())
            return mcp::ToolResult::fail("date must be YYYY-MM-DD");
        const int step = days < 0 ? -1 : 1;
        days = std::abs(days);
        while (days > 0) {
            d = d.addDays(step);
            if (d.dayOfWeek() < 6)
                --days;
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"date", d.toString(Qt::ISODate)}, {"calendar", "weekend-only"}});
    }
    if (endpoint == "scheduling/adjustment/adjust-date") {
        QDate d = iso_date(str(body, "date"));
        const QString method = str(body, "method", "following").toLower();
        if (!d.isValid())
            return mcp::ToolResult::fail("date must be YYYY-MM-DD");
        if (d.dayOfWeek() >= 6) {
            const int step = method.contains("preceding") ? -1 : 1;
            const int original_month = d.month();
            do {
                d = d.addDays(step);
            } while (d.dayOfWeek() >= 6);
            if (method.contains("modified") && d.month() != original_month) {
                d = iso_date(str(body, "date"));
                do {
                    d = d.addDays(-1);
                } while (d.dayOfWeek() >= 6);
            }
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"adjusted_date", d.toString(Qt::ISODate)}, {"method", method}, {"calendar", "weekend-only"}});
    }
    if (endpoint == "scheduling/adjustment/batch-adjust") {
        const QJsonArray dates = body.value("dates").toArray();
        QJsonArray adjusted;
        for (const auto& value : dates) {
            QJsonObject req = body;
            req["date"] = value.toString();
            adjusted.append(local_quantlib_call("scheduling/adjustment/adjust-date", req).data.toObject().value("adjusted_date").toString());
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"adjusted_dates", adjusted}, {"engine", "local"}});
    }
    if (endpoint == "scheduling/schedule/generate") {
        QDate d = iso_date(str(body, "start_date", "2024-01-01"));
        const QDate end = iso_date(str(body, "end_date", d.addYears(1).toString(Qt::ISODate)));
        const int months = std::max(1, months_from_tenor(str(body, "frequency", str(body, "tenor", "3M"))));
        QJsonArray dates;
        while (d.isValid() && end.isValid() && d <= end) {
            dates.append(d.toString(Qt::ISODate));
            d = d.addMonths(months);
        }
        if (dates.isEmpty() || dates.last().toString() != end.toString(Qt::ISODate))
            dates.append(end.toString(Qt::ISODate));
        return mcp::ToolResult::ok_data(QJsonObject{{"dates", dates}, {"engine", "local"}});
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
    if (endpoint.startsWith("statistics/distributions/") && endpoint.endsWith("/properties")) {
        const QString dist = endpoint.section('/', 2, 2);
        if (dist == "normal") {
            const double mu = num(body, "mean", 0.0);
            const double sigma = num(body, "std", num(body, "sigma", 1.0));
            return mcp::ToolResult::ok_data(QJsonObject{{"mean", mu}, {"variance", sigma * sigma}, {"std", sigma}, {"skewness", 0.0}, {"kurtosis_excess", 0.0}});
        }
        if (dist == "lognormal") {
            const double mu = num(body, "mu", 0.0);
            const double sigma = num(body, "sigma", 0.2);
            const double v = (std::exp(sigma * sigma) - 1.0) * std::exp(2.0 * mu + sigma * sigma);
            return mcp::ToolResult::ok_data(QJsonObject{{"mean", std::exp(mu + 0.5 * sigma * sigma)}, {"variance", v}, {"std", std::sqrt(v)}});
        }
        if (dist == "gamma") {
            const double a = num(body, "alpha", num(body, "shape", 2.0));
            const double b = num(body, "beta", num(body, "rate", 1.0));
            return mcp::ToolResult::ok_data(QJsonObject{{"mean", a / b}, {"variance", a / (b * b)}, {"std", std::sqrt(a) / b}});
        }
        if (dist == "exponential") {
            const double rate = num(body, "rate", 1.0);
            return mcp::ToolResult::ok_data(QJsonObject{{"mean", 1.0 / rate}, {"variance", 1.0 / (rate * rate)}, {"std", 1.0 / rate}});
        }
        if (dist == "poisson") {
            const double lambda = num(body, "lambda", num(body, "lam", 1.0));
            return mcp::ToolResult::ok_data(QJsonObject{{"mean", lambda}, {"variance", lambda}, {"std", std::sqrt(lambda)}});
        }
        if (dist == "binomial") {
            const double n = num(body, "n", 10.0);
            const double p = num(body, "p", 0.5);
            return mcp::ToolResult::ok_data(QJsonObject{{"mean", n * p}, {"variance", n * p * (1.0 - p)}, {"std", std::sqrt(n * p * (1.0 - p))}});
        }
    }
    if (endpoint == "statistics/distributions/lognormal/pdf" || endpoint == "statistics/distributions/lognormal/cdf" ||
        endpoint == "statistics/distributions/lognormal/ppf") {
        const double mu = num(body, "mu", num(body, "meanlog", 0.0));
        const double sigma = std::max(1e-12, num(body, "sigma", num(body, "sdlog", 0.2)));
        if (endpoint.endsWith("/ppf")) {
            const double p = num(body, "p", 0.5);
            return mcp::ToolResult::ok_data(QJsonObject{{"ppf", std::exp(mu + sigma * norm_inv(p))}, {"p", p}});
        }
        const double x = num(body, "x", 1.0);
        const double z = (std::log(std::max(1e-300, x)) - mu) / sigma;
        if (endpoint.endsWith("/pdf"))
            return mcp::ToolResult::ok_data(QJsonObject{{"x", x}, {"pdf", x <= 0.0 ? 0.0 : norm_pdf(z) / (x * sigma)}});
        return mcp::ToolResult::ok_data(QJsonObject{{"x", x}, {"cdf", x <= 0.0 ? 0.0 : norm_cdf(z)}});
    }
    if (endpoint == "statistics/distributions/beta/pdf" || endpoint == "statistics/distributions/beta/cdf") {
        const double x = num(body, "x", 0.5);
        const double a = num(body, "alpha", num(body, "a", 2.0));
        const double b = num(body, "beta", num(body, "b", 2.0));
        return mcp::ToolResult::ok_data(endpoint.endsWith("/pdf") ? QJsonObject{{"x", x}, {"pdf", beta_pdf_value(x, a, b)}}
                                                               : QJsonObject{{"x", x}, {"cdf", beta_cdf_value(x, a, b)}});
    }
    if (endpoint == "statistics/distributions/poisson/pmf" || endpoint == "statistics/distributions/poisson/cdf") {
        const int k = static_cast<int>(num(body, "k", num(body, "x", 0.0)));
        const double lambda = num(body, "lambda", num(body, "lam", 1.0));
        auto pmf = [lambda](int i) { return std::exp(-lambda + i * std::log(lambda) - std::lgamma(i + 1.0)); };
        if (endpoint.endsWith("/pmf"))
            return mcp::ToolResult::ok_data(QJsonObject{{"k", k}, {"pmf", pmf(k)}});
        double cdf = 0.0;
        for (int i = 0; i <= k; ++i)
            cdf += pmf(i);
        return mcp::ToolResult::ok_data(QJsonObject{{"k", k}, {"cdf", std::clamp(cdf, 0.0, 1.0)}});
    }
    if (endpoint == "statistics/distributions/binomial/pmf" || endpoint == "statistics/distributions/binomial/cdf") {
        const int k = static_cast<int>(num(body, "k", num(body, "x", 0.0)));
        const int n = static_cast<int>(num(body, "n", 10.0));
        const double p = num(body, "p", 0.5);
        auto pmf = [n, p](int i) {
            if (i < 0 || i > n)
                return 0.0;
            return std::exp(std::lgamma(n + 1.0) - std::lgamma(i + 1.0) - std::lgamma(n - i + 1.0) +
                            i * std::log(p) + (n - i) * std::log(1.0 - p));
        };
        if (endpoint.endsWith("/pmf"))
            return mcp::ToolResult::ok_data(QJsonObject{{"k", k}, {"pmf", pmf(k)}});
        double cdf = 0.0;
        for (int i = 0; i <= k; ++i)
            cdf += pmf(i);
        return mcp::ToolResult::ok_data(QJsonObject{{"k", k}, {"cdf", std::clamp(cdf, 0.0, 1.0)}});
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
    if (endpoint == "numerical/linalg/transpose") {
        const QVector<QVector<double>> a = matrix(body, "matrix");
        if (a.isEmpty())
            return mcp::ToolResult::fail("matrix is required");
        QVector<QVector<double>> out(a.first().size(), QVector<double>(a.size(), 0.0));
        for (int i = 0; i < a.size(); ++i)
            for (int j = 0; j < a.at(i).size(); ++j)
                out[j][i] = a.at(i).at(j);
        return mcp::ToolResult::ok_data(QJsonObject{{"transpose", to_json(out)}});
    }
    if (endpoint == "numerical/linalg/matvec") {
        const QVector<QVector<double>> a = matrix(body, "matrix");
        const QVector<double> x = nums_any(body, {"vector", "x"});
        QJsonArray out;
        for (const auto& row : a) {
            double v = 0.0;
            for (int i = 0; i < row.size() && i < x.size(); ++i)
                v += row.at(i) * x.at(i);
            out.append(v);
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"result", out}});
    }
    if (endpoint == "numerical/linalg/matmul") {
        const QVector<QVector<double>> a = matrix(body, "a");
        const QVector<QVector<double>> b = matrix(body, "b");
        if (a.isEmpty() || b.isEmpty())
            return mcp::ToolResult::fail("a and b matrices are required");
        QVector<QVector<double>> out(a.size(), QVector<double>(b.first().size(), 0.0));
        for (int i = 0; i < a.size(); ++i)
            for (int j = 0; j < b.first().size(); ++j)
                for (int k = 0; k < b.size() && k < a.at(i).size(); ++k)
                    out[i][j] += a.at(i).at(k) * b.at(k).at(j);
        return mcp::ToolResult::ok_data(QJsonObject{{"result", to_json(out)}});
    }
    if (endpoint == "numerical/linalg/outer") {
        const QVector<double> a = nums(body, "a");
        const QVector<double> b = nums(body, "b");
        QVector<QVector<double>> out(a.size(), QVector<double>(b.size(), 0.0));
        for (int i = 0; i < a.size(); ++i)
            for (int j = 0; j < b.size(); ++j)
                out[i][j] = a.at(i) * b.at(j);
        return mcp::ToolResult::ok_data(QJsonObject{{"outer", to_json(out)}});
    }
    if (endpoint == "numerical/differentiation/derivative") {
        const QString fn = str(body, "func_name", "sin");
        const double x = num(body, "x", 0.0);
        const double h = num(body, "h", 1e-5);
        return mcp::ToolResult::ok_data(QJsonObject{{"derivative", (unary_function(fn, x + h) - unary_function(fn, x - h)) / (2.0 * h)}, {"engine", "local"}});
    }
    if (endpoint == "numerical/roots/find-1d" || endpoint == "numerical/roots/newton") {
        const QString fn = str(body, "func_name", "sin");
        double lo = num(body, "lower", -10.0);
        double hi = num(body, "upper", 10.0);
        for (int i = 0; i < 100; ++i) {
            const double mid = 0.5 * (lo + hi);
            if (unary_function(fn, lo) * unary_function(fn, mid) <= 0.0)
                hi = mid;
            else
                lo = mid;
        }
        return mcp::ToolResult::ok_data(QJsonObject{{"root", 0.5 * (lo + hi)}, {"engine", "local"}});
    }
    if (endpoint == "numerical/integration/quadrature") {
        const QString fn = str(body, "func_name", "sin");
        const double a = num(body, "a", 0.0);
        const double b = num(body, "b", 1.0);
        return mcp::ToolResult::ok_data(QJsonObject{{"integral", integrate_simpson([fn](double x) { return unary_function(fn, x); }, a, b)}, {"engine", "local"}});
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
    if (endpoint == "physics/entropy/renyi" || endpoint == "physics/entropy/tsallis") {
        QVector<double> p = nums_any(body, {"probabilities", "p"});
        const double alpha = num(body, "alpha", 2.0);
        double s = 0.0;
        for (double x : p)
            if (x > 0.0)
                s += std::pow(x, alpha);
        const double value = endpoint.contains("renyi") ? std::log2(s) / (1.0 - alpha) : (1.0 - s) / (alpha - 1.0);
        return mcp::ToolResult::ok_data(QJsonObject{{"entropy", value}, {"alpha", alpha}, {"engine", "local"}});
    }
    if (endpoint == "physics/divergence/kl" || endpoint == "physics/divergence/js") {
        const QVector<double> p = nums(body, "p");
        const QVector<double> q = nums(body, "q");
        if (p.size() != q.size())
            return mcp::ToolResult::fail("p and q must have the same length");
        auto kl = [](const QVector<double>& a, const QVector<double>& b) {
            double out = 0.0;
            for (int i = 0; i < a.size(); ++i)
                if (a.at(i) > 0.0 && b.at(i) > 0.0)
                    out += a.at(i) * std::log2(a.at(i) / b.at(i));
            return out;
        };
        if (endpoint.endsWith("/kl"))
            return mcp::ToolResult::ok_data(QJsonObject{{"divergence", kl(p, q)}, {"engine", "local"}});
        QVector<double> m;
        for (int i = 0; i < p.size(); ++i)
            m.append(0.5 * (p.at(i) + q.at(i)));
        return mcp::ToolResult::ok_data(QJsonObject{{"divergence", 0.5 * kl(p, m) + 0.5 * kl(q, m)}, {"engine", "local"}});
    }
    if (endpoint.startsWith("analysis/ratios/") || endpoint.startsWith("analysis/fundamentals/")) {
        if (endpoint.contains("roa"))
            return mcp::ToolResult::ok_data(ratio_result("roa", num(body, "net_income"), num(body, "total_assets", num(body, "assets"))));
        if (endpoint.contains("roe"))
            return mcp::ToolResult::ok_data(ratio_result("roe", num(body, "net_income"), num(body, "shareholders_equity", num(body, "equity"))));
        if (endpoint.contains("roic"))
            return mcp::ToolResult::ok_data(ratio_result("roic", num(body, "nopat", num(body, "ebit", 0.0) * (1.0 - num(body, "tax_rate", 0.21))), num(body, "invested_capital")));
        if (endpoint.contains("gross-margin"))
            return mcp::ToolResult::ok_data(ratio_result("gross_margin", num(body, "gross_profit"), num(body, "revenue", num(body, "sales"))));
        if (endpoint.contains("net-margin"))
            return mcp::ToolResult::ok_data(ratio_result("net_margin", num(body, "net_income"), num(body, "revenue", num(body, "sales"))));
        if (endpoint.contains("ebitda-margin"))
            return mcp::ToolResult::ok_data(ratio_result("ebitda_margin", num(body, "ebitda"), num(body, "revenue", num(body, "sales"))));
        if (endpoint.contains("current-ratio"))
            return mcp::ToolResult::ok_data(ratio_result("current_ratio", num(body, "current_assets"), num(body, "current_liabilities")));
        if (endpoint.contains("quick-ratio"))
            return mcp::ToolResult::ok_data(ratio_result("quick_ratio", num(body, "current_assets") - num(body, "inventory"), num(body, "current_liabilities")));
        if (endpoint.contains("cash-ratio"))
            return mcp::ToolResult::ok_data(ratio_result("cash_ratio", num(body, "cash") + num(body, "marketable_securities"), num(body, "current_liabilities")));
        if (endpoint.contains("debt-to-equity"))
            return mcp::ToolResult::ok_data(ratio_result("debt_to_equity", num(body, "total_debt", num(body, "debt")), num(body, "shareholders_equity", num(body, "equity"))));
        if (endpoint.contains("interest-coverage"))
            return mcp::ToolResult::ok_data(ratio_result("interest_coverage", num(body, "ebit"), num(body, "interest_expense")));
        if (endpoint.endsWith("/pe"))
            return mcp::ToolResult::ok_data(ratio_result("pe", num(body, "market_cap", num(body, "price") * num(body, "shares_outstanding", 1.0)), num(body, "net_income", num(body, "eps"))));
        if (endpoint.endsWith("/pb"))
            return mcp::ToolResult::ok_data(ratio_result("pb", num(body, "market_cap", num(body, "price") * num(body, "shares_outstanding", 1.0)), num(body, "book_value", num(body, "equity"))));
        if (endpoint.endsWith("/ps"))
            return mcp::ToolResult::ok_data(ratio_result("ps", num(body, "market_cap", num(body, "price") * num(body, "shares_outstanding", 1.0)), num(body, "revenue", num(body, "sales"))));
        if (endpoint.contains("ev-ebitda"))
            return mcp::ToolResult::ok_data(ratio_result("ev_ebitda", num(body, "enterprise_value", num(body, "market_cap") + num(body, "total_debt") - num(body, "cash")), num(body, "ebitda")));
        if (endpoint.contains("dividend-yield"))
            return mcp::ToolResult::ok_data(ratio_result("dividend_yield", num(body, "dividend", num(body, "annual_dividend")), num(body, "price")));
        const double revenue = num(body, "revenue", num(body, "sales"));
        const double gross_profit = num(body, "gross_profit");
        const double net_income = num(body, "net_income");
        return mcp::ToolResult::ok_data(QJsonObject{{"profitability", QJsonObject{{"gross_margin", safe_div(gross_profit, revenue)}, {"net_margin", safe_div(net_income, revenue)}, {"roa", safe_div(net_income, num(body, "total_assets", num(body, "assets")))}, {"roe", safe_div(net_income, num(body, "equity"))}}},
                                                   {"liquidity", QJsonObject{{"current_ratio", safe_div(num(body, "current_assets"), num(body, "current_liabilities"))}, {"quick_ratio", safe_div(num(body, "current_assets") - num(body, "inventory"), num(body, "current_liabilities"))}}},
                                                   {"engine", "local"}});
    }
    if (endpoint.startsWith("analysis/valuation/dcf/")) {
        const QVector<double> cashflows = nums_any(body, {"cashflows", "fcff", "dividends"});
        const double discount = num(body, "discount_rate", num(body, "wacc", num(body, "cost_of_equity", 0.10)));
        const double growth = num(body, "terminal_growth", num(body, "growth", 0.02));
        if (endpoint.contains("wacc")) {
            const double e = num(body, "equity_value", num(body, "equity"));
            const double d = num(body, "debt_value", num(body, "debt"));
            const double re = num(body, "cost_of_equity", 0.10);
            const double rd = num(body, "cost_of_debt", 0.05);
            const double tax = num(body, "tax_rate", 0.21);
            return mcp::ToolResult::ok_data(QJsonObject{{"wacc", safe_div(e, e + d) * re + safe_div(d, e + d) * rd * (1.0 - tax)}, {"engine", "local"}});
        }
        if (endpoint.contains("cost-of-equity")) {
            const double rf = num(body, "risk_free_rate", 0.04);
            const double beta = num(body, "beta", 1.0);
            const double mrp = num(body, "market_risk_premium", 0.05);
            return mcp::ToolResult::ok_data(QJsonObject{{"cost_of_equity", rf + beta * mrp}, {"engine", "local"}});
        }
        if (cashflows.isEmpty() && endpoint.contains("terminal-value")) {
            const double cf = num(body, "cashflow", num(body, "fcf", 1.0));
            return mcp::ToolResult::ok_data(QJsonObject{{"terminal_value", cf * (1.0 + growth) / (discount - growth)}, {"engine", "local"}});
        }
        double value = 0.0;
        for (int i = 0; i < cashflows.size(); ++i)
            value += cashflows.at(i) / std::pow(1.0 + discount, i + 1);
        if (!cashflows.isEmpty() && discount > growth)
            value += cashflows.last() * (1.0 + growth) / (discount - growth) / std::pow(1.0 + discount, cashflows.size());
        return mcp::ToolResult::ok_data(QJsonObject{{"value", value}, {"discount_rate", discount}, {"terminal_growth", growth}, {"engine", "local"}});
    }
    if (endpoint == "analysis/valuation/predictive/altman-z") {
        const double assets = num(body, "total_assets", num(body, "assets"));
        const double z = 1.2 * safe_div(num(body, "working_capital"), assets) +
                         1.4 * safe_div(num(body, "retained_earnings"), assets) +
                         3.3 * safe_div(num(body, "ebit"), assets) +
                         0.6 * safe_div(num(body, "market_value_equity"), num(body, "total_liabilities")) +
                         1.0 * safe_div(num(body, "sales", num(body, "revenue")), assets);
        return mcp::ToolResult::ok_data(QJsonObject{{"altman_z", z}, {"zone", z > 2.99 ? "safe" : (z < 1.81 ? "distress" : "grey")}, {"engine", "local"}});
    }
    if (endpoint == "analysis/valuation/predictive/piotroski-f") {
        int score = 0;
        score += num(body, "net_income") > 0.0;
        score += num(body, "operating_cash_flow") > 0.0;
        score += num(body, "roa_delta") > 0.0;
        score += num(body, "operating_cash_flow") > num(body, "net_income");
        score += num(body, "leverage_delta") < 0.0;
        score += num(body, "current_ratio_delta") > 0.0;
        score += num(body, "shares_delta") <= 0.0;
        score += num(body, "gross_margin_delta") > 0.0;
        score += num(body, "asset_turnover_delta") > 0.0;
        return mcp::ToolResult::ok_data(QJsonObject{{"piotroski_f", score}, {"engine", "local"}});
    }
    if (endpoint.startsWith("regulatory/")) {
        if (endpoint == "regulatory/basel/capital-ratios") {
            const double cet1 = num(body, "cet1_capital", num(body, "tier1_capital"));
            const double tier1 = num(body, "tier1_capital", cet1);
            const double total = num(body, "total_capital", tier1);
            const double rwa = num(body, "rwa", num(body, "risk_weighted_assets", 1.0));
            return mcp::ToolResult::ok_data(QJsonObject{{"cet1_ratio", safe_div(cet1, rwa)}, {"tier1_ratio", safe_div(tier1, rwa)}, {"total_capital_ratio", safe_div(total, rwa)}, {"engine", "local"}});
        }
        if (endpoint == "regulatory/liquidity/lcr")
            return mcp::ToolResult::ok_data(ratio_result("lcr", num(body, "hqla"), num(body, "net_cash_outflows")));
        if (endpoint == "regulatory/liquidity/nsfr")
            return mcp::ToolResult::ok_data(ratio_result("nsfr", num(body, "available_stable_funding"), num(body, "required_stable_funding")));
        if (endpoint.contains("ifrs9") || endpoint.contains("ecl")) {
            const double ead = num(body, "ead", num(body, "exposure", 0.0));
            const double pd = num(body, "pd", 0.01);
            const double lgd = num(body, "lgd", 0.45);
            return mcp::ToolResult::ok_data(QJsonObject{{"expected_credit_loss", ead * pd * lgd}, {"pd", pd}, {"lgd", lgd}, {"ead", ead}, {"engine", "local"}});
        }
        if (endpoint == "regulatory/saccr/ead")
            return mcp::ToolResult::ok_data(QJsonObject{{"ead", 1.4 * (num(body, "replacement_cost") + num(body, "pfe"))}, {"engine", "local"}});
    }
    if (endpoint.startsWith("economics/utility/")) {
        const double wealth = num(body, "wealth", num(body, "x", 1.0));
        const double gamma = num(body, "gamma", num(body, "risk_aversion", 2.0));
        if (endpoint.endsWith("/log"))
            return mcp::ToolResult::ok_data(QJsonObject{{"utility", std::log(std::max(1e-12, wealth))}, {"engine", "local"}});
        if (endpoint.endsWith("/cara"))
            return mcp::ToolResult::ok_data(QJsonObject{{"utility", -std::exp(-gamma * wealth) / gamma}, {"engine", "local"}});
        if (endpoint.endsWith("/crra"))
            return mcp::ToolResult::ok_data(QJsonObject{{"utility", std::abs(gamma - 1.0) < 1e-12 ? std::log(wealth) : std::pow(wealth, 1.0 - gamma) / (1.0 - gamma)}, {"engine", "local"}});
        if (endpoint.endsWith("/quadratic"))
            return mcp::ToolResult::ok_data(QJsonObject{{"utility", wealth - 0.5 * gamma * wealth * wealth}, {"engine", "local"}});
    }
    if (endpoint == "economics/equilibrium/cobb-douglas") {
        const double A = num(body, "A", 1.0);
        const double K = num(body, "K", num(body, "capital", 1.0));
        const double L = num(body, "L", num(body, "labor", 1.0));
        const double alpha = num(body, "alpha", 0.33);
        return mcp::ToolResult::ok_data(QJsonObject{{"output", A * std::pow(K, alpha) * std::pow(L, 1.0 - alpha)}, {"engine", "local"}});
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
