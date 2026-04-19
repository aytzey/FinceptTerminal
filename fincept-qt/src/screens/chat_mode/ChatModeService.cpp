#include "screens/chat_mode/ChatModeService.h"

#include "ai_chat/LlmService.h"
#include "auth/AuthManager.h"
#include "core/logging/Logger.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrent>

#include <algorithm>

namespace fincept::chat_mode {

static constexpr const char* API_BASE = "https://api.fincept.in";

namespace {

static constexpr const char* LOCAL_STORE_FILE = "chat_mode_local.json";

bool local_chat_mode_enabled() {
    auto& auth = auth::AuthManager::instance();
    return auth.has_local_runtime() || !auth.has_fincept_api_key();
}

QString now_iso() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString new_local_id(const QString& prefix) {
    return prefix + "_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString local_store_path() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.trimmed().isEmpty())
        dir = QDir(QDir::homePath()).filePath(".local/share/FinceptTerminal");
    QDir().mkpath(dir);
    return QDir(dir).filePath(LOCAL_STORE_FILE);
}

QJsonObject empty_local_store() {
    return QJsonObject{
        {"version", 1},
        {"sessions", QJsonArray()},
        {"memory", QJsonArray()},
        {"schedules", QJsonArray()},
        {"tasks", QJsonArray()},
        {"mcp_servers", QJsonArray()},
        {"monitors", QJsonArray()},
    };
}

QJsonObject normalize_local_store(QJsonObject store) {
    QJsonObject base = empty_local_store();
    for (auto it = base.constBegin(); it != base.constEnd(); ++it) {
        if (!store.contains(it.key()))
            store.insert(it.key(), it.value());
    }
    return store;
}

QJsonObject load_local_store(QString* error = nullptr) {
    QFile file(local_store_path());
    if (!file.exists())
        return empty_local_store();
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = "Could not open local chat store.";
        return empty_local_store();
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = "Local chat store is invalid JSON.";
        return empty_local_store();
    }
    return normalize_local_store(doc.object());
}

bool save_local_store(const QJsonObject& store, QString* error = nullptr) {
    QSaveFile file(local_store_path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = "Could not write local chat store.";
        return false;
    }
    file.write(QJsonDocument(normalize_local_store(store)).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = "Could not commit local chat store.";
        return false;
    }
    return true;
}

int find_object_index(const QJsonArray& arr, const QString& key, const QString& value) {
    for (int i = 0; i < arr.size(); ++i) {
        if (arr.at(i).toObject().value(key).toString() == value)
            return i;
    }
    return -1;
}

QJsonObject normalize_session(QJsonObject session) {
    const QJsonArray messages = session.value("messages").toArray();
    session["message_count"] = messages.size();
    if (!session.contains("session_uuid"))
        session["session_uuid"] = new_local_id("session");
    if (!session.contains("created_at"))
        session["created_at"] = now_iso();
    if (!session.contains("updated_at"))
        session["updated_at"] = session.value("created_at").toString(now_iso());
    if (!session.contains("last_message_at") && !messages.isEmpty())
        session["last_message_at"] = messages.last().toObject().value("created_at").toString();
    if (!session.contains("title") || session.value("title").toString().trimmed().isEmpty())
        session["title"] = "New Conversation";
    return session;
}

QJsonObject public_session(QJsonObject session) {
    session = normalize_session(session);
    session.remove("messages");
    return session;
}

bool is_default_title(const QString& title) {
    const QString t = title.trimmed();
    return t.isEmpty() || t == "New Conversation" || t == "(Untitled)";
}

QString title_from_message(const QString& content) {
    QString title = content.simplified();
    if (title.size() > 54)
        title = title.left(51).trimmed() + "...";
    return title.isEmpty() ? QString("New Conversation") : title;
}

struct LocalMessageSaveResult {
    bool ok = false;
    QString error;
    QJsonObject message;
    QString new_title;
};

LocalMessageSaveResult append_local_message(const QString& session_uuid, const QString& role, const QString& content,
                                            const QString& provider, const QString& model, int tokens_used,
                                            int response_time_ms) {
    LocalMessageSaveResult result;
    QString error;
    QJsonObject store = load_local_store(&error);
    QJsonArray sessions = store.value("sessions").toArray();
    int idx = find_object_index(sessions, "session_uuid", session_uuid);
    const QString ts = now_iso();

    if (idx < 0) {
        QJsonObject session;
        session["session_uuid"] = session_uuid.isEmpty() ? new_local_id("session") : session_uuid;
        session["title"] = "New Conversation";
        session["is_active"] = true;
        session["created_at"] = ts;
        session["updated_at"] = ts;
        session["last_message_at"] = ts;
        session["messages"] = QJsonArray();
        sessions.prepend(session);
        idx = 0;
    }

    QJsonObject session = normalize_session(sessions.at(idx).toObject());
    QJsonArray messages = session.value("messages").toArray();
    QJsonObject msg;
    msg["message_uuid"] = new_local_id("msg");
    msg["role"] = role;
    msg["content"] = content;
    msg["created_at"] = ts;
    if (!provider.isEmpty())
        msg["provider"] = provider;
    if (!model.isEmpty())
        msg["model"] = model;
    if (tokens_used > 0)
        msg["tokens_used"] = tokens_used;
    if (response_time_ms > 0)
        msg["response_time_ms"] = response_time_ms;
    messages.append(msg);

    if (role == "user" && is_default_title(session.value("title").toString())) {
        result.new_title = title_from_message(content);
        session["title"] = result.new_title;
    }

    session["messages"] = messages;
    session["message_count"] = messages.size();
    session["updated_at"] = ts;
    session["last_message_at"] = ts;
    sessions[idx] = session;
    store["sessions"] = sessions;

    if (!save_local_store(store, &error)) {
        result.error = error;
        return result;
    }

    result.ok = true;
    result.message = msg;
    return result;
}

std::vector<ai_chat::ConversationMessage> local_history_for_session(const QString& session_uuid, int limit = 40) {
    std::vector<ai_chat::ConversationMessage> history;
    const QJsonArray sessions = load_local_store().value("sessions").toArray();
    const int idx = find_object_index(sessions, "session_uuid", session_uuid);
    if (idx < 0)
        return history;

    const QJsonArray messages = sessions.at(idx).toObject().value("messages").toArray();
    const int start = std::max(0, static_cast<int>(messages.size()) - limit);
    for (int i = start; i < messages.size(); ++i) {
        const QJsonObject msg = messages.at(i).toObject();
        const QString role = msg.value("role").toString();
        const QString content = msg.value("content").toString();
        if (content.trimmed().isEmpty())
            continue;
        if (role == "user" || role == "assistant" || role == "system")
            history.push_back(ai_chat::ConversationMessage{role, content});
    }
    return history;
}

ChatStats local_stats() {
    ChatStats stats;
    const QJsonArray sessions = load_local_store().value("sessions").toArray();
    stats.total_sessions = sessions.size();
    for (const auto& v : sessions) {
        const QJsonObject session = normalize_session(v.toObject());
        stats.total_messages += session.value("messages").toArray().size();
        if (session.value("is_active").toBool())
            ++stats.active_sessions;
    }
    return stats;
}

QJsonArray local_export_payload(const QStringList& uuids) {
    QJsonArray out;
    const QJsonArray sessions = load_local_store().value("sessions").toArray();
    for (const auto& v : sessions) {
        QJsonObject session = normalize_session(v.toObject());
        const QString uuid = session.value("session_uuid").toString();
        if (!uuids.isEmpty() && !uuids.contains(uuid))
            continue;
        out.append(session);
    }
    return out;
}

bool update_local_object(const QString& array_key, const QString& id_key, const QString& id,
                         const std::function<void(QJsonObject&)>& mutate, QString* error = nullptr) {
    QJsonObject store = load_local_store(error);
    QJsonArray arr = store.value(array_key).toArray();
    const int idx = find_object_index(arr, id_key, id);
    if (idx < 0) {
        if (error)
            *error = "Local item not found.";
        return false;
    }
    QJsonObject obj = arr.at(idx).toObject();
    mutate(obj);
    arr[idx] = obj;
    store[array_key] = arr;
    return save_local_store(store, error);
}

QJsonDocument local_get_document(const QString& path, bool* ok, QString* error) {
    *ok = true;
    error->clear();
    const QJsonObject store = load_local_store(error);

    if (path == "/chat/sessions") {
        QJsonArray arr;
        for (const auto& v : store.value("sessions").toArray())
            arr.append(public_session(v.toObject()));
        return QJsonDocument(QJsonObject{{"success", true}, {"data", QJsonObject{{"sessions", arr}}}});
    }

    if (path.startsWith("/chat/sessions/")) {
        const QString uuid = path.mid(QString("/chat/sessions/").size());
        const QJsonArray sessions = store.value("sessions").toArray();
        const int idx = find_object_index(sessions, "session_uuid", uuid);
        if (idx < 0) {
            *ok = false;
            *error = "Local conversation not found.";
            return {};
        }
        const QJsonObject session = normalize_session(sessions.at(idx).toObject());
        return QJsonDocument(QJsonObject{{"success", true},
                                         {"data",
                                          QJsonObject{{"session", public_session(session)},
                                                      {"messages", session.value("messages").toArray()}}}});
    }

    if (path == "/chat/stats") {
        const ChatStats stats = local_stats();
        return QJsonDocument(QJsonObject{{"success", true},
                                         {"data",
                                          QJsonObject{{"total_sessions", stats.total_sessions},
                                                      {"total_messages", stats.total_messages},
                                                      {"active_sessions", stats.active_sessions}}}});
    }

    if (path.startsWith("/chat/search")) {
        QUrl url("http://local" + path);
        const QString query = QUrlQuery(url).queryItemValue("query").trimmed();
        QJsonArray results;
        if (query.size() >= 2) {
            for (const auto& sv : store.value("sessions").toArray()) {
                const QJsonObject session = sv.toObject();
                for (const auto& mv : session.value("messages").toArray()) {
                    const QJsonObject msg = mv.toObject();
                    if (msg.value("content").toString().contains(query, Qt::CaseInsensitive))
                        results.append(msg);
                }
            }
        }
        return QJsonDocument(QJsonObject{{"success", true},
                                         {"data", QJsonObject{{"results", results},
                                                              {"total", results.size()},
                                                              {"query", query}}}});
    }

    if (path.startsWith("/user/profile")) {
        return QJsonDocument(QJsonObject{{"success", true}, {"data", QJsonObject{{"credit_balance", 0}}}});
    }

    *ok = false;
    *error = "Local Chat Mode does not implement endpoint: " + path;
    return {};
}

} // namespace

