// AppSmokeTests.swift — смоук-тесты UI-слоя без сети.
import XCTest

@testable import Aura

@MainActor
final class AppSmokeTests: XCTestCase {

    func testStoreStartsLoggedOut() {
        let store = AppStore()
        XCTAssertEqual(store.authState, .loggedOut)
        XCTAssertTrue(store.messages.isEmpty)
        XCTAssertTrue(store.tasks.isEmpty)
    }

    func testTaskDueParsing() {
        // Задачи из ISO-строк сервера должны разбираться в Date (для EventKit).
        XCTAssertNotNil(TasksView.parseISO("2030-01-01T10:00:00Z"))
        XCTAssertNotNil(TasksView.parseISO("2030-01-01T10:00:00.000Z"))
        XCTAssertNil(TasksView.parseISO(""))
    }
}
