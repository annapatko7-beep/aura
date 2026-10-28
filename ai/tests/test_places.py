"""Тесты для модуля поиска мест (places.py)."""

from selfvoy_ai import places


def test_score_place_basic():
    cafe = {
        "name": "Тестовое кафе",
        "rating": 4.5,
        "price": 2,
        "tags": ["coffee", "vegan"],
        "lat": 50.86,
        "lon": 6.06,
    }
    score_normal = places.score_place(cafe, diet=["vegan"])
    assert score_normal > 0.9  # 4.5/5 + 0.25 = 1.15

    score_meat = places.score_place(
        {**cafe, "tags": ["meat"]}, diet=["vegan"]
    )
    # penalty for meat when vegan requested
    assert score_meat < score_normal


def test_score_place_budget_penalty():
    cafe = {
        "name": "Дорогой ресторан",
        "rating": 4.8,
        "price": 4,
        "tags": ["dinner"],
        "lat": 50.86,
        "lon": 6.06,
    }
    score = places.score_place(cafe, budget_limit=2)
    score_no_limit = places.score_place(cafe, budget_limit=5)
    assert score < score_no_limit


def test_score_place_midpoint_proximity():
    close_cafe = {
        "name": "Рядом с серединой",
        "rating": 4.5,
        "price": 2,
        "tags": ["coffee"],
        "lat": 50.86,
        "lon": 6.06,
    }
    far_cafe = {
        "name": "Далеко",
        "rating": 4.5,
        "price": 2,
        "tags": ["coffee"],
        "lat": 51.50,
        "lon": 7.00,
    }
    midpoint = {"lat": 50.86, "lon": 6.06}
    score_close = places.score_place(close_cafe, midpoint=midpoint)
    score_far = places.score_place(far_cafe, midpoint=midpoint)
    assert score_close > score_far


def test_search_places_fallback():
    res = places.search_places(city="Керкраде", diet=["vegan"], limit=2)
    assert "results" in res
    assert len(res["results"]) <= 2
    assert res["count"] <= 2
    for item in res["results"]:
        assert "name" in item
        assert "score" in item
        assert "tags" in item


def test_search_places_cache():
    res1 = places.search_places(city="Маастрихт", diet=["coffee"], limit=2)
    res2 = places.search_places(city="Маастрихт", diet=["coffee"], limit=2)
    assert res1["count"] == res2["count"]
    assert res2.get("source") in ("cache", "fallback", "osm")
