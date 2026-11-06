#import "FBClaimLoop.inc"
#define FB_CLAIM_HISTORY_UPLOADER 1
#import "FBClaimHistory.inc"

static FBClaimLoop *fixture(NSArray *fees, NSString *result, NSMutableArray *events)
{
  __block NSString *screen = @"home";
  __block NSUInteger read = 0;
  FBClaimLoop *job = [FBClaimLoop new];
  job.interval = 1;
  job.timeout = 2;
  job.limit = 1;
  job.drive = ^id(NSString *op, NSString *key) {
    if ([op isEqual:@"error_reason"]) return [screen isEqual:@"error"] ? @"钱包密码错误" : nil;
    if ([op isEqual:@"exists"]) return @([key isEqual:screen]);
    if ([op isEqual:@"fee"]) return fees[MIN(read++, fees.count - 1)];
    [events addObject:key.length ? key : op];
    if ([key isEqual:@"claim"]) screen = @"fee_ready";
    if ([key isEqual:@"cancel"]) screen = @"home";
    if ([key isEqual:@"confirm"]) screen = @"password";
    if ([key isEqual:@"password"] && [result isEqual:@"early error"]) screen = @"error";
    if ([key isEqual:@"submit"]) screen = result;
    return nil;
  };
  [job begin:10];
  return job;
}

static void run(FBClaimLoop *job)
{
  for (NSUInteger i = 0; i < 35 && job.running; i++) [job tick:10 + i * .25];
}

static void testHistory(void)
{
  NSURL *url = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]];
  NSDictionary *running = @{@"id": @"test", @"revision": @1, @"running": @YES, @"observed_at": @123};
  NSCAssert(FBClaimHistoryUpdate(url, running, @"record"), @"Start checkpoint was not saved");
  NSCAssert([FBClaimHistoryNext(url) isEqual:running], @"Offline checkpoint was lost");
  NSCAssert(FBClaimHistoryUpdate(url, running, @"ack") && !FBClaimHistoryNext(url), @"Start checkpoint not acknowledged");
  NSCAssert(FBClaimHistoryUpdate(url, @{@"requires_review": @YES}, @"recover"), @"WDA restart lost the last checkpoint");
  NSDictionary *interrupted = FBClaimHistoryNext(url);
  NSCAssert([interrupted[@"state"] isEqual:@"已中断"] && [interrupted[@"observed_at"] isEqual:@123]
            && !interrupted[@"ended_at"] && [interrupted[@"requires_review"] boolValue], @"Restart invented a stop time or lost the marker");
  NSCAssert(FBClaimHistoryUpdate(url, running, @"ack") && [FBClaimHistoryNext(url) isEqual:interrupted],
            @"A stale acknowledgment deleted the newer snapshot");
  NSCAssert(FBClaimHistoryUpdate(url, interrupted, @"ack") && !FBClaimHistoryNext(url), @"Finished snapshot was not cleared");
  [NSFileManager.defaultManager removeItemAtURL:url error:nil];
}

static void testStartupDismissal(void)
{
  for (NSUInteger initial = 0; initial <= 3; initial++) {
    NSMutableArray *events = [NSMutableArray array];
    __block NSUInteger layer = initial;
    __block NSUInteger passwordTaps = 0;
    FBClaimLoop *job = [FBClaimLoop new];
    job.prepareOnStart = YES;
    job.interval = 1;
    job.timeout = 2;
    job.drive = ^id(NSString *op, NSString *key) {
      if ([op isEqual:@"error_reason"]) return layer == 2 ? @"钱包密码错误" : nil;
      if ([op isEqual:@"exists"]) {
        if ([key isEqual:@"password"]) return @(layer == 2);
        if ([key isEqual:@"fee_ready"]) return @(layer == 1 || layer == 2);
        if ([key isEqual:@"home"] || [key isEqual:@"claim"]) return @(layer < 3);
        return @NO;
      }
      [events addObject:key];
      if ([op isEqual:@"dismiss"] && [key isEqual:@"password_cancel"] && layer == 2) {
        if (++passwordTaps == 2) layer = 1; // The first tap returns before the popup closes.
      } else if ([op isEqual:@"dismiss"] && [key isEqual:@"cancel"] && layer == 1) {
        layer = 0;
      }
      return nil;
    };
    [job begin:10];
    for (NSUInteger i = 0; i <= 24 && job.attempts == 0; i++) [job tick:10 + .25 * i];
    NSArray *expected = initial == 2 ? @[@"password_cancel", @"password_cancel", @"cancel", @"claim"]
                       : initial == 1 ? @[@"cancel", @"claim"] : initial == 0 ? @[@"claim"] : @[];
    NSCAssert([events isEqual:expected], @"Startup performed a wrong action, skipped a layer or acted on another page");
    NSCAssert(job.running && job.attempts == (initial < 3 ? 1 : 0), @"Startup counted cleanup as a claim or stopped waiting");
  }
}

