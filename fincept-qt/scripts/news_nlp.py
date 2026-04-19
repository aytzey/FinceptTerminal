"""
News NLP Pipeline.

Core extraction paths are model-based:
  - GLiNER for entities
  - sentence-transformers for semantic clustering
  - sentence-transformers for geopolitics event labeling
"""
import sys
import json
import re
from collections import Counter
from functools import lru_cache
from pathlib import Path


GLINER_MODEL_NAME = "urchade/gliner_medium-v2.1"
EMBED_MODEL_NAME = "sentence-transformers/all-MiniLM-L6-v2"
FINBERT_MODEL_NAME = "ProsusAI/finbert"
ENTITY_LABELS = ["person", "organization", "company", "country", "city", "region", "location"]
GEO_ENTITY_LABELS = ["country", "city", "region", "location"]
EVENT_LABEL_TEXTS = {
    "armed_conflict": "military conflict, invasion, missile strike, airstrike, troop movement, armed confrontation",
    "terrorism": "terrorist attack, bombing, insurgent violence, militant operation, extremist attack",
    "protests": "public protest, street demonstration, civil unrest, labor strike, political rally",
    "riots": "riot, violent clashes, looting, mob violence, chaotic street unrest",
    "explosions": "major explosion, blast, sabotage detonation, industrial blast, unexplained explosion",
    "strategic": "sanctions, diplomacy, ceasefire, summit, election, referendum, blockade, embargo, tariff dispute",
    "crisis": "humanitarian crisis, geopolitical standoff, escalating tensions, state emergency, political crisis",
}
GEOCODE_CACHE_PATH = Path.home() / ".cache" / "fincept" / "news_geocode_cache.json"
GEOCODE_USER_AGENT = "FinceptTerminal/4.0"

# ── Country/Region normalization ─────────────────────────────────────────────

COUNTRIES = {
    "united states": "US", "usa": "US", "u.s.": "US", "america": "US",
    "china": "CN", "chinese": "CN", "beijing": "CN", "shanghai": "CN",
    "russia": "RU", "russian": "RU", "moscow": "RU", "kremlin": "RU",
    "ukraine": "UA", "ukrainian": "UA", "kyiv": "UA", "kiev": "UA",
    "united kingdom": "GB", "britain": "GB", "british": "GB", "london": "GB", "uk": "GB",
    "germany": "DE", "german": "DE", "berlin": "DE",
    "france": "FR", "french": "FR", "paris": "FR",
    "japan": "JP", "japanese": "JP", "tokyo": "JP",
    "india": "IN", "indian": "IN", "mumbai": "IN", "delhi": "IN",
    "brazil": "BR", "brazilian": "BR",
    "canada": "CA", "canadian": "CA",
    "australia": "AU", "australian": "AU",
    "south korea": "KR", "korean": "KR", "seoul": "KR",
    "north korea": "KP", "pyongyang": "KP",
    "iran": "IR", "iranian": "IR", "tehran": "IR",
    "israel": "IL", "israeli": "IL", "tel aviv": "IL",
    "palestine": "PS", "palestinian": "PS", "gaza": "PS", "west bank": "PS",
    "saudi arabia": "SA", "saudi": "SA", "riyadh": "SA",
    "turkey": "TR", "turkish": "TR", "ankara": "TR", "istanbul": "TR",
    "egypt": "EG", "egyptian": "EG", "cairo": "EG",
    "italy": "IT", "italian": "IT", "rome": "IT",
    "spain": "ES", "spanish": "ES", "madrid": "ES",
    "mexico": "MX", "mexican": "MX",
    "indonesia": "ID", "indonesian": "ID", "jakarta": "ID",
    "netherlands": "NL", "dutch": "NL",
    "switzerland": "CH", "swiss": "CH",
    "sweden": "SE", "swedish": "SE",
    "norway": "NO", "norwegian": "NO",
    "poland": "PL", "polish": "PL", "warsaw": "PL",
    "taiwan": "TW", "taiwanese": "TW", "taipei": "TW",
    "singapore": "SG",
    "hong kong": "HK",
    "south africa": "ZA",
    "nigeria": "NG", "nigerian": "NG", "lagos": "NG",
    "argentina": "AR",
    "colombia": "CO",
    "pakistan": "PK", "pakistani": "PK",
    "thailand": "TH", "thai": "TH", "bangkok": "TH",
    "vietnam": "VN", "vietnamese": "VN",
    "philippines": "PH", "filipino": "PH",
    "malaysia": "MY", "malaysian": "MY",
    "iraq": "IQ", "iraqi": "IQ", "baghdad": "IQ",
    "syria": "SY", "syrian": "SY", "damascus": "SY",
    "yemen": "YE", "yemeni": "YE",
    "libya": "LY", "libyan": "LY",
    "lebanon": "LB", "lebanese": "LB", "beirut": "LB",
    "afghanistan": "AF", "afghan": "AF", "kabul": "AF",
    "myanmar": "MM", "burmese": "MM",
    "ethiopia": "ET", "ethiopian": "ET",
    "sudan": "SD", "sudanese": "SD", "khartoum": "SD",
    "somalia": "SO", "somali": "SO",
    "venezuela": "VE", "venezuelan": "VE",
    "cuba": "CU", "cuban": "CU", "havana": "CU",
    "european union": "EU", "eu": "EU", "brussels": "EU",
}

