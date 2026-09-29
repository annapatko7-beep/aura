// LoginView.swift — вход, регистрация, подтверждение email и 2FA.
import SwiftUI

struct LoginView: View {

    @EnvironmentObject private var store: AppStore

    @State private var email = ""
    @State private var password = ""
    @State private var displayName = ""
    @State private var code = ""
    @State private var trustDevice = false
    @State private var isRegistering = false

    var body: some View {
        NavigationStack {
            Form {
                Section("Сервер") {
                    TextField("ws:// или wss://", text: $store.serverURLText)
                        .keyboardType(.URL)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                }

                switch store.authState {
                case .loggedOut:
                    Section("Аккаунт") {
                        if isRegistering {
                            TextField("Имя", text: $displayName)
                        }
                        TextField("Email", text: $email)
                            .keyboardType(.emailAddress)
                            .autocorrectionDisabled()
                            .textInputAutocapitalization(.never)
                        SecureField("Пароль", text: $password)
                        Button(isRegistering ? "Зарегистрироваться" : "Войти") {
                            if isRegistering {
                                store.register(displayName: displayName, email: email, password: password)
                            } else {
                                store.login(email: email, password: password)
                            }
                        }
                        Button(isRegistering ? "Уже есть аккаунт — войти" : "Создать аккаунт") {
                            isRegistering.toggle()
                        }
                        .font(.footnote)
                    }
                case .needsEmailCode(let pendingEmail):
                    Section("Подтверждение email") {
                        if !store.devEmailCode.isEmpty {
                            Text("Код (dev-почта): \(store.devEmailCode)")
                                .font(.footnote)
                        }
                        TextField("Код из письма", text: $code)
                            .keyboardType(.numberPad)
                        Button("Подтвердить") {
                            store.verifyEmail(email: pendingEmail, code: code)
                            code = ""
                        }
                    }
                case .needs2fa(let pendingEmail, let pendingPassword):
                    Section("Двухфакторная аутентификация") {
                        TextField("Код из приложения-аутентификатора", text: $code)
                            .keyboardType(.numberPad)
                        Toggle("Доверять этому устройству", isOn: $trustDevice)
                        Button("Подтвердить вход") {
                            store.login2fa(email: pendingEmail, password: pendingPassword,
                                           code: code, trustDevice: trustDevice)
                            code = ""
                        }
                        Button("Отмена") { store.authState = .loggedOut }
                            .font(.footnote)
                    }
                case .loggedIn:
                    EmptyView()
                }

                if !store.statusMessage.isEmpty {
                    Section {
                        Text(store.statusMessage)
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .navigationTitle("Aura")
        }
    }
}
