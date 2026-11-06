// Compiled into the WDA XCTest runner. No Network Extension or VPN required.
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Network/Network.h>
#import "FBPairKey.inc"
#import "FBPairEvents.inc"
#define FB_CLAIM_HISTORY_UPLOADER 1
#import "FBClaimHistory.inc"

static NSData *FBRemoteJPEG(NSData *original, NSUInteger maxPixels, CGFloat qualityValue)
{
  if (!original.length) return nil;
  CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)original, NULL);
  if (!source) return nil;
  NSDictionary *options = @{(__bridge NSString *)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
    (__bridge NSString *)kCGImageSourceThumbnailMaxPixelSize: @(maxPixels)};
  CGImageRef image = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)options);
  CFRelease(source);
  if (!image) return nil;
  NSMutableData *jpg = [NSMutableData data];
  CGImageDestinationRef output = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)jpg, CFSTR("public.jpeg"), 1, NULL);
  if (!output) { CGImageRelease(image); return nil; }
  NSDictionary *quality = @{(__bridge NSString *)kCGImageDestinationLossyCompressionQuality: @(qualityValue)};
  CGImageDestinationAddImage(output, image, (__bridge CFDictionaryRef)quality);
  BOOL ok = CGImageDestinationFinalize(output);
  CFRelease(output);
  CGImageRelease(image);
  return ok ? jpg : nil;
}

static NSData *FBRemotePreviewJPEG(NSData *data)
{
  id payload = [NSJSONSerialization JSONObjectWithData:data options:(NSJSONReadingOptions)0 error:nil];
  if (![payload isKindOfClass:NSDictionary.class] || ![payload[@"value"] isKindOfClass:NSString.class]) return nil;
  NSData *original = [[NSData alloc] initWithBase64EncodedString:(payload[@"value"] ?: @"") options:(NSDataBase64DecodingOptions)0];
  NSData *jpg = FBRemoteJPEG(original, 960, 0.55);
  if (!jpg) return nil;
  NSMutableDictionary *result = [payload mutableCopy];
  result[@"value"] = [jpg base64EncodedStringWithOptions:(NSDataBase64EncodingOptions)0];
  return [NSJSONSerialization dataWithJSONObject:result options:(NSJSONWritingOptions)0 error:nil];
}

@interface FBRemoteTunnel : NSObject <NSURLSessionTaskDelegate, NSURLSessionWebSocketDelegate>
@property (nonatomic) NSURLSession *session;
@property (nonatomic) NSURLSessionWebSocketTask *socket;
@property (nonatomic) NSURLSessionDataTask *localTask;
@property (nonatomic) NSURL *endpoint;
@property (nonatomic) NSString *token;
@property (nonatomic) NSString *historyAccess;
@property (nonatomic) dispatch_queue_t queue;
@property (nonatomic) BOOL stopped;
@property (nonatomic) NSUInteger generation;
@property (nonatomic) NSUInteger bindingGeneration;
@property (nonatomic) BOOL busy;
@property (nonatomic) NSMutableSet<NSString *> *seen;
@property (nonatomic) NSURLSessionWebSocketTask *videoSocket;
@property (nonatomic) nw_connection_t videoLocal;
@property (nonatomic) NSMutableData *videoBuffer;
@property (nonatomic) NSData *videoLatest;
@property (nonatomic) BOOL videoSending;
@property (nonatomic) NSUInteger videoCredits;
@property (nonatomic) BOOL videoAck;
@property (nonatomic) NSString *rejectedPairCode;
@property (nonatomic) id pairObserver;
@property (nonatomic) NSURLSessionDataTask *pairRequest;
- (void)start;
- (void)stop;
@end

@implementation FBRemoteTunnel

- (instancetype)init
{
  if ((self = [super init])) {
    _queue = dispatch_queue_create("wda.remote.tunnel", DISPATCH_QUEUE_SERIAL);
    _seen = [NSMutableSet new];
  }
  return self;
}

