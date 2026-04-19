#pragma once
// QuantLibClient.h — Shared local QuantLib engine for all QuantLib calls.
// Used by QuantLibScreen (async) and MCP QuantLibTools (sync-wrapped).

#include "mcp/McpTypes.h"

#include <QJsonObject>
#include <QObject>

#include <functional>

namespace fincept::services {

/// Callback type: delivers unwrapped data payload or error string.
/// On success: result.success == true, result.data has the payload.
/// On failure: result.success == false, result.error has the message.
using QuantLibCallback = std::function<void(mcp::ToolResult)>;

class QuantLibClient : public QObject {
    Q_OBJECT
  public:
    static QuantLibClient& instance();

    /// Local call; callback is delivered immediately with the local engine result.
    void call(const QString& endpoint, const QJsonObject& body, QuantLibCallback callback);

    /// Sync call for MCP tool handlers.
    mcp::ToolResult call_sync(const QString& endpoint, const QJsonObject& body);

    QuantLibClient(const QuantLibClient&) = delete;
    QuantLibClient& operator=(const QuantLibClient&) = delete;

  private:
    explicit QuantLibClient(QObject* parent = nullptr);
};

} // namespace fincept::services
