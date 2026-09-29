// swift-tools-version: 5.9
// AuraKit — общий слой iOS-клиента Aura: WS-транспорт, протокол, модели.
// macOS-платформа добавлена, чтобы тесты пакета гонялись `swift test` без симулятора.
import PackageDescription

let package = Package(
    name: "AuraKit",
    platforms: [
        .iOS(.v17),
        .macOS(.v14)
    ],
    products: [
        .library(name: "AuraKit", targets: ["AuraKit"])
    ],
    targets: [
        .target(name: "AuraKit"),
        .testTarget(name: "AuraKitTests", dependencies: ["AuraKit"])
    ]
)
