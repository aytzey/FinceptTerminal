// src/services/algo_trading/AlgoTradingService.cpp
#include "services/algo_trading/AlgoTradingService.h"

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "core/logging/Logger.h"
#include "python/PythonRunner.h"
#include "storage/cache/CacheManager.h"
#include "storage/sqlite/Database.h"
#include "trading/AccountManager.h"
#include "trading/UnifiedTrading.h"

#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QUuid>

#include <cmath>

namespace fincept::services::algo {

static constexpr int kStrategiesTtlSec = 30;
static constexpr int kDeploymentsTtlSec = 30;
static constexpr const char* kStrategiesCacheKey = "algo:strategies:registry";
static constexpr const char* kDeploymentsCacheKey = "algo:deployments";

// ── DB path helper ────────────────────────────────────────────────────────────
static QString algo_db_path() {
    return fincept::AppPaths::data() + "/fincept.db";
}

static QString normalize_mode(QString mode) {
    mode = mode.trimmed().toLower();
    return mode == "live" ? "live" : "paper";
}

static QString resolve_live_account(const QString& requested_account_id, QString* error) {
    auto& accounts = fincept::trading::AccountManager::instance();

    if (!requested_account_id.trimmed().isEmpty()) {
        const auto account = accounts.get_account(requested_account_id.trimmed());
        if (account.account_id.isEmpty()) {
            if (error)
                *error = "Live deployment account not found: " + requested_account_id;
            return {};
        }
        if (!account.is_active || account.trading_mode != "live") {
            if (error)
                *error = "Selected account must be active and in live mode for live deployment.";
            return {};
        }
        return account.account_id;
    }

    QStringList live_ids;
    for (const auto& account : accounts.active_accounts()) {
        if (account.trading_mode == "live")
            live_ids.append(account.account_id);
    }
    if (live_ids.size() == 1)
        return live_ids.first();

    if (error) {
        *error = live_ids.isEmpty()
                     ? "Live deployment requires an active live broker account."
                     : "Live deployment requires selecting one live broker account.";
    }
    return {};
}

static bool update_deployment_error(const QString& deploy_id, const QString& message) {
    auto r = fincept::Database::instance().execute(
        "UPDATE algo_deployments SET status = 'error', error_message = ?, updated_at = CURRENT_TIMESTAMP "
        "WHERE id = ?",
        {message, deploy_id});
    if (r.is_err()) {
        LOG_ERROR("AlgoTrading", QString("Failed to update deployment error: %1")
                                     .arg(QString::fromStdString(r.error())));
        return false;
    }
    return true;
}

static void ensure_bot_daemon_running() {
    const QString exe = QCoreApplication::applicationFilePath();
    if (exe.isEmpty())
        return;

    QStringList args{"--bot-daemon"};
    const QString profile = fincept::ProfileManager::instance().active();
    if (!profile.isEmpty())
        args << "--profile" << profile;

    qint64 pid = -1;
    if (QProcess::startDetached(exe, args, QFileInfo(exe).absolutePath(), &pid)) {
        LOG_INFO("AlgoTrading", QString("Bot daemon ensured: pid=%1").arg(pid));
    } else {
        LOG_WARN("AlgoTrading", "Could not start bot daemon process");
    }
}

AlgoTradingService& AlgoTradingService::instance() {
    static AlgoTradingService inst;
    return inst;
}

AlgoTradingService::AlgoTradingService(QObject* parent) : QObject(parent) {}

void AlgoTradingService::run_python(const QString& script, const QStringList& args, const QString& context,
                                    std::function<void(bool, const QString&)> cb) {
    QPointer<AlgoTradingService> self = this;
    python::PythonRunner::instance().run(script, args, [self, context, cb](python::PythonResult result) {
        if (!self)
            return;
        cb(result.success, result.success ? result.output : result.error);
    });
}

// ── Strategy CRUD ─────────────────────────────────────────────────────────────
void AlgoTradingService::save_strategy(const AlgoStrategy& strategy) {
    QJsonObject obj;
    obj["id"] = strategy.id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : strategy.id;
    obj["name"] = strategy.name;
    obj["description"] = strategy.description;
    obj["timeframe"] = strategy.timeframe;
    obj["entry_conditions"] = strategy.entry_conditions;
    obj["exit_conditions"] = strategy.exit_conditions;
    obj["entry_logic"] = strategy.entry_logic;
    obj["exit_logic"] = strategy.exit_logic;
    obj["stop_loss"] = strategy.stop_loss;
    obj["take_profit"] = strategy.take_profit;
    obj["trailing_stop"] = strategy.trailing_stop;

    auto json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    run_python("algo_trading/backtest_engine.py", {"save_strategy", json, "--db", algo_db_path()}, "save_strategy",
               [this, obj](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("save_strategy", out);
                       return;
                   }
                   fincept::CacheManager::instance().remove(kStrategiesCacheKey);
                   emit strategy_saved(obj["id"].toString());
               });
}