- (void)start
{
  NSURL *configURL = [[NSBundle bundleForClass:self.class] URLForResource:@"FBRemoteTunnel" withExtension:@"plist"];
  if (!configURL) {
    NSLog(@"[RemoteTunnel] No configuration; tunnel disabled");
    return;
  }
  NSDictionary *config = [NSDictionary dictionaryWithContentsOfURL:configURL ?: [NSURL fileURLWithPath:@"/nonexistent"]];
  if (![config[@"Enabled"] boolValue]) return;
  NSURLSessionConfiguration *sessionConfig = NSURLSessionConfiguration.ephemeralSessionConfiguration;
  sessionConfig.HTTPShouldSetCookies = NO;
  sessionConfig.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
  sessionConfig.connectionProxyDictionary = @{};
  self.session = [NSURLSession sessionWithConfiguration:sessionConfig delegate:self delegateQueue:nil];
  dispatch_async(self.queue, ^{
    self.historyAccess = [config[@"ClaimToken"] isKindOfClass:NSString.class] ? config[@"ClaimToken"] : nil;
    if ([self openEndpoint:config[@"URL"] token:config[@"Token"]]) return;
    NSString *origin = config[@"EnrollmentOrigin"];
    if (![origin isKindOfClass:NSString.class]) return;
    NSURLComponents *parts = [NSURLComponents componentsWithString:origin];
    if (![parts.scheme isEqualToString:@"https"] || !parts.host.length || parts.user || parts.password ||
        parts.path.length || parts.query || parts.fragment) return;
    // Give iOS a chance to request wireless data access before the user enters a pairing code.
    NSURL *probeURL = [NSURL URLWithString:[origin stringByAppendingString:@"/claim/api/options"]];
    if (!probeURL) return;
    NSURLRequest *probe = [NSURLRequest requestWithURL:probeURL
                                         cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:12];
    [[self.session dataTaskWithRequest:probe completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
      if ([error.domain isEqualToString:NSURLErrorDomain]) {
        NSLog(@"[RemoteTunnel] Startup network check failed (URL error %ld)", (long)error.code);
      }
    }] resume];
    __weak typeof(self) weakSelf = self;
    self.pairObserver = [NSNotificationCenter.defaultCenter addObserverForName:FBPairRequestEvent object:nil queue:nil
        usingBlock:^(NSNotification *event) {
      FBRemoteTunnel *tunnel = weakSelf;
      if (!tunnel) return;
      dispatch_async(tunnel.queue, ^{
        [tunnel enrollCode:event.userInfo[@"code"] atOrigin:origin receipt:event.userInfo[@"receipt"]];
      });
    }];
    [self pairAtOrigin:origin];
  });
}

- (BOOL)openEndpoint:(id)address token:(id)token
{
  if (![address isKindOfClass:NSString.class] || ![token isKindOfClass:NSString.class] || [token length] < 32) return NO;
  NSURL *url = [NSURL URLWithString:address];
  if (![url.scheme isEqualToString:@"wss"] || !url.host.length || url.user || url.password ||
      url.query || url.fragment || ![url.path hasPrefix:@"/device/"]) return NO;
  [self.localTask cancel];
  self.localTask = nil;
  self.busy = NO;
  [self.socket cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil];
  [self.videoSocket cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil];
  self.videoSocket = nil;
  self.videoSending = NO;
  [self stopLocalVideo];
  self.endpoint = url;
  self.token = token;
  NSLog(@"[RemoteTunnel] Outbound tunnel enabled");
  [self connect];
  [self connectVideo];
  [self syncClaimHistory:++self.bindingGeneration];
  return YES;
}

- (void)syncClaimHistory:(NSUInteger)binding
{
  if (self.stopped || !self.endpoint || binding != self.bindingGeneration) return;
  NSURL *journal = FBClaimHistoryURL(self.historyAccess ?: FBPairCredentials()[@"access"]);
  NSDictionary *record = FBClaimHistoryNext(journal);
  if (!record) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), self.queue, ^{ [self syncClaimHistory:binding]; });
    return;
  }
  NSURLComponents *url = [NSURLComponents componentsWithURL:self.endpoint resolvingAgainstBaseURL:NO];
  url.scheme = @"https";
  url.percentEncodedPath = [NSString stringWithFormat:@"/claim/api%@/history", url.percentEncodedPath];
  NSURL *target = url.URL;
  if (!target) return;
  NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:target
                              cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:10];
  request.HTTPMethod = @"POST";
  [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
  [request setValue:[@"Bearer " stringByAppendingString:self.token] forHTTPHeaderField:@"Authorization"];
  request.HTTPBody = [NSJSONSerialization dataWithJSONObject:record options:(NSJSONWritingOptions)0 error:nil];
  [[self.session dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
    dispatch_async(self.queue, ^{
      if (self.stopped || binding != self.bindingGeneration) return;
      BOOL acknowledged = !error && [response isKindOfClass:NSHTTPURLResponse.class] &&
          [(NSHTTPURLResponse *)response statusCode] == 204 && FBClaimHistoryUpdate(journal, record, @"ack");
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (acknowledged ? 1 : 15) * NSEC_PER_SEC),
                     self.queue, ^{ [self syncClaimHistory:binding]; });
    });
  }] resume];
}

