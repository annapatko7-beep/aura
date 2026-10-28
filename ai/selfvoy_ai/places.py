"""places.py — поиск реальных мест (кафе, рестораны) для встреч Аур.

Использует OpenStreetMap (Overpass API / Nominatim) с фильтрацией по диете,
бюджету и расстоянию до середины пути (midpoint).
При недоступности сети или таймауте автоматически откатывается на проверенную
базу мест с тем же интерфейсом.
"""

from __future__ import annotations

import logging
import math
import time
from typing import Any, Dict, List, Optional, Sequence

logger = logging.getLogger("selfvoy.places")

# Кэш геокодирования и запросов: {(lat, lon, diet_tuple): (timestamp, results)}
_GEO_CACHE: Dict[str, tuple[float, List[Dict[str, Any]]]] = {}
_CITY_COORDS_CACHE: Dict[str, tuple[float, float]] = {
    "москва": (55.7558, 37.6173),
    "санкт-петербург": (59.9343, 30.3351),
    "керкраде": (50.8658, 6.0625),
    "херлен": (50.8882, 5.9795),
    "маастрихт": (50.8514, 5.6910),
    "берлин": (52.5200, 13.4050),
    "лондон": (51.5074, -0.1278),
    "париж": (48.8566, 2.3522),
    "амстердам": (52.3676, 4.9041),
    "ереван": (40.1792, 44.4991),
    "тбилиси": (41.7151, 44.8271),
    "алматы": (43.2389, 76.8897),
}

# Резервная база проверенных заведений (fallback)
FALLBACK_CAFES: List[Dict[str, Any]] = [
    {"name": "Кофе на полпути", "city": "Керкраде", "address": "Markt 1", "tags": ["coffee", "quiet", "vegan"], "rating": 4.7, "price": 2, "lat": 50.861, "lon": 6.064},
    {"name": "Aurora Roasters", "city": "Керкраде", "address": "Stationstraat 12", "tags": ["coffee", "workspace", "gluten_free"], "rating": 4.6, "price": 3, "lat": 50.857, "lon": 6.071},
    {"name": "Grey Garden", "city": "Херлен", "address": "Pancratiusplein 4", "tags": ["brunch", "vegan", "quiet"], "rating": 4.5, "price": 2, "lat": 50.888, "lon": 5.978},
    {"name": "Steak House Nord", "city": "Херлен", "address": "Spoorweglaan 8", "tags": ["dinner", "meat"], "rating": 4.4, "price": 4, "lat": 50.885, "lon": 5.982},
    {"name": "Matcha Point", "city": "Маастрихт", "address": "Vrijthof 21", "tags": ["coffee", "vegan", "workspace"], "rating": 4.8, "price": 3, "lat": 50.849, "lon": 5.691},
    {"name": "Aura Central Cafe", "city": "Москва", "address": "ул. Тверская, 15", "tags": ["coffee", "workspace", "vegan", "quiet"], "rating": 4.8, "price": 2, "lat": 55.7610, "lon": 37.6080},
    {"name": "Зелёный Сад", "city": "Москва", "address": "Чистопрудный бул., 8", "tags": ["vegan", "vegetarian", "healthy"], "rating": 4.7, "price": 3, "lat": 55.7625, "lon": 37.6430},
    {"name": "Кофе и Точка", "city": "Санкт-Петербург", "address": "Невский пр., 44", "tags": ["coffee", "pastry", "workspace"], "rating": 4.6, "price": 2, "lat": 59.9350, "lon": 30.3340},
]


def score_place(
    place: Dict[str, Any],
    diet: Sequence[str] = (),
    budget_limit: Optional[float] = None,
    midpoint: Optional[Dict[str, float]] = None,
) -> float:
    """Вычисляет соответствие заведения предпочтениям пользователей."""
    rating = float(place.get("rating", 4.5))
    score = rating / 5.0
    tags = set(place.get("tags") or [])

    if diet:
        hits = sum(1 for d in diet if d in tags)
        score += 0.25 * hits
        if any(d in ("vegan", "vegetarian") for d in diet) and "meat" in tags and hits == 0:
            score -= 0.4
        if "halal" in diet and "halal" not in tags and "pork" in tags:
            score -= 0.5

    price = place.get("price", 2)
    if budget_limit and price > budget_limit:
        score -= 0.3

    if midpoint and "lat" in midpoint and "lon" in midpoint and "lat" in place and "lon" in place:
        dist = math.hypot(place["lat"] - midpoint["lat"], place["lon"] - midpoint["lon"])
        score += max(0.0, 0.3 - dist * 2.0)

    return round(score, 3)


