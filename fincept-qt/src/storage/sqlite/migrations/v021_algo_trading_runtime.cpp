// v021_algo_trading_runtime — Persist algo bot runtime/deployment state.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace fincept {
namespace {

Result<void> sql(QSqlDatabase& db, const char* stmt) {
    QSqlQuery q(db);
    if (!q.exec(stmt))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

bool column_exists(QSqlDatabase& db, const QString& table, const QString& column) {
    QSqlQuery q(db);
    if (!q.exec(QString("PRAGMA table_info(%1)").arg(table)))
        return false;
    while (q.next()) {
        if (q.value(1).toString().compare(column, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

Result<void> add_column_if_missing(QSqlDatabase& db, const QString& table, const QString& column,
                                   const char* definition) {
    if (column_exists(db, table, column))
        return Result<void>::ok();
    QSqlQuery q(db);
    const QString stmt = QString("ALTER TABLE %1 ADD COLUMN %2").arg(table, QString::fromUtf8(definition));
    if (!q.exec(stmt))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v021(QSqlDatabase& db) {
    auto r = sql(db, "CREATE TABLE IF NOT EXISTS algo_strategies ("
                     "  id TEXT PRIMARY KEY,"
                     "  name TEXT NOT NULL,"
                     "  description TEXT DEFAULT '',"
                     "  timeframe TEXT DEFAULT '1d',"
                     "  entry_conditions TEXT DEFAULT '[]',"
                     "  exit_conditions TEXT DEFAULT '[]',"
                     "  entry_logic TEXT DEFAULT 'AND',"
                     "  exit_logic TEXT DEFAULT 'AND',"
                     "  stop_loss REAL DEFAULT 0,"
                     "  take_profit REAL DEFAULT 0,"
                     "  trailing_stop REAL DEFAULT 0,"
                     "  trailing_stop_type TEXT DEFAULT 'percent',"
                     "  is_active INTEGER DEFAULT 1,"
                     "  created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
                     "  updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
                     ")");
    if (r.is_err())
        return r;

    r = add_column_if_missing(db, "algo_strategies", "trailing_stop_type",
                              "trailing_stop_type TEXT DEFAULT 'percent'");
    if (r.is_err())
        return r;

    r = sql(db, "CREATE TABLE IF NOT EXISTS algo_deployments ("
                "  id TEXT PRIMARY KEY,"
                "  strategy_id TEXT NOT NULL,"
                "  account_id TEXT,"
                "  symbol TEXT NOT NULL,"
                "  mode TEXT NOT NULL DEFAULT 'paper',"
                "  status TEXT NOT NULL DEFAULT 'starting',"
                "  timeframe TEXT NOT NULL DEFAULT '5m',"
                "  quantity REAL NOT NULL DEFAULT 1,"
                "  error_message TEXT DEFAULT '',"
                "  pid INTEGER,"
                "  started_at TEXT,"
                "  stopped_at TEXT,"
                "  created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
                "  updated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
                "  FOREIGN KEY (strategy_id) REFERENCES algo_strategies(id) ON DELETE CASCADE,"
                "  FOREIGN KEY (account_id) REFERENCES broker_accounts(id) ON DELETE SET NULL"
                ")");
    if (r.is_err())
        return r;

    r = add_column_if_missing(db, "algo_deployments", "account_id", "account_id TEXT");
    if (r.is_err())
        return r;
    r = add_column_if_missing(db, "algo_deployments", "pid", "pid INTEGER");
    if (r.is_err())
        return r;
    r = add_column_if_missing(db, "algo_deployments", "started_at", "started_at TEXT");
    if (r.is_err())
        return r;
    r = add_column_if_missing(db, "algo_deployments", "stopped_at", "stopped_at TEXT");
    if (r.is_err())
        return r;

    r = sql(db, "CREATE TABLE IF NOT EXISTS algo_metrics ("
                "  deployment_id TEXT PRIMARY KEY,"
                "  total_pnl REAL DEFAULT 0,"
                "  unrealized_pnl REAL DEFAULT 0,"
                "  total_trades INTEGER DEFAULT 0,"
                "  win_rate REAL DEFAULT 0,"
                "  max_drawdown REAL DEFAULT 0,"
                "  current_position_qty REAL DEFAULT 0,"
                "  current_position_side TEXT DEFAULT '',"
                "  current_position_entry REAL DEFAULT 0,"
                "  updated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
                "  FOREIGN KEY (deployment_id) REFERENCES algo_deployments(id) ON DELETE CASCADE"
                ")");
    if (r.is_err())
        return r;

    r = sql(db, "CREATE TABLE IF NOT EXISTS algo_trades ("
                "  id TEXT PRIMARY KEY,"
                "  deployment_id TEXT NOT NULL,"
                "  symbol TEXT NOT NULL,"
                "  side TEXT NOT NULL,"
                "  quantity REAL NOT NULL,"
                "  price REAL NOT NULL,"
                "  pnl REAL DEFAULT 0,"
                "  signal_reason TEXT DEFAULT '',"
                "  timestamp TEXT DEFAULT CURRENT_TIMESTAMP,"
                "  FOREIGN KEY (deployment_id) REFERENCES algo_deployments(id) ON DELETE CASCADE"
                ")");
    if (r.is_err())
        return r;

    r = sql(db, "CREATE TABLE IF NOT EXISTS algo_order_signals ("
                "  id TEXT PRIMARY KEY,"
                "  deployment_id TEXT NOT NULL,"
                "  account_id TEXT,"
                "  symbol TEXT NOT NULL,"
                "  side TEXT NOT NULL,"
                "  quantity REAL NOT NULL,"
                "  order_type TEXT DEFAULT 'MARKET',"
                "  price REAL,"
                "  status TEXT NOT NULL DEFAULT 'pending',"
                "  order_id TEXT,"
                "  error TEXT,"
                "  created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
                "  updated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
                "  processed_at TEXT,"
                "  FOREIGN KEY (deployment_id) REFERENCES algo_deployments(id) ON DELETE CASCADE,"
                "  FOREIGN KEY (account_id) REFERENCES broker_accounts(id) ON DELETE SET NULL"
                ")");
    if (r.is_err())
        return r;

    r = add_column_if_missing(db, "algo_order_signals", "account_id", "account_id TEXT");
    if (r.is_err())
        return r;
    r = add_column_if_missing(db, "algo_order_signals", "order_id", "order_id TEXT");
    if (r.is_err())
        return r;
    r = add_column_if_missing(db, "algo_order_signals", "error", "error TEXT");
    if (r.is_err())
        return r;
    r = add_column_if_missing(db, "algo_order_signals", "processed_at", "processed_at TEXT");
    if (r.is_err())
        return r;

    r = sql(db, "CREATE TABLE IF NOT EXISTS candle_cache ("
                "  symbol TEXT NOT NULL,"
                "  timeframe TEXT NOT NULL,"
                "  open_time INTEGER NOT NULL,"
                "  o REAL NOT NULL,"
                "  h REAL NOT NULL,"
                "  l REAL NOT NULL,"
                "  c REAL NOT NULL,"
                "  volume REAL DEFAULT 0,"
                "  is_closed INTEGER DEFAULT 1,"
                "  updated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
                "  PRIMARY KEY (symbol, timeframe, open_time)"
                ")");
    if (r.is_err())
        return r;

    r = sql(db, "CREATE TABLE IF NOT EXISTS strategy_price_cache ("
                "  symbol TEXT PRIMARY KEY,"
                "  price REAL NOT NULL,"
                "  bid REAL DEFAULT 0,"
                "  ask REAL DEFAULT 0,"
                "  updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
                ")");
    if (r.is_err())
        return r;

    const char* indexes[] = {
        "CREATE INDEX IF NOT EXISTS idx_algo_deployments_status ON algo_deployments(status)",
        "CREATE INDEX IF NOT EXISTS idx_algo_deployments_strategy ON algo_deployments(strategy_id)",
        "CREATE INDEX IF NOT EXISTS idx_algo_order_signals_status ON algo_order_signals(status, created_at)",
        "CREATE INDEX IF NOT EXISTS idx_algo_order_signals_deployment ON algo_order_signals(deployment_id)",
        "CREATE INDEX IF NOT EXISTS idx_algo_trades_deployment ON algo_trades(deployment_id, timestamp)",
        "CREATE INDEX IF NOT EXISTS idx_candle_cache_symbol_tf_time ON candle_cache(symbol, timeframe, open_time)",
        "CREATE INDEX IF NOT EXISTS idx_strategy_price_cache_updated ON strategy_price_cache(updated_at)",
    };
    for (const char* stmt : indexes) {
        r = sql(db, stmt);
        if (r.is_err())
            return r;
    }

    return Result<void>::ok();
}

} // namespace

void register_migration_v021() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({21, "algo_trading_runtime", apply_v021});
}

} // namespace fincept
