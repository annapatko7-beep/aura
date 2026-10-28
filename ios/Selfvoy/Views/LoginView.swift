// LoginView.swift — вход и регистрация (дизайн 2026), плюс шаги auth v3:
// код из письма, 2FA (TOTP или резервный код), сброс пароля.
import SelfvoyKit
import SwiftUI

struct LoginView: View {

    @EnvironmentObject private var store: AppStore
    @State private var register = false
    @State private var email = ""
    @State private var password = ""
    @State private var displayName = ""
    @State private var focus = ""
    @State private var refCode = ""
    @State private var showPassword = false
    @State private var code = ""
    @State private var newPassword = ""
    @State private var trustDevice = true
    @State private var serverURL = ""
    @State private var showServer = false

    private var isRegister: Bool { register && store.authState == .loggedOut }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                brand
                    .padding(.top, 24)
                VStack(alignment: .leading, spacing: 6) {
                    Text(title)
                        .font(InkFont.largeTitle)
                        .foregroundStyle(Ink.label)
                        .accessibilityAddTraits(.isHeader)
                    if !subtitle.isEmpty {
                        Text(subtitle).font(InkFont.callout).foregroundStyle(Ink.secondary)
                    }
                }
                form
                serverSection
                    .padding(.top, 8)
            }
            .padding(.horizontal, 20)
            .padding(.bottom, 32)
            .frame(maxWidth: 520)
            .frame(maxWidth: .infinity)
        }
        .scrollDismissesKeyboard(.interactively)
        .inkScreen()
        .onAppear { serverURL = store.serverURLText }
        // selfvoy://signup?ref=CODE — сразу форма регистрации с подставленным кодом.
        .onChange(of: store.signupRef, initial: true) { _, link in
            guard let link else { return }
            register = true
            if !link.isEmpty { refCode = link }
            store.signupRef = nil
        }
    }

    private var brand: some View {
        HStack(spacing: 10) {
            InkMark(size: 40)
            Text("Selfvoy").font(.system(size: 22, weight: .bold)).foregroundStyle(Ink.label)
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Selfvoy")
    }

    private var title: String {
        switch store.authState {
        case .needsEmailCode: return "Подтверждение email"
        case .needs2fa: return "Двухфакторный вход"
        case .needsResetCode: return "Новый пароль"
        default: return isRegister ? "Создайте свой Selfvoy" : "Вход"
        }
    }

    private var subtitle: String {
        switch store.authState {
        case .needsEmailCode(let address): return "Код отправлен на \(address)"
        case .needs2fa: return "Код из приложения-аутентификатора или резервный код XXXXX-XXXXX"
        case .needsResetCode(let address): return "Код сброса отправлен на \(address)"
        default: return isRegister ? "Запись клиентов, задачи и ассистент — в одном месте"
                                   : "Ваша персональная сеть ИИ-агентов"
        }
    }

    @ViewBuilder
    private var form: some View {
        switch store.authState {
        case .needsEmailCode(let address):
            InkField(text: $code, placeholder: "6 цифр", label: "Код из письма", keyboard: .numberPad)
            InkButton(title: "Подтвердить", busy: store.busy) {
                store.verifyEmail(email: address, code: code)
                code = ""
            }
            HStack {
                Button("Отправить код ещё раз") { store.resendCode(email: address) }
                    .buttonStyle(InkQuietButtonStyle())
                Spacer()
                Button("Назад ко входу") { store.cancelAuthFlow() }
                    .buttonStyle(InkQuietButtonStyle(tint: Ink.secondary))
            }
        case .needs2fa:
            InkField(text: $code, placeholder: "123456", label: "Код", keyboard: .numbersAndPunctuation)
            Toggle(isOn: $trustDevice) {
                Text("Доверять устройству 90 дней").font(InkFont.callout).foregroundStyle(Ink.label)
            }
            .tint(Ink.accent)
            .inkRow()
            .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Ink.card))
            InkButton(title: "Войти", busy: store.busy) {
                store.login2fa(code: code, trustDevice: trustDevice)
                code = ""
            }
            InkButton(title: "Отмена", kind: .quiet) { store.cancelAuthFlow() }
        case .needsResetCode(let address):
            InkField(text: $code, placeholder: "Код из письма", label: "Код", keyboard: .numberPad)
            InkField(text: $newPassword, placeholder: "8+ символов, буквы и цифры", label: "Новый пароль",
                     systemImage: "lock", secure: true)
            InkButton(title: "Сменить пароль", busy: store.busy) {
                store.resetPassword(email: address, code: code, newPassword: newPassword)
                code = ""
                newPassword = ""
            }
            InkButton(title: "Назад ко входу", kind: .quiet) { store.cancelAuthFlow() }
        default:
            if isRegister { registerForm } else { loginForm }
        }
    }

    @ViewBuilder
    private var loginForm: some View {
        InkField(text: $email, placeholder: "Введите email", label: "Email", systemImage: "envelope",
                 keyboard: .emailAddress, submitLabel: .next)
        InkField(text: $password, placeholder: "Пароль", label: "Пароль", systemImage: "lock", secure: true,
                 submitLabel: .go, onSubmit: login)
        HStack {
            Spacer()
            Button("Забыли пароль?") { store.forgotPassword(email: email) }
                .buttonStyle(InkQuietButtonStyle())
        }
        InkButton(title: "Войти", busy: store.busy, action: login)
        // Вход через Google сервер пока не поддерживает: кнопка честно неактивна.
        InkButton(title: "Продолжить с Google", kind: .secondary, enabled: false) {}
        HStack(spacing: 4) {
            Text("Нет аккаунта?").font(InkFont.callout).foregroundStyle(Ink.secondary)
            Button("Создать аккаунт") { register = true }
                .buttonStyle(InkQuietButtonStyle())
        }
        .frame(maxWidth: .infinity)
    }

    @ViewBuilder
    private var registerForm: some View {
        InkField(text: $displayName, placeholder: "Имя и фамилия", label: "Полное имя")
        InkField(text: $email, placeholder: "Введите email", label: "Email", keyboard: .emailAddress)
        InkField(text: $password, placeholder: "Минимум 8 символов, буквы и цифры", label: "Пароль",
                 systemImage: showPassword ? "eye.slash" : "eye",
                 trailingLabel: showPassword ? "Скрыть пароль" : "Показать пароль",
                 onTrailing: { showPassword.toggle() }, secure: !showPassword)
        InkField(text: .constant("\(TimeZone.current.identifier) · по системе"), placeholder: "",
                 label: "Часовой пояс", systemImage: "clock")
            .disabled(true)
        InkField(text: $focus, placeholder: "например, встречи, продуктивность, код",
                 label: "С чем должен помогать ваш Selfvoy?")
        InkField(text: $refCode, placeholder: "Например, ABCD2345", label: "Код приглашения (необязательно)",
                 keyboard: .asciiCapable)
            .onChange(of: refCode) { _, value in
                let cleaned = String(value.trimmingCharacters(in: .whitespaces).uppercased().prefix(ReferralCode.length))
                if cleaned != value { refCode = cleaned }
            }
        if !refCode.isEmpty && ReferralCode.normalize(refCode) == nil {
            InkNotice(text: "Код — \(ReferralCode.length) латинских букв и цифр; с неверным кодом регистрация пройдёт без приглашения",
                      tone: .attention)
        }
        InkButton(title: "Создать аккаунт", busy: store.busy) {
            store.register(displayName: displayName.trimmingCharacters(in: .whitespaces),
                           email: email.trimmingCharacters(in: .whitespaces),
                           password: password, focus: focus, ref: refCode)
        }
        .padding(.top, 6)
        InkFootnote("Пароль хранится как Argon2id, сессия — JWT", alignment: .center)
            .frame(maxWidth: .infinity)
        HStack(spacing: 4) {
            Text("Уже есть аккаунт?").font(InkFont.callout).foregroundStyle(Ink.secondary)
            Button("Войти") { register = false }
                .buttonStyle(InkQuietButtonStyle())
        }
        .frame(maxWidth: .infinity)
    }

    @ViewBuilder
    private var serverSection: some View {
        Button {
            withAnimation { showServer.toggle() }
        } label: {
            Label(showServer ? "Скрыть адрес сервера" : "Адрес сервера", systemImage: "server.rack")
        }
        .buttonStyle(InkQuietButtonStyle(tint: Ink.secondary))
        .frame(maxWidth: .infinity)
        if showServer {
            VStack(alignment: .leading, spacing: 10) {
                InkField(text: $serverURL, placeholder: "ws:// или wss://", keyboard: .URL)
                InkButton(title: "Сохранить адрес", kind: .secondary) { store.setServerURL(serverURL) }
                InkFootnote("Сейчас: \(store.serverURLText)")
            }
        }
    }

    private func login() {
        store.login(email: email.trimmingCharacters(in: .whitespaces), password: password)
    }
}