static QVector<AlgoStrategy> parse_strategies(const QJsonArray& arr) {
    QVector<AlgoStrategy> strategies;
    strategies.reserve(arr.size());
    for (const auto& v : arr) {
        auto o = v.toObject();
        AlgoStrategy s;
        s.id = o["id"].toString();
        s.name = o["name"].toString();
        s.description = o["description"].toString();
        s.timeframe = o["timeframe"].toString();
        s.entry_conditions = o["entry_conditions"].toArray();
        s.exit_conditions = o["exit_conditions"].toArray();
        s.entry_logic = o["entry_logic"].toString("AND");
        s.exit_logic = o["exit_logic"].toString("AND");
        s.stop_loss = o["stop_loss"].toDouble();
        s.take_profit = o["take_profit"].toDouble();
        s.trailing_stop = o["trailing_stop"].toDouble();
        s.created_at = o["created_at"].toString();
        s.updated_at = o["updated_at"].toString();
        strategies.append(s);
    }
    return strategies;
}

void AlgoTradingService::list_strategies() {
    // Fast path: read pre-generated registry_index.json directly — no Python spawn needed
    const QString json_path =
        python::PythonRunner::instance().scripts_dir() + "/strategies/registry_index.json";
    QFile f(json_path);
    if (f.open(QIODevice::ReadOnly)) {
        auto doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (!doc.isNull()) {
            LOG_INFO("AlgoTrading", QString("Loaded registry from %1").arg(json_path));
            emit strategies_loaded(parse_strategies(doc.object()["strategies"].toArray()));
            return;
        }
    }

    // Fallback: run Python to regenerate the index
    LOG_WARN("AlgoTrading", "registry_index.json missing — falling back to Python");
    run_python("algo_trading/backtest_engine.py", {"list_registry"}, "list_strategies",
               [this](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("list_strategies", out);
                       return;
                   }
                   auto doc = QJsonDocument::fromJson(python::extract_json(out).toUtf8());
                   auto obj = doc.object();
                   emit strategies_loaded(parse_strategies(obj["strategies"].toArray()));
               });
}

void AlgoTradingService::delete_strategy(const QString& id) {
    run_python("algo_trading/backtest_engine.py", {"delete_strategy", id, "--db", algo_db_path()}, "delete_strategy",
               [this, id](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("delete_strategy", out);
                       return;
                   }
                   fincept::CacheManager::instance().remove(kStrategiesCacheKey);
                   emit strategy_deleted(id);
               });
}