- (void)pairAtOrigin:(NSString *)origin
{
  if (self.stopped || self.endpoint || self.pairRequest) return;
  NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
  NSString *code = [[defaults stringForKey:@"WalletClickPairCode"] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  if (!code.length) {
    NSDictionary *saved = FBPairCredentials();
    if ([saved[@"origin"] isEqual:origin] && [self openEndpoint:saved[@"url"] token:saved[@"token"]]) return;
  }
  if (code.length != 24 || [code isEqual:self.rejectedPairCode]) {
    [self retryPairAtOrigin:origin];
    return;
  }
  [self enrollCode:code atOrigin:origin receipt:nil];
}

- (void)reportPair:(NSString *)receipt state:(NSString *)state
{
  if (!receipt) return;
  [NSNotificationCenter.defaultCenter postNotificationName:FBPairResultEvent object:nil
      userInfo:@{@"receipt": receipt, @"state": state}];
}

- (void)enrollCode:(NSString *)code atOrigin:(NSString *)origin receipt:(NSString *)receipt
{
  if (self.stopped || self.pairRequest) {
    [self reportPair:receipt state:@"busy"];
    return;
  }
  NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
  NSString *proof = [defaults stringForKey:@"WalletClickPairProof"];
  if (!proof.length) {
    proof = [NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""];
    [defaults setObject:proof forKey:@"WalletClickPairProof"];
  }
  NSString *publicKey = FBPairPublicKey();
  if (!publicKey.length) {
    [self reportPair:receipt state:@"key"];
    [self retryPairAtOrigin:origin];
    return;
  }
  NSURL *endpoint = [NSURL URLWithString:[origin stringByAppendingString:@"/claim/api/enroll"]];
  if (!endpoint) {
    [self reportPair:receipt state:@"network"];
    return;
  }
  NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:endpoint
                                                        cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:12];
  request.HTTPMethod = @"POST";
  [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
  request.HTTPBody = [NSJSONSerialization dataWithJSONObject:@{@"code": code, @"proof": proof, @"start_key": publicKey}
                                                    options:(NSJSONWritingOptions)0 error:nil];
  self.pairRequest = [self.session dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
    dispatch_async(self.queue, ^{
      if (self.stopped) return;
      self.pairRequest = nil;
      NSInteger status = [response isKindOfClass:NSHTTPURLResponse.class] ? [(NSHTTPURLResponse *)response statusCode] : 0;
      BOOL unavailable = [error.domain isEqualToString:NSURLErrorDomain] &&
          (error.code == NSURLErrorNotConnectedToInternet || error.code == NSURLErrorDataNotAllowed);
      NSString *state = unavailable ? @"offline" : @"network";
      if (!error && status == 200 && data.length && data.length < 4096) {
        id value = [NSJSONSerialization JSONObjectWithData:data options:(NSJSONReadingOptions)0 error:nil];
        if ([value isKindOfClass:NSDictionary.class]) {
          id device = value[@"device"], token = value[@"token"], access = value[@"access"];
          if ([device isKindOfClass:NSString.class] && [device rangeOfString:@"\\A[A-Za-z0-9_-]{1,56}\\z"
              options:NSRegularExpressionSearch].location != NSNotFound &&
              [token isKindOfClass:NSString.class] && [token length] >= 32 &&
              [access isKindOfClass:NSString.class] && [access length] >= 32) {
            NSString *url = [NSString stringWithFormat:@"wss://%@/device/%@", [origin substringFromIndex:8], device];
            if (FBPairSave(@{@"origin": origin, @"url": url, @"token": token, @"access": access})) {
              [defaults removeObjectForKey:@"WalletClickPairCode"];
              [defaults removeObjectForKey:@"WalletClickPairProof"];
              self.historyAccess = nil;
              if ([self openEndpoint:url token:token]) {
                [self reportPair:receipt state:@"success"];
                return;
              }
            }
            state = @"key";
          }
        }
      } else if (status == 400 || status == 403) {
        self.rejectedPairCode = code;
        state = @"rejected";
        NSLog(@"[RemoteTunnel] Pairing code rejected; request a new code");
      }
      [self reportPair:receipt state:state];
      [self retryPairAtOrigin:origin];
    });
  }];
  [self.pairRequest resume];
}

