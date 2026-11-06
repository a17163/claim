#if TARGET_OS_IOS
#import "FBClaimLoop.inc"
#import "FBClaimHistory.inc"
#import "FBBugly.inc"
#import "FBPairKey.inc"
#import "XCUIApplication+FBHelpers.h"
#import "XCUIElement+FBClassChain.h"
#import "XCUIElement+FBFind.h"
#import "XCUIElement+FBTyping.h"
#import "XCUIDevice.h"
#import <Security/Security.h>
#import <Vision/Vision.h>
#import <ImageIO/ImageIO.h>

static NSString *const FBClaimProfileKey = @"WalletClick.phone.profile.v1";
static NSString *const FBClaimStagesKey = @"WalletClick.phone.stages.v1";
static FBClaimLoop *FBClaimJob;
static BOOL FBClaimInspecting;
static NSString *FBClaimVerifyFee;
static NSTimeInterval FBClaimVerifyAt;
static NSString *FBClaimStartNonce;
static NSTimeInterval FBClaimStartNonceAt;

BOOL FBPhoneClaimBusy(void)
{
  return FBClaimInspecting || FBClaimJob.running;
}

static NSString *FBClaimDeviceID(void)
{
  return UIDevice.currentDevice.identifierForVendor.UUIDString ?: @"";
}

static NSDictionary *FBClaimStages(void)
{
  return @{@"home": @[@"home", @"claim"], @"fee": @[@"fee_ready", @"cancel", @"confirm"],
           @"password": @[@"password", @"submit"], @"success": @[@"success"]};
}

static NSString *FBClaimAccess(void)
{
  static NSString *token;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSURL *plugins = NSBundle.mainBundle.builtInPlugInsURL;
    NSURL *path = [[plugins URLByAppendingPathComponent:@"WebDriverAgentRunner.xctest"]
                   URLByAppendingPathComponent:@"FBRemoteTunnel.plist"];
    NSDictionary *config = path ? [NSDictionary dictionaryWithContentsOfURL:path ?: [NSURL fileURLWithPath:@"/nonexistent"]] : nil;
    id value = config[@"ClaimToken"];
    if ([value isKindOfClass:NSString.class] && [value length] >= 32) token = value;
  });
  return token ?: FBPairCredentials()[@"access"];
}

static BOOL FBClaimPending(void);
static NSString *FBClaimRunID;
static NSURL *FBClaimRunJournal;
static NSTimeInterval FBClaimRunStartedAt, FBClaimRunUptime, FBClaimNextCheckpoint;
static NSUInteger FBClaimRevision;
static BOOL FBClaimHistorySaved;

void FBPhoneClaimRecoverHistory(void)
{
  static NSURL *recovered;
  NSURL *url = FBClaimHistoryURL(FBClaimAccess());
  if (url && ![url isEqual:recovered] && FBClaimHistoryUpdate(url, @{@"requires_review": @(FBClaimPending())}, @"recover")) recovered = url;
}

static NSDictionary *FBClaimRunRecord(FBClaimLoop *job, BOOL checkpoint)
{
  NSTimeInterval now = NSDate.date.timeIntervalSince1970;
  NSTimeInterval duration = MAX(0, NSProcessInfo.processInfo.systemUptime - FBClaimRunUptime);
  NSMutableDictionary *record = [@{@"id": FBClaimRunID, @"revision": @(FBClaimRevision + 1),
    @"started_at": @(FBClaimRunStartedAt), @"observed_at": @(now), @"duration_seconds": @((NSUInteger)duration),
    @"running": @(job.running), @"state": job.running ? @"运行中" : job.state,
    @"step": (job.running ? job.state : job.endStep) ?: @"", @"action": job.action ?: @"",
    @"reason": job.reason ?: @"", @"attempts": @(job.attempts), @"successes": @(job.successes),
    @"fee": job.fee ?: @"", @"requires_review": @(FBClaimPending())} mutableCopy];
  if (!job.running && checkpoint) record[@"ended_at"] = @(now);
  return record;
}

static NSDictionary *FBClaimLastRecord;

static void FBClaimCheckpoint(FBClaimLoop *job)
{
  FBBuglySetClaimState(job.running ? job.state : job.endStep, job.reason, job.action);
  FBClaimLastRecord = FBClaimRunRecord(job, YES);
  FBClaimHistorySaved = FBClaimHistoryUpdate(FBClaimRunJournal, FBClaimLastRecord, @"record");
  if (FBClaimHistorySaved) FBClaimRevision++;
  FBClaimNextCheckpoint = NSProcessInfo.processInfo.systemUptime + 15;
}

