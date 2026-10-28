// SelfvoyKitTests.swift — тесты общего слоя без сети: JSON, кадры протокола,
// deep links. Гоняются `swift test` в ios/SelfvoyKit (или XCTest в Xcode).
import XCTest

@testable import SelfvoyKit

final class JSONValueTests: XCTestCase {

    func testRoundTrip() throws {
        let json = """
        {"id": 12, "ok": true, "text": "привет", "tags": ["a", "b"], "meta": null}
        """
        let value = try JSONDecoder().decode(JSONValue.self, from: Data(json.utf8))
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
        let frame = SelfvoyRequestFrame(id: "1", type: "auth.login",
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
        let frame = try JSONDecoder().decode(SelfvoyResponseFrame.self, from: Data(json.utf8))
        XCTAssertEqual(frame.type, "error")
        XCTAssertEqual(frame.code, "bad_request")
        XCTAssertEqual(frame.message, "нужен email")
    }

    func testEventFrameDecoding() throws {
        let json = """
        {"type":"event","event":"chat.message","payload":{"text":"ок"}}
        """
        let frame = try JSONDecoder().decode(SelfvoyResponseFrame.self, from: Data(json.utf8))
        XCTAssertEqual(frame.type, "event")
        XCTAssertEqual(frame.event, SelfvoyEventName.chatMessage)
        XCTAssertEqual(frame.payload?.string("text"), "ок")
    }
}

final class DeepLinkTests: XCTestCase {

    func testChats() {
        XCTAssertEqual(DeepLink(url: URL(string: "selfvoy://chats/42")!), .chats(id: 42))
        XCTAssertEqual(DeepLink(url: URL(string: "selfvoy://chats")!), .chats(id: nil))
    }

    func testSimpleRoutes() {
        XCTAssertEqual(DeepLink(url: URL(string: "selfvoy://voice")!), .voice)
        XCTAssertEqual(DeepLink(url: URL(string: "selfvoy://tasks")!), .tasks)
        XCTAssertEqual(DeepLink(url: URL(string: "selfvoy://settings")!), .settings)
        XCTAssertEqual(DeepLink(url: URL(string: "selfvoy://notifications")!), .notifications)
    }

    func testAsk() {
        let link = DeepLink(url: URL(string: "selfvoy://ask?text=%D0%9F%D1%80%D0%B8%D0%B2%D0%B5%D1%82")!)
        XCTAssertEqual(link, .ask(text: "Привет"))
    }

    func testOAuthCallback() {
        let link = DeepLink(url: URL(string: "selfvoy://oauth?provider=google_gmail&code=abc&state=st1")!)
        XCTAssertEqual(link, .oauth(provider: "google_gmail", code: "abc", state: "st1"))
    }

    func testSignupKeepsOnlyValidReferralCode() {
        XCTAssertEqual(DeepLink(url: URL(string: "selfvoy://signup?ref=abcd2345")!), .signup(ref: "ABCD2345"))
        XCTAssertEqual(DeepLink(url: URL(string: "selfvoy://signup?ref=bad")!), .signup(ref: ""))
        XCTAssertEqual(DeepLink(url: URL(string: "selfvoy://signup")!), .signup(ref: ""))
    }

    func testRejectsForeignSchemeAndUnknownHost() {
        XCTAssertNil(DeepLink(url: URL(string: "https://selfvoy.ai/chats/1")!))
        XCTAssertNil(DeepLink(url: URL(string: "selfvoy://unknown")!))
        XCTAssertNil(DeepLink(url: URL(string: "selfvoy://oauth?provider=x")!))  // нет code/state
    }
}

final class ProfileTests: XCTestCase {

    func testUserDecodesBioAndOldPayloads() throws {
        let fresh = """
        {"id": 7, "email": "a@b.c", "display_name": "Алекс", "bio": "люблю кофе", "avatar_url": ""}
        """
        let user = try JSONDecoder().decode(SelfvoyUser.self, from: Data(fresh.utf8))
        XCTAssertEqual(user, SelfvoyUser(id: 7, email: "a@b.c", displayName: "Алекс", bio: "люблю кофе"))

        // Старый сервер: ни bio, ни avatar_url — декодируется с пустыми строками.
        let old = """
        {"id": 7, "email": "a@b.c", "display_name": "Алекс"}
        """
        let legacy = try JSONDecoder().decode(SelfvoyUser.self, from: Data(old.utf8))
        XCTAssertEqual(legacy.bio, "")
        XCTAssertEqual(legacy.avatarUrl, "")

        let dynamic = try JSONDecoder().decode(JSONValue.self, from: Data(fresh.utf8))
        XCTAssertEqual(SelfvoyUser(json: dynamic), user)
    }

