#!/usr/bin/env python3
"""Проход по экранам Android-клиента на эмуляторе со скриншотами (adb + uiautomator).

Ищет элементы по тексту или описанию (contentDescription), нажимает в центр,
снимает экран. Нужны: запущенный эмулятор, установленный APK и dev-стек с
демо-данными (tools/dev_stack.sh, tools/demo_seed.py) на хосте — эмулятор
видит его как ws://10.0.2.2:9000, это адрес клиента по умолчанию.

    SELFVOY_DEMO_PASSWORD=... python3 tools/android_tour.py shots/android
Шаги, которые не удались, записываются в tour.log; код возврата 1, если
приложение упало или не удалось войти.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

PACKAGE = "app.selfvoy"
OUT = sys.argv[1] if len(sys.argv) > 1 else "shots/android"
EMAIL = os.environ.get("SELFVOY_TOUR_EMAIL", "alex@selfvoy.local")
PASSWORD = os.environ.get("SELFVOY_DEMO_PASSWORD", "")
problems: list[str] = []
counter = 0


def adb(*args: str, check: bool = False) -> str:
    result = subprocess.run(["adb", *args], capture_output=True, text=True, timeout=120)
    if check and result.returncode != 0:
        raise RuntimeError(f"adb {' '.join(args)}: {result.stderr}")
    return result.stdout


def log(message: str) -> None:
    print(message, flush=True)
    with open(os.path.join(OUT, "tour.log"), "a", encoding="utf-8") as fh:
        fh.write(message + "\n")


def shot(name: str) -> None:
    global counter
    counter += 1
    path = os.path.join(OUT, f"{counter:02d}-{name}.png")
    with open(path, "wb") as fh:
        fh.write(subprocess.run(["adb", "exec-out", "screencap", "-p"], capture_output=True, timeout=60).stdout)
    log(f"снимок {path}")


dump_errors = 0


def nodes() -> list[dict]:
    global dump_errors
    adb("shell", "rm", "-f", "/sdcard/ui.xml")
    status = adb("shell", "uiautomator", "dump", "/sdcard/ui.xml").strip()
    xml = adb("shell", "cat", "/sdcard/ui.xml")
    start = xml.find("<?xml")
    if start < 0:
        dump_errors += 1
        if dump_errors <= 5:
            log(f"  uiautomator: {status[:200]}")
        return []
    found = []
    for node in ET.fromstring(xml[start:]).iter("node"):
        bounds = re.findall(r"\d+", node.get("bounds", ""))
        if len(bounds) != 4:
            continue
        x1, y1, x2, y2 = map(int, bounds)
        found.append({"text": node.get("text", ""), "desc": node.get("content-desc", ""),
                      "x": (x1 + x2) // 2, "y": (y1 + y2) // 2})
    return found


def find(label: str, exact: bool = True, lowest: bool = False, timeout: float = 10) -> dict | None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        matches = [n for n in nodes() if any(
            (value == label) if exact else (label in value) for value in (n["text"], n["desc"]))]
        if matches:
            return max(matches, key=lambda n: n["y"]) if lowest else matches[0]
        time.sleep(1)
    return None


def tap(label: str, exact: bool = True, lowest: bool = False, timeout: float = 10, wait: float = 1.5) -> bool:
    node = find(label, exact, lowest, timeout)
    if node is None:
        problems.append(f"не найдено: «{label}»")
        log(f"! не найдено: «{label}»")
        return False
    adb("shell", "input", "tap", str(node["x"]), str(node["y"]))
    time.sleep(wait)
    return True


def keyboard_shown() -> bool:
    return "mInputShown=true" in adb("shell", "dumpsys", "input_method")


def hide_keyboard() -> None:
    if keyboard_shown():
        adb("shell", "input", "keyevent", "4")
        time.sleep(1)


def type_text(text: str) -> None:
    adb("shell", "input", "text", text.replace(" ", "%s"))
    time.sleep(0.5)


def swipe_up() -> None:
    adb("shell", "input", "swipe", "540", "1700", "540", "700", "400")
    time.sleep(1.5)


def crashed() -> bool:
    return "FATAL EXCEPTION" in adb("logcat", "-d", "-s", "AndroidRuntime:E")


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    if len(PASSWORD) < 8:
        sys.exit("Задайте SELFVOY_DEMO_PASSWORD")
    adb("shell", "pm", "grant", PACKAGE, "android.permission.POST_NOTIFICATIONS")
    adb("logcat", "-c")
    adb("shell", "am", "start", "-W", "-n", f"{PACKAGE}/.MainActivity", check=True)
    time.sleep(4)
    shot("login")

    # Вход
    tap("Введите email")
    type_text(EMAIL)
    tap("••••••••")
    type_text(PASSWORD)
    shot("login-keyboard")
    hide_keyboard()
    tap("Войти", lowest=True, wait=6)
    if find("Все системы работают", timeout=60) is None:
        # Снимок покажет, вошли ли мы: uiautomator мог просто не дождаться простоя.
        problems.append("не дождались текста «Все системы работают»")
        log("! главная не распознана, проход продолжается по снимкам")
    time.sleep(3)
    shot("home")
    swipe_up()
    shot("home-scrolled")

    # Мой Selfvoy: подсказка-чип, ответ, клавиатура над полем ввода
    tap("Selfvoy", lowest=True)
    shot("selfvoy")
    if tap("Что у меня сегодня в расписании?", wait=10):
        shot("selfvoy-reply")
    if tap("Напишите что-нибудь…", wait=2):
        shot("selfvoy-keyboard")
        hide_keyboard()

    # Переговоры: форма → раунд → итог
    tap("Переговоры", lowest=True)
    shot("negotiation")
    if find("Новые", timeout=3) and tap("Новые"):  # форма уже открыта, если раундов ещё не было
        shot("negotiation-form")
    if tap("Начать переговоры", lowest=True, wait=8):
        shot("negotiation-steps")
        swipe_up()
        shot("negotiation-result")

    # Задачи и новая задача
    tap("Задачи", lowest=True)
    shot("tasks")
    if tap("Новая задача", wait=2):
        shot("task-dialog")
        tap("Отмена")

    # Контакты: карточка и чат
    tap("Контакты", lowest=True)
    shot("contacts")
    if tap("Дима Волков", wait=2):
        shot("contact-card")
        if tap("Открыть чат", wait=3):
            shot("chat")
            adb("shell", "input", "keyevent", "4")
            time.sleep(1.5)

    # Шапка «Главной»: настройки, подтверждения, уведомления; «назад» системной кнопкой
    tap("Главная", lowest=True)
    if tap("АМ", timeout=5):
        shot("settings")
        swipe_up()
        shot("settings-scrolled")
        adb("shell", "input", "keyevent", "4")
        time.sleep(1.5)
        if find("Все системы работают", timeout=5) is None:
            problems.append("«назад» из настроек не вернул на главную")
        shot("back-to-home")
    if tap("Подтверждения", timeout=5):
        shot("confirmations")
        adb("shell", "input", "keyevent", "4")
        time.sleep(1.5)
    if tap("Входящая", timeout=5):
        shot("notifications")
        adb("shell", "input", "keyevent", "4")
        time.sleep(1.5)

    if crashed():
        log("! приложение упало:\n" + adb("logcat", "-d", "-s", "AndroidRuntime:E")[-4000:])
        return 1
    log("замечания: " + ("; ".join(problems) if problems else "нет"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
