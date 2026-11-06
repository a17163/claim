# 构建与验证记录

验证日期：2026-11-03。构建环境为 macOS arm64、Temurin JDK 17、Gradle 8.11.1、AGP 8.9.2、Android SDK 35 / Build Tools 35.0.0。

## 已完成

- 所有 Android Java 源码通过 SDK 35 编译。
- 纯 Java `FlowChecks` 的 **35 项检查**通过；同一组检查也通过 Gradle JUnit 测试（1 个测试入口，0 失败）。
- `testDebugUnitTest`、`assembleDebug`、`lintDebug` 成功。
- Lint：**0 错误、6 警告**。剩余提示为旧系统忽略 `isAccessibilityTool` 属性（旧系统默认同样为 false），以及当前中文界面的字符串国际化建议。
- APK 已生成并使用 Android 构建工具的调试密钥签名，APK Signature Scheme v2 校验通过。
- APK 权限清单没有联网权限。仅声明由系统保护的无障碍服务绑定权限，不提供外部启动自动任务的接口。
- 已禁用应用备份，并配置 Android 12+ 云备份与设备迁移排除规则。

构建时 Google Maven 的一个 Lint JAR 发生 TLS 下载中断；从相同官方地址补下原始 JAR，通过临时本地 Maven 仓库完成离线静态检查。此环境处理未写入工程依赖配置。正常环境直接使用项目中的 Gradle Wrapper 即可。

## 尚未验证

没有连接安卓真机或模拟器，因此以下仍需在目标手机验收：

- 无障碍服务授权、悬浮栏显示、节点采集及真实界面布局。
- 目标钱包的费用节点、密码输入及提交成功提示。
- Android Keystore 在目标设备上的实际保存、读取、删除。
- 厂商后台管理、锁屏/切后台暂停及服务销毁行为。

本次未宣称任何具体钱包、机型或系统版本已经真机兼容。测试不发送真实交易。APK 为自行安装测试版，未做商店发布。