static BOOL FBClaimAuthorized(id supplied)
{
  if (![supplied isKindOfClass:NSString.class]) return NO;
  NSData *secret = [FBClaimAccess() dataUsingEncoding:NSUTF8StringEncoding];
  NSData *candidate = [supplied dataUsingEncoding:NSUTF8StringEncoding];
  if (!secret.length || secret.length != candidate.length) return NO;
  const uint8_t *a = (const uint8_t *)secret.bytes, *b = (const uint8_t *)candidate.bytes;
  uint8_t difference = 0;
  for (NSUInteger i = 0; i < secret.length; i++) difference |= a[i] ^ b[i];
  return difference == 0;
}

static NSURL *FBClaimPendingPath(void)
{
  NSURL *dir = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
  return [dir URLByAppendingPathComponent:@"wallet-claim-pending"];
}

static BOOL FBClaimPending(void)
{
  NSString *path = FBClaimPendingPath().path;
  return path && [NSFileManager.defaultManager fileExistsAtPath:path];
}

static void FBClaimMark(void)
{
  NSURL *path = FBClaimPendingPath();
  NSURL *dir = path.URLByDeletingLastPathComponent;
  if (!path || !dir || ![NSFileManager.defaultManager createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:nil]
      || ![[NSData dataWithBytes:"pending" length:7] writeToURL:path options:NSDataWritingAtomic error:nil]) {
    FBClaimPause(@"无法保存待核对标记，未确认交易");
  }
}

static void FBClaimClear(void)
{
  NSURL *path = FBClaimPendingPath();
  if (FBClaimPending() && (!path || ![NSFileManager.defaultManager removeItemAtURL:path error:nil])) {
    FBClaimPause(@"待核对标记无法清除，请人工检查");
  }
}

static NSMutableDictionary *FBClaimCredentialQuery(NSString *bundle)
{
  return [@{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
           (__bridge id)kSecAttrService: @"WalletClick.PhoneClaim.v1", (__bridge id)kSecAttrAccount: bundle} mutableCopy];
}

static NSString *FBClaimFreshNonce(void)
{
  NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
  if (!FBClaimStartNonce || now - FBClaimStartNonceAt > 60) {
    uint8_t bytes[16];
    if (SecRandomCopyBytes(kSecRandomDefault, sizeof(bytes), bytes) != errSecSuccess) return nil;
    FBClaimStartNonce = [[NSData dataWithBytes:bytes length:sizeof(bytes)] base64EncodedStringWithOptions:0];
    FBClaimStartNonceAt = now;
  }
  return FBClaimStartNonce;
}

static NSDictionary *FBClaimStartInput(id sealed)
{
  if (![sealed isKindOfClass:NSString.class] || [sealed length] != 684) FBClaimPause(@"启动时须填写密码、成功间隔和领取次数");
  NSData *cipher = [[NSData alloc] initWithBase64EncodedString:sealed options:0];
  if (cipher.length != 512) FBClaimPause(@"启动参数无效");
  SecKeyRef secret = FBClaimStartKey(NO);
  if (!secret) FBClaimPause(@"请先通过 USB 配对启动密钥");
  NSData *clear = CFBridgingRelease(SecKeyCreateDecryptedData(secret, kSecKeyAlgorithmRSAEncryptionOAEPSHA256, (__bridge CFDataRef)cipher, NULL));
  CFRelease(secret);
  id data = clear ? [NSJSONSerialization JSONObjectWithData:clear options:0 error:nil] : nil;
  if (![data isKindOfClass:NSDictionary.class]) FBClaimPause(@"无法解密启动参数，请检查手机配对");
  if (!FBClaimStartNonce || NSProcessInfo.processInfo.systemUptime - FBClaimStartNonceAt > 60 ||
      ![data[@"nonce"] isEqual:FBClaimStartNonce]) FBClaimPause(@"启动请求过期或已使用，请重新读取状态");
  FBClaimStartNonce = nil;
  id password = data[@"password"], seconds = data[@"success_interval"], limit = data[@"limit"];
  id delay = data[@"start_delay"] ?: @0;
  if (![password isKindOfClass:NSString.class] || ![password length] || [password length] > 256 ||
      [password rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound) FBClaimPause(@"密码格式无效");
  if (![seconds isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)seconds) == CFBooleanGetTypeID() ||
      !isfinite([seconds doubleValue]) || [seconds doubleValue] <= 0) FBClaimPause(@"成功间隔须为正数");
  if (![limit isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)limit) == CFBooleanGetTypeID() ||
      !isfinite([limit doubleValue]) || [limit doubleValue] < 0 || [limit doubleValue] > 1000000 ||
      floor([limit doubleValue]) != [limit doubleValue]) FBClaimPause(@"领取次数须为非负整数");
  if (![delay isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)delay) == CFBooleanGetTypeID() ||
      !isfinite([delay doubleValue]) || [delay doubleValue] < 0 || [delay doubleValue] > 86400 ||
      floor([delay doubleValue]) != [delay doubleValue]) FBClaimPause(@"启动延迟须为 0–86400 秒的整数");
  return data;
}

