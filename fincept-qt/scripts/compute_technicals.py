"""
Compute technical indicators and lightweight analytics jobs.

The Qt app uses this file in two ways:
  - direct OHLCV JSON -> all indicator columns
  - workflow analytics nodes -> BACKTEST/CORRELATION/FACTOR_MODEL/PAIRS

It also accepts --symbol or positional "<symbol> [period]" for smoke tests and
local workflows that do not already have price data.
"""

import sys
import json
import pandas as pd
import numpy as np
import contextlib
import io
import warnings
import re
from datetime import datetime, timedelta, timezone

# Add script directory to path to allow importing technicals package
import os
script_dir = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, script_dir)

from technicals.momentum_indicators import calculate_all_momentum_indicators
from technicals.volume_indicators import calculate_all_volume_indicators
from technicals.volatility_indicators import calculate_all_volatility_indicators
from technicals.trend_indicators import calculate_all_trend_indicators
from technicals.others_indicators import calculate_all_others_indicators

SPECIAL_INDICATORS = {"BACKTEST", "CORRELATION", "FACTOR_MODEL", "PAIRS"}
YFINANCE_NATIVE_PERIODS = {"1d", "5d", "1mo", "3mo", "6mo", "1y", "2y", "5y", "10y", "ytd", "max"}


def _json_error(message):
    return json.dumps({"success": False, "error": str(message)})


def _safe_float(value, default=0.0):
    try:
        if value is None:
            return default
        return float(value)
    except Exception:
        return default


def _normalise_columns(df):
    df = df.copy()
    df.columns = [str(col).strip().lower().replace(" ", "_") for col in df.columns]
    return df


def _normalise_ohlcv_frame(df):
    df = _normalise_columns(df)

    if "adj_close" not in df.columns and "adjclose" in df.columns:
        df["adj_close"] = df["adjclose"]

    for date_col in ("date", "datetime"):
        if date_col in df.columns and "timestamp" not in df.columns:
            dates = pd.to_datetime(df[date_col], utc=True, errors="coerce")
            epoch = pd.Timestamp("1970-01-01", tz="UTC")
            df["timestamp"] = (dates - epoch).dt.total_seconds().where(dates.notna(), None)

    required_columns = ["open", "high", "low", "close"]
    missing = [col for col in required_columns if col not in df.columns]
    if missing:
        raise ValueError(f"Missing required column: {', '.join(missing)}")

    for col in ["open", "high", "low", "close", "volume", "adj_close"]:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    df = df.dropna(subset=required_columns)
    if df.empty:
        raise ValueError("No valid OHLCV rows available")
    return df


def _load_json(value):
    if value is None or value == "":
        return None
    if isinstance(value, (dict, list)):
        return value
    return json.loads(value)


def _payload_has_ohlcv(payload):
    try:
        data = _load_json(payload)
        if isinstance(data, dict) and "data" in data:
            data = data["data"]
        df = pd.DataFrame(data)
        cols = {str(c).strip().lower().replace(" ", "_") for c in df.columns}
        return {"open", "high", "low", "close"}.issubset(cols)
    except Exception:
        return False


def _normalise_yfinance_request(period, start_date=None, end_date=None):
    if start_date or end_date:
        return None, start_date, end_date

    p = str(period or "1y").strip().lower()
    if p in YFINANCE_NATIVE_PERIODS:
        return p, None, None

    # Numeric workflow periods are indicator lookbacks, not Yahoo ranges.
    if re.fullmatch(r"\d+", p):
        return "1y", None, None

    match = re.fullmatch(r"(\d+)(d|mo|y)", p)
    if not match:
        return "1y", None, None

    amount = int(match.group(1))
    unit = match.group(2)
    end = datetime.now(timezone.utc).date() + timedelta(days=1)
    if unit == "d":
        start = end - timedelta(days=amount + 5)
    elif unit == "mo":
        start = end - timedelta(days=amount * 31 + 5)
    else:
        start = end - timedelta(days=amount * 366 + 5)
    return None, start.isoformat(), end.isoformat()


