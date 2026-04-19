"""
Investing Calendar Data Fetcher
Economic calendar, earnings, IPO, dividend, splits, bond calendars via public endpoints.
"""
import sys
import json
import os
import requests
from datetime import datetime, timedelta
from typing import Dict, Any, Optional, List

API_KEY = os.environ.get('INVESTING_API_KEY', '')
BASE_URL = "https://api.investing.com/api/financialdata/calendar"

session = requests.Session()
adapter = requests.adapters.HTTPAdapter(pool_connections=10, pool_maxsize=10, max_retries=3)
session.mount('https://', adapter)
session.mount('http://', adapter)

CURRENCY_TO_COUNTRY = {
    "USD": "US", "EUR": "EU", "GBP": "GB", "JPY": "JP", "CAD": "CA", "AUD": "AU",
    "NZD": "NZ", "CHF": "CH", "CNY": "CN", "HKD": "HK", "SEK": "SE", "NOK": "NO",
}

FOREX_FACTORY_FEED = "https://nfs.faireconomy.media/ff_calendar_thisweek.json"

def _make_request(endpoint: str, params: Dict = None) -> Any:
    url = f"{BASE_URL}/{endpoint}" if not endpoint.startswith('http') else endpoint
    try:
        headers = {
            "User-Agent": "Mozilla/5.0 (compatible; FinceptTerminal/4.0)",
            "Accept": "application/json",
            "domain-id": "www",
        }
        response = session.get(url, params=params, headers=headers, timeout=30)
        response.raise_for_status()
        return response.json()
    except requests.exceptions.HTTPError as e:
        return {"error": f"HTTP {e.response.status_code}: {str(e)}"}
    except requests.exceptions.RequestException as e:
        return {"error": f"Request failed: {str(e)}"}
    except (json.JSONDecodeError, ValueError) as e:
        return {"error": f"JSON decode error: {str(e)}"}

def _ff_day_label(day) -> str:
    return day.strftime("%b").lower() + str(day.day) + "." + str(day.year)

def _forex_factory_json_range(from_date: str = None, to_date: str = None) -> Any:
    start = datetime.strptime(from_date, "%Y-%m-%d").date() if from_date else datetime.utcnow().date()
    end = datetime.strptime(to_date, "%Y-%m-%d").date() if to_date else start
    if end < start:
        end = start

    try:
        response = session.get(
            FOREX_FACTORY_FEED,
            headers={"User-Agent": "Mozilla/5.0", "Accept": "application/json"},
            timeout=30,
        )
        response.raise_for_status()
        raw_events = response.json()
    except requests.exceptions.RequestException as e:
        return {"error": f"ForexFactory JSON feed failed: {str(e)}"}
    except (json.JSONDecodeError, ValueError) as e:
        return {"error": f"ForexFactory JSON decode error: {str(e)}"}

    impact_map = {"Holiday": 0, "Non-Economic": 0, "Low": 1, "Medium": 2, "High": 3}
    events = []
    for item in raw_events:
        try:
            dt = datetime.fromisoformat(item.get("date", "").replace("Z", "+00:00"))
        except Exception:
            continue
        if dt.date() < start or dt.date() > end:
            continue
        country_code = item.get("country", "").strip().upper()
        events.append({
            "date": dt.date().isoformat(),
            "time": dt.strftime("%H:%M"),
            "country": CURRENCY_TO_COUNTRY.get(country_code, country_code),
            "country_code": country_code,
            "currency": country_code,
            "importance": impact_map.get(item.get("impact", ""), 0),
            "event": item.get("title", ""),
            "actual": item.get("actual", ""),
            "forecast": item.get("forecast", ""),
            "previous": item.get("previous", ""),
            "source": "forex_factory_json",
        })
    return {"events": events, "source": "forex_factory_json", "from_date": start.isoformat(), "to_date": end.isoformat()}

