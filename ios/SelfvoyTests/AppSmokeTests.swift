// AppSmokeTests.swift — смоук-тесты UI-слоя без сети.
import XCTest

@testable import Selfvoy

@MainActor
final class AppSmokeTests: XCTestCase {

    func testStoreStartsLoggedOut() {
        let store = AppStore()
        XCTAssertEqual(store.authState, .loggedOut)
        XCTAssertEqual(store.tab, .home)
        XCTAssertTrue(store.messages.isEmpty)
        XCTAssertTrue(store.tasks.isEmpty)
        XCTAssertTrue(store.dialog.isEmpty)
    }

    func testTaskDueParsing() {
        // Задачи из ISO-строк сервера должны разбираться в Date (для EventKit и экранов).
        XCTAssertNotNil(TasksView.parseISO("2030-01-01T10:00:00Z"))
        XCTAssertNotNil(TasksView.parseISO("2030-01-01T10:00:00.000Z"))
        XCTAssertNotNil(Format.parseISO("2030-01-01T10:00:00"))
        XCTAssertNil(TasksView.parseISO(""))
    }

    func testRussianPlurals() {
        XCTAssertEqual(Format.plural(1, "час", "часа", "часов"), "час")
        XCTAssertEqual(Format.plural(3, "час", "часа", "часов"), "часа")
        XCTAssertEqual(Format.plural(11, "час", "часа", "часов"), "часов")
        XCTAssertEqual(Format.plural(22, "час", "часа", "часов"), "часа")
    }

    func testGenitiveNames() {
        XCTAssertEqual(Format.genitive("Алекс"), "Алекса")
        XCTAssertEqual(Format.genitive("Дима"), "Димы")
        XCTAssertEqual(Format.genitive("Мария"), "Марии")
        XCTAssertEqual(Format.genitive("Ольга"), "Ольги")
    }

    func testTabsOfDesign2026() {
        // Четыре вкладки снизу; вложенные разделы возвращают «назад» в них.
        XCTAssertEqual([AppTab.home, .booking, .selfvoy, .more].filter(\.isRoot).count, 4)
        XCTAssertFalse(AppTab.packages.isRoot)
        XCTAssertFalse(AppTab.contacts.isRoot)
        XCTAssertFalse(AppTab.settings.isRoot)
        let store = AppStore()
        store.selectTab(.more)
        store.selectTab(.packages)
        XCTAssertEqual(store.rootTab, .more)
        XCTAssertEqual(BookingFocus(bookingId: 7, start: nil).id, "7-")
    }

    func testIsoRoundTrip() {
        let date = Date(timeIntervalSince1970: 1_900_000_000)
        XCTAssertEqual(Format.parseISO(Format.isoString(date)), date)
    }
}