// ── Deployment lifecycle ──────────────────────────────────────────────────────
void AlgoTradingService::deploy_strategy(const QString& strategy_id, const QString& symbol, const QString& mode,
                                         const QString& timeframe, double quantity, const QString& account_id) {
    const QString clean_symbol = symbol.trimmed();
    const QString clean_mode = normalize_mode(mode);
    const QString clean_timeframe = timeframe.trimmed().isEmpty() ? "5m" : timeframe.trimmed();

    if (strategy_id.trimmed().isEmpty()) {
        emit error_occurred("deploy", "Strategy id is required.");
        return;
    }
    if (clean_symbol.isEmpty()) {
        emit error_occurred("deploy", "Symbol is required.");
        return;
    }
    if (!std::isfinite(quantity) || quantity <= 0.0) {
        emit error_occurred("deploy", "Quantity must be greater than zero.");
        return;
    }

    QString resolved_account_id;
    if (clean_mode == "live") {
        QString account_error;
        resolved_account_id = resolve_live_account(account_id, &account_error);
        if (resolved_account_id.isEmpty()) {
            emit error_occurred("deploy", account_error);
            return;
        }
    }

    auto deploy_id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    const auto insert = fincept::Database::instance().execute(
        "INSERT INTO algo_deployments "
        "(id, strategy_id, account_id, symbol, mode, status, timeframe, quantity, error_message, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, 'starting', ?, ?, '', CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)",
        {deploy_id, strategy_id.trimmed(),
         resolved_account_id.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(resolved_account_id),
         clean_symbol, clean_mode, clean_timeframe, quantity});
    if (insert.is_err()) {
        emit error_occurred("deploy", QString("Failed to create deployment row: %1")
                                          .arg(QString::fromStdString(insert.error())));
        return;
    }

    auto& runner = python::PythonRunner::instance();
    const QString python_exe = runner.python_path();
    if (python_exe.isEmpty()) {
        const QString msg = "Python not available — run first-time setup from the app.";
        update_deployment_error(deploy_id, msg);
        emit error_occurred("deploy", msg);
        return;
    }

    const QString script_path = runner.scripts_dir() + "/algo_trading/algo_live_runner.py";
    if (!QFileInfo::exists(script_path)) {
        const QString msg = "Script not found: " + script_path;
        update_deployment_error(deploy_id, msg);
        emit error_occurred("deploy", msg);
        return;
    }

    QStringList args{
        script_path,
        "--deploy-id", deploy_id,
        "--strategy-id", strategy_id.trimmed(),
        "--symbol", clean_symbol,
        "--mode", clean_mode,
        "--timeframe", clean_timeframe,
        "--quantity", QString::number(quantity, 'g', 16),
        "--db", algo_db_path(),
    };
    if (!resolved_account_id.isEmpty())
        args << "--account-id" << resolved_account_id;

    QProcess proc;
    proc.setProgram(python_exe);
    proc.setArguments(args);
    proc.setWorkingDirectory(runner.scripts_dir());
    proc.setProcessEnvironment(runner.build_python_env());

#ifdef _WIN32
    proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* cpa) {
        cpa->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#endif

    qint64 pid = -1;
    if (!proc.startDetached(&pid)) {
        const QString msg = "Failed to start algo live runner: " + proc.errorString();
        update_deployment_error(deploy_id, msg);
        emit error_occurred("deploy", msg);
        return;
    }

    fincept::Database::instance().execute(
        "UPDATE algo_deployments SET pid = ?, started_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP "
        "WHERE id = ?",
        {QVariant::fromValue<qlonglong>(pid), deploy_id});

    if (clean_mode == "live") {
        ensure_bot_daemon_running();
        fincept::trading::UnifiedTrading::instance().start_order_bridge();
    }

    fincept::CacheManager::instance().remove(kDeploymentsCacheKey);
    emit deployment_started(deploy_id);
    LOG_INFO("AlgoTrading", QString("Deployment started: %1 pid=%2 mode=%3 symbol=%4")
                                .arg(deploy_id)
                                .arg(pid)
                                .arg(clean_mode, clean_symbol));
}

void AlgoTradingService::stop_deployment(const QString& deployment_id) {
    run_python("algo_trading/algo_manager.py", {"stop", deployment_id, "--db", algo_db_path()}, "stop_deployment",
               [this, deployment_id](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("stop_deployment", out);
                       return;
                   }
                   fincept::CacheManager::instance().remove(kDeploymentsCacheKey);
                   emit deployment_stopped(deployment_id);
               });
}

void AlgoTradingService::stop_all_deployments() {
    run_python("algo_trading/algo_manager.py", {"stop_all", "--db", algo_db_path()}, "stop_all",
               [this](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("stop_all", out);
                       return;
                   }
                   fincept::CacheManager::instance().remove(kDeploymentsCacheKey);
                   LOG_INFO("AlgoTrading", "All deployments stopped");
               });
}