@lru_cache(maxsize=1)
def get_gliner_model():
    from gliner import GLiNER

    return GLiNER.from_pretrained(GLINER_MODEL_NAME)


@lru_cache(maxsize=1)
def get_embed_model():
    from sentence_transformers import SentenceTransformer

    return SentenceTransformer(EMBED_MODEL_NAME, device="cpu")


@lru_cache(maxsize=1)
def get_finbert_model():
    from transformers import AutoModelForSequenceClassification, AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(FINBERT_MODEL_NAME)
    model = AutoModelForSequenceClassification.from_pretrained(FINBERT_MODEL_NAME)
    model.eval()
    return tokenizer, model


@lru_cache(maxsize=1)
def get_event_label_matrix():
    model = get_embed_model()
    labels = list(EVENT_LABEL_TEXTS.keys())
    descriptions = [EVENT_LABEL_TEXTS[label] for label in labels]
    embeddings = model.encode(descriptions, normalize_embeddings=True)
    return labels, embeddings


@lru_cache(maxsize=1)
def get_geocode_cache():
    try:
        if GEOCODE_CACHE_PATH.exists():
            return json.loads(GEOCODE_CACHE_PATH.read_text(encoding="utf-8"))
    except Exception:
        pass
    return {}


@lru_cache(maxsize=1)
def get_geocoder():
    from geopy.extra.rate_limiter import RateLimiter
    from geopy.geocoders import Nominatim

    geolocator = Nominatim(user_agent=GEOCODE_USER_AGENT, timeout=10)
    return RateLimiter(geolocator.geocode, min_delay_seconds=1.0)


def persist_geocode_cache():
    try:
        GEOCODE_CACHE_PATH.parent.mkdir(parents=True, exist_ok=True)
        GEOCODE_CACHE_PATH.write_text(json.dumps(get_geocode_cache()), encoding="utf-8")
    except Exception:
        pass


def country_to_code(name):
    try:
        import pycountry

        return pycountry.countries.lookup(name).alpha_2
    except Exception:
        return COUNTRIES.get(name.lower())


def normalize_place_key(text):
    return re.sub(r"\s+", " ", (text or "").strip().lower())


def geocode_place(name, kind="", country_hint=""):
    query = (name or "").strip()
    if not query:
        return None

    cache_key = "|".join(
        [
            kind or "",
            normalize_place_key(query),
            normalize_place_key(country_hint),
        ]
    )
    cache = get_geocode_cache()
    if cache_key in cache:
        return cache[cache_key]

    geocode = get_geocoder()
    query_text = query if not country_hint or kind == "country" else f"{query}, {country_hint}"
    country_codes = None
    if country_hint:
        code = country_to_code(country_hint)
        if code:
            country_codes = code.lower()

    location = None
    try:
        location = geocode(query_text, exactly_one=True, addressdetails=True, country_codes=country_codes)
    except TypeError:
        location = geocode(query_text, exactly_one=True, addressdetails=True)
    except Exception:
        location = None

    if location is None and query_text != query:
        try:
            location = geocode(query, exactly_one=True, addressdetails=True)
        except Exception:
            location = None

    if location is None:
        cache[cache_key] = None
        persist_geocode_cache()
        return None

    raw = getattr(location, "raw", {}) or {}
    address = raw.get("address", {})
    result = {
        "name": query,
        "lat": float(location.latitude),
        "lon": float(location.longitude),
        "country": address.get("country", ""),
        "city": address.get("city")
        or address.get("town")
        or address.get("village")
        or address.get("municipality")
        or address.get("state")
        or "",
    }
    cache[cache_key] = result
    persist_geocode_cache()
    return result