def _fetch_yfinance_frame(symbol, period="1y", start_date=None, end_date=None):
    try:
        import yfinance as yf
    except Exception as exc:
        raise RuntimeError("yfinance is required for symbol-based technical analysis") from exc

    buf = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        ticker = yf.Ticker(symbol)
        yf_period, yf_start, yf_end = _normalise_yfinance_request(period, start_date, end_date)
        if yf_start or yf_end:
            df = ticker.history(start=yf_start, end=yf_end, interval="1d")
        else:
            df = ticker.history(period=yf_period or "1y", interval="1d")

    if df is None or df.empty:
        raise ValueError(f"No data found for symbol: {symbol}")

    if isinstance(df.index, pd.DatetimeIndex):
        df = df.reset_index()

    df = _normalise_ohlcv_frame(df)
    df["symbol"] = symbol
    return df


def _price_series_from_records(value):
    if value is None:
        return pd.Series(dtype=float)

    if isinstance(value, dict) and "data" in value:
        value = value["data"]

    if isinstance(value, dict):
        for key in ("prices", "close", "closes", "values", "returns"):
            if key in value:
                return _price_series_from_records(value[key])
        if all(isinstance(v, (int, float)) for v in value.values()):
            return pd.Series(list(value.values()), dtype=float).dropna()

    if isinstance(value, list):
        if not value:
            return pd.Series(dtype=float)
        if all(isinstance(v, (int, float)) for v in value):
            return pd.Series(value, dtype=float).dropna()
        if all(isinstance(v, dict) for v in value):
            df = _normalise_columns(pd.DataFrame(value))
            for col in ("close", "price", "value", "return"):
                if col in df.columns:
                    return pd.to_numeric(df[col], errors="coerce").dropna().reset_index(drop=True)

    return pd.Series(dtype=float)


def _returns_from_prices(prices):
    series = pd.Series(prices, dtype=float).replace([np.inf, -np.inf], np.nan).dropna()
    if series.empty:
        return pd.Series(dtype=float)
    if series.abs().max() <= 1.0 and series.abs().mean() < 0.20:
        return series.reset_index(drop=True)
    return series.pct_change().replace([np.inf, -np.inf], np.nan).dropna().reset_index(drop=True)


def _drawdown(prices):
    series = pd.Series(prices, dtype=float).dropna()
    if series.empty:
        return 0.0
    running_max = series.cummax()
    dd = (series / running_max) - 1.0
    return float(dd.min())


def _sharpe(returns, risk_free_rate=0.0):
    series = pd.Series(returns, dtype=float).dropna()
    if len(series) < 2:
        return 0.0
    excess = series - risk_free_rate / 252.0
    std = excess.std(ddof=0)
    return float(excess.mean() / std * np.sqrt(252.0)) if std > 0 else 0.0


def _series_map_from_payload(payload, default_symbols=None, period="1y"):
    default_symbols = default_symbols or ["SPY", "QQQ", "IWM"]
    data = payload.get("data") if isinstance(payload, dict) else None
    series_map = {}

    if isinstance(data, dict):
        for key, value in data.items():
            series = _price_series_from_records(value)
            if len(series) >= 2:
                series_map[str(key)] = series.reset_index(drop=True)

    if isinstance(data, list) and data and all(isinstance(item, dict) for item in data):
        df = _normalise_columns(pd.DataFrame(data))
        symbol_col = next((col for col in ("symbol", "ticker", "asset") if col in df.columns), None)
        price_col = next((col for col in ("close", "price", "value") if col in df.columns), None)
        if symbol_col and price_col:
            for symbol, group in df.groupby(symbol_col):
                series = pd.to_numeric(group[price_col], errors="coerce").dropna()
                if len(series) >= 2:
                    series_map[str(symbol)] = series.reset_index(drop=True)

    if not series_map:
        symbols = payload.get("symbols") if isinstance(payload, dict) else None
        if not isinstance(symbols, list) or not symbols:
            symbols = default_symbols
        for symbol in symbols:
            try:
                df = _fetch_yfinance_frame(str(symbol), period=period)
                series_map[str(symbol)] = df["close"].reset_index(drop=True)
            except Exception:
                continue

    return series_map


