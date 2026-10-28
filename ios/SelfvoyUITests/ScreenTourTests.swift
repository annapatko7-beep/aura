// ScreenTourTests.swift — проход по экранам iOS-клиента со скриншотами.
//
// Нужен dev-стек с демо-данными (tools/dev_stack.sh + tools/demo_seed.py) на
// ws://127.0.0.1:9000 — адрес клиента по умолчанию; симулятор видит хост.
// Учётные данные приходят из окружения (xcodebuild передаёт TEST_RUNNER_*):
//   TEST_RUNNER_SELFVOY_TOUR_EMAIL, TEST_RUNNER_SELFVOY_TOUR_PASSWORD,
//   TEST_RUNNER_SELFVOY_TOUR_SHOTS — папка на хосте для PNG.
// Без них тест пропускается, поэтому обычный прогон тестов его не требует.
import XCTest

final class ScreenTourTests: XCTestCase {

    private var app: XCUIApplication!
    private var shotsDir: URL?
    private var counter = 0

    override func setUpWithError() throws {
        continueAfterFailure = true
        let env = ProcessInfo.processInfo.environment
        guard let email = env["SELFVOY_TOUR_EMAIL"], !email.isEmpty,
              let password = env["SELFVOY_TOUR_PASSWORD"], !password.isEmpty else {
            throw XCTSkip("Нет SELFVOY_TOUR_EMAIL / SELFVOY_TOUR_PASSWORD — проход по экранам пропущен")
        }
        if let dir = env["SELFVOY_TOUR_SHOTS"], !dir.isEmpty {
            shotsDir = URL(fileURLWithPath: dir, isDirectory: true)
            try? FileManager.default.createDirectory(at: shotsDir!, withIntermediateDirectories: true)
        }
        app = XCUIApplication()
    }

    func testTour() throws {
        let env = ProcessInfo.processInfo.environment
        app.launch()
        shot("login")

        // Вход
        let email = app.textFields.firstMatch
        XCTAssertTrue(email.waitForExistence(timeout: 10), "нет поля email")
        email.tap()
        email.typeText(env["SELFVOY_TOUR_EMAIL"] ?? "")
        let password = app.secureTextFields.firstMatch
        password.tap()
        password.typeText(env["SELFVOY_TOUR_PASSWORD"] ?? "")
        shot("login-keyboard")
        app.buttons["Войти"].firstMatch.tap()
        // После входа — «Сегодня» (дизайн 2026): вкладки «Сегодня · Запись · ИИ · Ещё» и «+».
        guard app.buttons["tab.home"].waitForExistence(timeout: 25) else {
            shot("login-failed")
            XCTFail("вход не удался")
            return
        }
        sleep(3)
        shot("home")
        app.swipeUp()
        shot("home-scrolled")

        // Запись: страницы, пост со свободными окнами, записи гостей
        tapTab("Запись")
        shot("booking")
        if tapButton(containing: "Пост со свободными окнами") {
            sleep(3)
            shot("slots-post")
            _ = tapButton(containing: "Готово")
        }

        // «+» — быстрые действия
        if tapButton(containing: "Создать", id: "tab.create") {
            shot("quick-create")
            app.swipeDown()
            sleep(1)
        }

        // Ассистент
        tapTab("ИИ")
        shot("selfvoy")
        if tapButton(containing: "расписании") {
            sleep(10)
            shot("selfvoy-reply")
        }
        let input = app.textFields.firstMatch
        if input.waitForExistence(timeout: 3) {
            input.tap()
            sleep(1)
            shot("selfvoy-keyboard")
        }

        // «Ещё»: тариф, награда за приглашение, разделы
        tapTab("Ещё")
        shot("more")
        if tapButton(containing: "Абонементы") {
            shot("packages")
            tapBack()
        }
        if tapButton(containing: "Подключения") {
            shot("integrations")
            tapBack()
        }

        // Переговоры
        tapTab("Ещё")
        _ = tapButton(containing: "Переговоры")
        shot("negotiation")
        if tapButton(containing: "Начать переговоры") {
            sleep(8)
            shot("negotiation-steps")
            app.swipeUp()
            sleep(1)
            shot("negotiation-result")
        }

        // Задачи
        tapBack()
        _ = tapButton(containing: "Задачи")
        shot("tasks")
        if tapButton(containing: "Новая задача") {
            sleep(1)
            shot("task-sheet")
            _ = tapButton(containing: "Отмена")
        }

        // Контакты
        tapBack()
        _ = tapButton(containing: "Контакты и чаты")
        shot("contacts")
        if tapButton(containing: "Дима Волков") {
            shot("contact-card")
            if tapButton(containing: "Открыть чат") {
                sleep(2)
                shot("chat")
                tapBack()
            }
        }

        // Шапка «Сегодня»
        tapTab("Сегодня")
        // Строка «назад» вложенных разделов закреплена над прокруткой, поэтому
        // кнопка доступна и после прокрутки длинных настроек.
        if tapButton(containing: "Настройки", id: "home.settings") {
            shot("settings")
            app.swipeUp()
            shot("settings-scrolled")
            tapBack()
        }
        if tapButton(containing: "Подтверждения", id: "home.confirmations") {
            shot("confirmations")
            tapBack()
        }
        if tapButton(containing: "Уведомления", id: "home.notifications") {
            shot("notifications")
            tapBack()
        }
        shot("end")
    }

