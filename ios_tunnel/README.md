# WDA Remote for iOS

本目录包含 iPhone 端的公网隧道、本机配对页和领取流程。它是对 Appium WebDriverAgent 的扩展，完整 Xcode 工程在编译时由固定版本的上游源码生成。

修改本目录或 iOS 工作流并推送后，GitHub Actions 的 **iOS WDA** 会运行本地逻辑测试并编译 iPhone 真机用的 Runner。也可在 **Actions → iOS WDA → Run workflow** 手动运行。

编译成功后，从运行详情的 **Artifacts → WDA-iOS-unsigned** 下载产物。里面的 `WDA-Runner-unsigned.zip` 包含 `WebDriverAgentRunner-Runner.app`。产物未签名，且不含中继凭据或配对配置。安装前需用自己的 Apple 开发签名和设备描述文件在 Mac 上签名。需要注入私密配置时，请在签名前使用 `prepare_wda.py --test-bundle … --config …` 在本机处理。
