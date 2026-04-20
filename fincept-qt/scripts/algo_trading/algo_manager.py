"""
Algo Manager — deployment management subcommands.

Called by the C++ AlgoTradingService for deployment control operations.
algo_live_runner.py only handles the long-running strategy loop; this script
handles all read/write management queries against the DB.

Usage:
    python algo_manager.py list_deployments --db <path>
    python algo_manager.py stop <deploy_id> --db <path>
    python algo_manager.py stop_all --db <path>
    python algo_manager.py kill_switch on|off|status --db <path>
"""

import json
import sys
import os
import argparse
import sqlite3
import signal
import time


def get_db_path(args_db: str) -> str:
    """Resolve DB path: use provided --db, or fall back to AppData location."""
    if args_db and os.path.exists(args_db):
        return args_db
    # Windows fallback
    appdata = os.environ.get('APPDATA', '')
    if appdata:
        candidate = os.path.join(appdata, 'Fincept', 'FinceptTerminal', 'fincept.db')
        if os.path.exists(candidate):
            return candidate
    # Last resort: same dir as script
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'fincept.db')


def open_db(db_path: str):
    conn = sqlite3.connect(db_path, timeout=10)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=5000")
    return conn


def is_process_running(pid: int) -> bool:
    if not pid or pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return False


def terminate_process(pid: int, timeout: float = 3.0) -> bool:
    """Terminate a deployment runner by PID. Returns True once it is gone."""
    if not is_process_running(pid):
        return True
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        return True
    except Exception:
        return False

    deadline = time.time() + timeout
    while time.time() < deadline:
        if not is_process_running(pid):
            return True
        time.sleep(0.1)

    if hasattr(signal, "SIGKILL"):
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            return True
        except Exception:
            return False
    return not is_process_running(pid)


def cmd_list_deployments(db_path: str):
    """Return all deployments with their live metrics."""
    try:
        conn = open_db(db_path)
        rows = conn.execute("""
            SELECT
                d.id, d.strategy_id, d.symbol, d.mode, d.status,
                d.account_id, d.timeframe, d.quantity, d.error_message,
                d.created_at, d.updated_at,
                s.name AS strategy_name,
                COALESCE(a.display_name, '')             AS account_name,
                COALESCE(m.total_pnl, 0)                AS total_pnl,
                COALESCE(m.unrealized_pnl, 0)           AS unrealized_pnl,
                COALESCE(m.total_trades, 0)              AS total_trades,
                COALESCE(m.win_rate, 0)                  AS win_rate,
                COALESCE(m.max_drawdown, 0)              AS max_drawdown,
                COALESCE(m.current_position_qty, 0)      AS current_position_qty,
                COALESCE(m.current_position_side, '')    AS current_position_side,
                COALESCE(m.current_position_entry, 0)    AS current_position_entry
            FROM algo_deployments d
            LEFT JOIN algo_strategies s ON s.id = d.strategy_id
            LEFT JOIN broker_accounts a ON a.id = d.account_id
            LEFT JOIN algo_metrics m ON m.deployment_id = d.id
            ORDER BY d.created_at DESC
        """).fetchall()
        conn.close()

        deployments = [dict(r) for r in rows]
        print(json.dumps({'success': True, 'deployments': deployments}))

    except sqlite3.OperationalError as e:
        # Table may not exist yet (first run before any deployment)
        if 'no such table' in str(e).lower():
            print(json.dumps({'success': True, 'deployments': []}))
        else:
            print(json.dumps({'success': False, 'error': str(e), 'deployments': []}))
    except Exception as e:
        print(json.dumps({'success': False, 'error': str(e), 'deployments': []}))


def cmd_stop(deploy_id: str, db_path: str):
    """Request stop and terminate the deployment runner process if known."""
    try:
        conn = open_db(db_path)
        row = conn.execute(
            "SELECT pid FROM algo_deployments WHERE id = ?",
            (deploy_id,)
        ).fetchone()
        pid = int(row["pid"]) if row and row["pid"] else 0
        conn.execute(
            "UPDATE algo_deployments SET status = 'stopping', updated_at = CURRENT_TIMESTAMP WHERE id = ?",
            (deploy_id,)
        )
        conn.commit()
        stopped = terminate_process(pid) if pid else True
        conn.execute(
            "UPDATE algo_deployments SET status = 'stopped', stopped_at = CURRENT_TIMESTAMP, "
            "updated_at = CURRENT_TIMESTAMP WHERE id = ?",
            (deploy_id,)
        )
        conn.commit()
        conn.close()
        print(json.dumps({'success': True, 'deployment_id': deploy_id, 'pid': pid, 'process_stopped': stopped}))
    except Exception as e:
        print(json.dumps({'success': False, 'error': str(e)}))