def _handle_backtest(payload):
    args = _load_json(payload) or {}
    if not isinstance(args, dict):
        args = {"data": args}

    strategy = args.get("strategy")
    symbol = args.get("symbol")
    if not symbol and isinstance(strategy, dict):
        symbol = strategy.get("symbol") or strategy.get("ticker") or strategy.get("benchmark")
    symbol = symbol or "SPY"
    start_date = args.get("start_date")
    end_date = args.get("end_date")
    initial_capital = _safe_float(args.get("initial_capital"), 100000.0)
    commission = _safe_float(args.get("commission"), 0.001)
    prices = _price_series_from_records(args.get("prices") or args.get("data") or args.get("strategy"))

    if len(prices) < 2:
        df = _fetch_yfinance_frame(str(symbol), start_date=start_date, end_date=end_date)
        prices = df["close"].reset_index(drop=True)

    if len(prices) < 2:
        raise ValueError("Need at least 2 prices for backtest")

    shares = (initial_capital * (1.0 - commission)) / float(prices.iloc[0])
    equity = prices.astype(float) * shares
    equity.iloc[-1] = equity.iloc[-1] * (1.0 - commission)
    returns = equity.pct_change().dropna()

    return {
        "success": True,
        "type": "backtest",
        "symbol": str(symbol),
        "initial_capital": initial_capital,
        "final_equity": float(equity.iloc[-1]),
        "total_return": float(equity.iloc[-1] / initial_capital - 1.0),
        "annualized_return": float((equity.iloc[-1] / initial_capital) ** (252.0 / max(len(returns), 1)) - 1.0),
        "max_drawdown": _drawdown(equity),
        "sharpe_ratio": _sharpe(returns),
        "trades": 2,
        "data_points": int(len(prices)),
        "equity_curve": [float(x) for x in equity.tail(250)],
    }


def _handle_correlation(payload):
    args = _load_json(payload) or {}
    if not isinstance(args, dict):
        args = {"data": args}

    method = str(args.get("method", "pearson")).lower()
    if method not in {"pearson", "spearman", "kendall"}:
        method = "pearson"
    period = str(args.get("period", "1y"))

    series_map = _series_map_from_payload(args, period=period)
    if len(series_map) < 2:
        raise ValueError("Need at least two assets for correlation analysis")

    returns = {}
    min_len = min(len(series) for series in series_map.values())
    for symbol, series in series_map.items():
        ret = _returns_from_prices(series.tail(min_len))
        if len(ret) >= 2:
            returns[symbol] = ret.reset_index(drop=True)

    frame = pd.DataFrame(returns).dropna()
    if frame.shape[1] < 2 or frame.empty:
        raise ValueError("Not enough overlapping returns for correlation analysis")

    matrix = frame.corr(method=method).replace([np.inf, -np.inf], np.nan)
    return {
        "success": True,
        "type": "correlation",
        "method": method,
        "symbols": list(matrix.columns),
        "matrix": matrix.where(pd.notnull(matrix), None).to_dict(),
        "data_points": int(len(frame)),
    }


def _fit_ols(y, factors):
    y = pd.Series(y, dtype=float).reset_index(drop=True)
    frame = pd.DataFrame({name: pd.Series(vals, dtype=float).reset_index(drop=True) for name, vals in factors.items()})
    min_len = min([len(y)] + [len(frame[col]) for col in frame.columns])
    y = y.tail(min_len).reset_index(drop=True)
    frame = frame.tail(min_len).reset_index(drop=True)
    valid = pd.concat([y.rename("asset"), frame], axis=1).replace([np.inf, -np.inf], np.nan).dropna()
    if len(valid) < len(factors) + 3:
        raise ValueError("Not enough overlapping returns for factor model")

    yv = valid["asset"].to_numpy()
    x = valid.drop(columns=["asset"]).to_numpy()
    x = np.column_stack([np.ones(len(x)), x])
    coeffs, _, _, _ = np.linalg.lstsq(x, yv, rcond=None)
    fitted = x @ coeffs
    ss_res = float(np.sum((yv - fitted) ** 2))
    ss_tot = float(np.sum((yv - np.mean(yv)) ** 2))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0
    names = ["alpha_daily"] + list(factors.keys())
    return {name: float(value) for name, value in zip(names, coeffs)}, float(r2), int(len(valid))


