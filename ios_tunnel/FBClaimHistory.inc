// Shared by the Runner's uploader and the on-device claim driver.
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>

static NSURL *FBClaimHistoryURL(NSString *access)
{
  NSData *data = [access dataUsingEncoding:NSUTF8StringEncoding];
  if (!data.length) return nil;
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
  NSMutableString *name = [NSMutableString stringWithString:@"wallet-claim-history-"];
  for (NSUInteger i = 0; i < sizeof(digest); i++) [name appendFormat:@"%02x", digest[i]];
  NSURL *dir = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
  return [dir URLByAppendingPathComponent:name];
}

static NSMutableArray<NSDictionary *> *FBClaimHistoryLoad(NSURL *url)
{
  NSArray *rows = [NSArray arrayWithContentsOfURL:url];
  if (rows) return [rows mutableCopy];
  // Keep an unreadable journal intact, including while the device is locked.
  return [NSFileManager.defaultManager fileExistsAtPath:url.path ?: @""] ? nil : [NSMutableArray new];
}

static BOOL FBClaimHistoryWrite(NSArray *rows, NSURL *url)
{
  NSURL *dir = url.URLByDeletingLastPathComponent;
  if (!dir || ![NSFileManager.defaultManager createDirectoryAtURL:dir
                              withIntermediateDirectories:YES attributes:nil error:nil]) return NO;
  return [rows writeToURL:url atomically:YES];
}

static BOOL FBClaimHistoryUpdate(NSURL *url, NSDictionary *record, NSString *mode)
{
  if (!url) return NO;
  @synchronized (NSProcessInfo.processInfo) {
    NSMutableArray<NSDictionary *> *rows = FBClaimHistoryLoad(url);
    if (!rows) return NO;
    if ([mode isEqual:@"recover"]) {
      BOOL changed = NO;
      for (NSUInteger i = 0; i < rows.count; i++) {
        NSDictionary *old = rows[i];
        if (![old[@"running"] boolValue]) continue;
        NSMutableDictionary *next = [old mutableCopy];
        next[@"running"] = @NO;
        next[@"state"] = @"已中断";
        if ([record[@"requires_review"] boolValue]) next[@"requires_review"] = @YES;
        next[@"reason"] = @"WDA 上次退出前未记录正常结束，准确停止时间未知";
        next[@"revision"] = @([old[@"revision"] unsignedIntegerValue] + 1);
        [next removeObjectForKey:@"synced"];
        rows[i] = next;
        changed = YES;
      }
      return !changed || FBClaimHistoryWrite(rows, url);
    }
    NSUInteger i = [rows indexOfObjectPassingTest:^BOOL(NSDictionary *row, NSUInteger idx, BOOL *stop) {
      return [row[@"id"] isEqual:record[@"id"]];
    }];
    if ([mode isEqual:@"ack"]) {
      if (i == NSNotFound || ![rows[i][@"revision"] isEqual:record[@"revision"]]) return YES;
      if ([record[@"running"] boolValue]) {
        NSMutableDictionary *saved = [rows[i] mutableCopy];
        saved[@"synced"] = @YES;
        rows[i] = saved;
      } else {
        [rows removeObjectAtIndex:i];
      }
    } else if (i == NSNotFound) {
      [rows addObject:record];
    } else {
      rows[i] = record;
    }
    return FBClaimHistoryWrite(rows, url);
  }
}

#ifdef FB_CLAIM_HISTORY_UPLOADER
static NSDictionary *FBClaimHistoryNext(NSURL *url)
{
  if (!url) return nil;
  @synchronized (NSProcessInfo.processInfo) {
    for (NSDictionary *row in FBClaimHistoryLoad(url)) {
      if (![row[@"synced"] boolValue]) return row;
    }
  }
  return nil;
}
#endif