- (void)retryPairAtOrigin:(NSString *)origin
{
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), self.queue, ^{ [self pairAtOrigin:origin]; });
}

- (void)connectVideo
{
  if (self.stopped || self.videoSocket) return;
  NSURL *url = [NSURL URLWithString:[self.endpoint.absoluteString stringByAppendingString:@"-mjpeg"] ?: @""];
  if (!url) return;
  NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
  [request setValue:[@"Bearer " stringByAppendingString:self.token] forHTTPHeaderField:@"Authorization"];
  [request setValue:@"1" forHTTPHeaderField:@"X-WDA-Video-Ack"];
  self.videoSocket = [self.session webSocketTaskWithRequest:request];
  self.videoSocket.maximumMessageSize = 1024;
  [self.videoSocket resume];
  [self receiveVideo:self.videoSocket];
}

- (void)stopLocalVideo
{
  nw_connection_t local = self.videoLocal;
  self.videoLocal = nil;
  if (local) nw_connection_cancel(local);
  self.videoBuffer = nil;
  self.videoLatest = nil;
  self.videoCredits = 0;
}

- (void)lostVideo:(NSURLSessionWebSocketTask *)socket
{
  if (socket != self.videoSocket) return;
  self.videoSocket = nil;
  self.videoSending = NO;
  [self stopLocalVideo];
  [socket cancelWithCloseCode:NSURLSessionWebSocketCloseCodeGoingAway reason:nil];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), self.queue, ^{ [self connectVideo]; });
}

- (void)receiveVideo:(NSURLSessionWebSocketTask *)socket
{
  [socket receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *message, NSError *error) {
    dispatch_async(self.queue, ^{
      if (self.stopped || socket != self.videoSocket) return;
      if (error || message.type != NSURLSessionWebSocketMessageTypeString) {
        [self lostVideo:socket]; return;
      }
      if ([message.string isEqualToString:@"start"] || [message.string isEqualToString:@"start-ack"]) {
        self.videoAck = [message.string isEqualToString:@"start-ack"];
        [self startLocalVideo];
      }
      else if ([message.string isEqualToString:@"stop"]) [self stopLocalVideo];
      else if ([message.string isEqualToString:@"ack"]) {
        if (self.videoLocal && self.videoAck) {
          self.videoCredits = MIN(2, self.videoCredits + 1);
          [self sendVideo];
        }
      }
      else { [self lostVideo:socket]; return; }
      [self receiveVideo:socket];
    });
  }];
}

- (void)startLocalVideo
{
  if (self.videoLocal) return;
  self.videoCredits = 2;
  self.videoBuffer = [NSMutableData data];
  nw_parameters_t parameters = nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL,
                                                                 NW_PARAMETERS_DEFAULT_CONFIGURATION);
  nw_connection_t local = nw_connection_create(nw_endpoint_create_host("127.0.0.1", "9100"), parameters);
  self.videoLocal = local;
  nw_connection_set_queue(local, self.queue);
  __weak typeof(self) weakSelf = self;
  nw_connection_set_state_changed_handler(local, ^(nw_connection_state_t state, nw_error_t error) {
    __strong typeof(weakSelf) strongSelf = weakSelf;
    if (!strongSelf || strongSelf.videoLocal != local) return;
    if (state == nw_connection_state_ready) {
      static const char request[] = "GET / HTTP/1.0\r\nHost: 127.0.0.1:9100\r\n\r\n";
      dispatch_data_t payload = dispatch_data_create(request, sizeof(request) - 1, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
      nw_connection_send(local, payload, NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false, ^(nw_error_t sendError) {
        if (strongSelf.videoLocal != local) return;
        if (sendError) [strongSelf lostVideo:strongSelf.videoSocket];
        else [strongSelf readVideo:local];
      });
    } else if (state == nw_connection_state_failed) {
      [strongSelf lostVideo:strongSelf.videoSocket];
    }
  });
  nw_connection_start(local);
}