def _handle_factor_model(payload):
    args = _load_json(payload) or {}
    if not isinstance(args, dict):
        args = {"data": args}

    model = str(args.get("model", "ff3")).lower()
    period = str(args.get("period", "3y"))
    symbol = str(args.get("symbol", "AAPL"))

    prices = _price_series_from_records(args.get("data") or args.get("prices"))
    if len(prices) < 30:
        prices = _fetch_yfinance_frame(symbol, period=period)["close"]
    asset_returns = _returns_from_prices(prices)

    market = _returns_from_prices(_fetch_yfinance_frame("SPY", period=period)["close"])
    factors = {"market": market}

    if model in {"ff3", "ff5", "carhart4"}:
        small = _returns_from_prices(_fetch_yfinance_frame("IWM", period=period)["close"])
        value = _returns_from_prices(_fetch_yfinance_frame("IVE", period=period)["close"])
        growth = _returns_from_prices(_fetch_yfinance_frame("IVW", period=period)["close"])
        min_len = min(len(market), len(small), len(value), len(growth))
        factors["smb_proxy"] = small.tail(min_len).reset_index(drop=True) - market.tail(min_len).reset_index(drop=True)
        factors["hml_proxy"] = value.tail(min_len).reset_index(drop=True) - growth.tail(min_len).reset_index(drop=True)

    if model in {"ff5", "carhart4"}:
        momentum = _returns_from_prices(_fetch_yfinance_frame("MTUM", period=period)["close"])
        min_len = min(len(momentum), len(market))
        factors["momentum_proxy"] = momentum.tail(min_len).reset_index(drop=True) - market.tail(min_len).reset_index(drop=True)

    coeffs, r2, n = _fit_ols(asset_returns, factors)
    alpha_daily = coeffs.pop("alpha_daily", 0.0)

    return {
        "success": True,
        "type": "factor_model",
        "model": model,
        "symbol": symbol,
        "alpha_daily": alpha_daily,
        "alpha_annualized": float((1.0 + alpha_daily) ** 252.0 - 1.0),
        "betas": coeffs,
        "r_squared": r2,
        "data_points": n,
        "factor_source": "local ETF proxy factors via yfinance",
    }


def _handle_pairs(payload):
    args = _load_json(payload) or {}
    if not isinstance(args, dict):
        args = {}

    symbol_a = str(args.get("symbol_a", "KO"))
    symbol_b = str(args.get("symbol_b", "PEP"))
    lookback = int(_safe_float(args.get("lookback"), 60))
    z_threshold = _safe_float(args.get("z_threshold"), 2.0)
    period = "1y" if lookback <= 252 else "2y"

    a = _fetch_yfinance_frame(symbol_a, period=period)["close"].tail(lookback).reset_index(drop=True)
    b = _fetch_yfinance_frame(symbol_b, period=period)["close"].tail(lookback).reset_index(drop=True)
    min_len = min(len(a), len(b))
    if min_len < 20:
        raise ValueError("Need at least 20 overlapping prices for pairs trading")
    a = a.tail(min_len).reset_index(drop=True)
    b = b.tail(min_len).reset_index(drop=True)

    beta = float(np.cov(a, b)[0, 1] / np.var(b)) if np.var(b) > 0 else 1.0
    spread = a - beta * b
    spread_std = float(spread.std(ddof=0))
    z_score = float((spread.iloc[-1] - spread.mean()) / spread_std) if spread_std > 0 else 0.0
    correlation = float(_returns_from_prices(a).corr(_returns_from_prices(b)))

    if z_score > z_threshold:
        signal = f"short_{symbol_a}_long_{symbol_b}"
    elif z_score < -z_threshold:
        signal = f"long_{symbol_a}_short_{symbol_b}"
    else:
        signal = "neutral"

    return {
        "success": True,
        "type": "pairs_trading",
        "symbol_a": symbol_a,
        "symbol_b": symbol_b,
        "lookback": lookback,
        "hedge_ratio": beta,
        "z_score": z_score,
        "z_threshold": z_threshold,
        "correlation": correlation,
        "signal": signal,
        "spread_mean": float(spread.mean()),
        "spread_std": spread_std,
        "latest_spread": float(spread.iloc[-1]),
        "data_points": int(min_len),
    }