// ── Singleton ─────────────────────────────────────────────────────────────────

ChatModeService& ChatModeService::instance() {
    static ChatModeService s;
    return s;
}

ChatModeService::ChatModeService(QObject* parent) : QObject(parent) {
    nam_ = new QNetworkAccessManager(this);
    sse_nam_ = new QNetworkAccessManager(this);
}

// ── Auth helpers ──────────────────────────────────────────────────────────────

QString ChatModeService::base_url() const {
    return QString::fromLatin1(API_BASE);
}

QString ChatModeService::api_key() const {
    return auth::AuthManager::instance().effective_api_key();
}

QString ChatModeService::session_token() const {
    return auth::AuthManager::instance().session().session_token;
}

QNetworkRequest ChatModeService::build_request(const QString& path) const {
    QNetworkRequest req{QUrl(base_url() + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader, "FinceptTerminal/4.0");
    const QString key = api_key();
    const QString tok = session_token();
    if (!key.isEmpty())
        req.setRawHeader("X-API-Key", key.toUtf8());
    if (!tok.isEmpty())
        req.setRawHeader("X-Session-Token", tok.toUtf8());
    return req;
}

// ── Reply handler ─────────────────────────────────────────────────────────────

void ChatModeService::handle_reply(QNetworkReply* reply,
                                   std::function<void(bool ok, QJsonDocument doc, QString error)> cb) {
    // 15-second timeout — abort hanging requests so they don't silently block
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->setInterval(15000);
    connect(timer, &QTimer::timeout, reply, [reply]() {
        LOG_WARN("ChatModeService", QString("Request timed out: %1").arg(reply->url().path()));
        reply->abort();
    });
    timer->start();

    connect(reply, &QNetworkReply::finished, this, [this, reply, cb = std::move(cb)]() mutable {
        // Read all data BEFORE deleteLater
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        const auto net_err = reply->error();
        const QString url_path = reply->url().path();
        reply->deleteLater();

        LOG_DEBUG("ChatModeService", QString("%1  HTTP %2  %3 bytes").arg(url_path).arg(status).arg(data.size()));

        if (status == 402) {
            LOG_WARN("ChatModeService", "Insufficient credits (402)");
            emit insufficient_credits();
            cb(false, {}, "Insufficient credits");
            return;
        }

        if (status == 401 || status == 403) {
            LOG_WARN("ChatModeService", QString("Auth error %1 on %2").arg(status).arg(url_path));
            cb(false, {}, QString("Auth error HTTP_%1").arg(status));
            return;
        }

        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(data, &pe);

        if (net_err != QNetworkReply::NoError && net_err != QNetworkReply::OperationCanceledError) {
            const QString err =
                QString("HTTP %1 %2: %3")
                    .arg(status)
                    .arg(url_path)
                    .arg(QNetworkReply::staticMetaObject
                             .enumerator(QNetworkReply::staticMetaObject.indexOfEnumerator("NetworkError"))
                             .valueToKey(net_err));
            LOG_WARN("ChatModeService", err);
            cb(false, doc, err);
            return;
        }

        if (pe.error != QJsonParseError::NoError) {
            const QString err = QString("JSON parse error on %1: %2").arg(url_path, pe.errorString());
            LOG_WARN("ChatModeService", err);
            cb(false, {}, err);
            return;
        }

        // Check API-level success flag if present
        const QJsonObject root = doc.object();
        if (root.contains("success") && !root["success"].toBool()) {
            const QString err = root["message"].toString("API returned success:false");
            LOG_WARN("ChatModeService", QString("%1: %2").arg(url_path, err));
            cb(false, doc, err);
            return;
        }

        cb(true, doc, {});
    });
}

// ── HTTP verbs ────────────────────────────────────────────────────────────────

void ChatModeService::get(const QString& path, std::function<void(bool, QJsonDocument, QString)> cb) {
    if (local_chat_mode_enabled()) {
        bool ok = false;
        QString error;
        QJsonDocument doc = local_get_document(path, &ok, &error);
        cb(ok, doc, error);
        return;
    }
    auto* reply = nam_->get(build_request(path));
    handle_reply(reply, std::move(cb));
}

void ChatModeService::post(const QString& path, const QJsonObject& body,
                           std::function<void(bool, QJsonDocument, QString)> cb) {
    if (local_chat_mode_enabled()) {
        cb(false, {}, "Local Chat Mode does not post to Fincept Cloud: " + path);
        return;
    }
    auto* reply = nam_->post(build_request(path), QJsonDocument(body).toJson(QJsonDocument::Compact));
    handle_reply(reply, std::move(cb));
}

void ChatModeService::put(const QString& path, const QJsonObject& body,
                          std::function<void(bool, QJsonDocument, QString)> cb) {
    if (local_chat_mode_enabled()) {
        cb(false, {}, "Local Chat Mode does not put to Fincept Cloud: " + path);
        return;
    }
    auto* reply = nam_->put(build_request(path), QJsonDocument(body).toJson(QJsonDocument::Compact));
    handle_reply(reply, std::move(cb));
}

void ChatModeService::del(const QString& path, std::function<void(bool, QJsonDocument, QString)> cb) {
    if (local_chat_mode_enabled()) {
        cb(false, {}, "Local Chat Mode does not delete from Fincept Cloud: " + path);
        return;
    }
    auto* reply = nam_->deleteResource(build_request(path));
    handle_reply(reply, std::move(cb));
}

void ChatModeService::del_with_body(const QString& path, const QJsonObject& body,
                                    std::function<void(bool, QJsonDocument, QString)> cb) {
    if (local_chat_mode_enabled()) {
        cb(false, {}, "Local Chat Mode does not delete from Fincept Cloud: " + path);
        return;
    }
    QNetworkRequest req = build_request(path);
    auto* reply = nam_->sendCustomRequest(req, "DELETE", QJsonDocument(body).toJson(QJsonDocument::Compact));
    handle_reply(reply, std::move(cb));
}

// ── Session CRUD ──────────────────────────────────────────────────────────────

void ChatModeService::create_session(const QString& title, SessionCallback cb) {
    LOG_INFO("ChatModeService", QString("Creating session: \"%1\"").arg(title));
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray sessions = store.value("sessions").toArray();
        const QString ts = now_iso();
        for (int i = 0; i < sessions.size(); ++i) {
            QJsonObject s = sessions.at(i).toObject();
            s["is_active"] = false;
            sessions[i] = s;
        }
        QJsonObject session;
        session["session_uuid"] = new_local_id("session");
        session["title"] = title.trimmed().isEmpty() ? QString("New Conversation") : title.trimmed();
        session["is_active"] = true;
        session["message_count"] = 0;
        session["created_at"] = ts;
        session["updated_at"] = ts;
        session["last_message_at"] = QString();
        session["messages"] = QJsonArray();
        sessions.prepend(session);
        store["sessions"] = sessions;
        if (!save_local_store(store, &error)) {
            cb(false, {}, error);
            return;
        }
        cb(true, ChatSession::from_json(public_session(session)), {});
        return;
    }
    QJsonObject body;
    body["title"] = title;
    post("/chat/sessions", body, [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            LOG_WARN("ChatModeService", "create_session failed: " + err);
            cb(false, {}, err);
            return;
        }
        const QJsonObject data = doc.object().value("data").toObject();
        const QJsonObject session = data.value("session").toObject();
        const QString uuid = session.value("session_uuid").toString();
        LOG_INFO("ChatModeService", QString("Session created: %1").arg(uuid));
        cb(true, ChatSession::from_json(session), {});
    });
}