- (void)readVideo:(nw_connection_t)local
{
  nw_connection_receive(local, 1, 65536, ^(dispatch_data_t data, nw_content_context_t context, bool complete, nw_error_t error) {
    if (self.videoLocal != local) return;
    if (error || !data) { [self lostVideo:self.videoSocket]; return; }
    dispatch_data_apply(data, ^bool(dispatch_data_t region, size_t offset, const void *bytes, size_t length) {
      [self.videoBuffer appendBytes:bytes length:length];
      return true;
    });
    if (![self parseVideo] || complete) { [self lostVideo:self.videoSocket]; return; }
    [self sendVideo];
    [self readVideo:local];
  });
}

- (BOOL)parseVideo
{
  NSData *boundary = [@"--BoundaryString\r\n" dataUsingEncoding:NSASCIIStringEncoding];
  NSData *separator = [@"\r\n\r\n" dataUsingEncoding:NSASCIIStringEncoding];
  const NSUInteger limit = 512 * 1024 - 8;
  while (self.videoBuffer.length) {
    NSUInteger length = self.videoBuffer.length;
    NSRange start = [self.videoBuffer rangeOfData:boundary options:(NSDataSearchOptions)0 range:NSMakeRange(0, length)];
    if (start.location == NSNotFound) return length <= 8192;
    NSUInteger from = NSMaxRange(start);
    NSRange end = [self.videoBuffer rangeOfData:separator options:(NSDataSearchOptions)0 range:NSMakeRange(from, length - from)];
    if (end.location == NSNotFound) return length - from <= 8192;
    NSString *header = [[NSString alloc] initWithData:[self.videoBuffer subdataWithRange:NSMakeRange(from, end.location - from)]
                                             encoding:NSASCIIStringEncoding];
    NSInteger count = 0;
    for (NSString *line in [header componentsSeparatedByString:@"\r\n"]) {
      if ([line.lowercaseString hasPrefix:@"content-length:"]) count = [line substringFromIndex:15].integerValue;
    }
    if (count < 4 || (NSUInteger)count > limit) return NO;
    NSUInteger body = NSMaxRange(end);
    if ((NSUInteger)count > length - body) return YES;
    const uint8_t *bytes = (const uint8_t *)self.videoBuffer.bytes;
    if (bytes[body] != 0xff || bytes[body + 1] != 0xd8 || bytes[body + count - 2] != 0xff || bytes[body + count - 1] != 0xd9) return NO;
    uint64_t stamp = CFSwapInt64HostToBig((uint64_t)(NSDate.date.timeIntervalSince1970 * 1000));
    NSMutableData *latest = [NSMutableData dataWithBytes:&stamp length:sizeof(stamp)];
    [latest appendBytes:bytes + body length:(NSUInteger)count];
    self.videoLatest = latest;
    [self.videoBuffer replaceBytesInRange:NSMakeRange(0, body + (NSUInteger)count) withBytes:NULL length:0];
  }
  return YES;
}

- (void)sendVideo
{
  NSURLSessionWebSocketTask *socket = self.videoSocket;
  NSData *packet = self.videoLatest;
  if (!socket || packet.length < 12 || self.videoSending || (self.videoAck && !self.videoCredits)) return;
  self.videoLatest = nil;
  NSData *jpeg = FBRemoteJPEG([packet subdataWithRange:NSMakeRange(8, packet.length - 8)], 560, 0.38);
  if (!jpeg) return;
  NSMutableData *encoded = [[packet subdataWithRange:NSMakeRange(0, 8)] mutableCopy];
  [encoded appendData:jpeg];
  if (self.videoAck) self.videoCredits--;
  self.videoSending = YES;
  [socket sendMessage:[[NSURLSessionWebSocketMessage alloc] initWithData:encoded] completionHandler:^(NSError *error) {
    dispatch_async(self.queue, ^{
      if (socket != self.videoSocket) return;
      self.videoSending = NO;
      if (error) [self lostVideo:socket];
      else [self sendVideo];
    });
  }];
}

