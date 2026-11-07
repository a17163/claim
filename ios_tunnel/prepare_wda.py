"""Inject the module into an existing WDA source tree without editing pbxproj."""
import argparse
import json
import plistlib
from pathlib import Path
import shutil


def prepare(root):
    # iOS attributes the wireless-data permission to the running test bundle, so the
    # Runner must carry the same identifier as the app build that had the toggle.
    project = root / 'WebDriverAgent.xcodeproj' / 'project.pbxproj'
    source = project.read_text()
    upstream_id = 'PRODUCT_BUNDLE_IDENTIFIER = com.facebook.WebDriverAgentRunner;'
    if upstream_id in source:
        project.write_text(source.replace(
            upstream_id, 'PRODUCT_BUNDLE_IDENTIFIER = com.walletclick.wda.s4rnp6ldd6;'))
    runner = root / 'WebDriverAgentRunner' / 'UITestingUITests.m'
    original = runner.read_text()
    if '#import "FBRemoteTunnel.m"' not in original:
        hook = '  [webServer startServing];'
        if original.count(hook) != 1:
            raise ValueError('WDA 入口已改变，拒绝自动修改')
        updated = original.replace('#import <XCTest/XCTest.h>',
                                  '#import <XCTest/XCTest.h>\n#import "FBRemoteTunnel.m"\nextern void FBBuglyStart(void);')
        updated = updated.replace(hook, '  FBRemoteTunnel *tunnel = [FBRemoteTunnel new];\n'
                                  '  [tunnel start];\n'
                                  '  @try { [webServer startServing]; }\n'
                                  '  @finally { [tunnel stop]; }')
        runner.with_suffix('.m.before-tunnel').write_text(original)
        runner.write_text(updated)
    # Bugly and its claim context must live only in WebDriverAgentLib. Linking
    # the static SDK into both binaries creates duplicate Objective-C classes.
    source = runner.read_text().replace('#import "FBBugly.inc"', 'extern void FBBuglyStart(void);')
    if 'extern void FBBuglyStart(void);' not in source:
        source = source.replace('#import "FBRemoteTunnel.m"',
                                '#import "FBRemoteTunnel.m"\nextern void FBBuglyStart(void);')
    if 'FBBuglyStart();' not in source:
        marker = '  FBRemoteTunnel *tunnel = [FBRemoteTunnel new];'
        if source.count(marker) != 1 or 'extern void FBBuglyStart(void);' not in source:
            raise ValueError('WDA 崩溃上报入口已改变，拒绝自动修改')
        source = source.replace(marker, '  FBBuglyStart();\n' + marker)
    runner.write_text(source)
    shutil.copy2(Path(__file__).with_name('FBRemoteTunnel.m'), runner.parent)
    for filename in ('FBPairKey.inc', 'FBClaimHistory.inc', 'FBPairEvents.inc', 'FBBugly.inc'):
        shutil.copy2(Path(__file__).with_name(filename), runner.parent)
    touch = root / 'WebDriverAgentLib' / 'Commands' / 'FBTouchActionCommands.m'
    source = touch.read_text()
    if '#import "FBRemoteGesture.inc"' not in source:
        marker = '    [[FBRoute POST:@"/actions"] respondWithTarget:self action:@selector(handlePerformW3CTouchActions:)],'
        if source.count(marker) != 1 or source.count('@end') != 1:
            raise ValueError('WDA 手势入口已改变，拒绝自动修改')
        touch.with_suffix('.m.before-tunnel').write_text(source)
        source = source.replace('@implementation FBTouchActionCommands',
            '#if !TARGET_OS_TV && !TARGET_OS_WATCH\n#import "FBMathUtils.h"\n#import "XCUIApplication.h"\n'
            '#import "XCUIDevice+FBHelpers.h"\n#import "XCUIElement+FBWebDriverAttributes.h"\n#endif\n\n'
            '@implementation FBTouchActionCommands')
        source = source.replace(marker, marker + '\n#if !TARGET_OS_TV && !TARGET_OS_WATCH\n'
            '    [[FBRoute GET:@"/wda/remote/geometry"] respondWithTarget:self action:@selector(handleRemoteGeometry:)],\n'
            '    [[FBRoute POST:@"/wda/remote/actions"] respondWithTarget:self action:@selector(handleRemoteGesture:)],\n#endif')
        touch.write_text(source.replace('@end', '#import "FBRemoteGesture.inc"\n\n@end'))
    shutil.copy2(Path(__file__).with_name('FBRemoteGesture.inc'), touch.parent)
    source = touch.read_text()
    if '#import "FBPhoneClaim.inc"' not in source:
        source = source.replace('@implementation FBTouchActionCommands',
                                '#import "FBPhoneClaim.inc"\n\n@implementation FBTouchActionCommands')
        marker = '    [[FBRoute POST:@"/actions"] respondWithTarget:self action:@selector(handlePerformW3CTouchActions:)],'
        source = source.replace(marker, marker + '\n#if TARGET_OS_IOS\n'
            '    [[FBRoute GET:@"/wda/claim/status"].withoutSession respondWithTarget:self action:@selector(handlePhoneClaimStatus:)],\n'
            '    [[FBRoute POST:@"/wda/claim/:op"].withoutSession respondWithTarget:self action:@selector(handlePhoneClaimCommand:)],\n#endif')
        source = source.replace('@end', '#if TARGET_OS_IOS\n'
            '+ (id<FBResponsePayload>)handlePhoneClaimStatus:(FBRouteRequest *)request\n'
            '{ return FBResponseWithObject(FBClaimStatus()); }\n\n'
            '+ (id<FBResponsePayload>)handlePhoneClaimCommand:(FBRouteRequest *)request\n'
            '{ return FBResponseWithObject(FBClaimCommand(request.parameters[@"op"] ?: @"", request.arguments)); }\n'
            '#endif\n\n@end')
        touch.write_text(source)
    for filename in ('FBClaimLoop.inc', 'FBPhoneClaim.inc', 'FBPairKey.inc', 'FBClaimHistory.inc', 'FBBugly.inc'):
        shutil.copy2(Path(__file__).with_name(filename), touch.parent)
    profile = json.loads(Path(__file__).with_name('tokenpocket-claim.json').read_text(encoding='utf-8'))
    literal = json.dumps(json.dumps(profile, ensure_ascii=True, separators=(',', ':')))
    (touch.parent / 'FBClaimBundledProfile.inc').write_text(
        f'static NSString *const FBClaimBundledProfileJSON = @{literal};\n', encoding='utf-8')

    web_server = root / 'WebDriverAgentLib' / 'Routing' / 'FBWebServer.m'
    source = web_server.read_text()
    if 'FBPhoneClaimBusy' not in source:
        marker = '        [FBLogger verboseLog:routeParams.description];'
        if source.count(marker) != 1:
            raise ValueError('WDA 请求入口已改变，拒绝自动修改')
        web_server.with_suffix('.m.before-tunnel').write_text(source)
        source = source.replace('@interface FBWebServer ()',
                                '#if TARGET_OS_IOS\nextern BOOL FBPhoneClaimBusy(void);\n#endif\n\n@interface FBWebServer ()')
        source = source.replace(marker, '#if TARGET_OS_IOS\n'
            '        NSString *path = request.url.path;\n'
            '        if (FBPhoneClaimBusy() && ![@[@"/status", @"/wda/claim/status", @"/wda/claim/stop"] containsObject:path ?: @""]) {\n'
            '          [FBResponseWithStatus([FBCommandStatus invalidElementStateErrorWithMessage:@"Phone claim task is active" traceback:nil]) dispatchWithResponse:response];\n'
            '          return;\n        }\n'
            '        if (![path hasPrefix:@"/wda/claim/"])\n#endif\n'
            + marker)
        web_server.write_text(source)

    source = web_server.read_text()
    if '#import "FBPhonePairPage.inc"' not in source:
        declaration = 'extern BOOL FBPhoneClaimBusy(void);'
        handler = '- (void)registerRouteHandlers:(NSArray *)commandHandlerClasses\n{'
        if source.count(declaration) != 1 or source.count(handler) != 1:
            raise ValueError('WDA 本机配对页入口已改变，拒绝自动修改')
        source = source.replace(declaration, declaration + '\n#import "FBPhonePairPage.inc"')
        source = source.replace(handler, handler + '''
#if TARGET_OS_IOS
  [self.server handleMethod:@"GET" withPath:FBPhonePairPath standalone:NO block:^(RouteRequest *request, RouteResponse *response) {
    FBPhonePairPage(request, response, NO);
  }];
  [self.server handleMethod:@"POST" withPath:FBPhonePairPath standalone:NO block:^(RouteRequest *request, RouteResponse *response) {
    FBPhonePairPage(request, response, YES);
  }];
#endif''')
        web_server.write_text(source)
    source = web_server.read_text()
    if 'FBPhoneClaimRecoverHistory' not in source:
        source = source.replace('extern BOOL FBPhoneClaimBusy(void);',
                                'extern BOOL FBPhoneClaimBusy(void);\nextern void FBPhoneClaimRecoverHistory(void);')
        source = source.replace('  [self.server handleMethod:@"GET" withPath:FBPhonePairPath',
                                '  FBPhoneClaimRecoverHistory();\n  [self.server handleMethod:@"GET" withPath:FBPhonePairPath')
        web_server.write_text(source)
    # Keep pairing state on the main route queue; do not coalesce submitted codes.
    source = web_server.read_text()
    updated = source.replace('withPath:FBPhonePairPath standalone:YES', 'withPath:FBPhonePairPath standalone:NO')
    if updated != source:
        web_server.write_text(updated)
    for filename in ('FBPhonePairPage.inc', 'FBPairEvents.inc'):
        shutil.copy2(Path(__file__).with_name(filename), web_server.parent)

    http_server = web_server.with_name('FBHTTPServer.m')
    source = http_server.read_text()
    if 'WalletClick pairing host guard' not in source:
        marker = '  FBPendingHTTPRequestHeader *pending = [FBPendingHTTPRequestHeader new];'
        if source.count(marker) != 1:
            raise ValueError('WDA HTTP 请求解析入口已改变，拒绝自动修改')
        source = source.replace(marker, '''#if TARGET_OS_IOS
  // WalletClick pairing host guard: reject DNS rebinding and cross-origin submissions.
  NSURLComponents *pairTarget = [NSURLComponents componentsWithString:requestLineParts[1]];
  if ([pairTarget.path isEqualToString:@"/wda/claim/pairing"]) {
    NSString *origin = requestHeaders[@"origin"];
    if (![requestHeaders[@"host"] isEqualToString:@"127.0.0.1:8100"] ||
        (origin && ![origin isEqualToString:@"http://127.0.0.1:8100"])) {
      [self respondBadRequestToClient:client];
      return nil;
    }
  }
#endif

''' + marker)
        http_server.write_text(source)

    config = root / 'WebDriverAgentLib' / 'Utilities' / 'FBConfiguration.m'
    source = config.read_text()
    marker = '- (NSString *)bindingIPAddress\n{'
    if 'WalletClick keeps WDA on-device' not in source:
        if source.count(marker) != 1:
            raise ValueError('WDA 监听配置入口已改变，拒绝自动修改')
        config.with_suffix('.m.before-tunnel').write_text(source)
        source = source.replace(marker, marker + '\n  // WalletClick keeps WDA on-device; USB forwarding and authenticated WSS still work.\n'
            '#if TARGET_OS_IOS\n  return @"127.0.0.1";\n#endif')
        config.write_text(source)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wda-source', required=True, type=Path)
    parser.add_argument('--test-bundle', type=Path,
                        help='构建后将配置放进 WebDriverAgentRunner.xctest；随后必须重新签名')
    parser.add_argument('--config', type=Path, help='本机私密 plist；不提供时隧道默认关闭')
    args = parser.parse_args()
    prepare(args.wda_source)
    if args.test_bundle:
        if not args.test_bundle.is_dir() or args.test_bundle.suffix != '.xctest':
            parser.error('test-bundle 须为构建后的 .xctest 目录')
        if args.config:
            config = plistlib.loads(args.config.read_bytes())
            if not isinstance(config, dict):
                parser.error('配置须为 plist 字典')
        else:
            config = {'Enabled': False}
        destination = args.test_bundle / 'FBRemoteTunnel.plist'
        destination.write_bytes(plistlib.dumps(config))
        destination.chmod(0o600)
    print('WDA 融合源码准备完成；配置注入后需要重新签名')


if __name__ == '__main__':
    main()