    // MARK: Помощники

    /// Вкладка нижней панели: среди кнопок с этим текстом — самая нижняя.
    private func tapTab(_ title: String) {
        // Пока открыта клавиатура, панель вкладок скрыта (так задумано) —
        // закрываем её клавишей Return, как сделал бы человек.
        if app.keyboards.firstMatch.exists {
            app.typeText("\n")
            sleep(1)
        }
        let matches = app.buttons.matching(NSPredicate(format: "label CONTAINS %@", title)).allElementsBoundByIndex
        guard let tab = matches.filter({ $0.isHittable }).max(by: { $0.frame.midY < $1.frame.midY }) else {
            XCTFail("нет вкладки «\(title)»")
            return
        }
        tab.tap()
        sleep(2)
    }

    /// Кнопка по подписи; если задан accessibilityIdentifier — подходит и он
    /// (у значков шапки к подписи добавляется счётчик, у идентификатора — нет).
    @discardableResult
    private func tapButton(containing text: String, id: String? = nil) -> Bool {
        let predicate = id.map { NSPredicate(format: "identifier == %@ OR label CONTAINS %@", $0, text) }
            ?? NSPredicate(format: "label CONTAINS %@", text)
        let button = app.buttons.matching(predicate).firstMatch
        guard button.waitForExistence(timeout: 5), waitUntilHittable(button) else {
            shot("missing-\(id ?? text)")
            XCTFail("нет кнопки «\(text)»")
            return false
        }
        button.tap()
        sleep(2)
        return true
    }

    /// Кнопка может на миг перекрываться (анимация смены раздела, плашка-тост
    /// сверху) — ждём, пока в неё можно попасть, а не проверяем один раз.
    private func waitUntilHittable(_ element: XCUIElement, timeout: TimeInterval = 5) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while !element.isHittable {
            if Date() > deadline { return false }
            usleep(250_000)
        }
        return true
    }

    /// «Назад» во вложенных разделах и в переписке (accessibilityIdentifier nav.back).
    private func tapBack() {
        _ = tapButton(containing: "Назад", id: "nav.back")
    }

    private func shot(_ name: String) {
        counter += 1
        let screenshot = XCUIScreen.main.screenshot()
        let attachment = XCTAttachment(screenshot: screenshot)
        attachment.name = String(format: "%02d-%@", counter, name)
        attachment.lifetime = .keepAlways
        add(attachment)
        if let dir = shotsDir {
            let url = dir.appendingPathComponent(String(format: "%02d-%@.png", counter, name))
            try? screenshot.pngRepresentation.write(to: url)
        }
    }
}