def _geocode_city(city: str) -> Optional[tuple[float, float]]:
    """Получает координаты города (из локального кэша или через Nominatim)."""
    clean_city = city.strip().lower()
    if clean_city in _CITY_COORDS_CACHE:
        return _CITY_COORDS_CACHE[clean_city]

    try:
        import httpx

        url = "https://nominatim.openstreetmap.org/search"
        headers = {"User-Agent": "Aura-Agent-Network/1.0 (https://github.com/USER/selfvoy)"}
        params = {"q": city, "format": "json", "limit": "1"}
        resp = httpx.get(url, params=params, headers=headers, timeout=4.0)
        if resp.status_code == 200:
            data = resp.json()
            if data and isinstance(data, list):
                lat = float(data[0]["lat"])
                lon = float(data[0]["lon"])
                _CITY_COORDS_CACHE[clean_city] = (lat, lon)
                return lat, lon
    except Exception as exc:
        logger.warning("Nominatim geocoding failed for %s: %s", city, exc)

    return None


def _query_overpass(lat: float, lon: float, radius: int = 2500) -> List[Dict[str, Any]]:
    """Запрашивает кафе и рестораны вокруг координат через OpenStreetMap Overpass API."""
    import httpx

    overpass_url = "https://overpass-api.de/api/interpreter"
    query = f"""
    [out:json][timeout:5];
    (
      node["amenity"~"cafe|restaurant|bistro"](around:{radius},{lat},{lon});
      way["amenity"~"cafe|restaurant|bistro"](around:{radius},{lat},{lon});
    );
    out center 30;
    """
    try:
        resp = httpx.post(overpass_url, data={"data": query}, timeout=6.0)
        if resp.status_code != 200:
            return []
        data = resp.json()
        elements = data.get("elements", [])
        places = []
        for elem in elements:
            tags = elem.get("tags", {})
            name = tags.get("name") or tags.get("name:ru") or tags.get("name:en")
            if not name:
                continue

            elem_lat = elem.get("lat") or (elem.get("center", {}).get("lat"))
            elem_lon = elem.get("lon") or (elem.get("center", {}).get("lon"))
            if elem_lat is None or elem_lon is None:
                continue

            place_tags = []
            amenity = tags.get("amenity", "cafe")
            place_tags.append(amenity)

            cuisine = tags.get("cuisine", "")
            if cuisine:
                for c in cuisine.replace(";", ",").split(","):
                    c_clean = c.strip().lower()
                    if c_clean:
                        place_tags.append(c_clean)

            if tags.get("diet:vegan") in ("yes", "only"):
                place_tags.append("vegan")
            if tags.get("diet:vegetarian") in ("yes", "only"):
                place_tags.append("vegetarian")
            if tags.get("diet:halal") in ("yes", "only"):
                place_tags.append("halal")
            if tags.get("diet:gluten_free") in ("yes", "only"):
                place_tags.append("gluten_free")
            if tags.get("internet_access") in ("wlan", "yes"):
                place_tags.append("workspace")

            street = tags.get("addr:street", "")
            house = tags.get("addr:housenumber", "")
            address = f"{street} {house}".strip()

            price = 2
            if "expensive" in tags.get("price_level", ""):
                price = 4
            elif "cheap" in tags.get("price_level", ""):
                price = 1

            places.append({
                "name": name,
                "city": tags.get("addr:city", ""),
                "address": address,
                "lat": float(elem_lat),
                "lon": float(elem_lon),
                "tags": list(set(place_tags)),
                "rating": 4.6,
                "price": price,
            })
        return places
    except Exception as exc:
        logger.warning("Overpass API query failed: %s", exc)
        return []


def search_places(
    city: str = "",
    midpoint: Optional[Dict[str, float]] = None,
    diet: Optional[Sequence[str]] = None,
    budget_limit: Optional[float] = None,
    limit: int = 3,
    radius: int = 2500,
) -> Dict[str, Any]:
    """Основная функция поиска мест: сначала пробует OSM, при сбое использует fallback."""
    diet_seq = list(diet or [])
    coords: Optional[tuple[float, float]] = None

    if midpoint and "lat" in midpoint and "lon" in midpoint:
        coords = (float(midpoint["lat"]), float(midpoint["lon"]))
    elif city:
        coords = _geocode_city(city)

    cache_key = f"{coords}_{tuple(sorted(diet_seq))}_{budget_limit}"
    now = time.time()
    if cache_key in _GEO_CACHE:
        ts, cached_res = _GEO_CACHE[cache_key]
        if now - ts < 1800:
            return {"results": cached_res[:limit], "source": "cache", "count": len(cached_res)}

    osm_places: List[Dict[str, Any]] = []
    if coords:
        osm_places = _query_overpass(coords[0], coords[1], radius=radius)

    source = "osm" if osm_places else "fallback"
    candidates = osm_places if osm_places else [
        c for c in FALLBACK_CAFES if not city or c.get("city", "").lower() == city.lower()
    ]
    if not candidates:
        candidates = list(FALLBACK_CAFES)

    scored = []
    for p in candidates:
        item = dict(p)
        item["score"] = score_place(item, diet=diet_seq, budget_limit=budget_limit, midpoint=midpoint)
        scored.append(item)

    scored.sort(key=lambda x: -x["score"])
    top = scored[: max(1, limit)]

    _GEO_CACHE[cache_key] = (now, top)

    return {
        "results": top,
        "source": source,
        "count": len(top),
        "midpoint": midpoint,
        "city": city,
    }