void ChatModeService::list_sessions(SessionsCallback cb) {
    LOG_DEBUG("ChatModeService", "Listing sessions");
    if (local_chat_mode_enabled()) {
        QVector<QJsonObject> objects;
        const QJsonArray arr = load_local_store().value("sessions").toArray();
        objects.reserve(arr.size());
        for (const auto& v : arr)
            objects.append(public_session(v.toObject()));
        std::sort(objects.begin(), objects.end(), [](const QJsonObject& a, const QJsonObject& b) {
            return a.value("updated_at").toString() > b.value("updated_at").toString();
        });
        QVector<ChatSession> sessions;
        sessions.reserve(objects.size());
        for (const auto& s : objects)
            sessions.append(ChatSession::from_json(s));
        cb(true, sessions, {});
        return;
    }
    get("/chat/sessions", [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            LOG_WARN("ChatModeService", "list_sessions failed: " + err);
            cb(false, {}, err);
            return;
        }
        const QJsonArray arr = doc.object().value("data").toObject().value("sessions").toArray();
        QVector<ChatSession> sessions;
        sessions.reserve(arr.size());
        for (const auto& v : arr)
            sessions.append(ChatSession::from_json(v.toObject()));
        LOG_INFO("ChatModeService", QString("Listed %1 sessions").arg(sessions.size()));
        cb(true, sessions, {});
    });
}

void ChatModeService::get_session(const QString& uuid, SessionCallback cb) {
    LOG_DEBUG("ChatModeService", QString("Getting session: %1").arg(uuid));
    if (local_chat_mode_enabled()) {
        const QJsonArray sessions = load_local_store().value("sessions").toArray();
        const int idx = find_object_index(sessions, "session_uuid", uuid);
        if (idx < 0) {
            cb(false, {}, "Local conversation not found.");
            return;
        }
        cb(true, ChatSession::from_json(public_session(sessions.at(idx).toObject())), {});
        return;
    }
    get("/chat/sessions/" + uuid, [cb = std::move(cb), uuid](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            LOG_WARN("ChatModeService", "get_session failed: " + err);
            cb(false, {}, err);
            return;
        }
        const QJsonObject data = doc.object().value("data").toObject();
        const QJsonObject session = data.value("session").toObject();
        const int msg_count = data.value("messages").toArray().size();
        LOG_INFO("ChatModeService", QString("Got session %1 with %2 messages").arg(uuid).arg(msg_count));
        cb(true, ChatSession::from_json(session), {});
    });
}

