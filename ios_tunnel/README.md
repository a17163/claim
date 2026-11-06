# WDA Remote for iOS

本目录包含 iPhone 端的公网隧道、本机配对页和领取流程。它是对 Appium WebDriverAgent 的扩展，完整 Xcode 工程在编译时由固定版本的上游源码生成。

修改本目录或 iOS 工作流并推送后，GitHub Actions 的 **iOS WDA** 会运行本地逻辑测试并编译 iPhone 真机用的 Runner。也可在 **Actions → iOS WDA → Run workflow** 手动运行。

编译成功后，从运行详情的 **Artifacts → WDA-iOS-unsigned-IPA** 下载 ZIP，解压可得到 `WDA-Remote-unsigned.ipa`。IPA 内含 `Payload/WebDriverAgentRunner-Runner.app`，并补齐 iOS 16 所需的两项 XCTest 运行库。

IPA 预置 `https://wda.trxbi.com` 的配对配置，不包含签名、设备描述文件、配对码或设备凭据。用自己的 Apple 开发证书和包含目标 iPhone 的描述文件自签时，需一并签好内嵌的 `Frameworks` 和 `PlugIns/WebDriverAgentRunner.xctest`。Mac 上可用本目录的 `sign_runner.py`，默认保留包内配置；要换服务器可用 `--config` 覆盖。安装后通过 Xcode 或 go-ios 启动 XCTest，再在 iPhone Safari 打开 `http://127.0.0.1:8100/wda/claim/pairing` 或扫描网页生成的配对二维码。

## 崩溃上报（腾讯 Bugly）

wda 启动时如果包内有 `PlugIns/WebDriverAgentRunner.xctest/FBBugly.plist`，就初始化 Bugly；没有这个文件时，上报代码整体不编译进产物，行为和以前一致。SDK 从腾讯 CDN 按固定 SHA-256 拉取，是静态库，不额外嵌入动态框架，自签时不需要单独签它。

在仓库 **Settings → Secrets and variables → Actions** 添加名为 `BUGLY_APP_ID` 的 secret（Bugly 控制台分配的应用 ID）。工作流只在 secret 非空时写入 plist，构建摘要会注明是否启用。

崩溃报告会附带设备名、最近一次任务阶段、操作和原因，因此后台「待人工核对」的现场能在 Bugly 里对上。工作流同时上传 **WDA-iOS-dSYM**，把里面的 dSYM 上传到 Bugly 的符号表页面才能看到函数名和行号；重签只换签名不改二进制 UUID，符号表仍然匹配。

验证上报是否生效：装上自签包，启动 WDA，在 Bugly 的「概览/崩溃分析」确认设备在线或出现一条测试崩溃；只有真机运行过的包才会出现在列表里。