static NSDictionary *FBClaimProfile(id input, BOOL complete)
{
  if (![input isKindOfClass:NSDictionary.class]) FBClaimPause(@"配置格式无效");
  NSString *bundle = input[@"bundle_id"], *mode = input[@"fee_mode"] ?: @"element";
  NSString *attribute = input[@"fee_attribute"] ?: @"label";
  NSString *successMode = input[@"success_mode"] ?: @"element";
  if (![@[@"element", @"password_dismissed"] containsObject:successMode]) FBClaimPause(@"提交结果判定方式无效");
  BOOL countDismissal = [successMode isEqual:@"password_dismissed"];
  NSString *device = input[@"device_uuid"];
  if (![bundle isKindOfClass:NSString.class] || bundle.length > 200 ||
      [bundle rangeOfString:@"\\A[A-Za-z0-9_-]+(?:\\.[A-Za-z0-9_-]+)+\\z" options:NSRegularExpressionSearch].location == NSNotFound) {
    FBClaimPause(@"需要真实钱包 Bundle ID");
  }
  NSString *current = FBClaimDeviceID();
  if (!current.length || (device && (![device isKindOfClass:NSString.class] || ![device isEqual:current]))) FBClaimPause(@"配置不属于当前设备");
  if (![@[@"element", @"ocr"] containsObject:mode] || ![@[@"name", @"label", @"value"] containsObject:attribute]) FBClaimPause(@"费用读取配置无效");
  id selectors = input[@"selectors"];
  if (![selectors isKindOfClass:NSDictionary.class]) FBClaimPause(@"定位规则无效");
  NSMutableDictionary *rules = [NSMutableDictionary dictionary];
  NSArray *names = @[@"home", @"claim", @"fee_ready", @"cancel", @"confirm", @"password", @"password_cancel", @"submit", @"success", @"fee", @"error"];
  for (NSString *name in names) {
    id rule = selectors[name];
    if ((!rule || [rule isEqual:@""]) && (([name isEqual:@"error"] && (!complete || !countDismissal)) ||
        [name isEqual:@"password_cancel"] || ([name isEqual:@"success"] && countDismissal) || ([name isEqual:@"fee"] && [mode isEqual:@"ocr"]) ||
        (!complete && ![@[@"home", @"claim"] containsObject:name]))) continue;
    if (![rule isKindOfClass:NSString.class] || ![rule length] || [rule length] > 2048) FBClaimPause(@"定位规则不完整");
    if ([rule hasPrefix:@"chain:"]) {
      if ([rule length] <= 6) FBClaimPause(@"层级定位规则为空");
    } else {
      [NSPredicate predicateWithFormat:rule];
    }
    rules[name] = rule;
  }
  for (NSString *cancel in @[@"cancel", @"password_cancel"]) {
    if ([rules[@"confirm"] isEqual:rules[cancel]] || [rules[@"submit"] isEqual:rules[cancel]]) {
      FBClaimPause(@"确认和取消不能使用同一定位规则");
    }
  }
  id interval = input[@"interval"] ?: @1, limit = input[@"limit"] ?: @1, timeout = input[@"timeout"] ?: @20;
  for (id value in @[interval, limit, timeout]) {
    if (![value isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() || !isfinite([value doubleValue])) FBClaimPause(@"运行参数无效");
  }
  if ([interval doubleValue] <= 0 || [limit doubleValue] < 0 || [limit doubleValue] > 1000000 ||
      floor([limit doubleValue]) != [limit doubleValue] || [timeout doubleValue] < 1 || [timeout doubleValue] > 300) FBClaimPause(@"运行参数超出范围");
  NSArray *region = @[];
  if ([mode isEqual:@"ocr"]) {
    id crop = input[@"ocr_region"];
    if (![crop isKindOfClass:NSArray.class] || [crop count] != 4) FBClaimPause(@"OCR 需要四个原始截图像素参数");
    for (id n in crop) {
      if (![n isKindOfClass:NSNumber.class] || !isfinite([n doubleValue]) || [n doubleValue] < 0 ||
          [n doubleValue] > 20000 || floor([n doubleValue]) != [n doubleValue]) FBClaimPause(@"OCR 区域无效");
    }
    if ([crop[2] integerValue] <= 0 || [crop[3] integerValue] <= 0) FBClaimPause(@"OCR 区域无效");
    region = crop;
  }
  return @{@"bundle_id": bundle, @"device_uuid": current, @"selectors": rules, @"fee_mode": mode,
           @"fee_attribute": attribute, @"success_mode": successMode, @"ocr_region": region, @"interval": interval, @"limit": limit, @"timeout": timeout};
}

#import "FBClaimBundledProfile.inc"

static NSDictionary *FBClaimBundledProfile(void)
{
  static NSDictionary *profile;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSData *data = [FBClaimBundledProfileJSON dataUsingEncoding:NSUTF8StringEncoding];
    profile = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  });
  return profile;
}