void ChatModeService::delete_session(const QString& uuid, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray sessions = store.value("sessions").toArray();
        const int idx = find_object_index(sessions, "session_uuid", uuid);
        if (idx >= 0)
            sessions.removeAt(idx);
        store["sessions"] = sessions;
        cb(save_local_store(store, &error), error);
        return;
    }
    del("/chat/sessions/" + uuid, [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

void ChatModeService::rename_session(const QString& uuid, const QString& title, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray sessions = store.value("sessions").toArray();
        const int idx = find_object_index(sessions, "session_uuid", uuid);
        if (idx < 0) {
            cb(false, "Local conversation not found.");
            return;
        }
        QJsonObject session = sessions.at(idx).toObject();
        session["title"] = title.trimmed();
        session["updated_at"] = now_iso();
        sessions[idx] = session;
        store["sessions"] = sessions;
        cb(save_local_store(store, &error), error);
        return;
    }
    QJsonObject body;
    body["title"] = title;
    put("/chat/sessions/" + uuid + "/title", body,
        [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

void ChatModeService::activate_session(const QString& uuid, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray sessions = store.value("sessions").toArray();
        bool found = false;
        for (int i = 0; i < sessions.size(); ++i) {
            QJsonObject session = sessions.at(i).toObject();
            const bool match = session.value("session_uuid").toString() == uuid;
            session["is_active"] = match;
            found = found || match;
            sessions[i] = session;
        }
        if (!found) {
            cb(false, "Local conversation not found.");
            return;
        }
        store["sessions"] = sessions;
        cb(save_local_store(store, &error), error);
        return;
    }
    put("/chat/sessions/" + uuid + "/activate", {},
        [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

// ── Save message ──────────────────────────────────────────────────────────────

void ChatModeService::save_message(const QString& session_uuid, const QString& role, const QString& content,
                                   MessageCallback cb, const QString& provider, const QString& model, int tokens_used,
                                   int response_time_ms) {
    LOG_INFO("ChatModeService", QString("Saving %1 message to session %2").arg(role, session_uuid));
    if (local_chat_mode_enabled()) {
        const auto saved =
            append_local_message(session_uuid, role, content, provider, model, tokens_used, response_time_ms);
        if (!saved.ok) {
            cb(false, {}, {}, saved.error);
            return;
        }
        cb(true, ChatMessage::from_json(saved.message), saved.new_title, {});
        return;
    }
    QJsonObject body;
    body["role"] = role;
    body["content"] = content;
    if (!provider.isEmpty())
        body["provider"] = provider;
    if (!model.isEmpty())
        body["model"] = model;
    if (tokens_used > 0)
        body["tokens_used"] = tokens_used;
    if (response_time_ms > 0)
        body["response_time_ms"] = response_time_ms;

    post("/chat/sessions/" + session_uuid + "/save-message", body,
         [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
             if (!ok) {
                 cb(false, {}, {}, err);
                 return;
             }
             const QJsonObject data = doc.object().value("data").toObject();
             const QJsonObject msg_obj = data.value("message").toObject();
             ChatMessage msg;
             msg.uuid = msg_obj["message_uuid"].toString();
             msg.role = msg_obj["role"].toString();
             msg.content = msg_obj["content"].toString();
             msg.created_at = msg_obj["created_at"].toString();
             const QString new_title = data["new_title"].toString();
             cb(true, msg, new_title, {});
         });
}

// ── Session utilities ─────────────────────────────────────────────────────────

void ChatModeService::get_stats(StatsCallback cb) {
    if (local_chat_mode_enabled()) {
        cb(true, local_stats(), {});
        return;
    }
    get("/chat/stats", [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, err);
            return;
        }
        const QJsonObject data = doc.object().value("data").toObject(doc.object());
        cb(true, ChatStats::from_json(data), {});
    });
}

void ChatModeService::search_messages(const QString& query, SearchCallback cb) {
    if (local_chat_mode_enabled()) {
        QVector<ChatMessage> results;
        const QString needle = query.trimmed();
        if (needle.size() >= 2) {
            const QJsonArray sessions = load_local_store().value("sessions").toArray();
            for (const auto& sv : sessions) {
                const QJsonArray messages = sv.toObject().value("messages").toArray();
                for (const auto& mv : messages) {
                    const QJsonObject msg = mv.toObject();
                    if (msg.value("content").toString().contains(needle, Qt::CaseInsensitive))
                        results.append(ChatMessage::from_json(msg));
                }
            }
        }
        cb(true, results, {});
        return;
    }
    const QString path =
        QString("/chat/search?query=%1&limit=20").arg(QString::fromUtf8(QUrl::toPercentEncoding(query)));
    get(path, [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, err);
            return;
        }
        // Response: { data: { results: [...], total: N, query: "..." } }
        const QJsonArray arr = doc.object().value("data").toObject().value("results").toArray();
        QVector<ChatMessage> results;
        results.reserve(arr.size());
        for (const auto& v : arr) {
            const QJsonObject o = v.toObject();
            ChatMessage m;
            m.uuid = o["message_uuid"].toString();
            m.role = o["role"].toString();
            m.content = o["content"].toString();
            m.created_at = o["created_at"].toString();
            results.append(m);
        }
        cb(true, results, {});
    });
}

void ChatModeService::bulk_delete_sessions(const QStringList& uuids, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray kept;
        for (const auto& v : store.value("sessions").toArray()) {
            const QJsonObject session = v.toObject();
            if (!uuids.contains(session.value("session_uuid").toString()))
                kept.append(session);
        }
        store["sessions"] = kept;
        cb(save_local_store(store, &error), error);
        return;
    }
    QJsonObject body;
    QJsonArray arr;
    for (const auto& u : uuids)
        arr.append(u);
    body["session_uuids"] = arr;
    del_with_body("/chat/sessions/bulk-delete", body,
                  [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

void ChatModeService::export_sessions(const QStringList& uuids, std::function<void(bool, QJsonArray, QString)> cb) {
    if (local_chat_mode_enabled()) {
        cb(true, local_export_payload(uuids), {});
        return;
    }
    QJsonObject body;
    QJsonArray arr;
    for (const auto& u : uuids)
        arr.append(u);
    body["session_uuids"] = arr;
    body["format"] = "json";
    post("/chat/export", body, [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, err);
            return;
        }
        const QJsonArray data = doc.object().value("data").toObject().value("data").toArray();
        cb(true, data, {});
    });
}

// ── Optimize prompt ───────────────────────────────────────────────────────────

void ChatModeService::optimize_prompt(const QString& prompt, const QString& mode, OptimizeCallback cb) {
    LOG_INFO("ChatModeService", QString("Optimizing prompt (%1): \"%2\"").arg(mode, prompt.left(60)));
    if (local_chat_mode_enabled()) {
        if (!ai_chat::LlmService::instance().is_configured()) {
            cb(false, {}, "No local LLM provider configured. Connect Codex OAuth or set an LLM provider.");
            return;
        }
        QPointer<ChatModeService> self = this;
        auto future = QtConcurrent::run([self, prompt, mode, cb = std::move(cb)]() mutable {
            const QString request =
                QString("Rewrite this as a precise, finance-focused analysis prompt. Keep the user's intent, add "
                        "useful constraints, and return only the improved prompt.\n\nMode: %1\n\nPrompt:\n%2")
                    .arg(mode.isEmpty() ? QString("lite") : mode, prompt);
            auto resp = ai_chat::LlmService::instance().chat(request, {}, false);
            QMetaObject::invokeMethod(
                self,
                [self, prompt, resp, cb = std::move(cb)]() mutable {
                    if (!self)
                        return;
                    if (!resp.success) {
                        cb(false, {}, resp.error);
                        return;
                    }
                    OptimizedPrompt result;
                    result.original = prompt;
                    result.optimized = resp.content.trimmed();
                    cb(true, result, {});
                },
                Qt::QueuedConnection);
        });
        Q_UNUSED(future);
        return;
    }
    QJsonObject body;
    body["prompt"] = prompt;
    if (!mode.isEmpty())
        body["mode"] = mode;

    // Longer timeout for LLM calls
    auto* reply =
        nam_->post(build_request("/chat/optimize-prompt"), QJsonDocument(body).toJson(QJsonDocument::Compact));
    // Override default 15s timeout — LLM needs more time
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->setInterval(45000);
    connect(timer, &QTimer::timeout, reply, [reply]() {
        LOG_WARN("ChatModeService", "optimize-prompt timed out");
        reply->abort();
    });
    timer->start();

    connect(reply, &QNetworkReply::finished, this, [this, reply, cb = std::move(cb)]() mutable {
        const QByteArray data = reply->readAll();
        const auto net_err = reply->error();
        reply->deleteLater();

        if (net_err != QNetworkReply::NoError && net_err != QNetworkReply::OperationCanceledError) {
            cb(false, {}, "Optimize prompt request failed");
            return;
        }

        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
        if (pe.error != QJsonParseError::NoError) {
            cb(false, {}, "JSON parse error");
            return;
        }

        const QJsonObject root = doc.object();
        if (root.contains("success") && !root["success"].toBool()) {
            cb(false, {}, root["message"].toString("Optimize failed"));
            return;
        }

        const QJsonObject res_data = root.value("data").toObject();
        OptimizedPrompt result;
        result.original = res_data["original"].toString();

        // Response can have optimized_prompt as array or optimized as string
        if (res_data.contains("optimized")) {
            result.optimized = res_data["optimized"].toString();
        } else if (res_data.contains("optimized_prompt")) {
            // Array format — extract the "text" type entry
            const QJsonArray arr = res_data["optimized_prompt"].toArray();
            for (const auto& v : arr) {
                const QJsonObject item = v.toObject();
                if (item["type"].toString() == "text") {
                    result.optimized = item["text"].toString();
                    break;
                }
            }
        }

        LOG_INFO("ChatModeService", QString("Prompt optimized: %1 chars -> %2 chars")
                                        .arg(result.original.length())
                                        .arg(result.optimized.length()));
        cb(true, result, {});
    });
}

// ── Agent chat (non-streaming) ────────────────────────────────────────────────

void ChatModeService::agent_chat(const QString& query, const QString& session_id, StreamMode mode, AgentChatCallback cb,
                                 const QString& source, bool auto_approve) {
    LOG_INFO("ChatModeService",
             QString("Agent chat [%1]: \"%2\"").arg(mode == StreamMode::Deep ? "deep" : "lite", query.left(60)));
    if (local_chat_mode_enabled()) {
        if (!ai_chat::LlmService::instance().is_configured()) {
            cb(false, {}, "No local LLM provider configured. Connect Codex OAuth or set an LLM provider.");
            return;
        }

        QString target_session_id = session_id;
        if (target_session_id.isEmpty()) {
            create_session("New Conversation",
                           [&target_session_id](bool ok, ChatSession session, QString) {
                               if (ok)
                                   target_session_id = session.uuid;
                           });
        }
        const auto history = local_history_for_session(target_session_id);
        save_message(target_session_id, "user", query, [](bool, ChatMessage, QString, QString) {});

        QPointer<ChatModeService> self = this;
        auto future = QtConcurrent::run([self, query, session_id = target_session_id, history, cb = std::move(cb)]() mutable {
            QElapsedTimer timer;
            timer.start();
            auto resp = ai_chat::LlmService::instance().chat(query, history, true);
            const int elapsed_ms = static_cast<int>(timer.elapsed());
            const QString provider = ai_chat::LlmService::instance().active_provider();
            const QString model = ai_chat::LlmService::instance().active_model();
            QMetaObject::invokeMethod(
                self,
                [self, session_id, resp, provider, model, elapsed_ms, cb = std::move(cb)]() mutable {
                    if (!self)
                        return;
                    if (!resp.success) {
                        cb(false, {}, resp.error);
                        return;
                    }
                    QString new_title;
                    self->save_message(session_id, "assistant", resp.content,
                                       [&new_title](bool, ChatMessage, QString title, QString) { new_title = title; },
                                       provider, model, resp.total_tokens, elapsed_ms);
                    AgentChatResponse out;
                    out.session_id = session_id;
                    out.thread_id = session_id;
                    out.response = resp.content;
                    out.status = "completed";
                    out.total_tokens = resp.total_tokens;
                    out.new_title = new_title;
                    cb(true, out, {});
                },
                Qt::QueuedConnection);
        });
        Q_UNUSED(future);
        return;
    }

    QJsonObject body;
    body["query"] = query;
    body["mode"] = (mode == StreamMode::Deep) ? "deep" : "lite";
    body["auto_approve"] = auto_approve;
    if (!session_id.isEmpty())
        body["session_id"] = session_id;
    if (!source.isEmpty())
        body["source"] = source;

    // Longer timeout for agent calls
    auto* reply = nam_->post(build_request("/chat/agent/chat"), QJsonDocument(body).toJson(QJsonDocument::Compact));
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->setInterval(120000); // 2 min for deep analysis
    connect(timer, &QTimer::timeout, reply, [reply]() {
        LOG_WARN("ChatModeService", "agent/chat timed out");
        reply->abort();
    });
    timer->start();

    connect(reply, &QNetworkReply::finished, this, [this, reply, cb = std::move(cb)]() mutable {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        const auto net_err = reply->error();
        reply->deleteLater();

        if (status == 402) {
            emit insufficient_credits();
            cb(false, {}, "Insufficient credits");
            return;
        }

        if (net_err != QNetworkReply::NoError && net_err != QNetworkReply::OperationCanceledError) {
            cb(false, {}, "Agent chat request failed");
            return;
        }

        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
        if (pe.error != QJsonParseError::NoError) {
            cb(false, {}, "JSON parse error");
            return;
        }

        const QJsonObject root = doc.object();
        if (root.contains("success") && !root["success"].toBool()) {
            cb(false, {}, root["message"].toString("Agent chat failed"));
            return;
        }

        cb(true, AgentChatResponse::from_json(root), {});
    });
}

// ── Agent memory ──────────────────────────────────────────────────────────────

void ChatModeService::list_memory(MemoriesCallback cb) {
    if (local_chat_mode_enabled()) {
        QVector<AgentMemory> memories;
        for (const auto& v : load_local_store().value("memory").toArray())
            memories.append(AgentMemory::from_json(v.toObject()));
        cb(true, memories, {});
        return;
    }
    get("/chat/agent/memory", [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, err);
            return;
        }
        QVector<AgentMemory> memories;
        const QJsonArray arr = doc.object().value("memories").toArray();
        for (const auto& v : arr)
            memories.append(AgentMemory::from_json(v.toObject()));
        cb(true, memories, {});
    });
}

