// Main-queue state machine. The driver supplies UI actions and the durable
// submission marker; no network connection is needed after start.
#import <Foundation/Foundation.h>

static NSString *FBClaimCanonicalFee(NSString *text)
{
  if (![text isKindOfClass:NSString.class] || text.length > 100) return nil;
  NSString *amount = [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  static NSRegularExpression *regex;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    regex = [NSRegularExpression regularExpressionWithPattern:
      @"\\A(?:[¥$€] *)?((?:0|[1-9][0-9]*|[1-9][0-9]{0,2}(?:,[0-9]{3})+)(?:\\.[0-9]+)?)(?: *(?:ETH|BNB|BTC|SOL|TRX|USDT|USDC|DAI(?: \\(\\$[0-9]+(?:\\.[0-9]+)?\\))?|TON|MATIC|POL|AVAX|SUI|APT|OKT|HT|FTM|CRO|DOT|ATOM|ADA|XRP|DOGE|USD|CNY|EUR|GWEI|WEI|gwei|wei|元))?\\z"
      options:(NSRegularExpressionOptions)0 error:nil];
  });
  NSTextCheckingResult *match = [regex firstMatchInString:amount options:(NSMatchingOptions)0 range:NSMakeRange(0, amount.length)];
  if (!match) return nil;
  amount = [[amount substringWithRange:[match rangeAtIndex:1]] stringByReplacingOccurrencesOfString:@"," withString:@""];
  if ([amount containsString:@"."]) {
    while ([amount hasSuffix:@"0"]) amount = [amount substringToIndex:amount.length - 1];
    if ([amount hasSuffix:@"."]) amount = [amount substringToIndex:amount.length - 1];
  }
  return amount;
}

static void FBClaimPause(NSString *reason)
{
  @throw [NSException exceptionWithName:@"FBClaimPause" reason:reason userInfo:nil];
}

typedef id (^FBClaimDriver)(NSString *operation, NSString *key);

@interface FBClaimLoop : NSObject
@property (nonatomic, copy) FBClaimDriver drive;
@property (nonatomic, copy) NSString *state;
@property (nonatomic, copy) NSString *reason;
@property (nonatomic, copy) NSString *fee;
@property (nonatomic, copy) NSString *endStep;
@property (nonatomic, copy) NSString *action;
@property (nonatomic, copy) NSString *firstFee;
@property (nonatomic) NSUInteger attempts;
@property (nonatomic) NSUInteger successes;
@property (nonatomic) NSUInteger limit;
@property (nonatomic) NSTimeInterval interval;
@property (nonatomic) NSTimeInterval successInterval;
@property (nonatomic) NSTimeInterval startDelay;
@property (nonatomic) BOOL afterSuccess;
@property (nonatomic) BOOL prepareOnStart;
@property (nonatomic) NSTimeInterval cleanSince;
@property (nonatomic) BOOL ownFee;
@property (nonatomic) BOOL confirmationPending;
@property (nonatomic) NSTimeInterval timeout;
@property (nonatomic) NSTimeInterval deadline;
@property (nonatomic) NSTimeInterval next;
@property (nonatomic) NSTimeInterval passwordGoneSince;
@property (nonatomic) BOOL countDismissal;
@property (nonatomic) BOOL running;
@property (nonatomic) BOOL stopRequested;
@property (nonatomic) BOOL stepping;
- (void)begin:(NSTimeInterval)now;
- (void)tick:(NSTimeInterval)now;
@end

@implementation FBClaimLoop
- (void)enter:(NSString *)state now:(NSTimeInterval)now delay:(NSTimeInterval)delay
{
  self.state = state;
  self.next = now + delay;
  self.deadline = self.next + self.timeout;
}

- (void)begin:(NSTimeInterval)now
{
  self.running = YES;
  self.reason = @"";
  self.fee = @"";
  self.cleanSince = -1;
  NSString *initial = self.startDelay > 0 ? @"等待启动" : self.prepareOnStart ? @"准备领取页" : @"等待领取页";
  [self enter:initial now:now delay:self.startDelay];
}

