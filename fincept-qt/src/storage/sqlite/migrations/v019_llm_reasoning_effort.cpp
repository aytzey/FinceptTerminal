// v019_llm_reasoning_effort — Add reasoning_effort to llm_configs.
// Stores per-provider Codex/OpenAI reasoning effort selection.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace fincept {
namespace {

static Result<void> sql(QSqlDatabase& db, const char* stmt) {
    QSqlQuery q(db);
    if (!q.exec(stmt))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v019(QSqlDatabase& db) {
    auto r = sql(db, "ALTER TABLE llm_configs ADD COLUMN reasoning_effort TEXT DEFAULT 'medium'");
    if (r.is_err()) {
        QSqlQuery check(db);
        check.exec("SELECT reasoning_effort FROM llm_configs LIMIT 1");
        if (check.lastError().isValid())
            return r;
    }

    QSqlQuery backfill(db);
    backfill.exec("UPDATE llm_configs SET reasoning_effort = 'medium' "
                  "WHERE reasoning_effort IS NULL OR trim(reasoning_effort) = ''");
    return Result<void>::ok();
}

} // anonymous namespace

void register_migration_v019() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({19, "llm_reasoning_effort", apply_v019});
}

} // namespace fincept