static XCUIApplication *FBClaimFront(NSDictionary *profile)
{
  if (FBClaimJob.running && FBClaimJob.stopRequested) FBClaimPause(@"收到停止请求");
  if (XCUIDevice.sharedDevice.fb_isScreenLocked) FBClaimPause(@"手机已锁屏");
  XCUIApplication *app = XCUIApplication.fb_activeApplication;
  if (![app.bundleID isEqual:profile[@"bundle_id"]]) FBClaimPause(@"目标钱包不在前台，请手动打开领取网页");
  return app;
}

static XCUIElement *FBClaimElement(NSDictionary *profile, NSString *key, BOOL required)
{
  XCUIApplication *app = FBClaimFront(profile);
  NSString *rule = profile[@"selectors"][key];
  // TokenPocket exposes an Image for the fee close and a distinct Button for password close.
  if (!rule.length && [key isEqual:@"password_cancel"] && [profile[@"bundle_id"] isEqual:@"com.global.wallet.ios"]) {
    rule = @"type == 'XCUIElementTypeButton' AND (name == 'icon_close20' OR label == 'icon_close20')";
  }
  if (!rule.length && !required) return nil;
  if (!rule.length) FBClaimPause(@"缺少定位规则");
  NSArray *items = [rule hasPrefix:@"chain:"]
    ? [app fb_descendantsMatchingClassChain:[rule substringFromIndex:6] shouldReturnAfterFirstMatch:NO]
    : [app fb_descendantsMatchingPredicate:[NSPredicate predicateWithFormat:rule] shouldReturnAfterFirstMatch:NO];
  XCUIElement *match = nil;
  for (XCUIElement *item in items) {
    if (!item.isWDVisible) continue;
    if (match) FBClaimPause([@"定位匹配多个可见元素：" stringByAppendingString:key]);
    match = item;
  }
  if (!match && required) FBClaimPause([@"定位不可见：" stringByAppendingString:key]);
  return match;
}

static NSString *FBClaimOCR(NSDictionary *profile)
{
  XCUIApplication *app = FBClaimFront(profile);
  UIInterfaceOrientation orientation = app.interfaceOrientation;
  NSData *data = [XCUIDevice.sharedDevice fb_screenshotWithError:nil];
  if (!data.length) return nil;
  CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
  if (!source) return nil;
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
  CFRelease(source);
  if (!image) return nil;
  NSArray *r = profile[@"ocr_region"];
  CGRect rect = CGRectMake([r[0] doubleValue], [r[1] doubleValue], [r[2] doubleValue], [r[3] doubleValue]);
  CGImageRef crop = CGRectGetMaxX(rect) <= CGImageGetWidth(image) && CGRectGetMaxY(rect) <= CGImageGetHeight(image)
    ? CGImageCreateWithImageInRect(image, rect) : NULL;
  CGImageRelease(image);
  if (!crop) return nil;
  VNRecognizeTextRequest *request = [VNRecognizeTextRequest new];
  request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
  request.usesLanguageCorrection = NO;
  VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:crop options:@{}];
  BOOL ok = [handler performRequests:@[request] error:nil];
  CGImageRelease(crop);
  if (!ok || request.results.count != 1 || FBClaimFront(profile).interfaceOrientation != orientation) return nil;
  VNRecognizedText *text = [[request.results.firstObject topCandidates:1] firstObject];
  return text.confidence >= 0.85 ? text.string : nil;
}

