"""
News Geolocation.

Model-based location extraction via GLiNER plus geopy/Nominatim geocoding.
Also queries nearby infrastructure via OpenStreetMap Overpass API.

Commands:
  extract_and_geocode <json_articles_or_headlines>
  nearby_infrastructure <lat> <lon> <radius_km>
"""
import json
import sys

from news_nlp import GEO_ENTITY_LABELS, country_to_code, geocode_place, predict_entities


def article_payload(item, fallback_id):
    if isinstance(item, str):
        return str(fallback_id), item

    if isinstance(item, dict):
        article_id = item.get("id", str(fallback_id))
        text = (item.get("headline", "") + " " + item.get("summary", "")).strip()
        return article_id, text

    return str(fallback_id), str(item)


def extract_locations(text):
    entities = predict_entities(text, GEO_ENTITY_LABELS, threshold=0.35)
    countries = []
    places = []

    for entity in entities:
        name = entity.get("text", "").strip()
        label = entity.get("label", "").lower()
        if not name:
            continue
        if label == "country" and name not in countries:
            countries.append(name)
        elif label in {"city", "region", "location"} and name not in places:
            places.append((name, label))

    results = []
    for country in countries[:3]:
        geocoded = geocode_place(country, kind="country")
        if not geocoded:
            continue
        results.append(
            {
                "name": country,
                "code": country_to_code(country) or "",
                "lat": geocoded["lat"],
                "lon": geocoded["lon"],
                "type": "country",
            }
        )

    country_hint = countries[0] if countries else ""
    for place, label in places[:5]:
        geocoded = geocode_place(place, kind="city", country_hint=country_hint)
        if not geocoded:
            continue
        code = country_to_code(geocoded.get("country", "")) or country_to_code(country_hint) or ""
        results.append(
            {
                "name": place,
                "code": code,
                "lat": geocoded["lat"],
                "lon": geocoded["lon"],
                "type": "city" if label == "city" else label,
            }
        )

    deduped = []
    seen = set()
    for item in results:
        key = (item["name"].lower(), round(item["lat"], 4), round(item["lon"], 4), item["type"])
        if key in seen:
            continue
        seen.add(key)
        deduped.append(item)
    return deduped[:5]


def extract_and_geocode(articles_json):
    """Extract locations from article text and return coordinates."""
    try:
        articles = json.loads(articles_json) if isinstance(articles_json, str) else articles_json
    except json.JSONDecodeError:
        return {"success": False, "error": "Invalid JSON"}

    results = []
    unique_locations = {}

    for idx, article in enumerate(articles):
        article_id, text = article_payload(article, idx)
        if not text:
            continue

        locations = extract_locations(text)
        if not locations:
            continue

        for location in locations:
            key = (location["name"].lower(), location["type"])
            unique_locations[key] = location

        results.append(
            {
                "id": article_id,
                "locations": locations,
                "primary_lat": locations[0]["lat"],
                "primary_lon": locations[0]["lon"],
            }
        )

    return {
        "success": True,
        "geolocated_articles": results,
        "unique_locations": list(unique_locations.values()),
        "coverage": f"{len(results)}/{len(articles)} articles geolocated",
        "method": {"entities": "GLiNER", "geocoding": "geopy Nominatim"},
    }


def nearby_infrastructure(lat, lon, radius_km):
    """Query OpenStreetMap Overpass API for nearby infrastructure."""
    try:
        lat = float(lat)
        lon = float(lon)
        radius_km = int(radius_km)
    except (ValueError, TypeError):
        return {"success": False, "error": "Invalid coordinates or radius"}

    radius_m = radius_km * 1000

    query = f"""
    [out:json][timeout:10];
    (
      node["aeroway"="aerodrome"](around:{radius_m},{lat},{lon});
      node["military"](around:{radius_m},{lat},{lon});
      node["power"="plant"](around:{radius_m},{lat},{lon});
      node["harbour"](around:{radius_m},{lat},{lon});
      way["aeroway"="aerodrome"](around:{radius_m},{lat},{lon});
      way["military"](around:{radius_m},{lat},{lon});
    );
    out center 20;
    """

    try:
        import urllib.parse
        import urllib.request
        from math import atan2, cos, radians, sin, sqrt

        url = "https://overpass-api.de/api/interpreter"
        data = urllib.parse.urlencode({"data": query}).encode()
        req = urllib.request.Request(url, data=data)
        req.add_header("User-Agent", "FinceptTerminal/4.0")

        with urllib.request.urlopen(req, timeout=15) as resp:
            result = json.loads(resp.read().decode())

        infrastructure = []
        for elem in result.get("elements", []):
            tags = elem.get("tags", {})
            e_lat = elem.get("lat") or elem.get("center", {}).get("lat")
            e_lon = elem.get("lon") or elem.get("center", {}).get("lon")

            if not e_lat or not e_lon:
                continue

            name = tags.get("name", "Unknown")
            infra_type = "unknown"
            if "aeroway" in tags:
                infra_type = "airport"
            elif "military" in tags:
                infra_type = "military"
            elif "power" in tags:
                infra_type = "power_plant"
            elif "harbour" in tags:
                infra_type = "port"

            earth_radius_km = 6371
            dlat = radians(float(e_lat) - lat)
            dlon = radians(float(e_lon) - lon)
            a = sin(dlat / 2) ** 2 + cos(radians(lat)) * cos(radians(float(e_lat))) * sin(dlon / 2) ** 2
            distance_km = earth_radius_km * 2 * atan2(sqrt(a), sqrt(1 - a))

            infrastructure.append(
                {
                    "name": name,
                    "type": infra_type,
                    "lat": float(e_lat),
                    "lon": float(e_lon),
                    "distance_km": round(distance_km, 1),
                }
            )

        infrastructure.sort(key=lambda x: x["distance_km"])

        return {
            "success": True,
            "infrastructure": infrastructure[:15],
            "center": {"lat": lat, "lon": lon},
            "radius_km": radius_km,
            "count": len(infrastructure),
        }

    except Exception as e:
        return {"success": False, "error": f"Overpass API error: {str(e)}"}


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
        except Exception:
            return arg
    return arg


def main(args=None):
    if args is None:
        args = sys.argv[1:]

    if len(args) < 2:
        print(json.dumps({"success": False, "error": "Usage: news_geolocation.py <command> <args...>"}))
        return

    command = args[0]

    if command == "extract_and_geocode":
        result = extract_and_geocode(resolve_arg(args[1]))
    elif command == "nearby_infrastructure":
        if len(args) < 4:
            result = {"success": False, "error": "Usage: nearby_infrastructure <lat> <lon> <radius_km>"}
        else:
            result = nearby_infrastructure(args[1], args[2], args[3])
    else:
        result = {"success": False, "error": f"Unknown command: {command}"}

    print(json.dumps(result))


if __name__ == "__main__":
    main()