static QVector<AlgoDeployment> parse_deployments(const QJsonArray& arr) {
    QVector<AlgoDeployment> deployments;
    deployments.reserve(arr.size());
    for (const auto& v : arr) {
        auto o = v.toObject();
        AlgoDeployment d;
        d.id = o["id"].toString();
        d.strategy_id = o["strategy_id"].toString();
        d.strategy_name = o["strategy_name"].toString();
        d.account_id = o["account_id"].toString();
        d.account_name = o["account_name"].toString();
        d.symbol = o["symbol"].toString();
        d.mode = o["mode"].toString();
        d.status = o["status"].toString();
        d.timeframe = o["timeframe"].toString();
        d.quantity = o["quantity"].toDouble();
        d.error_message = o["error_message"].toString();
        d.total_pnl = o["total_pnl"].toDouble();
        d.unrealized_pnl = o["unrealized_pnl"].toDouble();
        d.total_trades = o["total_trades"].toInt();
        d.win_rate = o["win_rate"].toDouble();
        d.max_drawdown = o["max_drawdown"].toDouble();
        d.position_qty = o["current_position_qty"].toDouble();
        d.position_side = o["current_position_side"].toString();
        d.position_entry = o["current_position_entry"].toDouble();
        d.created_at = o["created_at"].toString();
        d.updated_at = o["updated_at"].toString();
        deployments.append(d);
    }
    return deployments;
}

void AlgoTradingService::list_deployments() {
    const QVariant cached = fincept::CacheManager::instance().get(kDeploymentsCacheKey);
    if (!cached.isNull()) {
        auto doc = QJsonDocument::fromJson(cached.toString().toUtf8());
        if (!doc.isNull()) {
            emit deployments_loaded(parse_deployments(doc.object()["deployments"].toArray()));
            return;
        }
    }

    run_python("algo_trading/algo_manager.py", {"list_deployments", "--db", algo_db_path()}, "list_deployments",
               [this](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("list_deployments", out);
                       return;
                   }
                   auto doc = QJsonDocument::fromJson(python::extract_json(out).toUtf8());
                   auto obj = doc.object();
                   fincept::CacheManager::instance().put(
                       kDeploymentsCacheKey,
                       QVariant(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact))),
                       kDeploymentsTtlSec, "algo_trading");
                   emit deployments_loaded(parse_deployments(obj["deployments"].toArray()));
               });
}

// ── Backtesting ───────────────────────────────────────────────────────────────
void AlgoTradingService::run_backtest(const QString& strategy_id, const QString& symbol, const QString& start_date,
                                      const QString& end_date, double capital) {
    QJsonObject params;
    params["strategy_id"] = strategy_id;
    params["symbol"] = symbol;
    params["start_date"] = start_date;
    params["end_date"] = end_date;
    params["initial_capital"] = capital;
    auto json = QJsonDocument(params).toJson(QJsonDocument::Compact);

    run_python("algo_trading/backtest_engine.py", {"run_backtest", json, "--db", algo_db_path()}, "backtest",
               [this](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("backtest", out);
                       return;
                   }
                   auto doc = QJsonDocument::fromJson(python::extract_json(out).toUtf8());
                   emit backtest_result(doc.object());
               });
}

// ── Scanner ───────────────────────────────────────────────────────────────────
void AlgoTradingService::run_scan(const QJsonArray& conditions, const QStringList& symbols, const QString& timeframe,
                                  int lookback_days, const QString& logic) {
    QJsonObject params;
    params["conditions"] = conditions;
    params["symbols"] = QJsonArray::fromStringList(symbols);
    params["timeframe"] = timeframe;
    params["lookback_days"] = lookback_days;
    params["logic"] = logic;
    auto json = QJsonDocument(params).toJson(QJsonDocument::Compact);

    run_python("algo_trading/scanner_engine.py", {"scan", json, "--db", algo_db_path()}, "scan",
               [this](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("scan", out);
                       return;
                   }
                   auto doc = QJsonDocument::fromJson(python::extract_json(out).toUtf8());
                   emit scan_result(doc.object());
               });
}

} // namespace fincept::services::algo