    func testProfileUpdatePayloadHasOnlyPassedFields() {
        XCTAssertEqual(SelfvoyClient.profileUpdatePayload(displayName: "Алекс"),
                       ["display_name": .string("Алекс")])
        XCTAssertEqual(SelfvoyClient.profileUpdatePayload(displayName: "Алекс", bio: ""),
                       ["display_name": .string("Алекс"), "bio": .string("")])
        XCTAssertEqual(SelfvoyClient.profileUpdatePayload(avatarUrl: "https://x.y/a.png"),
                       ["avatar_url": .string("https://x.y/a.png")])
        XCTAssertEqual(SelfvoyClient.profileUpdatePayload(), [:])
    }

    func testProfileRules() {
        XCTAssertNil(SelfvoyProfileRules.validate(displayName: "Алекс", bio: ""))
        XCTAssertNotNil(SelfvoyProfileRules.validate(displayName: "   ", bio: ""))
        XCTAssertNil(SelfvoyProfileRules.validate(displayName: String(repeating: "я", count: 80), bio: ""))
        XCTAssertNotNil(SelfvoyProfileRules.validate(displayName: String(repeating: "я", count: 81), bio: ""))
        XCTAssertNil(SelfvoyProfileRules.validate(displayName: "А", bio: String(repeating: "б", count: 300)))
        XCTAssertNotNil(SelfvoyProfileRules.validate(displayName: "А", bio: String(repeating: "б", count: 301)))
    }
}

final class PrivacyTests: XCTestCase {