- (void)connect
{
  if (self.stopped) return;
  self.generation++;
  [self.seen removeAllObjects];
  NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:self.endpoint];
  [request setValue:[@"Bearer " stringByAppendingString:self.token] forHTTPHeaderField:@"Authorization"];
  self.socket = [self.session webSocketTaskWithRequest:request];
  self.socket.maximumMessageSize = 16 * 1024 * 1024;
  [self.socket resume];
  [self receive:self.generation];
  [self ping:self.generation];
}

- (void)lost:(NSUInteger)generation
{
  if (generation != self.generation || self.stopped) return;
  self.generation++;
  [self.localTask cancel];
  self.localTask = nil;
  self.busy = NO;
  [self.socket cancelWithCloseCode:NSURLSessionWebSocketCloseCodeGoingAway reason:nil];
  self.socket = nil;
  // Reconnect only the device channel; no command queue or replay.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), self.queue, ^{
    if (!self.stopped && !self.socket) [self connect];
  });
}

- (void)ping:(NSUInteger)generation
{
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), self.queue, ^{
    if (generation != self.generation || self.stopped) return;
    [self.socket sendPingWithPongReceiveHandler:^(NSError *error) {
      dispatch_async(self.queue, ^{
        if (error) [self lost:generation];
        else [self ping:generation];
      });
    }];
  });
}

- (void)receive:(NSUInteger)generation
{
  [self.socket receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *frame, NSError *error) {
    dispatch_async(self.queue, ^{
      if (generation != self.generation || self.stopped) return;
      if (error || frame.type != NSURLSessionWebSocketMessageTypeString) {
        NSInteger status = [self.socket.response isKindOfClass:NSHTTPURLResponse.class]
          ? ((NSHTTPURLResponse *)self.socket.response).statusCode : 0;
        NSLog(@"[RemoteTunnel] Connection closed (code=%ld HTTP=%ld)", (long)error.code, (long)status);
        [self lost:generation]; return;
      }
      NSData *data = [frame.string dataUsingEncoding:NSUTF8StringEncoding];
      id message = [NSJSONSerialization JSONObjectWithData:data options:(NSJSONReadingOptions)0 error:nil];
      if (![message isKindOfClass:NSDictionary.class] || ![self accept:message generation:generation]) {
        NSLog(@"[RemoteTunnel] Request rejected; connection revoked");
        [self lost:generation]; return;
      }
      [self receive:generation];
    });
  }];
}