void ChatModeService::save_memory(const QString& key, const QString& value, const QString& memory_type,
                                  VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray memory = store.value("memory").toArray();
        const int idx = find_object_index(memory, "key", key);
        QJsonObject item;
        item["key"] = key;
        item["value"] = value;
        item["memory_type"] = memory_type;
        item["created_at"] = now_iso();
        if (idx >= 0)
            memory[idx] = item;
        else
            memory.prepend(item);
        store["memory"] = memory;
        cb(save_local_store(store, &error), error);
        return;
    }
    QJsonObject body;
    body["key"] = key;
    body["value"] = value;
    body["memory_type"] = memory_type;
    post("/chat/agent/memory", body, [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

void ChatModeService::delete_memory(const QString& key, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray kept;
        for (const auto& v : store.value("memory").toArray()) {
            if (v.toObject().value("key").toString() != key)
                kept.append(v);
        }
        store["memory"] = kept;
        cb(save_local_store(store, &error), error);
        return;
    }
    del("/chat/agent/memory/" + key, [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

void ChatModeService::clear_all_memory(VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        store["memory"] = QJsonArray();
        cb(save_local_store(store, &error), error);
        return;
    }
    del("/chat/agent/memory", [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

// ── Agent schedules ───────────────────────────────────────────────────────────

void ChatModeService::list_schedules(SchedulesCallback cb) {
    if (local_chat_mode_enabled()) {
        QVector<AgentSchedule> schedules;
        for (const auto& v : load_local_store().value("schedules").toArray())
            schedules.append(AgentSchedule::from_json(v.toObject()));
        cb(true, schedules, {});
        return;
    }
    get("/chat/agent/schedules", [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, err);
            return;
        }
        QVector<AgentSchedule> schedules;
        const QJsonArray arr = doc.object().value("schedules").toArray();
        for (const auto& v : arr)
            schedules.append(AgentSchedule::from_json(v.toObject()));
        cb(true, schedules, {});
    });
}

void ChatModeService::create_schedule(const QString& query, const QString& cron_expression, const QString& session_id,
                                      ScheduleCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray schedules = store.value("schedules").toArray();
        QJsonObject schedule;
        schedule["schedule_id"] = new_local_id("schedule");
        schedule["query"] = query;
        schedule["cron_expression"] = cron_expression;
        schedule["status"] = "active";
        schedule["session_id"] = session_id;
        schedule["created_at"] = now_iso();
        schedules.prepend(schedule);
        store["schedules"] = schedules;
        if (!save_local_store(store, &error)) {
            cb(false, {}, error);
            return;
        }
        cb(true, AgentSchedule::from_json(schedule), {});
        return;
    }
    QJsonObject body;
    body["query"] = query;
    body["cron_expression"] = cron_expression;
    if (!session_id.isEmpty())
        body["session_id"] = session_id;
    post("/chat/agent/schedules", body, [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, err);
            return;
        }
        const QJsonObject data = doc.object().value("data").toObject(doc.object());
        cb(true, AgentSchedule::from_json(data), {});
    });
}

void ChatModeService::delete_schedule(const QString& schedule_id, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray kept;
        for (const auto& v : store.value("schedules").toArray()) {
            if (v.toObject().value("schedule_id").toString() != schedule_id)
                kept.append(v);
        }
        store["schedules"] = kept;
        cb(save_local_store(store, &error), error);
        return;
    }
    del("/chat/agent/schedules/" + schedule_id,
        [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

void ChatModeService::pause_schedule(const QString& schedule_id, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray schedules = store.value("schedules").toArray();
        const int idx = find_object_index(schedules, "schedule_id", schedule_id);
        if (idx < 0) {
            cb(false, "Local schedule not found.");
            return;
        }
        QJsonObject schedule = schedules.at(idx).toObject();
        schedule["status"] = "paused";
        schedules[idx] = schedule;
        store["schedules"] = schedules;
        cb(save_local_store(store, &error), error);
        return;
    }
    put("/chat/agent/schedules/" + schedule_id + "/pause", {},
        [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

void ChatModeService::resume_schedule(const QString& schedule_id, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray schedules = store.value("schedules").toArray();
        const int idx = find_object_index(schedules, "schedule_id", schedule_id);
        if (idx < 0) {
            cb(false, "Local schedule not found.");
            return;
        }
        QJsonObject schedule = schedules.at(idx).toObject();
        schedule["status"] = "active";
        schedules[idx] = schedule;
        store["schedules"] = schedules;
        cb(save_local_store(store, &error), error);
        return;
    }
    put("/chat/agent/schedules/" + schedule_id + "/resume", {},
        [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

// ── Agent tasks ───────────────────────────────────────────────────────────────

void ChatModeService::list_tasks(TasksCallback cb) {
    if (local_chat_mode_enabled()) {
        QVector<AgentTask> tasks;
        for (const auto& v : load_local_store().value("tasks").toArray())
            tasks.append(AgentTask::from_json(v.toObject()));
        cb(true, tasks, {});
        return;
    }
    get("/chat/agent/tasks?limit=50", [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, err);
            return;
        }
        QVector<AgentTask> tasks;
        const QJsonArray arr = doc.object().value("tasks").toArray();
        for (const auto& v : arr)
            tasks.append(AgentTask::from_json(v.toObject()));
        cb(true, tasks, {});
    });
}

void ChatModeService::create_task(const QString& query, const QString& session_id, TaskCallback cb) {
    if (local_chat_mode_enabled()) {
        if (!ai_chat::LlmService::instance().is_configured()) {
            cb(false, {}, "No local LLM provider configured. Connect Codex OAuth or set an LLM provider.");
            return;
        }
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray tasks = store.value("tasks").toArray();
        const QString ts = now_iso();
        QJsonObject task;
        task["task_id"] = new_local_id("task");
        task["query"] = query;
        task["status"] = "running";
        task["session_id"] = session_id;
        task["created_at"] = ts;
        task["updated_at"] = ts;
        task["started_at"] = ts;
        tasks.prepend(task);
        store["tasks"] = tasks;
        if (!save_local_store(store, &error)) {
            cb(false, {}, error);
            return;
        }
        cb(true, AgentTask::from_json(task), {});

        const QString task_id = task.value("task_id").toString();
        const auto history = local_history_for_session(session_id);
        auto future = QtConcurrent::run([query, task_id, history]() {
            auto resp = ai_chat::LlmService::instance().chat(query, history, true);
            QString update_error;
            update_local_object("tasks", "task_id", task_id, [resp](QJsonObject& t) {
                t["status"] = resp.success ? "completed" : "error";
                t["result"] = resp.success ? resp.content : resp.error;
                t["updated_at"] = now_iso();
                t["completed_at"] = now_iso();
            }, &update_error);
        });
        Q_UNUSED(future);
        return;
    }
    QJsonObject body;
    body["query"] = query;
    if (!session_id.isEmpty())
        body["session_id"] = session_id;
    post("/chat/agent/tasks", body, [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, err);
            return;
        }
        const QJsonObject data = doc.object().value("data").toObject(doc.object());
        cb(true, AgentTask::from_json(data), {});
    });
}

void ChatModeService::get_task(const QString& task_id, TaskCallback cb) {
    if (local_chat_mode_enabled()) {
        const QJsonArray tasks = load_local_store().value("tasks").toArray();
        const int idx = find_object_index(tasks, "task_id", task_id);
        if (idx < 0) {
            cb(false, {}, "Local task not found.");
            return;
        }
        cb(true, AgentTask::from_json(tasks.at(idx).toObject()), {});
        return;
    }
    get("/chat/agent/tasks/" + task_id, [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, err);
            return;
        }
        const QJsonObject data = doc.object().value("data").toObject(doc.object());
        cb(true, AgentTask::from_json(data), {});
    });
}

void ChatModeService::cancel_task(const QString& task_id, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        const bool ok = update_local_object("tasks", "task_id", task_id,
                                            [](QJsonObject& t) {
                                                t["status"] = "cancelled";
                                                t["updated_at"] = now_iso();
                                                t["completed_at"] = now_iso();
                                            },
                                            &error);
        cb(ok, error);
        return;
    }
    del("/chat/agent/tasks/" + task_id, [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

void ChatModeService::send_task_feedback(const QString& task_id, const QString& feedback, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        const bool ok = update_local_object("tasks", "task_id", task_id,
                                            [feedback](QJsonObject& t) {
                                                QJsonArray feedbacks = t.value("feedback").toArray();
                                                feedbacks.append(QJsonObject{{"text", feedback}, {"created_at", now_iso()}});
                                                t["feedback"] = feedbacks;
                                                t["updated_at"] = now_iso();
                                            },
                                            &error);
        cb(ok, error);
        return;
    }
    QJsonObject body;
    body["feedback"] = feedback;
    post("/chat/agent/tasks/" + task_id + "/feedback", body,
         [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

void ChatModeService::get_task_history(const QString& task_id, HistoryCallback cb) {
    if (local_chat_mode_enabled()) {
        const QJsonArray tasks = load_local_store().value("tasks").toArray();
        const int idx = find_object_index(tasks, "task_id", task_id);
        if (idx < 0) {
            cb(false, {}, {}, {}, "Local task not found.");
            return;
        }
        const QJsonObject task = tasks.at(idx).toObject();
        TaskHistoryStep step;
        step.step = 1;
        step.timestamp = task.value("updated_at").toString(task.value("created_at").toString());
        step.next = QStringList{task.value("status").toString()};
        cb(true, task_id, task_id, QVector<TaskHistoryStep>{step}, {});
        return;
    }
    get("/chat/agent/tasks/" + task_id + "/history",
        [cb = std::move(cb), task_id](bool ok, QJsonDocument doc, QString err) {
            if (!ok) {
                cb(false, {}, {}, {}, err);
                return;
            }
            const QJsonObject root = doc.object();
            const QString tid = root["task_id"].toString();
            const QString thread = root["thread_id"].toString();
            QVector<TaskHistoryStep> steps;
            for (const auto& v : root["history"].toArray())
                steps.append(TaskHistoryStep::from_json(v.toObject()));
            cb(true, tid, thread, steps, {});
        });
}

void ChatModeService::get_task_activity(const QString& task_id, ActivityCallback cb, int after_id, int limit) {
    if (local_chat_mode_enabled()) {
        Q_UNUSED(after_id);
        Q_UNUSED(limit);
        const QJsonArray tasks = load_local_store().value("tasks").toArray();
        const int idx = find_object_index(tasks, "task_id", task_id);
        if (idx < 0) {
            cb(false, {}, {}, 0, "Local task not found.");
            return;
        }
        const QJsonObject task = tasks.at(idx).toObject();
        TaskActivity event;
        event.id = 1;
        event.event_type = task.value("status").toString();
        event.timestamp = task.value("updated_at").toString(task.value("created_at").toString());
        event.data = QJsonObject{{"result", task.value("result").toString()}};
        cb(true, task_id, QVector<TaskActivity>{event}, 1, {});
        return;
    }
    QString path = QString("/chat/agent/tasks/%1/activity?limit=%2").arg(task_id).arg(limit);
    if (after_id > 0)
        path += QString("&after_id=%1").arg(after_id);

    get(path, [cb = std::move(cb), task_id](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, {}, 0, err);
            return;
        }
        const QJsonObject root = doc.object();
        QVector<TaskActivity> events;
        for (const auto& v : root["events"].toArray())
            events.append(TaskActivity::from_json(v.toObject()));
        const int count = root["count"].toInt();
        cb(true, task_id, events, count, {});
    });
}

// ── Task activity SSE stream ──────────────────────────────────────────────────

QNetworkReply* ChatModeService::stream_task_activity(const QString& task_id, int after_id) {
    if (local_chat_mode_enabled()) {
        Q_UNUSED(task_id);
        Q_UNUSED(after_id);
        QTimer::singleShot(0, this, [this]() { emit task_activity_done(); });
        return nullptr;
    }
    abort_task_activity_stream();

    const QString path = QString("/chat/agent/tasks/%1/activity/stream?after_id=%2").arg(task_id).arg(after_id);
    QNetworkRequest req = build_request(path);
    req.setRawHeader("Accept", "text/event-stream");
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);

    task_sse_reply_ = sse_nam_->get(req);
    task_sse_event_.clear();

    connect(task_sse_reply_, &QNetworkReply::readyRead, this, [this]() {
        while (task_sse_reply_ && task_sse_reply_->canReadLine()) {
            const QByteArray line = task_sse_reply_->readLine().trimmed();
            handle_task_sse_line(line);
        }
    });

    connect(task_sse_reply_, &QNetworkReply::finished, this, [this]() {
        if (!task_sse_reply_)
            return;
        task_sse_reply_->deleteLater();
        task_sse_reply_ = nullptr;
    });

    return task_sse_reply_;
}

void ChatModeService::abort_task_activity_stream() {
    if (local_chat_mode_enabled()) {
        emit task_activity_done();
        return;
    }
    if (task_sse_reply_) {
        task_sse_reply_->abort();
        task_sse_reply_->deleteLater();
        task_sse_reply_ = nullptr;
    }
}

void ChatModeService::handle_task_sse_line(const QByteArray& line) {
    if (line.startsWith("event:")) {
        task_sse_event_ = QString::fromUtf8(line.mid(6).trimmed());
        return;
    }

    if (line.startsWith("data:")) {
        const QByteArray json_bytes = line.mid(5).trimmed();
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(json_bytes, &pe);
        if (pe.error != QJsonParseError::NoError)
            return;

        const QString& ev = task_sse_event_;
        if (ev == "activity") {
            emit task_activity_event(TaskActivity::from_json(doc.object()));
        } else if (ev == "done") {
            emit task_activity_done();
        }

        task_sse_event_.clear();
    }
}

// ── Credits ──────────────────────────────────────────────────────────────────

void ChatModeService::get_credits(CreditsCallback cb) {
    if (local_chat_mode_enabled()) {
        cb(true, -1, {});
        return;
    }
    const QString path = QString("/user/profile?_t=%1").arg(QDateTime::currentMSecsSinceEpoch());
    get(path, [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, 0, err);
            return;
        }
        const int credits = doc.object().value("data").toObject().value("credit_balance").toInt();
        cb(true, credits, {});
    });
}

