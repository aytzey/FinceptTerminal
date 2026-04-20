// v023_algo_order_apply_state — Ensure live algo fills are applied exactly once.

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

Result<void> apply_v023(QSqlDatabase& db) {
    auto r = add_column_if_missing(db, "algo_order_signals", "applied_at", "applied_at TEXT");
    if (r.is_err())
        return r;
    return add_column_if_missing(db, "algo_order_signals", "signal_reason", "signal_reason TEXT");
}

} // namespace

void register_migration_v023() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({23, "algo_order_apply_state", apply_v023});
}

} // namespace fincept