    func testDefaultsAreTrue() throws {
        XCTAssertEqual(SelfvoyPrivacy(json: nil), SelfvoyPrivacy())
        XCTAssertEqual(SelfvoyPrivacy(preferences: .emptyObject),
                       SelfvoyPrivacy(shareFreeSlotsOnly: true, hideEventTitles: true, allowPlaceChoice: true))
        let partial = try JSONDecoder().decode(SelfvoyPrivacy.self, from: Data(#"{"hide_event_titles": false}"#.utf8))
        XCTAssertEqual(partial, SelfvoyPrivacy(hideEventTitles: false))
    }

    func testFromPreferences() throws {
        let json = """
        {"theme": "dark", "privacy": {"share_free_slots_only": false, "hide_event_titles": true,
                                      "allow_place_choice": false}}
        """
        let prefs = try JSONDecoder().decode(JSONValue.self, from: Data(json.utf8))
        let privacy = SelfvoyPrivacy(preferences: prefs)
        XCTAssertFalse(privacy[.shareFreeSlotsOnly])
        XCTAssertTrue(privacy[.hideEventTitles])
        XCTAssertFalse(privacy[.allowPlaceChoice])
        XCTAssertEqual(privacy.json, prefs["privacy"])
    }

    func testSetPayloadIsAtRootWithoutWrapper() {
        let payload = SelfvoyClient.privacyPayload([.allowPlaceChoice: false])
        XCTAssertEqual(payload, .object(["privacy": .object(["allow_place_choice": .bool(false)])]))
        XCTAssertNil(payload["preferences"])
    }
}

final class BookingModelTests: XCTestCase {

    private func decode(_ text: String) throws -> JSONValue {
        try JSONDecoder().decode(JSONValue.self, from: Data(text.utf8))
    }

    func testPageAndPayload() throws {
        let page = BookingPage(json: try decode("""
        {"id": 3, "slug": "abc123def4", "url": "", "title": "Консультация", "description": "Час",
         "duration_minutes": 60, "days_ahead": 14, "max_per_day": 4, "notice_hours": 2,
         "max_series": 3, "weekends": true, "status": "paused", "upcoming": 2}
        """))
        XCTAssertEqual(page.link, "#/b/abc123def4")
        XCTAssertEqual(page.fields.maxSeries, 3)
        XCTAssertTrue(page.fields.weekends)
        XCTAssertEqual(page.status, "paused")
        let payload = BookingPageFields(title: " Консультация ", durationMinutes: 60).payload
        XCTAssertEqual(payload["title"], .string("Консультация"))
        XCTAssertEqual(payload["duration_minutes"], .number(60))
        XCTAssertEqual(Set(payload.keys), ["title", "description", "duration_minutes", "days_ahead",
                                           "max_per_day", "notice_hours", "max_series", "weekends",
                                           "guest_reminders", "hide_branding", "price", "currency", "prepay",
                                           "deposit", "payment_url", "payment_note", "pay_within_minutes",
                                           "cancel_hours", "late_cancel", "policy_text", "packages"])
        // Старый сервер без полей F1–F5 — значения по умолчанию.
        XCTAssertTrue(page.fields.guestReminders)
        XCTAssertEqual(page.fields.prepay, .off)
        XCTAssertEqual(page.fields.currency, "RUB")
        XCTAssertEqual(page.fields.payWithinMinutes, 60)
    }

    func testValidateLikeServer() {
        XCTAssertNil(BookingPageFields(title: "Встреча").validate())
        XCTAssertNotNil(BookingPageFields(title: "  ").validate())
        XCTAssertNotNil(BookingPageFields(title: "X", durationMinutes: 10).validate())
        XCTAssertNotNil(BookingPageFields(title: "X", maxSeries: 13).validate())
        XCTAssertNotNil(BookingPageFields(title: "X", noticeHours: 169).validate())
        XCTAssertNotNil(BookingPageFields(title: String(repeating: "а", count: 121)).validate())
    }

    func testBookingAndReferral() throws {
        let booking = Booking(json: try decode("""
        {"id": 9, "page_id": 3, "page_title": "Консультация", "guest_name": "Пётр", "guest_email": "p@x.ru",
         "guest_comment": "", "status": "active", "cancelled_by": "", "reschedules": 1, "occurrences": [
           {"start": "2026-10-28T07:00:00Z", "end": "2026-10-28T08:00:00Z", "status": "cancelled"},
           {"start": "2026-10-21T07:00:00Z", "end": "2026-10-21T08:00:00Z", "status": "active", "local": "2026-10-21T10:00"}]}
        """))
        XCTAssertEqual(booking.occurrences.first?.start, "2026-10-21T07:00:00Z")
        XCTAssertEqual(booking.occurrences.first?.local, "2026-10-21T10:00")
        XCTAssertEqual(booking.occurrences.last?.active, false)
        let referral = ReferralInfo(json: try decode("""
        {"code": "ABCD2345", "url": "", "joined": 2, "verified": 1,
         "referrals": [{"display_name": "Фёдор", "source": "booking", "joined_at": "2026-10-14T08:00:00Z", "verified": true}]}
        """))
        // Без SELFVOY_PUBLIC_URL делимся кодом, а не голым «#/signup?ref=…».
        XCTAssertFalse(referral.hasLink)
        XCTAssertEqual(referral.link, "ABCD2345")
        XCTAssertEqual(referral.shareText, "Код приглашения в Selfvoy: ABCD2345")
        XCTAssertEqual(referral.referrals.first?.displayName, "Фёдор")
        XCTAssertEqual(referral.verified, 1)
    }
}

final class ReferralTests: XCTestCase {

    private func decode(_ text: String) throws -> JSONValue {
        try JSONDecoder().decode(JSONValue.self, from: Data(text.utf8))
    }

    func testReferralWithPublicUrlSharesLink() throws {
        let info = ReferralInfo(json: try decode("""
        {"code": "ABCD2345", "url": "https://selfvoy.example/#/signup?ref=ABCD2345"}
        """))
        XCTAssertTrue(info.hasLink)
        XCTAssertEqual(info.link, "https://selfvoy.example/#/signup?ref=ABCD2345")
        XCTAssertEqual(info.shareText, "Заведи свой Selfvoy: https://selfvoy.example/#/signup?ref=ABCD2345")
    }

    func testCodeIsNormalizedAndValidated() {
        XCTAssertEqual(ReferralCode.normalize(" abcd2345 "), "ABCD2345")
        XCTAssertNil(ReferralCode.normalize(nil))
        XCTAssertNil(ReferralCode.normalize(""))
        XCTAssertNil(ReferralCode.normalize("ABCD234"))
        XCTAssertNil(ReferralCode.normalize("ABCD-2345"))
        XCTAssertNil(ReferralCode.normalize("АБВГ2345"))
    }

    func testRegisterPayloadWithoutRef() {
        let payload = SelfvoyClient.registerPayload(email: "a@b.c", password: "secret123", displayName: "Аня")
        XCTAssertEqual(Set(payload.keys), ["email", "password", "display_name"])
        XCTAssertEqual(Set(SelfvoyClient.registerPayload(email: "a@b.c", password: "secret123", displayName: "Аня",
                                                      ref: "bad").keys),
                       ["email", "password", "display_name"])
        XCTAssertEqual(Set(SelfvoyClient.registerPayload(email: "a@b.c", password: "secret123", displayName: "Аня",
                                                      ref: "  ").keys),
                       ["email", "password", "display_name"])
    }

    func testRegisterPayloadWithRef() {
        let payload = SelfvoyClient.registerPayload(email: "a@b.c", password: "secret123", displayName: "Аня",
                                                 ref: " abcd2345")
        XCTAssertEqual(Set(payload.keys), ["email", "password", "display_name", "ref", "ref_source"])
        XCTAssertEqual(payload["ref"], .string("ABCD2345"))
        XCTAssertEqual(payload["ref_source"], .string("link"))
    }
}
