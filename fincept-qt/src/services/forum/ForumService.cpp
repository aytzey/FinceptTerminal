// src/services/forum/ForumService.cpp
#include "services/forum/ForumService.h"

#include "auth/AuthManager.h"
#include "core/config/AppPaths.h"
#include "core/logging/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>

namespace fincept::services {
namespace {

QString local_store_path() {
    return fincept::AppPaths::data() + "/forum_local.json";
}

QString now_iso() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

QJsonObject default_profile_object() {
    const auto& session = auth::AuthManager::instance().session();
    const QString username = session.user_info.username.isEmpty() ? QStringLiteral("local_user") : session.user_info.username;
    const QString email = session.user_info.email;
    return QJsonObject{{"user_id", session.user_info.id},
                       {"username", username},
                       {"display_name", username == "local_user" ? QStringLiteral("Local User") : username},
                       {"bio", QStringLiteral("Local Fincept forum profile")},
                       {"avatar_color", QStringLiteral("#d97706")},
                       {"signature", QString()},
                       {"reputation", 0},
                       {"posts_count", 0},
                       {"comments_count", 0},
                       {"likes_received", 0},
                       {"likes_given", 0},
                       {"email", email},
                       {"created_at", now_iso()},
                       {"last_active_at", now_iso()}};
}

QJsonArray default_categories() {
    const QString created = now_iso();
    return QJsonArray{
        QJsonObject{{"id", 1},
                    {"name", "Markets"},
                    {"description", "Market structure, macro, flows, news and trade ideas"},
                    {"color", "#16a34a"},
                    {"post_count", 0},
                    {"display_order", 1},
                    {"created_at", created}},
        QJsonObject{{"id", 2},
                    {"name", "Research"},
                    {"description", "Equity research, quant workflows, valuation and portfolio notes"},
                    {"color", "#0891b2"},
                    {"post_count", 0},
                    {"display_order", 2},
                    {"created_at", created}},
        QJsonObject{{"id", 3},
                    {"name", "Terminal"},
                    {"description", "Local setup, Codex workflows, data sources and product feedback"},
                    {"color", "#d97706"},
                    {"post_count", 0},
                    {"display_order", 3},
                    {"created_at", created}},
    };
}

QJsonObject load_local_store() {
    QDir().mkpath(fincept::AppPaths::data());
    QFile file(local_store_path());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject existing = QJsonDocument::fromJson(file.readAll()).object();
        if (!existing.isEmpty()) {
            QJsonObject store = existing;
            if (!store.value("categories").isArray())
                store["categories"] = default_categories();
            if (!store.value("posts").isArray())
                store["posts"] = QJsonArray{};
            if (!store.value("comments").isArray())
                store["comments"] = QJsonArray{};
            if (!store.value("profile").isObject())
                store["profile"] = default_profile_object();
            if (store.value("next_post_id").toInt() <= 0)
                store["next_post_id"] = store.value("posts").toArray().size() + 1;
            if (store.value("next_comment_id").toInt() <= 0)
                store["next_comment_id"] = store.value("comments").toArray().size() + 1;
            return store;
        }
    }