def _scrape_forex_factory_range(from_date: str = None, to_date: str = None) -> Any:
    try:
        from selenium import webdriver
        from selenium.webdriver.chrome.options import Options
        from selenium.webdriver.common.by import By
        from selenium.webdriver.support.ui import WebDriverWait
        from selenium.webdriver.support import expected_conditions as EC
    except Exception as e:
        return {"error": f"Selenium unavailable: {str(e)}"}

    start = datetime.strptime(from_date, "%Y-%m-%d").date() if from_date else datetime.utcnow().date()
    end = datetime.strptime(to_date, "%Y-%m-%d").date() if to_date else start
    if end < start:
        end = start

    max_days = min((end - start).days + 1, 7)
    options = Options()
    options.add_argument("--headless=new")
    options.add_argument("--no-sandbox")
    options.add_argument("--disable-dev-shm-usage")
    options.add_argument("--disable-gpu")
    options.add_argument("--user-agent=Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36")

    driver = webdriver.Chrome(options=options)
    all_events = []

    try:
        for offset in range(max_days):
            current = start + timedelta(days=offset)
            url = f"https://www.forexfactory.com/calendar?day={_ff_day_label(current)}"
            driver.get(url)

            wait = WebDriverWait(driver, 10)
            wait.until(EC.presence_of_element_located((By.CLASS_NAME, "calendar__table")))

            rows = driver.find_elements(By.CSS_SELECTOR, "tr.calendar__row")
            for row in rows:
                try:
                    event_elem = row.find_elements(By.CLASS_NAME, "calendar__event")
                    event_name = event_elem[0].text.strip() if event_elem else ""
                    if not event_name or event_name == "All Day":
                        continue

                    time_elem = row.find_elements(By.CLASS_NAME, "calendar__time")
                    currency_elem = row.find_elements(By.CLASS_NAME, "calendar__currency")
                    impact_elem = row.find_elements(By.CLASS_NAME, "calendar__impact")
                    actual_elem = row.find_elements(By.CLASS_NAME, "calendar__actual")
                    forecast_elem = row.find_elements(By.CLASS_NAME, "calendar__forecast")
                    previous_elem = row.find_elements(By.CLASS_NAME, "calendar__previous")

                    currency = currency_elem[0].text.strip() if currency_elem else ""
                    impact = 0
                    if impact_elem:
                        impact_spans = impact_elem[0].find_elements(By.TAG_NAME, "span")
                        impact = len([
                            s for s in impact_spans
                            if "icon--ff-impact-red" in s.get_attribute("class")
                            or "icon--ff-impact-ora" in s.get_attribute("class")
                            or "icon--ff-impact-yel" in s.get_attribute("class")
                        ])

                    all_events.append({
                        "date": current.isoformat(),
                        "time": time_elem[0].text.strip() if time_elem else "",
                        "country": CURRENCY_TO_COUNTRY.get(currency, currency),
                        "country_code": currency,
                        "currency": currency,
                        "importance": impact,
                        "event": event_name,
                        "actual": actual_elem[0].text.strip() if actual_elem else "",
                        "forecast": forecast_elem[0].text.strip() if forecast_elem else "",
                        "previous": previous_elem[0].text.strip() if previous_elem else "",
                        "source": "forex_factory",
                    })
                except Exception:
                    continue
    except Exception as e:
        return {"error": f"ForexFactory scrape failed: {str(e)}"}
    finally:
        driver.quit()

    return {
        "events": all_events,
        "source": "forex_factory",
        "from_date": start.isoformat(),
        "to_date": (start + timedelta(days=max_days - 1)).isoformat(),
    }

def _te_fallback(endpoint: str, params: Dict = None) -> Any:
    url = f"https://api.tradingeconomics.com/{endpoint}"
    try:
        headers = {"User-Agent": "Mozilla/5.0 (compatible; FinceptTerminal/4.0)", "Accept": "application/json"}
        response = session.get(url, params=params, headers=headers, timeout=30)
        response.raise_for_status()
        return response.json()
    except requests.exceptions.RequestException as e:
        return {"error": f"Request failed: {str(e)}"}
    except (json.JSONDecodeError, ValueError) as e:
        return {"error": f"JSON decode error: {str(e)}"}