- (BOOL)accept:(NSDictionary *)message generation:(NSUInteger)generation
{
  NSString *identifier = message[@"id"], *method = message[@"method"], *path = message[@"path"];
  NSNumber *expires = message[@"expires"];
  NSString *encoded = message[@"body"];
  NSRegularExpression *ids = [NSRegularExpression regularExpressionWithPattern:@"^[a-f0-9]{32}$" options:(NSRegularExpressionOptions)0 error:nil];
  if (self.busy || ![message[@"type"] isEqual:@"request"]
      || ![identifier isKindOfClass:NSString.class]
      || [ids numberOfMatchesInString:identifier options:(NSMatchingOptions)0 range:NSMakeRange(0, identifier.length)] != 1
      || [self.seen containsObject:identifier] || self.seen.count >= 100000
      || ![@[@"GET", @"POST", @"DELETE"] containsObject:method ?: @""]
      || ![path isKindOfClass:NSString.class] || path.length > 4096
      || ![path hasPrefix:@"/"] || [path hasPrefix:@"//"]
      || [path containsString:@"?"] || [path containsString:@"#"]
      || [[path.stringByRemovingPercentEncoding componentsSeparatedByString:@"/"] containsObject:@".."]
      || [path rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound
      || ![expires isKindOfClass:NSNumber.class]
      || expires.doubleValue < NSDate.date.timeIntervalSince1970
      || expires.doubleValue > NSDate.date.timeIntervalSince1970 + 30
      || ![encoded isKindOfClass:NSString.class]) return NO;
  NSData *body = [[NSData alloc] initWithBase64EncodedString:encoded options:(NSDataBase64DecodingOptions)0];
  if (!body || body.length > 1024 * 1024) return NO;
  NSString *decodedPath = path.stringByRemovingPercentEncoding;
  if (self.pairRequest && [decodedPath isEqualToString:@"/wda/claim/start"]) return NO;
  if ([decodedPath containsString:@"/wda/claim/"] &&
      ![@[@"/wda/claim/status", @"/wda/claim/start", @"/wda/claim/stop"] containsObject:decodedPath]) return NO;
  NSURL *url = [NSURL URLWithString:[@"http://127.0.0.1:8100" stringByAppendingString:path]];
  if (!url || ![url.host isEqual:@"127.0.0.1"] || url.port.integerValue != 8100) return NO;
  [self.seen addObject:identifier];
  self.busy = YES;
  NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
  request.HTTPMethod = method;
  request.timeoutInterval = MIN(6, MAX(0.1, expires.doubleValue - NSDate.date.timeIntervalSince1970));
  if (body.length) request.HTTPBody = body;
  [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
  self.localTask = [self.session dataTaskWithRequest:request completionHandler:
    ^(NSData *responseData, NSURLResponse *response, NSError *error) {
      dispatch_async(self.queue, ^{
        if (generation != self.generation || self.stopped) return;
        self.localTask = nil;
        if (error || ![response isKindOfClass:NSHTTPURLResponse.class]
            || responseData.length > 12 * 1024 * 1024) {
          NSLog(@"[RemoteTunnel] Local WDA response unavailable (code=%ld)", (long)error.code);
          [self lost:generation]; return;
        }
        NSInteger status = ((NSHTTPURLResponse *)response).statusCode;
        NSData *replyBody = responseData ?: NSData.data;
        if (status == 200 && [method isEqual:@"GET"] && [path hasSuffix:@"/screenshot"]) {
          NSData *preview = FBRemotePreviewJPEG(replyBody);
          if (preview) replyBody = preview;
        }
        NSDictionary *result = @{@"type": @"response", @"id": identifier,
          @"status": @(status), @"body": [replyBody base64EncodedStringWithOptions:(NSDataBase64EncodingOptions)0]};
        NSData *json = [NSJSONSerialization dataWithJSONObject:result options:(NSJSONWritingOptions)0 error:nil];
        NSURLSessionWebSocketMessage *frame = [[NSURLSessionWebSocketMessage alloc]
          initWithString:([[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding] ?: @"")];
        [self.socket sendMessage:frame completionHandler:^(NSError *sendError) {
          dispatch_async(self.queue, ^{
            if (sendError) [self lost:generation];
          });
        }];
        self.busy = NO;
      });
    }];
  [self.localTask resume];
  return YES;
}

// Do not forward credentials across redirects, or replay POST on redirects.
- (void)URLSession:(NSURLSession *)session webSocketTask:(NSURLSessionWebSocketTask *)webSocketTask
  didOpenWithProtocol:(NSString *)protocol
{
  dispatch_async(self.queue, ^{
    if (webSocketTask == self.videoSocket) NSLog(@"[RemoteTunnel] MJPEG WebSocket connected");
    else if (webSocketTask == self.socket) NSLog(@"[RemoteTunnel] WebSocket connected");
  });
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
  willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request
  completionHandler:(void (^)(NSURLRequest *))completionHandler
{
  completionHandler(nil);
}

- (void)stop
{
  dispatch_async(self.queue, ^{
    self.stopped = YES;
    if (self.pairObserver) [NSNotificationCenter.defaultCenter removeObserver:self.pairObserver];
    self.pairObserver = nil;
    [self.pairRequest cancel];
    self.pairRequest = nil;
    self.generation++;
    [self stopLocalVideo];
    [self.videoSocket cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil];
    self.videoSocket = nil;
    [self.localTask cancel];
    [self.socket cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil];
    [self.session invalidateAndCancel];
    self.session = nil;
  });
}
@end
