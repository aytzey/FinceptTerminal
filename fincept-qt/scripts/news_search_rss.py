"""
Targeted live news search feeds.

Currently uses Google News RSS search as a free, no-signup query backend.
"""
import hashlib
import json
import re
import sys
import urllib.parse
from datetime import timezone
from email.utils import parsedate_to_datetime
from html import unescape
from urllib.request import Request, urlopen
import xml.etree.ElementTree as ET


USER_AGENT = "FinceptTerminal/4.0"
GOOGLE_NEWS_SEARCH = "https://news.google.com/rss/search"


CATEGORY_QUERIES = {
    "armed_conflict": '("missile strike" OR airstrike OR invasion OR offensive OR artillery OR "drone strike" OR incursion OR military)',
    "terrorism": '(terror OR bombing OR insurgent OR militant OR "suicide attack")',
    "protests": '(protest OR demonstrations OR unrest OR strike OR rally)',
    "riots": '(riot OR clashes OR looting OR unrest)',
    "explosions": '(explosion OR blast OR sabotage OR detonation)',
    "strategic": '(sanctions OR embargo OR ceasefire OR summit OR diplomatic OR tariffs OR election OR referendum OR coup OR blockade)',
    "crisis": '(crisis OR standoff OR escalation OR tensions OR humanitarian)',
}


def normalize_text(text):
    if not text:
        return ""
    text = unescape(text)
    text = re.sub(r"<[^>]+>", " ", text)
    text = re.sub(r"\s+", " ", text)
    return text.strip()


def trim_source_suffix(title, source):
    if not title or not source:
        return title
    suffix = f" - {source}"
    if title.endswith(suffix):
        return title[: -len(suffix)].strip()
    return title


def parse_pub_date(value):
    try:
        dt = parsedate_to_datetime(value)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return int(dt.timestamp())
    except Exception:
        return 0


def build_query(country, city, category):
    terms = []
    category_key = (category or "").strip().lower()
    if category_key:
        terms.append(CATEGORY_QUERIES.get(category_key, f'"{category}"'))
    else:
        terms.append(
            '("missile strike" OR sanctions OR protest OR airstrike OR ceasefire OR coup OR election OR blockade OR riot)'
        )
    if country.strip():
        terms.append(f'"{country.strip()}"')
    if city.strip():
        terms.append(f'"{city.strip()}"')
    return " ".join(terms)


def fetch_google_news(query, limit):
    params = {
        "q": query,
        "hl": "en-US",
        "gl": "US",
        "ceid": "US:en",
    }
    url = GOOGLE_NEWS_SEARCH + "?" + urllib.parse.urlencode(params)
    req = Request(url, headers={"User-Agent": USER_AGENT})
    with urlopen(req, timeout=20) as resp:
        raw = resp.read()

    root = ET.fromstring(raw)
    articles = []
    seen = set()

    for item in root.findall("./channel/item"):
        title = normalize_text(item.findtext("title", default=""))
        link = normalize_text(item.findtext("link", default=""))
        source = normalize_text(item.findtext("source", default=""))
        description = normalize_text(item.findtext("description", default=""))
        title = trim_source_suffix(title, source)
        if not title or not link:
            continue

        fingerprint = link or title.lower()
        if fingerprint in seen:
            continue
        seen.add(fingerprint)

        sort_ts = parse_pub_date(item.findtext("pubDate", default=""))
        summary = description
        if summary.startswith(title):
            summary = summary[len(title) :].lstrip(" -:|")

        articles.append(
            {
                "id": hashlib.sha1(fingerprint.encode("utf-8")).hexdigest()[:16],
                "headline": title,
                "summary": summary[:300],
                "source": source or "GOOGLE NEWS",
                "region": "GLOBAL",
                "category": "GEOPOLITICS",
                "link": link,
                "sort_ts": sort_ts,
                "tier": 2,
                "lang": "en",
            }
        )
        if len(articles) >= limit:
            break

    return {"success": True, "query": query, "articles": articles}


def geopolitics(country, city, category, limit):
    limit = max(1, min(int(limit), 50))
    query = build_query(country, city, category)
    return fetch_google_news(query, limit)


def main():
    args = sys.argv[1:]
    if len(args) < 1:
        print(json.dumps({"success": False, "error": "Usage: news_search_rss.py <command> ..."}))
        return

    command = args[0]
    try:
        if command == "google_search":
            if len(args) < 3:
                result = {"success": False, "error": "Usage: news_search_rss.py google_search <query> <limit>"}
            else:
                result = fetch_google_news(args[1], max(1, min(int(args[2]), 50)))
        elif command == "geopolitics":
            if len(args) < 5:
                result = {
                    "success": False,
                    "error": "Usage: news_search_rss.py geopolitics <country> <city> <category> <limit>",
                }
            else:
                result = geopolitics(args[1], args[2], args[3], args[4])
        else:
            result = {"success": False, "error": f"Unknown command: {command}"}
    except Exception as exc:
        result = {"success": False, "error": str(exc)}

    print(json.dumps(result))


if __name__ == "__main__":
    main()
