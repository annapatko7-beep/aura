// AuraKitTests.swift — тесты общего слоя без сети: JSON, кадры протокола,
// deep links. Гоняются `swift test` в ios/AuraKit (или XCTest в Xcode).
import XCTest

@testable import AuraKit

final class JSONValueTests: XCTestCase {

    func testRoundTrip() throws {
        let json = """
        {"id": 12, "ok": true, "text": "привет", "tags": ["a", "b"], "meta": null}
        """
        let value = try JSONDecoder().decode(JSONValue.self, from Data(json.utf8))
        XCTAssertEqual(value.int("id"), 12)
        XCTAssertEqual(value.bool("ok"), true)
        XCTAssertEqual(value.string("text"), "привет")
        XCTAssertEqual(value.array("tags").count, 2)
        XCTAssertEqual(value["meta"], .null)

        let reEncoded = try JSONEncoder().encode(value)
        let decodedAgain = try JSONDecoder().decode(JSONValue.self, from: reEncoded)
        XCTAssertEqual(decodedAgain, value)
    }

    func testDefaults() {
        let value = JSONValue.object([:])
        XCTAssertEqual(value.string("missing", default: "x"), "x")
        XCTAssertEqual(value.int("missing", default: 7), 7)
        XCTAssertEqual(value.array("missing"), [])
    }
}

final class ProtocolFrameTests: XCTestCase {

    func testRequestFrameShape() throws {
        let frame = AuraRequestFrame(id: "1", type: "auth.login",
                                     payload: .object(["email": .string("a@b.c")]))
        let data = try JSONEncoder().encode(frame)
        let object = try JSONSerialization.jsonObject(with: data) as? [String: Any]
        XCTAssertEqual(object?["id"] as? String, "1")
        XCTAssertEqual(object?["type"] as? String, "auth.login")
        let payload = object?["payload"] as? [String: Any]
        XCTAssertEqual(payload?["email"] as? String, "a@b.c")
    }

    func testErrorFrameDecoding() throws {
        let json = """
        {"id":"1","type":"error","code":"bad_request","message":"нужен email"}
        """
        let frame = try JSONDecoder().decode(AuraResponseFrame.self, from: Data(json.utf8))
        XCTAssertEqual(frame.type, "error")
        XCTAssertEqual(frame.code, "bad_request")
        XCTAssertEqual(frame.message, "нужен email")
    }

    func testEventFrameDecoding() throws {
        let json = """
        {"type":"event","event":"chat.message","payload":{"text":"ок"}}
        """
        let frame = try JSONDecoder().decode(AuraResponseFrame.self, from: Data(json.utf8))
        XCTAssertEqual(frame.type, "event")
        XCTAssertEqual(frame.event, AuraEventName.chatMessage)
        XCTAssertEqual(frame.payload?.string("text"), "ок")
    }
}

final class DeepLinkTests: XCTestCase {

    func testChats() {
        XCTAssertEqual(DeepLink(url: URL(string: "aura://chats/42")!), .chats(id: 42))
        XCTAssertEqual(DeepLink(url: URL(string: "aura://chats")!), .chats(id: nil))
    }

    func testSimpleRoutes() {
        XCTAssertEqual(DeepLink(url: URL(string: "aura://voice")!), .voice)
        XCTAssertEqual(DeepLink(url: URL(string: "aura://tasks")!), .tasks)
        XCTAssertEqual(DeepLink(url: URL(string: "aura://settings")!), .settings)
    }

    func testAsk() {
        let link = DeepLink(url: URL(string: "aura://ask?text=%D0%9F%D1%80%D0%B8%D0%B2%D0%B5%D1%82")!)
        XCTAssertEqual(link, .ask(text: "Привет"))
    }

    func testOAuthCallback() {
        let link = DeepLink(url: URL(string: "aura://oauth?provider=google_gmail&code=abc&state=st1")!)
        XCTAssertEqual(link, .oauth(provider: "google_gmail", code: "abc", state: "st1"))
    }

    func testRejectsForeignSchemeAndUnknownHost() {
        XCTAssertNil(DeepLink(url: URL(string: "https://aura.ai/chats/1")!))
        XCTAssertNil(DeepLink(url: URL(string: "aura://unknown")!))
        XCTAssertNil(DeepLink(url: URL(string: "aura://oauth?provider=x")!))  // нет code/state
    }
}