def get_economic_calendar(countries: List[str] = None, importance: List[int] = None, from_date: str = None, to_date: str = None) -> Any:
    params = {}
    if from_date:
        params["dateFrom"] = from_date
    if to_date:
        params["dateTo"] = to_date
    if countries:
        params["country[]"] = countries
    if importance:
        params["importance[]"] = importance
    result = _make_request("economic", params)
    if "error" in result:
        result = _forex_factory_json_range(from_date, to_date)
    if "error" in result:
        result = _scrape_forex_factory_range(from_date, to_date)
    if "error" not in result:
        if countries and "events" in result:
            allowed = {c.strip().upper() for c in countries if c.strip()}
            result["events"] = [
                e for e in result["events"]
                if e.get("country", "").upper() in allowed or e.get("country_code", "").upper() in allowed
            ]
        if importance and "events" in result:
            allowed_importance = {int(x) for x in importance}
            result["events"] = [e for e in result["events"] if int(e.get("importance", 0)) in allowed_importance]
    return result

def get_earnings_calendar(from_date: str = None, to_date: str = None) -> Any:
    params = {}
    if from_date:
        params["dateFrom"] = from_date
    if to_date:
        params["dateTo"] = to_date
    return _make_request("earnings", params)

def get_ipo_calendar(from_date: str = None, to_date: str = None) -> Any:
    params = {}
    if from_date:
        params["dateFrom"] = from_date
    if to_date:
        params["dateTo"] = to_date
    return _make_request("ipo", params)

def get_dividend_calendar(from_date: str = None, to_date: str = None) -> Any:
    params = {}
    if from_date:
        params["dateFrom"] = from_date
    if to_date:
        params["dateTo"] = to_date
    return _make_request("dividends", params)

def get_splits_calendar(from_date: str = None, to_date: str = None) -> Any:
    params = {}
    if from_date:
        params["dateFrom"] = from_date
    if to_date:
        params["dateTo"] = to_date
    return _make_request("splits", params)

def get_bond_calendar(from_date: str = None, to_date: str = None) -> Any:
    params = {}
    if from_date:
        params["dateFrom"] = from_date
    if to_date:
        params["dateTo"] = to_date
    return _make_request("bonds", params)

def main(args=None):
    if args is None:
        args = sys.argv[1:]
    if not args:
        print(json.dumps({"error": "No command provided"}))
        return
    command = args[0]
    result = {"error": f"Unknown command: {command}"}
    if command == "economic":
        countries_arg = args[1].strip() if len(args) > 1 else ""
        countries = countries_arg.split(",") if countries_arg else None
        from_date = args[2] if len(args) > 2 else None
        to_date = args[3] if len(args) > 3 else None
        result = get_economic_calendar(countries=countries, from_date=from_date, to_date=to_date)
    elif command == "earnings":
        from_date = args[1] if len(args) > 1 else None
        to_date = args[2] if len(args) > 2 else None
        result = get_earnings_calendar(from_date, to_date)
    elif command == "ipo":
        from_date = args[1] if len(args) > 1 else None
        to_date = args[2] if len(args) > 2 else None
        result = get_ipo_calendar(from_date, to_date)
    elif command == "dividends":
        from_date = args[1] if len(args) > 1 else None
        to_date = args[2] if len(args) > 2 else None
        result = get_dividend_calendar(from_date, to_date)
    elif command == "splits":
        from_date = args[1] if len(args) > 1 else None
        to_date = args[2] if len(args) > 2 else None
        result = get_splits_calendar(from_date, to_date)
    elif command == "bonds":
        from_date = args[1] if len(args) > 1 else None
        to_date = args[2] if len(args) > 2 else None
        result = get_bond_calendar(from_date, to_date)
    print(json.dumps(result))

if __name__ == "__main__":
    main()