- (BOOL)has:(NSString *)key
{
  return [self.drive(@"exists", key) boolValue];
}

- (void)require:(NSString *)key
{
  if (![self has:key]) FBClaimPause(@"操作前页面阶段已改变");
}

- (void)checkWalletError
{
  NSString *reason = self.drive(@"error_reason", @"error");
  if (reason.length) FBClaimPause(reason);
}

- (void)noPreviousSuccess
{
  if ([self has:@"success"]) FBClaimPause(@"仍有旧的提交成功提示");
}

- (NSString *)readFee
{
  NSString *fee = FBClaimCanonicalFee(self.drive(@"fee", @""));
  if (!fee) FBClaimPause(@"无法可靠读取单一网络费");
  return fee;
}

- (void)finish:(NSString *)state reason:(NSString *)reason
{
  self.endStep = self.state;
  self.state = state;
  self.reason = reason;
  self.running = NO;
  self.drive = nil; // Release the run's credential and profile adapter.
}

- (void)preparePage:(NSTimeInterval)now
{
  // Inspect the topmost known prompt on every pass, including after animations.
  if ([self has:@"password"]) {
    self.cleanSince = -1;
    self.reason = @"正在关闭密码弹窗";
    self.drive(@"dismiss", @"password_cancel");
    [self enter:@"准备领取页" now:now delay:1];
  } else if ([self has:@"fee_ready"]) {
    self.cleanSince = -1;
    self.reason = @"正在关闭网络费弹窗";
    self.drive(@"dismiss", @"cancel");
    [self enter:@"准备领取页" now:now delay:1];
  } else {
    [self checkWalletError];
    [self noPreviousSuccess];
    if (![self has:@"home"] || ![self has:@"claim"]) {
      self.cleanSince = -1;
      self.reason = @"请打开钱包内配置对应的领取页";
      [self enter:@"准备领取页" now:now delay:1];
      return;
    }
    if (self.cleanSince < 0) self.cleanSince = now;
    if (now - self.cleanSince >= 0.75) {
      self.reason = @"";
      [self enter:@"等待领取页" now:now delay:0];
    }
  }
}