def compute_special_analytics(indicator, payload):
    try:
        indicator = (indicator or "").upper()
        if indicator == "BACKTEST":
            result = _handle_backtest(payload)
        elif indicator == "CORRELATION":
            result = _handle_correlation(payload)
        elif indicator == "FACTOR_MODEL":
            result = _handle_factor_model(payload)
        elif indicator == "PAIRS":
            result = _handle_pairs(payload)
        else:
            raise ValueError(f"Unsupported analytics indicator: {indicator}")
        return json.dumps(result, allow_nan=False)
    except Exception as e:
        return _json_error(e)


def compute_all_technicals(historical_data_json):
    """
    Compute all technical indicators from historical data

    Args:
        historical_data_json: JSON string with array of OHLCV data

    Returns:
        JSON string with all computed technical indicators
    """
    try:
        data = _load_json(historical_data_json)
        if isinstance(data, dict) and "data" in data:
            data = data["data"]
        df = _normalise_ohlcv_frame(pd.DataFrame(data))

        result_df = df.copy()
        warning_messages = []

        def apply_block(name, func, frame):
            try:
                with warnings.catch_warnings(record=True) as caught:
                    warnings.simplefilter("always")
                    result = func(frame)
                for item in caught:
                    warning_messages.append(f"{name}: {item.message}")
                return result
            except Exception as exc:
                warning_messages.append(f"{name}: {exc}")
                return frame

        # Trend indicators
        result_df = apply_block("trend", calculate_all_trend_indicators, result_df)

        # Momentum indicators
        result_df = apply_block("momentum", calculate_all_momentum_indicators, result_df)

        # Volatility indicators
        result_df = apply_block("volatility", calculate_all_volatility_indicators, result_df)

        # Volume indicators (only if volume data exists)
        if 'volume' in result_df.columns:
            result_df = apply_block("volume", calculate_all_volume_indicators, result_df)

        # Other indicators
        result_df = apply_block("others", calculate_all_others_indicators, result_df)

        # Replace NaN/inf with None for strict JSON encoding
        result_df = result_df.replace([np.inf, -np.inf], np.nan).where(pd.notnull(result_df), None)

        # Convert to JSON
        result_json = result_df.to_json(orient='records')

        response = {
            "success": True,
            "data": json.loads(result_json),
            "indicator_columns": {
                "trend": [
                    "sma_20", "ema_12", "wma_9", "macd", "macd_signal", "macd_diff",
                    "trix", "mass_index", "ichimoku_conversion", "ichimoku_base",
                    "ichimoku_a", "ichimoku_b", "kst", "kst_signal", "dpo", "cci",
                    "adx", "adx_pos", "adx_neg", "vortex_pos", "vortex_neg",
                    "psar", "psar_up", "psar_down", "psar_up_indicator", "psar_down_indicator",
                    "stc", "aroon_up", "aroon_down", "aroon_indicator"
                ],
                "momentum": [
                    "rsi", "stoch_k", "stoch_d", "stoch_rsi", "stoch_rsi_k", "stoch_rsi_d",
                    "williams_r", "ao", "kama", "roc", "tsi", "uo",
                    "ppo", "ppo_signal", "ppo_hist", "pvo", "pvo_signal", "pvo_hist"
                ],
                "volatility": [
                    "atr", "bb_mavg", "bb_hband", "bb_lband", "bb_pband", "bb_wband",
                    "bb_hband_indicator", "bb_lband_indicator",
                    "kc_mavg", "kc_hband", "kc_lband", "kc_pband", "kc_wband",
                    "kc_hband_indicator", "kc_lband_indicator",
                    "dc_hband", "dc_lband", "dc_mband", "dc_pband", "dc_wband", "ui"
                ],
                "volume": [
                    "adi", "obv", "cmf", "fi", "eom", "eom_signal",
                    "vpt", "nvi", "vwap", "mfi"
                ],
                "others": [
                    "daily_return", "daily_log_return", "cumulative_return"
                ]
            }
        }
        if warning_messages:
            response["warnings"] = warning_messages
        return json.dumps(response)

    except Exception as e:
        return _json_error(e)


