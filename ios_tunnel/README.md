# WDA Remote for iOS

本目录包含 iPhone 端的公网隧道、本机配对页和领取流程。它是对 Appium WebDriverAgent 的扩展，完整 Xcode 工程在编译时由固定版本的上游源码生成。

修改本目录或 iOS 工作流并推送后，GitHub Actions 的 **iOS WDA** 会运行本地逻辑测试并编译 iPhone 真机用的 Runner。也可在 **Actions → iOS WDA → Run workflow** 手动运行。

编译成功后，从运行详情的 **Artifacts → WDA-iOS-unsigned-IPA** 下载 ZIP，解压可得到 `WDA-Remote-unsigned.ipa`。IPA 内含 `Payload/WebDriverAgentRunner-Runner.app`，并补齐 iOS 16 所需的两项 XCTest 运行库。

IPA 预置 `https://wda.trxbi.com` 的配对配置，不包含签名、设备描述文件、配对码或设备凭据。用自己的 Apple 开发证书和包含目标 iPhone 的描述文件自签时，需一并签好内嵌的 `Frameworks` 和 `PlugIns/WebDriverAgentRunner.xctest`。Mac 上可用本目录的 `sign_runner.py`，默认保留包内配置；要换服务器可用 `--config` 覆盖。安装后通过 Xcode 或 go-ios 启动 XCTest，再在 iPhone Safari 打开 `http://127.0.0.1:8100/wda/claim/pairing` 或扫描网页生成的配对二维码。