- (void)tick:(NSTimeInterval)now
{
  if (!self.running || self.stepping) return;
  if (self.stopRequested) {
    [self finish:@"已停止" reason:@"已停止发送新操作"];
    return;
  }
  if (now < self.next) return;
  if ([self.state isEqual:@"等待启动"]) {
    [self enter:self.prepareOnStart ? @"准备领取页" : @"等待领取页" now:now delay:0];
  }
  self.stepping = YES;
  self.action = nil;
  @try {
    if ([self.state isEqual:@"准备领取页"]) {
      [self preparePage:now];
      return;
    }
    if (now > self.deadline) FBClaimPause(@"等待页面超时，请检查手机与提交结果");
    [self checkWalletError];
    NSString *state = self.state;
    if ([state isEqual:@"等待恢复"]) {
      // Reuse only a fee prompt opened by this run. Never adopt a password prompt.
      if ([self has:@"password"]) FBClaimPause(@"存在密码弹窗，请先检查并关闭");
      [self noPreviousSuccess];
      if ([self has:@"fee_ready"]) {
        if (!self.ownFee) FBClaimPause(@"存在无法确认来源的费用弹窗，请先检查并关闭");
        // After a failed round, discard its verified fee prompt before retrying.
        [self require:@"fee_ready"];
        self.drive(@"tap", @"cancel");
        self.ownFee = NO;
        [self enter:@"等待返回" now:now delay:0];
      } else if ([self has:@"home"]) {
        self.ownFee = NO;
        self.reason = @"";
        [self enter:@"等待领取页" now:now delay:0];
      } else {
        [self enter:@"等待恢复" now:now delay:3];
      }
    } else if ([state isEqual:@"等待领取页"]) {
      if (![self has:@"home"]) return;
      if ([self has:@"password"] || [self has:@"fee_ready"]) FBClaimPause(@"领取页仍有弹窗");
      [self noPreviousSuccess];
      [self require:@"home"];
      self.drive(@"tap", @"claim");
      self.attempts++;
      self.ownFee = YES;
      [self enter:@"等待费用" now:now delay:0];
    } else if ([state isEqual:@"等待费用"]) {
      if (![self has:@"fee_ready"]) return;
      self.firstFee = [self readFee];
      [self enter:@"复查费用" now:now delay:0.35];
    } else if ([state isEqual:@"复查费用"]) {
      [self require:@"fee_ready"];
      NSString *fee = [self readFee];
      if (![fee isEqual:self.firstFee]) FBClaimPause(@"网络费发生变化，请人工检查");
      self.fee = fee;
      if ([fee isEqual:@"0"]) {
        self.afterSuccess = NO;
        [self require:@"fee_ready"];
        self.drive(@"tap", @"cancel");
        self.ownFee = NO;
        [self enter:@"等待返回" now:now delay:0];
      } else {
        [self noPreviousSuccess];
        [self require:@"fee_ready"];
        if (![[self readFee] isEqual:fee]) FBClaimPause(@"确认前网络费已改变");
        self.drive(@"mark", @"");
        self.confirmationPending = YES;
        self.ownFee = NO;
        self.drive(@"tap", @"confirm");
        [self enter:@"等待密码" now:now delay:0];
      }
    } else if ([state isEqual:@"等待密码"]) {
      if (![self has:@"password"]) return;
      self.drive(@"type", @"password");
      [self enter:@"准备提交" now:now delay:0];
    } else if ([state isEqual:@"准备提交"]) {
      [self noPreviousSuccess];
      [self require:@"password"];
      self.drive(@"tap", @"submit");
      self.passwordGoneSince = -1;
      [self enter:@"等待提交结果" now:now delay:0];
    } else if ([state isEqual:@"等待提交结果"]) {
      if (self.countDismissal) {
        if ([self has:@"password"] || [self has:@"fee_ready"]) {
          self.passwordGoneSince = -1;
          return;
        }
        if (self.passwordGoneSince < 0) self.passwordGoneSince = now;
        if (now - self.passwordGoneSince < 1) return;
        [self checkWalletError];
      } else if (![self has:@"success"]) {
        return;
      }
      self.drive(@"clear", @"");
      self.confirmationPending = NO;
      self.successes++;
      if (self.limit && self.successes >= self.limit) {
        [self finish:@"完成" reason:self.countDismissal ? @"密码弹窗已关闭（临时计数，未核验提交）" : @"钱包已提示提交成功"];
      } else {
        self.afterSuccess = YES;
        [self enter:@"等待返回" now:now delay:0];
      }
    } else if ([state isEqual:@"等待返回"]) {
      if (![self has:@"home"] || [self has:@"success"] || [self has:@"fee_ready"] || [self has:@"password"]) return;
      self.reason = @"";
      [self enter:@"等待领取页" now:now delay:self.afterSuccess && self.successInterval > 0 ? self.successInterval : self.interval];
    }
  } @catch (NSException *error) {
    NSString *reason = [error.name isEqual:@"FBClaimPause"] ? error.reason : @"系统或界面操作失败，请人工检查";
    if (self.stopRequested) {
      [self finish:@"已停止" reason:@"已停止发送新操作"];
    } else if ([self.state isEqual:@"准备领取页"]) {
      self.cleanSince = -1;
      self.reason = reason ?: @"等待领取页恢复";
      [self enter:@"准备领取页" now:now delay:3];
    } else if (self.confirmationPending || [reason isEqual:@"钱包密码错误"]) {
      [self finish:@"已暂停" reason:reason ?: @"请人工检查"];
    } else {
      self.reason = reason ?: @"等待页面恢复";
      self.firstFee = nil;
      [self enter:@"等待恢复" now:now delay:MAX(3, self.interval)];
    }
  } @finally {
    self.stepping = NO;
  }
}
@end
