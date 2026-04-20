// v022_algo_order_reconciliation — Track broker order status for algo signals.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace fincept {
namespace {

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

Result<void> sql(QSqlDatabase& db, const char* stmt) {
    QSqlQuery q(db);
    if (!q.exec(stmt))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v022(QSqlDatabase& db) {
    auto r = add_column_if_missing(db, "algo_order_signals", "broker_status", "broker_status TEXT");
    if (r.is_err())
        return r;
    r = add_column_if_missing(db, "algo_order_signals", "filled_qty", "filled_qty REAL DEFAULT 0");
    if (r.is_err())
        return r;
    r = add_column_if_missing(db, "algo_order_signals", "avg_price", "avg_price REAL DEFAULT 0");
    if (r.is_err())
        return r;
    r = add_column_if_missing(db, "algo_order_signals", "submitted_at", "submitted_at TEXT");
    if (r.is_err())
        return r;
    r = add_column_if_missing(db, "algo_order_signals", "last_reconciled_at", "last_reconciled_at TEXT");
    if (r.is_err())
        return r;
    r = add_column_if_missing(db, "algo_order_signals", "applied_at", "applied_at TEXT");
    if (r.is_err())
        return r;
    r = add_column_if_missing(db, "algo_order_signals", "signal_reason", "signal_reason TEXT");
    if (r.is_err())
        return r;

    return sql(db, "CREATE INDEX IF NOT EXISTS idx_algo_order_signals_order_id "
                   "ON algo_order_signals(order_id)");
}

} // namespace

void register_migration_v022() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({22, "algo_order_reconciliation", apply_v022});
}

} // namespace fincept