// ── Terminal tool bridge ──────────────────────────────────────────────────────

void ChatModeService::register_terminal_tools(const QJsonArray& tools, const QString& version, int tool_count,
                                              RegisterCallback cb) {
    LOG_INFO("ChatModeService", QString("Registering %1 terminal tools (v%2)").arg(tool_count).arg(version));
    if (local_chat_mode_enabled()) {
        Q_UNUSED(tools);
        cb(true, tool_count, {});
        return;
    }
    QJsonObject body;
    body["tools"] = tools;
    body["terminal_version"] = version;
    body["tool_count"] = tool_count;
    post("/chat/agent/terminal-tools/register", body, [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, 0, err);
            return;
        }
        const int reg = doc.object()["registered"].toInt();
        cb(true, reg, {});
    });
}

void ChatModeService::poll_pending_calls(PendingCallsCallback cb, int limit) {
    if (local_chat_mode_enabled()) {
        Q_UNUSED(limit);
        cb(true, QJsonArray(), {});
        return;
    }
    const QString path = QString("/chat/agent/terminal-tools/pending?limit=%1").arg(limit);
    get(path, [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, err);
            return;
        }
        cb(true, doc.object()["calls"].toArray(), {});
    });
}

void ChatModeService::submit_tool_result(const QString& call_id, const QJsonObject& result, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        Q_UNUSED(call_id);
        Q_UNUSED(result);
        cb(true, {});
        return;
    }
    QJsonObject body;
    body["call_id"] = call_id;
    body["result"] = result;
    post("/chat/agent/terminal-tools/result", body,
         [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

// ── MCP servers ───────────────────────────────────────────────────────────────

void ChatModeService::list_mcp_servers(McpServersCallback cb) {
    if (local_chat_mode_enabled()) {
        QVector<McpServer> servers;
        int total_tools = 0;
        for (const auto& v : load_local_store().value("mcp_servers").toArray()) {
            const McpServer server = McpServer::from_json(v.toObject());
            total_tools += server.tools_count;
            servers.append(server);
        }
        cb(true, servers, total_tools, {});
        return;
    }
    get("/chat/agent/mcp/servers", [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, 0, err);
            return;
        }
        const QJsonObject root = doc.object();
        QVector<McpServer> servers;
        for (const auto& v : root["servers"].toArray())
            servers.append(McpServer::from_json(v.toObject()));
        const int total_tools = root["total_tools"].toInt();
        cb(true, servers, total_tools, {});
    });
}

