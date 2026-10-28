// swift-tools-version: 5.9
// SelfvoyKit — общий слой iOS-клиента Selfvoy: WS-транспорт, протокол, модели.
// macOS-платформа добавлена, чтобы тесты пакета гонялись `swift test` без симулятора.
import PackageDescription

let package = Package(
    name: "SelfvoyKit",
    platforms: [
        .iOS(.v17),
        .macOS(.v14)
    ],
    products: [
        .library(name: "SelfvoyKit", targets: ["SelfvoyKit"])
    ],
    targets: [
        .target(name: "SelfvoyKit"),
        .testTarget(name: "SelfvoyKitTests", dependencies: ["SelfvoyKit"])
    ]
)