def cmd_stop_all(db_path: str):
    """Stop all running/starting deployments and terminate known runner PIDs."""
    try:
        conn = open_db(db_path)
        rows = conn.execute(
            "SELECT id, pid FROM algo_deployments WHERE status IN ('running', 'starting', 'pending', 'stopping')"
        ).fetchall()
        conn.execute(
            "UPDATE algo_deployments SET status = 'stopping', updated_at = CURRENT_TIMESTAMP "
            "WHERE status IN ('running', 'starting', 'pending', 'stopping')"
        )
        conn.commit()
        stopped = []
        for row in rows:
            pid = int(row["pid"]) if row["pid"] else 0
            stopped.append({
                "deployment_id": row["id"],
                "pid": pid,
                "process_stopped": terminate_process(pid) if pid else True,
            })
        conn.execute(
            "UPDATE algo_deployments SET status = 'stopped', stopped_at = CURRENT_TIMESTAMP, "
            "updated_at = CURRENT_TIMESTAMP WHERE status = 'stopping'"
        )
        conn.commit()
        conn.close()
        print(json.dumps({'success': True, 'stopped': stopped}))
    except Exception as e:
        print(json.dumps({'success': False, 'error': str(e)}))


def cmd_kill_switch(state: str, db_path: str):
    """Enable, disable, or read the global trading kill switch."""
    try:
        conn = open_db(db_path)
        conn.execute("""
            CREATE TABLE IF NOT EXISTS settings (
                key TEXT PRIMARY KEY,
                value TEXT,
                category TEXT DEFAULT 'general',
                updated_at TEXT DEFAULT CURRENT_TIMESTAMP
            )
        """)
        state = (state or "status").lower()
        if state in {"on", "enable", "enabled", "true", "1"}:
            value = "true"
            conn.execute(
                "INSERT OR REPLACE INTO settings (key, value, category, updated_at) "
                "VALUES ('trading.kill_switch', ?, 'trading', CURRENT_TIMESTAMP)",
                (value,)
            )
            conn.commit()
        elif state in {"off", "disable", "disabled", "false", "0"}:
            value = "false"
            conn.execute(
                "INSERT OR REPLACE INTO settings (key, value, category, updated_at) "
                "VALUES ('trading.kill_switch', ?, 'trading', CURRENT_TIMESTAMP)",
                (value,)
            )
            conn.commit()
        else:
            row = conn.execute(
                "SELECT value FROM settings WHERE key = 'trading.kill_switch'"
            ).fetchone()
            value = row["value"] if row else "false"
        conn.close()
        enabled = str(value).lower() in {"1", "true", "yes", "on"}
        print(json.dumps({'success': True, 'kill_switch': enabled}))
    except Exception as e:
        print(json.dumps({'success': False, 'error': str(e)}))


def main():
    parser = argparse.ArgumentParser(description='Algo Deployment Manager')
    parser.add_argument('command', choices=['list_deployments', 'stop', 'stop_all', 'kill_switch'])
    parser.add_argument('deploy_id', nargs='?', default=None,
                        help='Deployment ID (required for stop)')
    parser.add_argument('--db', default=None, help='SQLite database path')

    args = parser.parse_args()
    db_path = get_db_path(args.db)

    if args.command == 'list_deployments':
        cmd_list_deployments(db_path)
    elif args.command == 'stop':
        if not args.deploy_id:
            print(json.dumps({'success': False, 'error': 'deploy_id required for stop'}))
            sys.exit(1)
        cmd_stop(args.deploy_id, db_path)
    elif args.command == 'stop_all':
        cmd_stop_all(db_path)
    elif args.command == 'kill_switch':
        cmd_kill_switch(args.deploy_id or 'status', db_path)


if __name__ == '__main__':
    main()