void ChatModeService::add_mcp_server(const QString& name, const QJsonObject& config, McpServerCallback cb) {
    LOG_INFO("ChatModeService", QString("Adding MCP server: %1").arg(name));
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray servers = store.value("mcp_servers").toArray();
        const int idx = find_object_index(servers, "name", name);
        QJsonObject server;
        server["name"] = name;
        server["transport"] = config.value("transport").toString("stdio");
        server["status"] = "configured";
        server["tools_count"] = 0;
        server["tool_names"] = QJsonArray();
        server["tools"] = QJsonArray();
        server["config"] = config;
        if (idx >= 0)
            servers[idx] = server;
        else
            servers.prepend(server);
        store["mcp_servers"] = servers;
        if (!save_local_store(store, &error)) {
            cb(false, {}, error);
            return;
        }
        cb(true, McpServer::from_json(server), {});
        return;
    }
    QJsonObject body;
    body["name"] = name;
    body["config"] = config;
    post("/chat/agent/mcp/servers", body, [cb = std::move(cb), name](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, err);
            return;
        }
        const QJsonObject root = doc.object();
        McpServer srv;
        srv.name = root["name"].toString(name);
        srv.tools_count = root["tools_loaded"].toInt();
        srv.status = root.contains("error") && !root["error"].toString().isEmpty() ? "error" : "connected";
        for (const auto& v : root["tools"].toArray())
            srv.tools.append(McpTool::from_json(v.toObject()));
        cb(true, srv, {});
    });
}

void ChatModeService::delete_mcp_server(const QString& server_name, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray kept;
        for (const auto& v : store.value("mcp_servers").toArray()) {
            if (v.toObject().value("name").toString() != server_name)
                kept.append(v);
        }
        store["mcp_servers"] = kept;
        cb(save_local_store(store, &error), error);
        return;
    }
    del("/chat/agent/mcp/servers/" + server_name,
        [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

void ChatModeService::refresh_mcp_servers(McpServersCallback cb) {
    if (local_chat_mode_enabled()) {
        list_mcp_servers(std::move(cb));
        return;
    }
    post("/chat/agent/mcp/servers/refresh", {}, [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, 0, err);
            return;
        }
        const QJsonObject root = doc.object();
        QVector<McpServer> servers;
        for (const auto& v : root["servers"].toArray())
            servers.append(McpServer::from_json(v.toObject()));
        const int total_tools = root["total_tools"].toInt();
        cb(true, servers, total_tools, {});
    });
}

// ── Agent monitors ────────────────────────────────────────────────────────────

void ChatModeService::list_monitors(MonitorsCallback cb) {
    if (local_chat_mode_enabled()) {
        QVector<AgentMonitor> monitors;
        for (const auto& v : load_local_store().value("monitors").toArray())
            monitors.append(AgentMonitor::from_json(v.toObject()));
        cb(true, monitors, {});
        return;
    }
    get("/chat/agent/monitors", [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, err);
            return;
        }
        QVector<AgentMonitor> monitors;
        for (const auto& v : doc.object()["monitors"].toArray())
            monitors.append(AgentMonitor::from_json(v.toObject()));
        cb(true, monitors, {});
    });
}

void ChatModeService::create_monitor(const QString& name, const QString& source_type, const QJsonObject& source_config,
                                     const QJsonObject& trigger_config, const QString& analysis_query,
                                     int check_interval_seconds, const QString& session_id, MonitorCallback cb) {
    LOG_INFO("ChatModeService", QString("Creating monitor: %1").arg(name));
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray monitors = store.value("monitors").toArray();
        QJsonObject monitor;
        monitor["monitor_id"] = new_local_id("monitor");
        monitor["name"] = name;
        monitor["source_type"] = source_type;
        monitor["source_config"] = source_config;
        monitor["trigger_config"] = trigger_config;
        monitor["analysis_query"] = analysis_query;
        monitor["check_interval_seconds"] = check_interval_seconds;
        monitor["session_id"] = session_id;
        monitor["status"] = "active";
        monitor["created_at"] = now_iso();
        monitors.prepend(monitor);
        store["monitors"] = monitors;
        if (!save_local_store(store, &error)) {
            cb(false, {}, error);
            return;
        }
        cb(true, AgentMonitor::from_json(monitor), {});
        return;
    }
    QJsonObject body;
    body["name"] = name;
    body["source_type"] = source_type;
    body["source_config"] = source_config;
    body["trigger_config"] = trigger_config;
    body["analysis_query"] = analysis_query;
    body["check_interval_seconds"] = check_interval_seconds;
    if (!session_id.isEmpty())
        body["session_id"] = session_id;

    post("/chat/agent/monitors", body, [cb = std::move(cb)](bool ok, QJsonDocument doc, QString err) {
        if (!ok) {
            cb(false, {}, err);
            return;
        }
        const QJsonObject root = doc.object();
        cb(true, AgentMonitor::from_json(root), {});
    });
}