def compute_all_technicals_from_symbol(symbol, period="1y"):
    try:
        df = _fetch_yfinance_frame(symbol, period=period or "1y")
        records = df.to_json(orient="records", date_format="iso")
        return compute_all_technicals(records)
    except Exception as e:
        return _json_error(e)


def parse_args(args):
    """
    Parse command-line arguments supporting both:
      - Named flags: --data <json> --indicator <name> --period <n> [--symbol <sym>]
      - Legacy positional: <json_data>
    """
    data = None
    indicator = None
    period = None
    symbol = None
    positionals = []

    i = 0
    while i < len(args):
        if args[i].startswith("--data="):
            data = args[i].split("=", 1)[1]
            i += 1
        elif args[i] == "--data" and i + 1 < len(args):
            data = args[i + 1]
            i += 2
        elif args[i].startswith("--indicator="):
            indicator = args[i].split("=", 1)[1]
            i += 1
        elif args[i] == "--indicator" and i + 1 < len(args):
            indicator = args[i + 1]
            i += 2
        elif args[i].startswith("--period="):
            period = args[i].split("=", 1)[1]
            i += 1
        elif args[i] == "--period" and i + 1 < len(args):
            period = args[i + 1]
            i += 2
        elif args[i].startswith("--symbol="):
            symbol = args[i].split("=", 1)[1]
            i += 1
        elif args[i] == "--symbol" and i + 1 < len(args):
            symbol = args[i + 1]
            i += 2
        elif not args[i].startswith("--"):
            positionals.append(args[i])
            i += 1
        else:
            i += 1

    if positionals and data is None and symbol is None:
        first = positionals[0].strip()
        if first.startswith("[") or first.startswith("{"):
            data = positionals[0]
        else:
            symbol = positionals[0]
        if len(positionals) > 1 and period is None:
            period = positionals[1]

    return data, indicator, period, symbol


def main(args=None):
    """
    Main entry point for worker pool and subprocess execution

    Args:
        args: List of arguments (for worker pool) or None (for subprocess/CLI)

    Returns:
        JSON string with technical indicators or error
    """
    # Support both worker pool (args parameter) and subprocess/CLI (sys.argv)
    if args is None:
        args = sys.argv[1:]

    if len(args) < 1:
        return json.dumps({
            "success": False,
            "error": "Usage: python compute_technicals.py --data '<json>' [--indicator <name>] [--period <n>] or compute_technicals.py <symbol> [period]"
        })

    historical_data_json, indicator, period, symbol = parse_args(args)
    indicator_name = (indicator or "").upper()

    if indicator_name in SPECIAL_INDICATORS:
        return compute_special_analytics(indicator_name, historical_data_json)

    if symbol and (not historical_data_json or not _payload_has_ohlcv(historical_data_json)):
        return compute_all_technicals_from_symbol(symbol, period or "1y")

    if not historical_data_json:
        return _json_error("No data provided. Use --data '<json>', --symbol <ticker>, or pass '<ticker> [period]'.")

    result = compute_all_technicals(historical_data_json)
    return result


if __name__ == "__main__":
    result = main()
    print(result)