static NSString *FBClaimFee(NSDictionary *profile)
{
  XCUIElement *element = FBClaimElement(profile, @"fee", [profile[@"fee_mode"] isEqual:@"element"]);
  if (element) return [element fb_valueForWDAttributeName:profile[@"fee_attribute"]];
  return FBClaimOCR(profile);
}

static void FBClaimHome(NSDictionary *profile)
{
  FBClaimElement(profile, @"home", YES);
  if (FBClaimElement(profile, @"fee_ready", NO) || FBClaimElement(profile, @"password", NO) ||
      FBClaimElement(profile, @"success", NO) || FBClaimElement(profile, @"error", NO)) {
    FBClaimPause(@"请手动回到领取页并关闭旧提示");
  }
}

static NSDictionary *FBClaimStatus(void)
{
  if (!FBClaimJob) FBPhoneClaimRecoverHistory();
  NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
  NSDictionary *savedProfile = [defaults dictionaryForKey:FBClaimProfileKey];
  NSDictionary *profile = savedProfile ?: FBClaimBundledProfile();
  NSMutableDictionary *status = [@{@"ok": @YES, @"running": @(FBClaimJob.running), @"requires_review": @(FBClaimPending()),
           @"state": FBClaimJob.state ?: @"未启动", @"reason": FBClaimJob.reason ?: @"",
           @"attempts": @(FBClaimJob.attempts), @"successes": @(FBClaimJob.successes), @"fee": FBClaimJob.fee ?: @"",
           @"device_uuid": FBClaimDeviceID(), @"bundle_id": profile[@"bundle_id"] ?: @"",
           @"success_mode": profile[@"success_mode"] ?: @"element",
           @"runtime_validation": !savedProfile && profile != nil ? @YES : @NO,
           @"start_delay_supported": @YES,
           @"verified": [defaults arrayForKey:FBClaimStagesKey] ?: @[],
           @"start_nonce": FBClaimJob.running ? @"" : (FBClaimFreshNonce() ?: @"")} mutableCopy];
  if (FBClaimJob && FBClaimRunID) {
    if (!FBClaimHistorySaved && !FBClaimJob.running && FBClaimLastRecord) {
      FBClaimHistorySaved = FBClaimHistoryUpdate(FBClaimRunJournal, FBClaimLastRecord, @"record");
    }
    status[@"run"] = FBClaimJob.running ? FBClaimRunRecord(FBClaimJob, NO) : FBClaimLastRecord;
    status[@"history_saved"] = @(FBClaimHistorySaved);
  }
  return status;
}

static void FBClaimSchedule(FBClaimLoop *job)
{
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.25 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
    if (FBClaimJob != job || !job.running) return;
    [job tick:NSProcessInfo.processInfo.systemUptime];
    if (!job.running || NSProcessInfo.processInfo.systemUptime >= FBClaimNextCheckpoint) FBClaimCheckpoint(job);
    if (job.running) FBClaimSchedule(job);
  });
}

