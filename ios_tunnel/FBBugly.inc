// Tencent Bugly crash reporting. Both entry points are no-ops unless FBBugly.plist supplies an App ID.
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#if __has_include(<Bugly/Bugly.h>)
#import <Bugly/Bugly.h>
#define FB_BUGLY_LINKED 1
#endif

static NSString *FBBuglyClaimState = @"";
static NSString *FBBuglyClaimReason = @"";
static NSString *FBBuglyClaimAction = @"";

void FBBuglySetClaimState(NSString *state, NSString *reason, NSString *action)
{
  FBBuglyClaimState = [state isKindOfClass:NSString.class] ? state : @"";
  FBBuglyClaimReason = [reason isKindOfClass:NSString.class] ? reason : @"";
  FBBuglyClaimAction = [action isKindOfClass:NSString.class] ? action : @"";
}

#ifdef FB_BUGLY_LINKED

// Same bundle layout the claim code reads its own configuration from.
static NSDictionary *FBBuglyConfiguration(void)
{
  NSURL *path = [[NSBundle.mainBundle.builtInPlugInsURL
                  URLByAppendingPathComponent:@"WebDriverAgentRunner.xctest"]
                 URLByAppendingPathComponent:@"FBBugly.plist"];
  NSDictionary *config = path ? [NSDictionary dictionaryWithContentsOfURL:path] : nil;
  return [config isKindOfClass:NSDictionary.class] ? config : nil;
}

// Reported with every crash so a report names the phone and the step that stopped.
@interface FBBuglyContext : NSObject <BuglyDelegate>
@property (nonatomic, copy) NSString *deviceName;
@end

@implementation FBBuglyContext
- (NSString *)attachmentForException:(NSException *)exception
{
  NSMutableArray<NSString *> *parts = [NSMutableArray array];
  if (self.deviceName.length) [parts addObject:[@"设备：" stringByAppendingString:self.deviceName]];
  [parts addObject:[@"阶段：" stringByAppendingString:(FBBuglyClaimState.length ? FBBuglyClaimState : @"未启动")]];
  if (FBBuglyClaimAction.length) [parts addObject:[@"操作：" stringByAppendingString:FBBuglyClaimAction]];
  if (FBBuglyClaimReason.length) [parts addObject:[@"原因：" stringByAppendingString:FBBuglyClaimReason]];
  if (exception.name.length) [parts addObject:[@"异常：" stringByAppendingString:exception.name]];
  return [parts componentsJoinedByString:@" | "];
}
@end

static FBBuglyContext *FBBuglyContextHolder;

void FBBuglyStart(void)
{
  @try {
    NSDictionary *settings = FBBuglyConfiguration();
    NSString *appId = [settings[@"AppId"] isKindOfClass:NSString.class] ? settings[@"AppId"] : nil;
    if (!appId.length) return;

    UIDevice *device = UIDevice.currentDevice;
    NSString *vendor = device.identifierForVendor.UUIDString ?: @"";
    NSDictionary *info = NSBundle.mainBundle.infoDictionary ?: @{};
    NSString *build = [info[@"CFBundleVersion"] isKindOfClass:NSString.class] ? info[@"CFBundleVersion"] : @"1";
    NSString *channel = [settings[@"Channel"] isKindOfClass:NSString.class] ? settings[@"Channel"] : @"wda-ios";

    FBBuglyContextHolder = [FBBuglyContext new];
    FBBuglyContextHolder.deviceName = [settings[@"Device"] isKindOfClass:NSString.class] ? settings[@"Device"] : @"";

    BuglyConfig *config = [BuglyConfig new];
    config.version = build;
    config.channel = channel;
    config.deviceIdentifier = vendor;
    config.debugMode = [settings[@"Debug"] boolValue];
    // Watchdog and memory kills raise no exception, so ask for termination reports too.
    config.unexpectedTerminatingDetectionEnable = YES;
    config.symbolicateInProcessEnable = YES;
    config.delegate = FBBuglyContextHolder;

    [Bugly startWithAppId:appId config:config];
    if (vendor.length) [Bugly setUserIdentifier:vendor];
    [Bugly setUserValue:build forKey:@"wda_build"];
    [Bugly setUserValue:device.systemVersion ?: @"" forKey:@"wda_ios"];
    [Bugly setUserValue:device.model ?: @"" forKey:@"wda_model"];
  } @catch (NSException *error) {
    // Reporting must never stop the automation server from starting.
    NSLog(@"[Bugly] start failed: %@", error.name);
  }
}

#else

void FBBuglyStart(void) {}

#endif