static void testStartDelay(void)
{
  NSMutableArray *events = [NSMutableArray array];
  FBClaimLoop *job = fixture(@[@"0"], @"home", events);
  job.startDelay = 2;
  [job begin:10];
  [job tick:10];
  [job tick:11.99];
  NSCAssert([job.state isEqual:@"等待启动"] && job.running && events.count == 0, @"Acted before the start delay");
  [job tick:12];
  NSCAssert([events isEqual:@[@"claim"]], @"Did not start when the delay elapsed");

  [events removeAllObjects];
  job = fixture(@[@"0"], @"home", events);
  job.startDelay = 2;
  job.prepareOnStart = YES;
  [job begin:10];
  [job tick:11.99];
  NSCAssert([job.state isEqual:@"等待启动"] && events.count == 0, @"Started preparing early");
  [job tick:12];
  NSCAssert([job.state isEqual:@"准备领取页"], @"Delayed run bypassed page preparation");

  job = fixture(@[@"0"], @"home", events);
  job.startDelay = 2;
  [job begin:10];
  job.stopRequested = YES;
  [job tick:11];
  [job tick:12];
  NSCAssert([job.state isEqual:@"已停止"] && !job.running && events.count == 0, @"Stop did not cancel delayed start");
}

int main(void)
{
  @autoreleasepool {
    testHistory();
    testStartupDismissal();
    testStartDelay();
    NSCAssert([FBClaimCanonicalFee(@"0.000 DAI") isEqual:@"0"], @"Zero DAI");
    NSCAssert([FBClaimCanonicalFee(@"0 DAI ($0.0000)") isEqual:@"0"], @"Combined zero DAI");
    NSCAssert([FBClaimCanonicalFee(@"0.00001 DAI ($0.0000)") isEqual:@"0.00001"], @"Rounded USD estimate hid a positive fee");
    NSCAssert([FBClaimCanonicalFee(@"0.0000000000000000001 BNB") isEqual:@"0.0000000000000000001"], @"Tiny positive decimal");
    NSCAssert([FBClaimCanonicalFee(@"1,234.50 USDT") isEqual:@"1234.5"], @"Grouped amount");
    for (NSString *text in @[@"<0.01", @"1e-9", @"-1", @"0 ETH ($0.00)", @"0 DAI (0.01 DAI)", @"約 0", @"0\n1"]) {
      NSCAssert(!FBClaimCanonicalFee(text), @"Ambiguous input accepted");
    }
    NSMutableArray *events = [NSMutableArray array];
    FBClaimLoop *job = fixture(@[@"0", @"0.0001"], @"success", events);
    for (NSUInteger i = 0; i <= 3; i++) [job tick:10 + i * .25];
    NSCAssert([job.state isEqual:@"等待恢复"] && job.running && [events isEqual:@[@"claim"]],
              @"Changing fee did not wait safely");
    run(job);
    NSCAssert(job.successes == 1 && [events containsObject:@"confirm"], @"Stable fee did not resume after rechecking");
    [events removeAllObjects];
    job = fixture(@[@"0", @"0.0"], @"success", events);
    [job tick:10];[job tick:10.25];[job tick:10.75];[job tick:11];[job tick:11.75];
    NSCAssert([events isEqual:(@[@"claim", @"cancel"])], @"Zero fee did not cancel and wait");
    job.stopRequested = YES;[job tick:12];
    NSCAssert([job.state isEqual:@"已停止"] && events.count == 2, @"Stop allowed a new click");
    [events removeAllObjects];
    job = fixture(@[@"0.0001 ETH"], @"success", events);run(job);
    NSCAssert(job.successes == 1 && [job.state isEqual:@"完成"], @"Success was not counted");
    NSCAssert([events isEqual:(@[@"claim", @"mark", @"confirm", @"password", @"submit", @"clear"])], @"Action/marker order wrong");
    [events removeAllObjects];
    job = fixture(@[@"0.001"], @"unknown", events);run(job);
    NSCAssert(job.successes == 0 && [job.state isEqual:@"已暂停"], @"Unknown result counted as success");
    NSCAssert([events isEqual:(@[@"claim", @"mark", @"confirm", @"password", @"submit"])], @"Unknown result retried or cleared marker");
    for (NSString *result in @[@"error", @"early error"]) {
      [events removeAllObjects];
      job = fixture(@[@"0.001"], result, events);
      run(job);
      NSArray *expected = [result isEqual:@"early error"]
        ? @[@"claim", @"mark", @"confirm", @"password"] : @[@"claim", @"mark", @"confirm", @"password", @"submit"];
      NSCAssert([job.state isEqual:@"已暂停"] && job.successes == 0 && [events isEqual:expected], @"Password error retried, cleared marker or counted as success");
    }
    [events removeAllObjects];
    job = fixture(@[@"0.001"], @"home", events);
    job.countDismissal = YES;
    for (NSUInteger i = 0; i <= 8; i++) [job tick:10 + i * .25];
    NSCAssert(job.successes == 0 && ![events containsObject:@"clear"], @"Counted before dismissal stayed stable");
    run(job);
    NSCAssert(job.successes == 1 && [job.reason containsString:@"未核验提交"], @"Stable dismissal not counted as provisional");
    [events removeAllObjects];
    job = fixture(@[@"0.001"], @"home", events);
    job.countDismissal = YES;
    job.limit = 0;
    run(job);
    NSCAssert(job.running && job.successes >= 2, @"Unlimited mode stopped after the first result");
    NSUInteger eventsBeforeStop = events.count;
    job.stopRequested = YES;
    [job tick:19];
    [job tick:20];
    NSCAssert([job.state isEqual:@"已停止"] && events.count == eventsBeforeStop, @"Unlimited mode acted after stop");
    [events removeAllObjects];
    job = fixture(@[@"0", @"0", @"0.001"], @"home", events);
    job.countDismissal = YES;
    job.successInterval = 4;
    job.limit = 2;
    double now = 10;
    for (; now < 20 && job.successes == 0; now += .25) [job tick:now];
    NSCAssert(job.successes == 1 && job.attempts == 2, @"Zero fee consumed the success limit or used the success wait");
    for (double until = now + 3.5; now < until; now += .25) [job tick:now];
    NSCAssert(job.attempts == 2, @"Retried before the configured success wait");
    for (; now < 30 && job.running; now += .25) [job tick:now];
    NSCAssert(job.successes == 2 && [job.state isEqual:@"完成"], @"Did not stop at requested success count");
    for (NSString *result in @[@"password", @"fee_ready", @"error"]) {
      [events removeAllObjects];
      job = fixture(@[@"0.001"], result, events);
      job.countDismissal = YES;
      run(job);
      NSCAssert(job.successes == 0 && [job.state isEqual:@"已暂停"] && ![events containsObject:@"clear"], @"Invalid dismissal cleared review marker");
    }
    [events removeAllObjects];
    job = fixture(@[@"0"], @"home", events);
    FBClaimDriver normal = job.drive;
    __block BOOL failRead = YES;
    job.drive = ^id(NSString *op, NSString *key) {
      if ([op isEqual:@"fee"] && failRead) {
        failRead = NO;
        @throw [NSException exceptionWithName:@"SyntheticUIError" reason:@"fixture" userInfo:nil];
      }
      return normal(op, key);
    };
    for (NSUInteger i = 0; i < 42; i++) [job tick:10 + i * .25];
    NSCAssert(job.running && job.attempts >= 2 && [events containsObject:@"cancel"] &&
              ![events containsObject:@"confirm"], @"A recoverable UI exception stopped the run or confirmed a fee");
    [events removeAllObjects];
    job = fixture(@[@"0.01"], @"home", events);
    FBClaimDriver positive = job.drive;
    job.drive = ^id(NSString *op, NSString *key) {
      id value = positive(op, key);
      if ([op isEqual:@"tap"] && [key isEqual:@"submit"]) {
        @throw [NSException exceptionWithName:@"SyntheticSubmitError" reason:@"fixture" userInfo:nil];
      }
      return value;
    };
    run(job);
    NSCAssert([job.state isEqual:@"已暂停"] && [events isEqual:(@[@"claim", @"mark", @"confirm", @"password", @"submit"])],
              @"An uncertain submit was retried or cleared");
    [events removeAllObjects];
    job = fixture(@[@"0"], @"home", events);
    FBClaimDriver unlocked = job.drive;
    __block BOOL locked = YES;
    job.drive = ^id(NSString *op, NSString *key) {
      if (locked) FBClaimPause(@"手机已锁屏");
      return unlocked(op, key);
    };
    [job tick:10]; [job tick:13];
    NSCAssert(job.running && [job.state isEqual:@"等待恢复"] && !events.count, @"Locked screen did not wait without actions");
    locked = NO;
    [job tick:16]; [job tick:16.25];
    NSCAssert(job.running && [events isEqual:@[@"claim"]], @"Unlock did not resume on a clean claim page");
    puts("Phone claim simulations passed");
  }
  return 0;
}