def predict_entities(text, labels, threshold=0.4):
    model = get_gliner_model()
    entities = model.predict_entities(text, labels, threshold=threshold)
    entities.sort(key=lambda item: item.get("score", 0.0), reverse=True)
    return entities


def semantic_event_category(text):
    model = get_embed_model()
    labels, label_embeddings = get_event_label_matrix()
    query_embedding = model.encode([text], normalize_embeddings=True)[0]
    scores = (label_embeddings @ query_embedding).tolist()
    best_idx = max(range(len(scores)), key=lambda idx: scores[idx])
    return labels[best_idx], float(scores[best_idx])


def best_geographic_signal(text):
    entities = predict_entities(text, GEO_ENTITY_LABELS, threshold=0.35)
    by_label = {"city": [], "country": [], "region": [], "location": []}
    for entity in entities:
        label = entity.get("label", "").lower()
        if label in by_label:
            by_label[label].append(entity.get("text", "").strip())

    for label in ("city", "country", "region", "location"):
        if by_label[label]:
            return by_label[label][0], label, entities
    return "", "", entities


def extract_entities(headlines_json):
    """Extract countries, organizations, people, and tickers with GLiNER."""
    try:
        articles = json.loads(headlines_json) if isinstance(headlines_json, str) else headlines_json
    except json.JSONDecodeError:
        return {"success": False, "error": "Invalid JSON"}

    all_countries = Counter()
    all_orgs = Counter()
    all_people = Counter()
    all_tickers = Counter()
    per_article = []

    for article in articles:
        headline = article.get("headline", "")
        summary = article.get("summary", "")
        text = (headline + " " + summary).strip()

        countries = []
        orgs = []
        people = []
        tickers = []

        entities = predict_entities(text, ENTITY_LABELS, threshold=0.4)
        for entity in entities:
            label = entity.get("label", "").lower()
            name = entity.get("text", "").strip()
            if not name:
                continue
            if label == "person":
                people.append(name)
                all_people[name] += 1
            elif label in {"organization", "company"}:
                orgs.append(name)
                all_orgs[name] += 1
            elif label == "country":
                code = country_to_code(name)
                countries.append({"name": name, "code": code or ""})
                if code:
                    all_countries[code] += 1

        # Ticker extraction (uppercase 2-5 chars, filter common words)
        common = {"THE", "FOR", "AND", "BUT", "NOT", "FROM", "WITH", "THIS", "THAT",
                  "HAVE", "WILL", "BEEN", "THEY", "WERE", "SAID", "HAS", "ITS", "NEW",
                  "ARE", "WAS", "WHO", "HOW", "WHY", "ALL", "CAN", "MAY", "NOW", "SEC",
                  "CEO", "CFO", "IPO", "ETF", "GDP", "CPI", "PMI"}
        country_codes = {c["code"] for c in countries if c.get("code")}
        for m in re.finditer(r'\b[A-Z]{2,5}\b', text):
            t = m.group()
            if t not in common and t not in country_codes:
                tickers.append(t)
                all_tickers[t] += 1

        # Deduplicate
        countries = list({c["code"]: c for c in countries}.values())[:5]
        orgs = list(dict.fromkeys(orgs))[:5]
        people = list(dict.fromkeys(people))[:5]
        tickers = list(dict.fromkeys(tickers))[:5]

        per_article.append({
            "id": article.get("id", ""),
            "countries": countries,
            "organizations": orgs,
            "people": people,
            "tickers": tickers,
        })

    return {
        "success": True,
        "entities": per_article,
        "top_countries": [{"code": c, "count": n} for c, n in all_countries.most_common(10)],
        "top_organizations": [{"name": o, "count": n} for o, n in all_orgs.most_common(10)],
        "top_people": [{"name": p, "count": n} for p, n in all_people.most_common(10)],
        "top_tickers": [{"symbol": t, "count": n} for t, n in all_tickers.most_common(10)],
    }