void ChatModeService::delete_monitor(const QString& monitor_id, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        QJsonObject store = load_local_store(&error);
        QJsonArray kept;
        for (const auto& v : store.value("monitors").toArray()) {
            if (v.toObject().value("monitor_id").toString() != monitor_id)
                kept.append(v);
        }
        store["monitors"] = kept;
        cb(save_local_store(store, &error), error);
        return;
    }
    del("/chat/agent/monitors/" + monitor_id,
        [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

void ChatModeService::pause_monitor(const QString& monitor_id, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        const bool ok = update_local_object("monitors", "monitor_id", monitor_id,
                                            [](QJsonObject& m) { m["status"] = "paused"; }, &error);
        cb(ok, error);
        return;
    }
    put("/chat/agent/monitors/" + monitor_id + "/pause", {},
        [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

void ChatModeService::resume_monitor(const QString& monitor_id, VoidCallback cb) {
    if (local_chat_mode_enabled()) {
        QString error;
        const bool ok = update_local_object("monitors", "monitor_id", monitor_id,
                                            [](QJsonObject& m) { m["status"] = "active"; }, &error);
        cb(ok, error);
        return;
    }
    put("/chat/agent/monitors/" + monitor_id + "/resume", {},
        [cb = std::move(cb)](bool ok, QJsonDocument, QString err) { cb(ok, err); });
}

// ── SSE streaming ─────────────────────────────────────────────────────────────

QNetworkReply* ChatModeService::stream_message(const QString& message, const QString& session_id, StreamMode mode,
                                               const QString& source, bool auto_approve, int profile_id) {
    LOG_INFO("ChatModeService", QString("Streaming message to session %1 [%2]: \"%3\"")
                                    .arg(session_id)
                                    .arg(mode == StreamMode::Deep ? "deep" : "lite")
                                    .arg(message.left(60)));

    // Abort any previous stream
    abort_stream();

    if (local_chat_mode_enabled()) {
        Q_UNUSED(source);
        Q_UNUSED(auto_approve);
        Q_UNUSED(profile_id);
        if (!ai_chat::LlmService::instance().is_configured()) {
            emit stream_error("No local LLM provider configured. Connect Codex OAuth or set an LLM provider.");
            return nullptr;
        }

        const quint64 generation = ++local_stream_generation_;
        local_stream_active_ = true;
        const auto history = local_history_for_session(session_id);
        QString new_title;
        save_message(session_id, "user", message,
                     [&new_title](bool, ChatMessage, QString title, QString) { new_title = title; });
        emit stream_session_meta(session_id, new_title);
        emit stream_step_start(1);

        QPointer<ChatModeService> self = this;
        auto future = QtConcurrent::run([self, generation, message, session_id, history]() {
            QElapsedTimer timer;
            timer.start();
            auto resp = ai_chat::LlmService::instance().chat(message, history, true);
            const int elapsed_ms = static_cast<int>(timer.elapsed());
            const QString provider = ai_chat::LlmService::instance().active_provider();
            const QString model = ai_chat::LlmService::instance().active_model();

            QMetaObject::invokeMethod(
                self,
                [self, generation, session_id, resp, provider, model, elapsed_ms]() {
                    if (!self || generation != self->local_stream_generation_)
                        return;
                    self->local_stream_active_ = false;
                    if (!resp.success) {
                        emit self->stream_error(resp.error.isEmpty() ? QString("Local LLM request failed.") : resp.error);
                        return;
                    }
                    if (!resp.content.isEmpty())
                        emit self->stream_text_delta(resp.content);
                    self->save_message(session_id, "assistant", resp.content,
                                       [](bool, ChatMessage, QString, QString) {}, provider, model, resp.total_tokens,
                                       elapsed_ms);
                    emit self->stream_step_finish(resp.total_tokens);
                    emit self->stream_finish(resp.total_tokens);
                },
                Qt::QueuedConnection);
        });
        Q_UNUSED(future);
        return nullptr;
    }

    QJsonObject body;
    body["message"] = message;
    body["session_id"] = session_id;
    body["mode"] = (mode == StreamMode::Deep) ? "deep" : "lite";
    body["auto_approve"] = auto_approve;
    body["profile_id"] = profile_id;
    if (!source.isEmpty())
        body["source"] = source;
    else
        body["source"] = QJsonValue::Null;

    QNetworkRequest req = build_request("/chat/agent/stream");
    // Accept SSE
    req.setRawHeader("Accept", "text/event-stream");
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);

    sse_reply_ = sse_nam_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    sse_current_event_.clear();

    connect(sse_reply_, &QNetworkReply::readyRead, this, [this]() {
        while (sse_reply_ && sse_reply_->canReadLine()) {
            const QByteArray line = sse_reply_->readLine().trimmed();
            handle_sse_line(line);
        }
    });

    connect(sse_reply_, &QNetworkReply::finished, this, [this]() {
        if (!sse_reply_)
            return;
        const int status = sse_reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto net_err = sse_reply_->error();
        LOG_INFO("ChatModeService", QString("SSE stream finished — HTTP %1, net_err=%2").arg(status).arg(net_err));
        if (status == 402)
            emit insufficient_credits();
        if (net_err != QNetworkReply::NoError && net_err != QNetworkReply::OperationCanceledError) {
            const QString body = QString::fromUtf8(sse_reply_->readAll().left(200));
            LOG_WARN("ChatModeService", QString("SSE error body: %1").arg(body));
            emit stream_error(QString("Stream failed (HTTP %1)").arg(status));
        }
        sse_reply_->deleteLater();
        sse_reply_ = nullptr;
    });

    return sse_reply_;
}

void ChatModeService::abort_stream() {
    if (local_stream_active_) {
        ++local_stream_generation_;
        local_stream_active_ = false;
        emit stream_error("Stream stopped.");
        return;
    }
    if (sse_reply_) {
        sse_reply_->abort();
        sse_reply_->deleteLater();
        sse_reply_ = nullptr;
    }
}

void ChatModeService::handle_sse_line(const QByteArray& line) {
    if (line.startsWith("event:")) {
        sse_current_event_ = QString::fromUtf8(line.mid(6).trimmed());
        return;
    }

    if (line.startsWith("data:")) {
        const QByteArray json_bytes = line.mid(5).trimmed();
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(json_bytes, &pe);
        if (pe.error != QJsonParseError::NoError)
            return;

        const QJsonObject data = doc.object();
        const QString& ev = sse_current_event_;

        if (ev == "session-meta") {
            const QString sid = data["session_id"].toString();
            const QString title = data["new_title"].toString();
            LOG_INFO("ChatModeService", QString("SSE session-meta: id=%1 title=%2").arg(sid, title));
            emit stream_session_meta(sid, title);
        } else if (ev == "text-delta") {
            emit stream_text_delta(data["text"].toString());
        } else if (ev == "tool-end") {
            const QString tool = data["tool"].toString();
            const int dur = data["durationMs"].toInt();
            LOG_DEBUG("ChatModeService", QString("SSE tool-end: %1 (%2ms)").arg(tool).arg(dur));
            emit stream_tool_end(tool, dur);
        } else if (ev == "step-start") {
            const int step = data["stepNumber"].toInt();
            LOG_DEBUG("ChatModeService", QString("SSE step-start: step %1").arg(step));
            emit stream_step_start(step);
        } else if (ev == "step-finish") {
            LOG_DEBUG("ChatModeService", QString("SSE step-finish: %1 tokens").arg(data["tokensUsed"].toInt()));
            emit stream_step_finish(data["tokensUsed"].toInt());
        } else if (ev == "thinking") {
            const QString content = data["content"].toString();
            LOG_DEBUG("ChatModeService", QString("SSE thinking: %1 chars").arg(content.length()));
            emit stream_thinking(content);
        } else if (ev == "finish") {
            const int total = data["totalTokens"].toInt();
            LOG_INFO("ChatModeService", QString("SSE finish: %1 total tokens").arg(total));
            emit stream_finish(total);
        } else if (ev == "heartbeat") {
            LOG_DEBUG("ChatModeService", "SSE heartbeat");
            emit stream_heartbeat();
        } else if (ev == "error") {
            const QString msg = data["message"].toString();
            LOG_WARN("ChatModeService", "SSE error: " + msg);
            emit stream_error(msg);
        } else if (!ev.isEmpty()) {
            LOG_DEBUG("ChatModeService", "SSE unknown event: " + ev);
        }

        sse_current_event_.clear();
    }
}

} // namespace fincept::chat_mode
