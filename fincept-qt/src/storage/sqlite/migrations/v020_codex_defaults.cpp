// v020_codex_defaults — Upgrade legacy Codex defaults to gpt-5.4/high.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace fincept {
namespace {

Result<void> apply_v020(QSqlDatabase& db) {
    QSqlQuery q1(db);
    if (!q1.exec("UPDATE llm_configs SET model = 'gpt-5.4' "
                 "WHERE lower(provider) = 'openai-codex' "
                 "AND (model IS NULL OR trim(model) = '' OR model = 'gpt-5.3-codex')")) {
        return Result<void>::err(q1.lastError().text().toStdString());
    }

    QSqlQuery q2(db);
    if (!q2.exec("UPDATE llm_configs SET reasoning_effort = 'high' "
                 "WHERE lower(provider) = 'openai-codex' "
                 "AND (reasoning_effort IS NULL OR trim(reasoning_effort) = '' OR lower(reasoning_effort) = 'medium')")) {
        return Result<void>::err(q2.lastError().text().toStdString());
    }

    return Result<void>::ok();
}

} // namespace

void register_migration_v020() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({20, "codex_defaults", apply_v020});
}

} // namespace fincept