def cluster_semantic(headlines_json):
    """Cluster semantically with sentence-transformers community detection."""
    try:
        articles = json.loads(headlines_json) if isinstance(headlines_json, str) else headlines_json
    except json.JSONDecodeError:
        return {"success": False, "error": "Invalid JSON"}

    if len(articles) < 2:
        return {"success": True, "clusters": [], "method": "too_few_articles"}

    from sentence_transformers import util

    texts = [(a.get("headline", "") + ". " + a.get("summary", "")).strip() for a in articles]
    model = get_embed_model()
    embeddings = model.encode(texts, normalize_embeddings=True)
    communities = util.community_detection(embeddings, min_community_size=2, threshold=0.72)

    result_clusters = []
    for indices in communities:
        items = [{"id": articles[i].get("id", ""), "headline": articles[i].get("headline", "")} for i in indices]
        result_clusters.append({
            "primary": items[0],
            "items": items,
            "size": len(items),
        })

    result_clusters.sort(key=lambda c: c["size"], reverse=True)

    return {
        "success": True,
        "clusters": result_clusters[:20],
        "method": "sentence_transformers_community_detection",
        "total_articles": len(articles),
        "clustered_count": sum(c["size"] for c in result_clusters),
    }


def extract_geopolitics_events(headlines_json, country_filter="", city_filter="", category_filter="", limit=50):
    try:
        articles = json.loads(headlines_json) if isinstance(headlines_json, str) else headlines_json
    except json.JSONDecodeError:
        return {"success": False, "error": "Invalid JSON"}

    limit = max(1, min(int(limit), 250))
    country_filter = country_filter.strip().lower()
    city_filter = city_filter.strip().lower()
    category_filter = category_filter.strip().lower()

    events = []
    for article in articles:
        source_category = (article.get("category") or "").upper()
        if source_category not in {"GEOPOLITICS", "DEFENSE", "REGULATORY", "ECONOMIC"}:
            continue

        headline = (article.get("headline") or "").strip()
        summary = (article.get("summary") or "").strip()
        text = (headline + ". " + summary).strip()
        if not text:
            continue

        event_category, score = semantic_event_category(text)
        if score < 0.20:
            continue

        _, _, geo_entities = best_geographic_signal(text)
        geo_terms = []
        countries = []
        cities = []
        for entity in geo_entities:
            entity_text = entity.get("text", "").strip()
            label = entity.get("label", "").lower()
            if not entity_text:
                continue
            if entity_text not in geo_terms:
                geo_terms.append(entity_text)
            if label == "country" and entity_text not in countries:
                countries.append(entity_text)
            elif label in {"city", "region", "location"} and entity_text not in cities:
                cities.append(entity_text)

        if not countries and not cities:
            continue

        country = countries[0] if countries else ""
        city = cities[0] if cities else ""
        geocoded = None
        if city:
            geocoded = geocode_place(city, kind="city", country_hint=country)
        elif country:
            geocoded = geocode_place(country, kind="country")

        latitude = None
        longitude = None
        if geocoded:
            latitude = geocoded.get("lat")
            longitude = geocoded.get("lon")
            if not country:
                country = geocoded.get("country") or country
            if not city:
                city = geocoded.get("city") or city

        country = (country or "").strip()
        city = (city or "").strip()
        if country_filter and country_filter not in country.lower():
            continue
        if city_filter and city_filter not in city.lower():
            continue
        if category_filter and category_filter != event_category.lower():
            continue

        events.append(
            {
                "url": article.get("link", ""),
                "domain": article.get("source", ""),
                "event_category": event_category,
                "matched_keywords": headline[:180],
                "city": city,
                "country": country,
                "latitude": latitude,
                "longitude": longitude,
                "extracted_date": "",
                "created_at": article.get("sort_ts", 0),
                "context": {
                    "similarity": round(score, 4),
                    "geo_terms": geo_terms,
                },
            }
        )

    events.sort(key=lambda item: item.get("created_at", 0), reverse=True)
    trimmed = events[:limit]
    for event in trimmed:
        ts = int(event.get("created_at") or 0)
        if ts > 0:
            from datetime import datetime, timezone

            dt = datetime.fromtimestamp(ts, tz=timezone.utc)
            event["extracted_date"] = dt.date().isoformat()
            event["created_at"] = dt.isoformat()
        else:
            event["created_at"] = ""

    return {
        "success": True,
        "events": trimmed,
        "total": len(trimmed),
        "method": {
            "entities": "GLiNER",
            "categories": "sentence-transformers semantic labeling",
        },
    }