static NSDictionary *FBClaimCommand(NSString *op, NSDictionary *args)
{
  if (!FBClaimAuthorized(args[@"access"])) return @{@"ok": @NO, @"message": @"设备自动化凭据无效"};
  if ([op isEqual:@"stop"]) {
    if (FBClaimJob.running) FBClaimJob.stopRequested = YES;
    return FBClaimStatus();
  }
  if (FBPhoneClaimBusy()) return @{@"ok": @NO, @"message": @"已有任务或验证正在执行"};
  FBClaimInspecting = YES;
  @try {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSDictionary *profile = [defaults dictionaryForKey:FBClaimProfileKey];
    if ([op isEqual:@"configure"]) {
      if (FBClaimPending()) FBClaimPause(@"请先核对上一次交易结果");
      [defaults setObject:FBClaimProfile(args[@"profile"], NO) forKey:FBClaimProfileKey];
      [defaults removeObjectForKey:FBClaimStagesKey];
      FBClaimVerifyFee = nil;
      FBClaimJob = nil;
      return FBClaimStatus();
    }
    BOOL useBundledProfile = profile == nil;
    if (useBundledProfile) {
      profile = FBClaimProfile(FBClaimBundledProfile(), YES);
    } else if (![profile[@"device_uuid"] isEqual:FBClaimDeviceID()]) {
      FBClaimPause(@"已保存的规则不属于当前设备");
    }
    if ([op isEqual:@"start-key"]) {
      NSString *publicKey = FBPairPublicKey();
      if (!publicKey) FBClaimPause(@"无法取得手机启动公钥，请保持解锁");
      return @{@"ok": @YES, @"start_key": publicKey};
    }
    if ([op isEqual:@"delete-password"]) {
      OSStatus status = SecItemDelete((__bridge CFDictionaryRef)FBClaimCredentialQuery(profile[@"bundle_id"]));
      if (status != errSecSuccess && status != errSecItemNotFound) FBClaimPause(@"钥匙串删除失败，请保持解锁");
      return FBClaimStatus();
    }
    if ([op isEqual:@"acknowledge"]) {
      if (![args[@"reviewed"] isEqual:@YES]) FBClaimPause(@"请人工核对提交结果后明确确认");
      FBClaimHome(profile);
      FBClaimClear();
      return FBClaimStatus();
    }
    NSArray *verified = [defaults arrayForKey:FBClaimStagesKey] ?: @[];
    if ([op isEqual:@"verify"]) {
      NSString *stage = args[@"stage"];
      NSArray *keys = [stage isKindOfClass:NSString.class] ? FBClaimStages()[stage] : nil;
      if (!keys) FBClaimPause(@"未知验证阶段");
      if (![stage isEqual:@"password"] && FBClaimElement(profile, @"error", NO)) FBClaimPause(@"请清除钱包错误提示");
      if ([stage isEqual:@"home"]) FBClaimHome(profile);
      for (NSString *key in keys) {
        XCUIElement *element = FBClaimElement(profile, key, YES);
        if ([@[@"claim", @"cancel", @"confirm", @"password", @"submit"] containsObject:key] && !element.isWDEnabled) FBClaimPause(@"待验证元素不可操作");
        if ([key isEqual:@"password"] && element.elementType != XCUIElementTypeSecureTextField) FBClaimPause(@"只支持系统安全密码框");
      }
      if ([stage isEqual:@"fee"]) {
        NSString *fee = FBClaimCanonicalFee(FBClaimFee(profile));
        if (!fee) FBClaimPause(@"不能可靠读取费用");
        NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
        if (!FBClaimVerifyFee || now - FBClaimVerifyAt > 10) {
          FBClaimVerifyFee = fee;
          FBClaimVerifyAt = now;
          return @{@"ok": @YES, @"verify_again": @YES, @"fee": fee};
        }
        if (now - FBClaimVerifyAt < 0.35) return @{@"ok": @YES, @"verify_again": @YES, @"fee": fee};
        BOOL same = [fee isEqual:FBClaimVerifyFee];
        FBClaimVerifyFee = nil;
        if (!same) FBClaimPause(@"费用尚未稳定");
      }
      if (![verified containsObject:stage]) [defaults setObject:[verified arrayByAddingObject:stage] forKey:FBClaimStagesKey];
      return FBClaimStatus();
    }
    if (![op isEqual:@"start"]) FBClaimPause(@"未知自动化命令");
    if (FBClaimPending()) FBClaimPause(@"上一次交易结果待核对，禁止重新开始");
    if (!useBundledProfile) FBClaimProfile(profile, YES);
    BOOL countDismissal = [profile[@"success_mode"] isEqual:@"password_dismissed"];
    if (!useBundledProfile) {
      for (NSString *stage in FBClaimStages()) {
        if (countDismissal && [stage isEqual:@"success"]) continue;
        if (![verified containsObject:stage]) FBClaimPause(@"请先只读验证所需页面阶段");
      }
    }
    NSDictionary *input = FBClaimStartInput(args[@"sealed"]);
    NSTimeInterval delay = [input[@"start_delay"] doubleValue];
    if (delay == 0) FBClaimFront(profile);
    NSString *password = input[@"password"];
    FBClaimLoop *job = [FBClaimLoop new];
    job.countDismissal = countDismissal;
    job.prepareOnStart = YES;
    job.interval = [profile[@"interval"] doubleValue];
    job.successInterval = [input[@"success_interval"] doubleValue];
    job.startDelay = delay;
    job.timeout = [profile[@"timeout"] doubleValue];
    job.limit = [input[@"limit"] unsignedIntegerValue];
    __weak FBClaimLoop *weakJob = job;
    job.drive = ^id(NSString *operation, NSString *key) {
      weakJob.action = key.length ? [NSString stringWithFormat:@"%@:%@", operation, key] : operation;
      if ([operation isEqual:@"mark"]) {
        if (FBClaimElement(profile, @"password", NO) ||
            !FBClaimElement(profile, @"confirm", YES).isWDEnabled) {
          FBClaimPause(@"网络费弹窗的确认按钮不可操作");
        }
        FBClaimMark();
        return nil;
      }
      if ([operation isEqual:@"clear"]) { FBClaimClear(); return nil; }
      if ([operation isEqual:@"fee"]) return FBClaimFee(profile);
      if ([operation isEqual:@"error_reason"]) {
        weakJob.action = @"exists:error";
        XCUIElement *error = FBClaimElement(profile, @"error", NO);
        if (!error) return nil;
        NSString *label = [error fb_valueForWDAttributeName:@"label"];
        if ([@[@"密码错误", @"密碼錯誤", @"钱包密码错误", @"錢包密碼錯誤"] containsObject:label ?: @""]) {
          return @"钱包密码错误";
        }
        return @"钱包显示错误提示，请检查";
      }
      if ([operation isEqual:@"exists"]) return @(FBClaimElement(profile, key, NO) != nil);
      BOOL dismiss = [operation isEqual:@"dismiss"];
      if (dismiss) {
        BOOL password = FBClaimElement(profile, @"password", NO) != nil;
        if ([key isEqual:@"password_cancel"] ? !password : password || !FBClaimElement(profile, @"fee_ready", NO)) {
          FBClaimPause(@"关闭前弹窗已改变，正在重新识别");
        }
      }
      XCUIElement *item = FBClaimElement(profile, key, YES);
      if (dismiss && [key isEqual:@"password_cancel"]) {
        NSString *name = [item fb_valueForWDAttributeName:@"name"];
        NSString *label = [item fb_valueForWDAttributeName:@"label"];
        NSArray *closeLabels = @[@"icon_close20", @"取消", @"关闭", @"關閉", @"Cancel", @"Close"];
        if (item.elementType != XCUIElementTypeButton ||
            (![closeLabels containsObject:name ?: @""] && ![closeLabels containsObject:label ?: @""])) {
          FBClaimPause(@"无法确认密码弹窗的关闭按钮，请手动关闭");
        }
      }
      if (!item.isWDEnabled) FBClaimPause(@"目标控件不可操作");
      FBClaimFront(profile);
      if (dismiss || [operation isEqual:@"tap"]) [item tap];
      else if ([operation isEqual:@"type"]) {
        FBClaimElement(profile, @"submit", YES);
        FBClaimFront(profile);
        if (item.elementType != XCUIElementTypeSecureTextField || ![item fb_typeText:password shouldClear:YES error:nil]) {
          FBClaimPause(@"无法在系统安全密码框输入，请人工检查");
        }
      }
      return nil;
    };
    FBPhoneClaimRecoverHistory();
    FBClaimRunJournal = FBClaimHistoryURL(FBClaimAccess());
    FBClaimRunID = NSUUID.UUID.UUIDString;
    FBClaimRunStartedAt = NSDate.date.timeIntervalSince1970;
    FBClaimRunUptime = NSProcessInfo.processInfo.systemUptime;
    FBClaimRevision = 0;
    FBClaimJob = job;
    [job begin:FBClaimRunUptime];
    FBClaimCheckpoint(job);
    FBClaimSchedule(job);
    return FBClaimStatus();
  } @catch (NSException *error) {
    return @{@"ok": @NO, @"message": [error.name isEqual:@"FBClaimPause"] ? error.reason ?: @"请人工检查" : @"配置或界面检查失败"};
  } @finally {
    FBClaimInspecting = NO;
  }
}
#endif
