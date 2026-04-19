"""
Article extractor for local news analysis.

Uses trafilatura when available, then falls back to a light HTML parser.
"""
import json
import re
import sys
from html import unescape
from html.parser import HTMLParser
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import Request, urlopen


USER_AGENT = "FinceptTerminal/4.0"
SUMMARY_LIMIT = 300


class TextHTMLParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self._chunks = []
        self._skip_depth = 0

    def handle_starttag(self, tag, attrs):
        if tag in {"script", "style", "noscript"}:
            self._skip_depth += 1

    def handle_endtag(self, tag):
        if tag in {"script", "style", "noscript"} and self._skip_depth > 0:
            self._skip_depth -= 1

    def handle_data(self, data):
        if self._skip_depth == 0 and data and not data.isspace():
            self._chunks.append(data)

    def text(self):
        return normalize_text(" ".join(self._chunks))


def normalize_text(text):
    if not text:
        return ""
    text = unescape(text)
    text = re.sub(r"\s+", " ", text)
    return text.strip()


def fetch_html(url):
    req = Request(
        url,
        headers={
            "User-Agent": USER_AGENT,
            "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        },
    )
    with urlopen(req, timeout=15) as resp:
        charset = resp.headers.get_content_charset() or "utf-8"
        return resp.read().decode(charset, "replace")


def first_match(text, pattern):
    match = re.search(pattern, text, flags=re.IGNORECASE | re.DOTALL)
    if not match:
        return ""
    return normalize_text(match.group(1))


def extract_with_trafilatura(url):
    try:
        import trafilatura  # type: ignore
    except Exception:
        return None

    try:
        downloaded = trafilatura.fetch_url(url)
        if not downloaded:
            return None

        result = {}
        if hasattr(trafilatura, "bare_extraction"):
            extracted = trafilatura.bare_extraction(
                downloaded,
                favor_precision=True,
                include_comments=False,
                include_tables=False,
                with_metadata=True,
            )
            if isinstance(extracted, dict):
                result = extracted

        if not result:
            text = trafilatura.extract(
                downloaded,
                favor_precision=True,
                include_comments=False,
                include_tables=False,
            )
            if not text:
                return None
            result = {"text": text}

        title = normalize_text(result.get("title", ""))
        body = normalize_text(result.get("text", ""))
        summary = normalize_text(result.get("description", "")) or body[:SUMMARY_LIMIT]
        source = normalize_text(result.get("sitename", ""))
        return {
            "success": bool(title or summary or body),
            "title": title or summary[:120],
            "summary": summary,
            "body": body,
            "source": source,
            "method": "trafilatura",
        }
    except Exception:
        return None


def extract_with_html(url):
    html = fetch_html(url)

    title = first_match(html, r"<meta[^>]+property=[\"']og:title[\"'][^>]+content=[\"']([^\"']+)[\"']")
    if not title:
        title = first_match(html, r"<title[^>]*>(.*?)</title>")

    summary = first_match(html, r"<meta[^>]+property=[\"']og:description[\"'][^>]+content=[\"']([^\"']+)[\"']")
    if not summary:
        summary = first_match(html, r"<meta[^>]+name=[\"']description[\"'][^>]+content=[\"']([^\"']+)[\"']")

    source = first_match(html, r"<meta[^>]+property=[\"']og:site_name[\"'][^>]+content=[\"']([^\"']+)[\"']")
    if not source:
        host = urlparse(url).hostname or ""
        source = host.upper()

    parser = TextHTMLParser()
    parser.feed(html)
    body = parser.text()
    if not summary:
        summary = body[:SUMMARY_LIMIT]
    if not title:
        title = summary[:120]

    return {
        "success": bool(title or summary or body),
        "title": title,
        "summary": summary,
        "body": body,
        "source": source,
        "method": "html_fallback",
    }


def extract_article(url):
    trafilatura_result = extract_with_trafilatura(url)
    if trafilatura_result and trafilatura_result.get("success"):
        return trafilatura_result

    fallback = extract_with_html(url)
    fallback["method"] = fallback.get("method") if trafilatura_result is None else "html_fallback_after_trafilatura"
    return fallback


def main():
    if len(sys.argv) < 2:
        print(json.dumps({"success": False, "error": "Usage: news_article_extract.py <url>"}))
        return

    url = sys.argv[1].strip()
    try:
        result = extract_article(url)
        if not result.get("success"):
            result["error"] = "Could not extract readable article text"
        print(json.dumps(result))
    except (HTTPError, URLError) as exc:
        print(json.dumps({"success": False, "error": str(exc)}))
    except Exception as exc:
        print(json.dumps({"success": False, "error": str(exc)}))


if __name__ == "__main__":
    main()