    return QJsonObject{{"schema", 1},
                       {"categories", default_categories()},
                       {"posts", QJsonArray{}},
                       {"comments", QJsonArray{}},
                       {"profile", default_profile_object()},
                       {"next_post_id", 1},
                       {"next_comment_id", 1}};
}

bool save_local_store(const QJsonObject& store) {
    QDir().mkpath(fincept::AppPaths::data());
    QFile file(local_store_path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(store).toJson(QJsonDocument::Indented));
    return true;
}

QJsonObject category_by_id(const QJsonObject& store, int category_id) {
    for (const auto& value : store.value("categories").toArray()) {
        const QJsonObject category = value.toObject();
        if (category.value("id").toInt() == category_id)
            return category;
    }
    return {};
}

QJsonArray comments_for_post(const QJsonObject& store, const QString& post_uuid) {
    QJsonArray out;
    for (const auto& value : store.value("comments").toArray()) {
        const QJsonObject comment = value.toObject();
        if (comment.value("post_uuid").toString() == post_uuid)
            out.append(comment);
    }
    return out;
}

QJsonObject post_with_derived_fields(const QJsonObject& store, QJsonObject post) {
    const QJsonObject category = category_by_id(store, post.value("category_id").toInt());
    post["category_name"] = category.value("name").toString();
    post["category_color"] = category.value("color").toString();
    post["reply_count"] = comments_for_post(store, post.value("post_uuid").toString()).size();
    return post;
}

QJsonArray categories_with_counts(const QJsonObject& store) {
    QJsonArray categories;
    const QJsonArray posts = store.value("posts").toArray();
    for (const auto& value : store.value("categories").toArray()) {
        QJsonObject category = value.toObject();
        int count = 0;
        const int id = category.value("id").toInt();
        for (const auto& post_value : posts) {
            if (post_value.toObject().value("category_id").toInt() == id)
                ++count;
        }
        category["post_count"] = count;
        categories.append(category);
    }
    return categories;
}

QJsonObject permissions_object() {
    return QJsonObject{{"can_create_posts", true}, {"can_vote", true}, {"can_comment", true}};
}

QVector<QJsonObject> filtered_posts(const QJsonObject& store, int category_id, const QString& query = {}) {
    QVector<QJsonObject> posts;
    const QString q = query.toLower().trimmed();
    for (const auto& value : store.value("posts").toArray()) {
        QJsonObject post = post_with_derived_fields(store, value.toObject());
        if (category_id > 0 && post.value("category_id").toInt() != category_id)
            continue;
        if (!q.isEmpty()) {
            const QString haystack = (post.value("title").toString() + " " + post.value("content").toString() + " "
                                      + post.value("category_name").toString())
                                         .toLower();
            if (!haystack.contains(q))
                continue;
        }
        posts.append(post);
    }
    return posts;
}

QJsonObject posts_page_object(const QJsonObject& store, QVector<QJsonObject> posts, int category_id, int page,
                              const QString& sort) {
    const QString sort_by = sort.isEmpty() ? QStringLiteral("latest") : sort;
    std::sort(posts.begin(), posts.end(), [sort_by](const QJsonObject& a, const QJsonObject& b) {
        if (sort_by == "top") {
            const int as = a.value("likes").toInt() + a.value("reply_count").toInt();
            const int bs = b.value("likes").toInt() + b.value("reply_count").toInt();
            if (as != bs)
                return as > bs;
        }
        return a.value("created_at").toString() > b.value("created_at").toString();
    });

    const int limit = 20;
    page = std::max(1, page);
    const int total = posts.size();
    const int pages = std::max(1, (total + limit - 1) / limit);
    const int start = (page - 1) * limit;
    QJsonArray arr;
    for (int i = start; i < std::min(start + limit, total); ++i)
        arr.append(posts[i]);

    return QJsonObject{{"posts", arr},
                       {"pagination", QJsonObject{{"page", page}, {"limit", limit}, {"total", total}, {"pages", pages}}},
                       {"sort_by", sort_by},
                       {"category", category_by_id(store, category_id)}};
}

QJsonObject profile_envelope(const QJsonObject& store, const QString& requested_username = {}) {
    QJsonObject profile = store.value("profile").toObject(default_profile_object());
    const QString username = profile.value("username").toString("local_user");
    if (!requested_username.isEmpty() && requested_username.compare(username, Qt::CaseInsensitive) != 0)
        return {};

    int posts_count = 0;
    int comments_count = 0;
    int likes_received = 0;
    QJsonArray recent_posts;
    QVector<QJsonObject> own_posts = filtered_posts(store, 0);
    for (const auto& post : own_posts) {
        if (post.value("author_name").toString() == username) {
            ++posts_count;
            likes_received += post.value("likes").toInt();
            if (recent_posts.size() < 5)
                recent_posts.append(post);
        }
    }
    for (const auto& value : store.value("comments").toArray()) {
        const QJsonObject comment = value.toObject();
        if (comment.value("author_name").toString() == username) {
            ++comments_count;
            likes_received += comment.value("likes").toInt();
        }
    }
    profile["posts_count"] = posts_count;
    profile["comments_count"] = comments_count;
    profile["likes_received"] = likes_received;
    profile["reputation"] = likes_received + posts_count * 2 + comments_count;
    profile["last_active_at"] = now_iso();
    return QJsonObject{{"profile", profile}, {"recent_posts", recent_posts}, {"is_own_profile", true}};
}

QJsonObject stats_object(const QJsonObject& store) {
    const QJsonArray categories = categories_with_counts(store);
    const QJsonArray posts = store.value("posts").toArray();
    const QJsonArray comments = store.value("comments").toArray();
    int votes = 0;
    for (const auto& value : posts) {
        if (!value.toObject().value("user_vote").toString().isEmpty())
            ++votes;
    }
    for (const auto& value : comments) {
        if (!value.toObject().value("user_vote").toString().isEmpty())
            ++votes;
    }
    QJsonObject profile = profile_envelope(store).value("profile").toObject();
    return QJsonObject{{"total_categories", categories.size()},
                       {"total_posts", posts.size()},
                       {"total_comments", comments.size()},
                       {"total_votes", votes},
                       {"recent_posts_24h", posts.size()},
                       {"popular_categories", categories},
                       {"top_contributors",
                        QJsonArray{QJsonObject{{"display_name", profile.value("display_name").toString()},
                                               {"username", profile.value("username").toString()},
                                               {"reputation", profile.value("reputation").toInt()},
                                               {"posts_count", profile.value("posts_count").toInt()}}}}};
}

bool local_get(const QString& path, QJsonObject& data, QString& error) {
    QJsonObject store = load_local_store();
    QUrl url(QStringLiteral("local://forum") + path);
    const QString route = url.path();
    const QStringList parts = route.split('/', Qt::SkipEmptyParts);
    QUrlQuery query(url);

    if (route == "/forum/categories") {
        data = QJsonObject{{"categories", categories_with_counts(store)}, {"permissions", permissions_object()}};
        return true;
    }
    if (route == "/forum/stats") {
        data = stats_object(store);
        return true;
    }
    if (route == "/forum/posts/trending") {
        QVector<QJsonObject> posts = filtered_posts(store, 0);
        std::sort(posts.begin(), posts.end(), [](const QJsonObject& a, const QJsonObject& b) {
            const int as = a.value("likes").toInt() + a.value("reply_count").toInt() + a.value("views").toInt();
            const int bs = b.value("likes").toInt() + b.value("reply_count").toInt() + b.value("views").toInt();
            return as > bs;
        });
        QJsonArray arr;
        for (int i = 0; i < std::min(20, static_cast<int>(posts.size())); ++i)
            arr.append(posts[i]);
        data = QJsonObject{{"trending_posts", arr}, {"total", posts.size()}};
        return true;
    }
    if (route == "/forum/search") {
        const QString q = query.queryItemValue("q");
        const int page = query.queryItemValue("page").toInt();
        const auto page_obj = posts_page_object(store, filtered_posts(store, 0, q), 0, page, "latest");
        data = QJsonObject{{"results",
                            QJsonObject{{"posts", page_obj.value("posts").toArray()},
                                        {"total_results",
                                         page_obj.value("pagination").toObject().value("total").toInt()}}},
                           {"pagination", page_obj.value("pagination").toObject()}};
        return true;
    }
    if (route == "/forum/profile") {
        data = profile_envelope(store);
        return true;
    }
    if (parts.size() == 3 && parts[0] == "forum" && parts[1] == "profile") {
        data = profile_envelope(store, parts[2]);
        if (data.isEmpty()) {
            error = "Local forum profile not found";
            return false;
        }
        return true;
    }
    if (parts.size() == 4 && parts[0] == "forum" && parts[1] == "categories" && parts[3] == "posts") {
        const int category_id = parts[2].toInt();
        const int page = query.queryItemValue("page").toInt();
        const QString sort = query.queryItemValue("sort");
        data = posts_page_object(store, filtered_posts(store, category_id), category_id, page, sort);
        return true;
    }
    if (parts.size() == 3 && parts[0] == "forum" && parts[1] == "posts") {
        const QString uuid = parts[2];
        QJsonArray posts = store.value("posts").toArray();
        for (int i = 0; i < posts.size(); ++i) {
            QJsonObject post = posts[i].toObject();
            if (post.value("post_uuid").toString() != uuid)
                continue;
            post["views"] = post.value("views").toInt() + 1;
            posts.replace(i, post);
            store["posts"] = posts;
            save_local_store(store);
            const QJsonArray comments = comments_for_post(store, uuid);
            data = QJsonObject{{"post", post_with_derived_fields(store, post)},
                               {"comments", comments},
                               {"total_comments", comments.size()},
                               {"permissions", permissions_object()}};
            return true;
        }
        error = "Local forum post not found";
        return false;
    }

    error = "Local forum route not implemented: " + path;
    return false;
}

bool local_post(const QString& path, const QJsonObject& body, QJsonObject& data, QString& error) {
    QJsonObject store = load_local_store();
    const QStringList parts = path.split('/', Qt::SkipEmptyParts);
    QJsonObject profile = profile_envelope(store).value("profile").toObject();
    const QString username = profile.value("username").toString("local_user");
    const QString display_name = profile.value("display_name").toString(username);

    if (parts.size() == 4 && parts[0] == "forum" && parts[1] == "categories" && parts[3] == "posts") {
        const int category_id = parts[2].toInt();
        if (category_by_id(store, category_id).isEmpty()) {
            error = "Local forum category not found";
            return false;
        }
        QJsonArray posts = store.value("posts").toArray();
        const int id = store.value("next_post_id").toInt(1);
        QJsonObject post{{"id", id},
                         {"post_uuid", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                         {"category_id", category_id},
                         {"title", body.value("title").toString().trimmed()},
                         {"content", body.value("content").toString().trimmed()},
                         {"views", 0},
                         {"likes", 0},
                         {"reply_count", 0},
                         {"created_at", now_iso()},
                         {"updated_at", now_iso()},
                         {"author_name", username},
                         {"author_display_name", display_name},
                         {"user_vote", QString()}};
        posts.append(post);
        store["posts"] = posts;
        store["next_post_id"] = id + 1;
        if (!save_local_store(store)) {
            error = "Failed to save local forum store";
            return false;
        }
        data = post_with_derived_fields(store, post);
        return true;
    }

    if (parts.size() == 4 && parts[0] == "forum" && parts[1] == "posts" && parts[3] == "comments") {
        const QString post_uuid = parts[2];
        bool post_found = false;
        for (const auto& value : store.value("posts").toArray()) {
            if (value.toObject().value("post_uuid").toString() == post_uuid) {
                post_found = true;
                break;
            }
        }
        if (!post_found) {
            error = "Local forum post not found";
            return false;
        }
        QJsonArray comments = store.value("comments").toArray();
        const int id = store.value("next_comment_id").toInt(1);
        QJsonObject comment{{"id", id},
                            {"comment_uuid", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                            {"post_uuid", post_uuid},
                            {"content", body.value("content").toString().trimmed()},
                            {"likes", 0},
                            {"dislikes", 0},
                            {"created_at", now_iso()},
                            {"author_name", username},
                            {"author_display_name", display_name},
                            {"parent_comment_id", 0},
                            {"user_vote", QString()}};
        comments.append(comment);
        store["comments"] = comments;
        store["next_comment_id"] = id + 1;
        if (!save_local_store(store)) {
            error = "Failed to save local forum store";
            return false;
        }
        data = comment;
        return true;
    }

    if (parts.size() == 4 && parts[0] == "forum" && parts[1] == "posts" && parts[3] == "vote") {
        QJsonArray posts = store.value("posts").toArray();
        const QString uuid = parts[2];
        const QString vote = body.value("vote_type").toString();
        for (int i = 0; i < posts.size(); ++i) {
            QJsonObject post = posts[i].toObject();
            if (post.value("post_uuid").toString() != uuid)
                continue;
            const QString previous = post.value("user_vote").toString();
            int likes = post.value("likes").toInt();
            if (previous == "up")
                --likes;
            if (vote == "up")
                ++likes;
            post["likes"] = std::max(0, likes);
            post["user_vote"] = vote == "up" ? vote : QString();
            post["updated_at"] = now_iso();
            posts.replace(i, post);
            store["posts"] = posts;
            save_local_store(store);
            data = post_with_derived_fields(store, post);
            return true;
        }
        error = "Local forum post not found";
        return false;
    }

    if (parts.size() == 4 && parts[0] == "forum" && parts[1] == "comments" && parts[3] == "vote") {
        QJsonArray comments = store.value("comments").toArray();
        const QString uuid = parts[2];
        const QString vote = body.value("vote_type").toString();
        for (int i = 0; i < comments.size(); ++i) {
            QJsonObject comment = comments[i].toObject();
            if (comment.value("comment_uuid").toString() != uuid)
                continue;
            const QString previous = comment.value("user_vote").toString();
            int likes = comment.value("likes").toInt();
            int dislikes = comment.value("dislikes").toInt();
            if (previous == "up")
                --likes;
            if (previous == "down")
                --dislikes;
            if (vote == "up")
                ++likes;
            if (vote == "down")
                ++dislikes;
            comment["likes"] = std::max(0, likes);
            comment["dislikes"] = std::max(0, dislikes);
            comment["user_vote"] = (vote == "up" || vote == "down") ? vote : QString();
            comments.replace(i, comment);
            store["comments"] = comments;
            save_local_store(store);
            data = comment;
            return true;
        }
        error = "Local forum comment not found";
        return false;
    }

    error = "Local forum route not implemented: " + path;
    return false;
}

bool local_put(const QString& path, const QJsonObject& body, QJsonObject& data, QString& error) {
    if (path != "/forum/profile") {
        error = "Local forum route not implemented: " + path;
        return false;
    }
    QJsonObject store = load_local_store();
    QJsonObject profile = store.value("profile").toObject(default_profile_object());
    profile["display_name"] = body.value("display_name").toString(profile.value("display_name").toString()).trimmed();
    profile["bio"] = body.value("bio").toString().trimmed();
    profile["signature"] = body.value("signature").toString().trimmed();
    profile["avatar_color"] = body.value("avatar_color").toString(profile.value("avatar_color").toString()).trimmed();
    profile["last_active_at"] = now_iso();
    store["profile"] = profile;
    if (!save_local_store(store)) {
        error = "Failed to save local forum profile";
        return false;
    }
    data = profile_envelope(store);
    return true;
}

} // namespace

ForumService& ForumService::instance() {
    static ForumService s;
    return s;
}

ForumService::ForumService() = default;

// ── Low-level local store helpers ────────────────────────────────────────────

void ForumService::get(const QString& path, std::function<void(bool, QJsonObject)> cb) {
    QJsonObject data;
    QString error;
    const bool ok = local_get(path, data, error);
    if (!ok)
        LOG_WARN("ForumService", error);
    cb(ok, data);
}

void ForumService::post_req(const QString& path, const QJsonObject& body, std::function<void(bool, QJsonObject)> cb) {
    QJsonObject data;
    QString error;
    const bool ok = local_post(path, body, data, error);
    if (!ok)
        LOG_WARN("ForumService", error);
    cb(ok, data);
}

void ForumService::put_req(const QString& path, const QJsonObject& body, std::function<void(bool, QJsonObject)> cb) {
    QJsonObject data;
    QString error;
    const bool ok = local_put(path, body, data, error);
    if (!ok)
        LOG_WARN("ForumService", error);
    cb(ok, data);
}

// ── Parsers ───────────────────────────────────────────────────────────────────

ForumCategory ForumService::parse_category(const QJsonObject& o) {
    ForumCategory c;
    c.id = o.value("id").toInt();
    c.name = o.value("name").toString();
    c.description = o.value("description").toString();
    c.color = o.value("color").toString();
    c.post_count = o.value("post_count").toInt();
    c.display_order = o.value("display_order").toInt();
    c.created_at = o.value("created_at").toString();
    return c;
}

ForumPost ForumService::parse_post(const QJsonObject& o) {
    ForumPost p;
    p.id = o.value("id").toInt();
    p.post_uuid = o.value("post_uuid").toString();
    p.title = o.value("title").toString();
    p.content = o.value("content").toString();
    p.views = o.value("views").toInt();
    p.likes = o.value("likes").toInt();
    p.reply_count = o.value("reply_count").toInt();
    p.created_at = o.value("created_at").toString();
    p.updated_at = o.value("updated_at").toString();
    p.author_name = o.value("author_name").toString();
    p.author_display_name = o.value("author_display_name").toString();
    p.category_name = o.value("category_name").toString();
    p.category_color = o.value("category_color").toString();
    p.user_vote = o.value("user_vote").toString();
    return p;
}

ForumComment ForumService::parse_comment(const QJsonObject& o) {
    ForumComment c;
    c.id = o.value("id").toInt();
    c.comment_uuid = o.value("comment_uuid").toString();
    c.content = o.value("content").toString();
    c.likes = o.value("likes").toInt();
    c.dislikes = o.value("dislikes").toInt();
    c.created_at = o.value("created_at").toString();
    c.author_name = o.value("author_name").toString();
    c.author_display_name = o.value("author_display_name").toString();
    c.parent_comment_id = o.value("parent_comment_id").toInt(0);
    c.user_vote = o.value("user_vote").toString();
    return c;
}

ForumStats ForumService::parse_stats(const QJsonObject& o) {
    ForumStats s;
    s.total_categories = o.value("total_categories").toInt();
    s.total_posts = o.value("total_posts").toInt();
    s.total_comments = o.value("total_comments").toInt();
    s.total_votes = o.value("total_votes").toInt();
    s.recent_posts_24h = o.value("recent_posts_24h").toInt();
    for (const auto& v : o.value("popular_categories").toArray())
        s.popular_categories.append(parse_category(v.toObject()));
    for (const auto& v : o.value("top_contributors").toArray()) {
        auto obj = v.toObject();
        ForumContributor c;
        c.display_name = obj.value("display_name").toString();
        c.username = obj.value("username").toString();
        c.reputation = obj.value("reputation").toInt();
        c.posts_count = obj.value("posts_count").toInt();
        s.top_contributors.append(c);
    }
    return s;
}

ForumProfile ForumService::parse_profile(const QJsonObject& o) {
    auto pobj = o.value("profile").toObject();
    ForumProfile p;
    p.user_id = pobj.value("user_id").toInt();
    p.username = pobj.value("username").toString();
    p.display_name = pobj.value("display_name").toString();
    p.bio = pobj.value("bio").toString();
    p.avatar_color = pobj.value("avatar_color").toString("#d97706");
    p.signature = pobj.value("signature").toString();
    p.reputation = pobj.value("reputation").toInt();
    p.posts_count = pobj.value("posts_count").toInt();
    p.comments_count = pobj.value("comments_count").toInt();
    p.likes_received = pobj.value("likes_received").toInt();
    p.likes_given = pobj.value("likes_given").toInt();
    p.email = pobj.value("email").toString();
    p.created_at = pobj.value("created_at").toString();
    p.last_active_at = pobj.value("last_active_at").toString();
    p.is_own_profile = o.value("is_own_profile").toBool();
    for (const auto& v : o.value("recent_posts").toArray())
        p.recent_posts.append(parse_post(v.toObject()));
    return p;
}

// ── Public API ────────────────────────────────────────────────────────────────

void ForumService::fetch_categories(CategoriesCallback cb) {
    get("/forum/categories", [cb](bool ok, QJsonObject data) {
        if (!ok) {
            cb(false, {}, {});
            return;
        }
        QVector<ForumCategory> cats;
        for (const auto& v : data.value("categories").toArray())
            cats.append(parse_category(v.toObject()));
        ForumPermissions perms;
        auto po = data.value("permissions").toObject();
        perms.can_create_posts = po.value("can_create_posts").toBool();
        perms.can_vote = po.value("can_vote").toBool();
        perms.can_comment = po.value("can_comment").toBool();
        cb(true, cats, perms);
    });
}

void ForumService::fetch_posts(int category_id, int page, const QString& sort, PostsCallback cb) {
    QString path = QString("/forum/categories/%1/posts?page=%2&sort=%3")
                       .arg(category_id)
                       .arg(page)
                       .arg(sort.isEmpty() ? "latest" : sort);
    get(path, [cb](bool ok, QJsonObject data) {
        if (!ok) {
            cb(false, {});
            return;
        }
        ForumPostsPage result;
        for (const auto& v : data.value("posts").toArray())
            result.posts.append(parse_post(v.toObject()));
        auto pag = data.value("pagination").toObject();
        result.page = pag.value("page").toInt(1);
        result.limit = pag.value("limit").toInt(20);
        result.total = pag.value("total").toInt();
        result.pages = pag.value("pages").toInt(1);
        result.sort_by = data.value("sort_by").toString();
        auto cobj = data.value("category").toObject();
        result.category.id = cobj.value("id").toInt();
        result.category.name = cobj.value("name").toString();
        result.category.color = cobj.value("color").toString();
        cb(true, result);
    });
}

void ForumService::fetch_post(const QString& post_uuid, PostDetailCallback cb) {
    get("/forum/posts/" + post_uuid, [cb](bool ok, QJsonObject data) {
        if (!ok) {
            cb(false, {});
            return;
        }
        ForumPostDetail detail;
        detail.post = parse_post(data.value("post").toObject());
        for (const auto& v : data.value("comments").toArray())
            detail.comments.append(parse_comment(v.toObject()));
        detail.total_comments = data.value("total_comments").toInt();
        auto po = data.value("permissions").toObject();
        detail.permissions.can_comment = po.value("can_comment").toBool();
        detail.permissions.can_vote = po.value("can_vote").toBool();
        cb(true, detail);
    });
}

void ForumService::fetch_stats(StatsCallback cb) {
    get("/forum/stats", [cb](bool ok, QJsonObject data) {
        if (!ok) {
            cb(false, {});
            return;
        }
        cb(true, parse_stats(data));
    });
}

void ForumService::fetch_trending(PostsCallback cb) {
    get("/forum/posts/trending", [cb](bool ok, QJsonObject data) {
        if (!ok) {
            cb(false, {});
            return;
        }
        ForumPostsPage result;
        for (const auto& v : data.value("trending_posts").toArray())
            result.posts.append(parse_post(v.toObject()));
        result.total = data.value("total").toInt();
        cb(true, result);
    });
}

void ForumService::search(const QString& query, int page, PostsCallback cb) {
    QString path =
        QString("/forum/search?q=%1&page=%2").arg(QString::fromUtf8(QUrl::toPercentEncoding(query))).arg(page);
    get(path, [cb](bool ok, QJsonObject data) {
        if (!ok) {
            cb(false, {});
            return;
        }
        ForumPostsPage result;
        auto res = data.value("results").toObject();
        for (const auto& v : res.value("posts").toArray())
            result.posts.append(parse_post(v.toObject()));
        result.total = res.value("total_results").toInt();
        auto pag = data.value("pagination").toObject();
        result.page = pag.value("page").toInt(1);
        result.limit = pag.value("limit").toInt(20);
        cb(true, result);
    });
}

void ForumService::fetch_my_profile(ProfileCallback cb) {
    get("/forum/profile", [cb](bool ok, QJsonObject data) {
        if (!ok) {
            cb(false, {});
            return;
        }
        cb(true, parse_profile(data));
    });
}

void ForumService::fetch_profile(const QString& username, ProfileCallback cb) {
    get("/forum/profile/" + username, [cb](bool ok, QJsonObject data) {
        if (!ok) {
            cb(false, {});
            return;
        }
        cb(true, parse_profile(data));
    });
}

void ForumService::create_post(int category_id, const QString& title, const QString& content, BoolCallback cb) {
    QJsonObject body;
    body["title"] = title;
    body["content"] = content;
    post_req(QString("/forum/categories/%1/posts").arg(category_id), body,
             [cb](bool ok, QJsonObject) { cb(ok, ok ? "Post created" : "Failed to create post"); });
}

void ForumService::create_comment(const QString& post_uuid, const QString& content, BoolCallback cb) {
    QJsonObject body;
    body["content"] = content;
    post_req("/forum/posts/" + post_uuid + "/comments", body,
             [cb](bool ok, QJsonObject) { cb(ok, ok ? "Comment posted" : "Failed to post comment"); });
}

void ForumService::vote_post(const QString& post_uuid, const QString& vote_type, BoolCallback cb) {
    QJsonObject body;
    body["vote_type"] = vote_type;
    post_req("/forum/posts/" + post_uuid + "/vote", body, [cb](bool ok, QJsonObject) { cb(ok, {}); });
}

void ForumService::vote_comment(const QString& comment_uuid, const QString& vote_type, BoolCallback cb) {
    QJsonObject body;
    body["vote_type"] = vote_type;
    post_req("/forum/comments/" + comment_uuid + "/vote", body, [cb](bool ok, QJsonObject) { cb(ok, {}); });
}

void ForumService::update_profile(const QString& display_name, const QString& bio, const QString& signature,
                                  const QString& avatar_color, BoolCallback cb) {
    QJsonObject body;
    body["display_name"] = display_name;
    body["bio"] = bio;
    body["signature"] = signature;
    body["avatar_color"] = avatar_color;
    put_req("/forum/profile", body,
            [cb](bool ok, QJsonObject) { cb(ok, ok ? "Profile updated" : "Failed to update profile"); });
}

} // namespace fincept::services
