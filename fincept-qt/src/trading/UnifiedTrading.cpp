// Unified Trading — routes orders to live broker or paper trading engine

#include "trading/UnifiedTrading.h"

#include "core/logging/Logger.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/sqlite/Database.h"
#include "trading/AccountManager.h"
#include "trading/PaperTrading.h"

#include <QJsonObject>
#include <QDateTime>
#include <QHash>
#include <QMetaType>
#include <QMetaObject>
#include <QMutexLocker>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QVariantList>

#include <cmath>

namespace fincept::trading {

namespace {

QString basic_order_error(const UnifiedOrder& order) {
    if (order.symbol.trimmed().isEmpty())
        return "Order symbol is required";
    if (!std::isfinite(order.quantity) || order.quantity <= 0.0)
        return "Order quantity must be greater than zero";
    if ((order.order_type == OrderType::Limit || order.order_type == OrderType::StopLossLimit) &&
        (!std::isfinite(order.price) || order.price <= 0.0)) {
        return "Limit order price must be greater than zero";
    }
    if ((order.order_type == OrderType::StopLoss || order.order_type == OrderType::StopLossLimit) &&
        (!std::isfinite(order.stop_price) || order.stop_price <= 0.0)) {
        return "Stop price must be greater than zero";
    }
    return {};
}

OrderType parse_order_type(const QString& raw) {
    const QString s = raw.trimmed().toUpper();
    if (s == "LIMIT")
        return OrderType::Limit;
    if (s == "STOP" || s == "STOP_LOSS" || s == "SL")
        return OrderType::StopLoss;
    if (s == "STOP_LIMIT" || s == "STOP_LOSS_LIMIT" || s == "SL_LIMIT")
        return OrderType::StopLossLimit;
    return OrderType::Market;
}

void split_symbol(const QString& raw, UnifiedOrder* order) {
    const int colon = raw.indexOf(':');
    if (colon > 0) {
        order->exchange = raw.left(colon).trimmed();
        order->symbol = raw.mid(colon + 1).trimmed();
    } else {
        order->symbol = raw.trimmed();
    }
}

bool exec_bridge_update(const QString& sql, const QVariantList& params) {
    auto r = fincept::Database::instance().execute(sql, params);
    if (r.is_err()) {
        LOG_ERROR("UnifiedTrading", QString("Order bridge DB update failed: %1")
                                        .arg(QString::fromStdString(r.error())));
        return false;
    }
    return true;
}

bool setting_bool(const QStringList& keys, bool default_value = false) {
    for (const QString& key : keys) {
        auto r = fincept::SettingsRepository::instance().get(key);
        if (!r.is_ok() || r.value().trimmed().isEmpty())
            continue;
        const QString v = r.value().trimmed().toLower();
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }
    return default_value;
}

double setting_double(const QString& key, double default_value) {
    auto r = fincept::SettingsRepository::instance().get(key);
    if (!r.is_ok())
        return default_value;
    bool ok = false;
    const double value = r.value().trimmed().toDouble(&ok);
    return ok && std::isfinite(value) ? value : default_value;
}

bool trading_kill_switch_enabled() {
    return setting_bool({"trading.kill_switch", "algo.kill_switch", "algo_trading.kill_switch"}, false);
}

double max_live_order_value() {
    const double value = setting_double("trading.max_live_order_value", 1'000'000.0);
    return value > 0.0 ? value : 1'000'000.0;
}

bool is_filled_status(const QString& status) {
    const QString s = status.trimmed().toLower();
    return s.contains("complete") || s.contains("filled") || s.contains("executed") || s == "closed";
}

bool is_failed_status(const QString& status) {
    const QString s = status.trimmed().toLower();
    return s.contains("reject") || s.contains("cancel") || s.contains("fail") || s.contains("error") ||
           s.contains("expired");
}

} // namespace

UnifiedTrading& UnifiedTrading::instance() {
    static UnifiedTrading ut;
    return ut;
}

// ============================================================================
// Session Management
// ============================================================================

TradingSession UnifiedTrading::init_session(const QString& broker, const QString& mode,
                                            const QString& paper_portfolio_id) {
    QMutexLocker lock(&mutex_);

    TradingSession session;
    session.broker = broker;
    session.mode = (mode == "live") ? "live" : "paper";
    session.is_connected = false;

    if (session.mode == "paper") {
        if (!paper_portfolio_id.isEmpty()) {
            session.paper_portfolio_id = paper_portfolio_id;
        } else {
            auto portfolio = pt_create_portfolio(broker + " Paper Trading", 1000000.0, "INR", 1.0, "cross", 0.0003);
            session.paper_portfolio_id = portfolio.id;
        }
    }

    session_ = session;
    return session;
}

std::optional<TradingSession> UnifiedTrading::get_session() const {
    QMutexLocker lock(&mutex_);
    return session_;
}

TradingSession UnifiedTrading::switch_mode(const QString& mode) {
    QMutexLocker lock(&mutex_);

    if (!session_) {
        session_ = TradingSession{};
        session_->broker = "fyers";
    }

    session_->mode = (mode == "live") ? "live" : "paper";

    if (session_->mode == "paper" && session_->paper_portfolio_id.isEmpty()) {
        auto portfolio =
            pt_create_portfolio(session_->broker + " Paper Trading", 1000000.0, "INR", 1.0, "cross", 0.0003);
        session_->paper_portfolio_id = portfolio.id;
    }

    return *session_;
}

// ============================================================================
// Order Routing
// ============================================================================

UnifiedOrderResponse UnifiedTrading::place_order(const UnifiedOrder& order) {
    QMutexLocker lock(&mutex_);

    if (!session_) {
        return {false, "", "No active trading session. Call init_session first.", ""};
    }
    const QString validation = basic_order_error(order);
    if (!validation.isEmpty())
        return {false, "", validation, session_->mode};

    if (session_->mode == "paper") {
        return place_paper_order(*session_, order);
    }
    return place_live_order(*session_, order);
}

UnifiedOrderResponse UnifiedTrading::place_paper_order(const TradingSession& session, const UnifiedOrder& order) {
    if (session.paper_portfolio_id.isEmpty()) {
        return {false, "", "No paper portfolio configured", "paper"};
    }

    QString symbol = order.exchange.isEmpty() ? order.symbol : order.exchange + ":" + order.symbol;

    QString side_str = order_side_str(order.side);
    QString type_str = order_type_str(order.order_type);

    std::optional<double> price_opt;
    if (order.order_type == OrderType::Market) {
        price_opt = 1000.0;
    } else if (order.price > 0) {
        price_opt = order.price;
    }

    std::optional<double> stop_opt;
    if (order.stop_price > 0)
        stop_opt = order.stop_price;

    try {
        auto paper_order = pt_place_order(session.paper_portfolio_id, symbol, side_str, type_str, order.quantity,
                                          price_opt, stop_opt, false);

        if (order.order_type == OrderType::Market) {
            double fill_price = order.price > 0 ? order.price : 1000.0;
            pt_fill_order(paper_order.id, fill_price);
        }

        return {true, paper_order.id, "Paper order placed", "paper"};
    } catch (const std::exception& e) {
        return {false, "", QString("Paper order failed: %1").arg(e.what()), "paper"};
    }
}

UnifiedOrderResponse UnifiedTrading::place_live_order(const TradingSession& session, const UnifiedOrder& order) {
    auto* broker = BrokerRegistry::instance().get(session.broker);
    if (!broker) {
        return {false, "", "Broker not found: " + session.broker, "live"};
    }

    auto creds = broker->load_credentials();
    if (creds.access_token.isEmpty()) {
        return {false, "", "No credentials for " + session.broker + ". Please authenticate.", "live"};
    }

    auto result = broker->place_order(creds, order);
    return {result.success, result.order_id, result.error, "live"};
}

UnifiedOrderResponse UnifiedTrading::cancel_order(const QString& order_id) {
    QMutexLocker lock(&mutex_);

    if (!session_) {
        return {false, "", "No active trading session", ""};
    }

    if (session_->mode == "paper") {
        try {
            pt_cancel_order(order_id);
            return {true, order_id, "Paper order cancelled", "paper"};
        } catch (const std::exception& e) {
            return {false, "", QString("Cancel failed: %1").arg(e.what()), "paper"};
        }
    }

    auto* broker = BrokerRegistry::instance().get(session_->broker);
    if (!broker)
        return {false, "", "Broker not found", "live"};

    auto creds = broker->load_credentials();
    auto result = broker->cancel_order(creds, order_id);
    return {result.success, order_id, result.error, "live"};
}

// ============================================================================
// Account-Aware Order Routing (new multi-account API)
// ============================================================================

UnifiedOrderResponse UnifiedTrading::place_order(const QString& account_id, const UnifiedOrder& order) {
    auto account = AccountManager::instance().get_account(account_id);
    if (account.account_id.isEmpty())
        return {false, "", "Account not found: " + account_id, ""};
    if (!account.is_active)
        return {false, "", "Account is disabled: " + account.display_name, account.trading_mode};
    if (account.trading_mode == "live" && trading_kill_switch_enabled())
        return {false, "", "Trading kill switch is enabled", "live"};
    const QString validation = basic_order_error(order);
    if (!validation.isEmpty())
        return {false, "", validation, account.trading_mode};

    if (account.trading_mode == "paper")
        return place_paper_order_for_account(account_id, order);
    return place_live_order_for_account(account_id, order);
}

UnifiedOrderResponse UnifiedTrading::cancel_order(const QString& account_id, const QString& order_id) {
    auto account = AccountManager::instance().get_account(account_id);
    if (account.account_id.isEmpty())
        return {false, "", "Account not found: " + account_id, ""};

    if (account.trading_mode == "paper") {
        try {
            pt_cancel_order(order_id);
            return {true, order_id, "Paper order cancelled", "paper"};
        } catch (const std::exception& e) {
            return {false, "", QString("Cancel failed: %1").arg(e.what()), "paper"};
        }
    }

    auto* broker = BrokerRegistry::instance().get(account.broker_id);
    if (!broker)
        return {false, "", "Broker not found: " + account.broker_id, "live"};

    auto creds = AccountManager::instance().load_credentials(account_id);
    auto result = broker->cancel_order(creds, order_id);
    return {result.success, order_id, result.error, "live"};
}

UnifiedOrderResponse UnifiedTrading::modify_order(const QString& account_id, const QString& order_id,
                                                   const QJsonObject& modifications) {
    auto account = AccountManager::instance().get_account(account_id);
    if (account.account_id.isEmpty())
        return {false, "", "Account not found: " + account_id, ""};

    if (account.trading_mode == "paper")
        return {false, "", "Modify not supported for paper orders", "paper"};

    auto* broker = BrokerRegistry::instance().get(account.broker_id);
    if (!broker)
        return {false, "", "Broker not found: " + account.broker_id, "live"};

    auto creds = AccountManager::instance().load_credentials(account_id);
    auto result = broker->modify_order(creds, order_id, modifications);
    return {result.success, order_id, result.error, "live"};
}

UnifiedOrderResponse UnifiedTrading::place_paper_order_for_account(const QString& account_id,
                                                                    const UnifiedOrder& order) {
    auto account = AccountManager::instance().get_account(account_id);
    if (account.paper_portfolio_id.isEmpty())
        return {false, "", "No paper portfolio for this account", "paper"};

    QString symbol = order.exchange.isEmpty() ? order.symbol : order.exchange + ":" + order.symbol;
    QString side_str = order_side_str(order.side);
    QString type_str = order_type_str(order.order_type);

    std::optional<double> price_opt;
    if (order.order_type == OrderType::Market)
        price_opt = 1000.0;
    else if (order.price > 0)
        price_opt = order.price;

    std::optional<double> stop_opt;
    if (order.stop_price > 0)
        stop_opt = order.stop_price;

    try {
        auto paper_order = pt_place_order(account.paper_portfolio_id, symbol, side_str, type_str, order.quantity,
                                          price_opt, stop_opt, false);
        if (order.order_type == OrderType::Market) {
            double fill_price = order.price > 0 ? order.price : 1000.0;
            pt_fill_order(paper_order.id, fill_price);
        }
        return {true, paper_order.id, "Paper order placed", "paper"};
    } catch (const std::exception& e) {
        return {false, "", QString("Paper order failed: %1").arg(e.what()), "paper"};
    }
}

UnifiedOrderResponse UnifiedTrading::place_live_order_for_account(const QString& account_id,
                                                                   const UnifiedOrder& order) {
    auto account = AccountManager::instance().get_account(account_id);
    auto* broker = BrokerRegistry::instance().get(account.broker_id);
    if (!broker)
        return {false, "", "Broker not found: " + account.broker_id, "live"};

    auto creds = AccountManager::instance().load_credentials(account_id);
    if (creds.access_token.isEmpty())
        return {false, "", "No credentials for account " + account.display_name + ". Please authenticate.", "live"};

    auto result = broker->place_order(creds, order);
    return {result.success, result.order_id, result.error, "live"};
}

// ============================================================================
// Broadcast Order (multi-account simultaneous placement)
// ============================================================================

QVector<UnifiedTrading::BroadcastResult> UnifiedTrading::broadcast_order(const QStringList& account_ids,
                                                                         const UnifiedOrder& order) {
    QVector<BroadcastResult> results;
    results.reserve(account_ids.size());

    for (const auto& acct_id : account_ids) {
        auto account = AccountManager::instance().get_account(acct_id);
        if (account.account_id.isEmpty()) {
            results.append({acct_id, "Unknown", {false, "", "Account not found", ""}});
            continue;
        }
        auto response = place_order(acct_id, order);
        results.append({acct_id, account.display_name, response});
    }

    return results;
}

// ============================================================================
// Order Bridge
// ============================================================================

void UnifiedTrading::start_order_bridge() {
    if (thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(this, [this]() { start_order_bridge(); }, Qt::QueuedConnection);
        return;
    }
    if (!bridge_timer_) {
        bridge_timer_ = new QTimer(this);
        bridge_timer_->setInterval(750);
        connect(bridge_timer_, &QTimer::timeout, this, &UnifiedTrading::poll_order_signals);
    }
    bridge_running_.store(true);
    if (!bridge_timer_->isActive())
        bridge_timer_->start();
    LOG_INFO("UnifiedTrading", "Order signal bridge started");
}

void UnifiedTrading::stop_order_bridge() {
    if (thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(this, [this]() { stop_order_bridge(); }, Qt::QueuedConnection);
        return;
    }
    bridge_running_.store(false);
    if (bridge_timer_)
        bridge_timer_->stop();
    LOG_INFO("UnifiedTrading", "Order signal bridge stopping");
}

bool UnifiedTrading::is_bridge_running() const {
    return bridge_running_.load();
}

void UnifiedTrading::poll_order_signals() {
    if (!bridge_running_.load() || !fincept::Database::instance().is_open())
        return;

    reconcile_submitted_order_signals();

    QSqlQuery q(fincept::Database::instance().raw_db());
    if (!q.exec("SELECT s.id, s.deployment_id, COALESCE(s.account_id, d.account_id) AS account_id, "
                "s.symbol, s.side, s.quantity, s.order_type, COALESCE(s.price, 0) AS price "
                "FROM algo_order_signals s "
                "LEFT JOIN algo_deployments d ON d.id = s.deployment_id "
                "WHERE s.status = 'pending' "
                "ORDER BY s.created_at ASC "
                "LIMIT 20")) {
        const QString err = q.lastError().text();
        if (!err.contains("no such table", Qt::CaseInsensitive))
            LOG_ERROR("UnifiedTrading", "Order bridge poll failed: " + err);
        return;
    }

    struct PendingSignal {
        QString id;
        QString deployment_id;
        QString account_id;
        QString symbol;
        QString side;
        double quantity = 0.0;
        QString order_type;
        double price = 0.0;
    };

    QVector<PendingSignal> pending_signals;
    while (q.next()) {
        PendingSignal s;
        s.id = q.value(0).toString();
        s.deployment_id = q.value(1).toString();
        s.account_id = q.value(2).toString();
        s.symbol = q.value(3).toString();
        s.side = q.value(4).toString();
        s.quantity = q.value(5).toDouble();
        s.order_type = q.value(6).toString();
        s.price = q.value(7).toDouble();
        pending_signals.append(std::move(s));
    }

    for (const auto& signal : pending_signals) {
        auto claim = fincept::Database::instance().execute(
            "UPDATE algo_order_signals SET status = 'processing', updated_at = CURRENT_TIMESTAMP "
            "WHERE id = ? AND status = 'pending'",
            {signal.id});
        if (claim.is_err()) {
            LOG_ERROR("UnifiedTrading", QString("Failed to claim order signal %1: %2")
                                            .arg(signal.id, QString::fromStdString(claim.error())));
            continue;
        }
        if (claim.value().numRowsAffected() != 1)
            continue;

        auto fail_signal = [&](const QString& message) {
            exec_bridge_update("UPDATE algo_order_signals "
                               "SET status = 'failed', error = ?, processed_at = CURRENT_TIMESTAMP, "
                               "updated_at = CURRENT_TIMESTAMP WHERE id = ?",
                               {message, signal.id});
            LOG_ERROR("UnifiedTrading", QString("Order signal %1 failed: %2").arg(signal.id, message));
        };

        const QString account_id = signal.account_id.trimmed();
        if (account_id.isEmpty()) {
            fail_signal("No account_id attached to live order signal");
            continue;
        }

        UnifiedOrder order;
        split_symbol(signal.symbol, &order);
        order.side = signal.side.compare("SELL", Qt::CaseInsensitive) == 0 ? OrderSide::Sell : OrderSide::Buy;
        order.order_type = parse_order_type(signal.order_type);
        order.quantity = signal.quantity;
        order.price = signal.price;
        if (order.order_type == OrderType::StopLoss || order.order_type == OrderType::StopLossLimit)
            order.stop_price = signal.price;

        const auto account = AccountManager::instance().get_account(account_id);
        if (account.account_id.isEmpty()) {
            fail_signal("Account not found: " + account_id);
            continue;
        }
        if (account.trading_mode == "live") {
            if (trading_kill_switch_enabled()) {
                fail_signal("Trading kill switch is enabled");
                continue;
            }
            if (!std::isfinite(order.price) || order.price <= 0.0) {
                fail_signal("Live algo order signal requires a positive reference price");
                continue;
            }
            const double order_value = order.quantity * order.price;
            const double max_value = max_live_order_value();
            if (std::isfinite(order_value) && order_value > max_value) {
                fail_signal(QString("Live algo order value %1 exceeds max %2")
                                .arg(order_value, 0, 'f', 2)
                                .arg(max_value, 0, 'f', 2));
                continue;
            }
        }

        auto response = place_order(account_id, order);
        if (response.success) {
            exec_bridge_update("UPDATE algo_order_signals "
                               "SET status = 'submitted', order_id = ?, error = NULL, "
                               "submitted_at = CURRENT_TIMESTAMP, processed_at = CURRENT_TIMESTAMP, "
                               "updated_at = CURRENT_TIMESTAMP WHERE id = ?",
                               {response.order_id, signal.id});
            LOG_INFO("UnifiedTrading", QString("Order signal %1 submitted as %2")
                                           .arg(signal.id, response.order_id));
        } else {
            fail_signal(response.message);
        }
    }
}

void UnifiedTrading::reconcile_submitted_order_signals() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - last_reconcile_ms_ < 5000)
        return;
    last_reconcile_ms_ = now;

    QSqlQuery accounts_q(fincept::Database::instance().raw_db());
    if (!accounts_q.exec("SELECT DISTINCT COALESCE(s.account_id, d.account_id) AS account_id "
                         "FROM algo_order_signals s "
                         "LEFT JOIN algo_deployments d ON d.id = s.deployment_id "
                         "WHERE s.status = 'submitted' AND s.order_id IS NOT NULL AND s.order_id != ''")) {
        const QString err = accounts_q.lastError().text();
        if (!err.contains("no such table", Qt::CaseInsensitive))
            LOG_ERROR("UnifiedTrading", "Order reconciliation account query failed: " + err);
        return;
    }

    QStringList account_ids;
    while (accounts_q.next()) {
        const QString account_id = accounts_q.value(0).toString().trimmed();
        if (!account_id.isEmpty())
            account_ids.append(account_id);
    }

    for (const QString& account_id : account_ids) {
        const auto account = AccountManager::instance().get_account(account_id);
        if (account.account_id.isEmpty() || account.trading_mode != "live")
            continue;

        auto* broker = BrokerRegistry::instance().get(account.broker_id);
        if (!broker)
            continue;

        const auto creds = AccountManager::instance().load_credentials(account_id);
        if (creds.access_token.isEmpty())
            continue;

        const auto order_book = broker->get_orders(creds);
        if (!order_book.success || !order_book.data) {
            LOG_WARN("UnifiedTrading", QString("Order reconciliation failed for %1: %2")
                                           .arg(account.display_name, order_book.error));
            continue;
        }

        QHash<QString, BrokerOrderInfo> by_id;
        for (const auto& info : *order_book.data) {
            if (!info.order_id.isEmpty())
                by_id.insert(info.order_id, info);
            if (!info.exchange_order_id.isEmpty())
                by_id.insert(info.exchange_order_id, info);
        }

        QSqlQuery signals_q(fincept::Database::instance().raw_db());
        signals_q.prepare("SELECT s.id, s.order_id FROM algo_order_signals s "
                          "LEFT JOIN algo_deployments d ON d.id = s.deployment_id "
                          "WHERE s.status = 'submitted' AND COALESCE(s.account_id, d.account_id) = ? "
                          "AND s.order_id IS NOT NULL AND s.order_id != ''");
        signals_q.bindValue(0, account_id);
        if (!signals_q.exec()) {
            LOG_ERROR("UnifiedTrading", "Order reconciliation signal query failed: " + signals_q.lastError().text());
            continue;
        }

        while (signals_q.next()) {
            const QString signal_id = signals_q.value(0).toString();
            const QString order_id = signals_q.value(1).toString();
            if (!by_id.contains(order_id)) {
                exec_bridge_update("UPDATE algo_order_signals "
                                   "SET last_reconciled_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP "
                                   "WHERE id = ?",
                                   {signal_id});
                continue;
            }

            const auto info = by_id.value(order_id);
            QString next_status = "submitted";
            QString error;
            if (is_filled_status(info.status)) {
                next_status = "filled";
            } else if (is_failed_status(info.status)) {
                next_status = "failed";
                error = info.message.isEmpty() ? info.status : info.message;
            }

            exec_bridge_update("UPDATE algo_order_signals "
                               "SET status = ?, broker_status = ?, filled_qty = ?, avg_price = ?, error = ?, "
                               "last_reconciled_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP "
                               "WHERE id = ?",
                               {next_status, info.status, info.filled_qty, info.avg_price,
                                error.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(error),
                                signal_id});
        }
    }
}

} // namespace fincept::trading