def analyze_sentiment_batch(headlines_json):
    """Batch financial sentiment analysis with FinBERT on CPU."""
    try:
        articles = json.loads(headlines_json) if isinstance(headlines_json, str) else headlines_json
    except json.JSONDecodeError:
        return {"success": False, "error": "Invalid JSON"}

    if not articles:
        return {
            "success": True,
            "results": [],
            "aggregate": {"bullish": 0, "bearish": 0, "neutral": 0},
            "overall_score": 0.0,
            "method": {"sentiment": FINBERT_MODEL_NAME, "device": "cpu"},
        }

    try:
        import torch

        tokenizer, model = get_finbert_model()
    except Exception as exc:
        return {
            "success": False,
            "error": f"FinBERT model unavailable: {type(exc).__name__}: {exc}",
            "method": {"sentiment": FINBERT_MODEL_NAME, "device": "cpu"},
        }

    label_map = {int(idx): label.lower() for idx, label in model.config.id2label.items()}
    texts = [
        (article.get("headline", "") + ". " + article.get("summary", "")).strip() or article.get("id", "")
        for article in articles
    ]

    results = []
    with torch.no_grad():
        for start in range(0, len(texts), 16):
            batch_texts = texts[start:start + 16]
            encoded = tokenizer(batch_texts, padding=True, truncation=True, max_length=256, return_tensors="pt")
            probs = torch.softmax(model(**encoded).logits, dim=-1)
            for offset, row in enumerate(probs):
                article = articles[start + offset]
                probability_by_label = {
                    label_map.get(idx, str(idx)): float(row[idx].item()) for idx in range(row.shape[0])
                }
                positive = probability_by_label.get("positive", 0.0)
                negative = probability_by_label.get("negative", 0.0)
                neutral = probability_by_label.get("neutral", 0.0)
                ranked_label = max(probability_by_label, key=probability_by_label.get)

                if ranked_label == "positive":
                    sentiment = "BULLISH"
                elif ranked_label == "negative":
                    sentiment = "BEARISH"
                else:
                    sentiment = "NEUTRAL"

                results.append({
                    "id": article.get("id", ""),
                    "sentiment": sentiment,
                    "score": round(positive - negative, 4),
                    "confidence": round(max(positive, negative, neutral), 4),
                    "probabilities": {
                        "positive": round(positive, 4),
                        "negative": round(negative, 4),
                        "neutral": round(neutral, 4),
                    },
                })

    # Aggregate
    bull = sum(1 for r in results if r["sentiment"] == "BULLISH")
    bear = sum(1 for r in results if r["sentiment"] == "BEARISH")
    neut = sum(1 for r in results if r["sentiment"] == "NEUTRAL")

    return {
        "success": True,
        "results": results,
        "aggregate": {"bullish": bull, "bearish": bear, "neutral": neut},
        "overall_score": round(sum(r["score"] for r in results) / max(len(results), 1), 3),
        "method": {"sentiment": FINBERT_MODEL_NAME, "device": "cpu"},
    }


def resolve_arg(arg):
    """If arg starts with '@', read content from that file path and delete it."""
    if arg and arg.startswith("@"):
        path = arg[1:]
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = f.read()
            try:
                import os
                os.remove(path)
            except Exception:
                pass
            return data
        except Exception as e:
            return arg  # fallback: return as-is
    return arg


def main(args=None):
    if args is None:
        args = sys.argv[1:]

    if len(args) < 2:
        print(json.dumps({"success": False, "error": "Usage: news_nlp.py <command> <json_data>"}))
        return

    command = args[0]
    data = resolve_arg(args[1])

    if command == "extract_entities":
        result = extract_entities(data)
    elif command == "cluster_semantic":
        result = cluster_semantic(data)
    elif command == "extract_geopolitics_events":
        country = args[2] if len(args) > 2 else ""
        city = args[3] if len(args) > 3 else ""
        category = args[4] if len(args) > 4 else ""
        limit = args[5] if len(args) > 5 else "50"
        result = extract_geopolitics_events(data, country, city, category, limit)
    elif command == "analyze_sentiment_batch":
        result = analyze_sentiment_batch(data)
    else:
        result = {"success": False, "error": f"Unknown command: {command}"}

    print(json.dumps(result))


if __name__ == "__main__":
    main()
