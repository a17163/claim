// Native request gate checks. Run with macOS clang; no phone/UI operations.
#import "FBRemoteTunnel.m"

@interface FBRemoteTunnel (TestAccess)
- (BOOL)accept:(NSDictionary *)message generation:(NSUInteger)generation;
- (BOOL)parseVideo;
@end

int main(void)
{
  @autoreleasepool {
    FBRemoteTunnel *tunnel = [FBRemoteTunnel new];
    NSDictionary *valid = @{@"type": @"request", @"id": @"0123456789abcdef0123456789abcdef",
      @"method": @"GET", @"path": @"/status", @"body": @"",
      @"expires": @(NSDate.date.timeIntervalSince1970 + 5)};
    // Without a configured NSURLSession this sends no request; checks only the gate.
    NSCAssert([tunnel accept:valid generation:0], @"Valid gate rejected");
    NSCAssert(![tunnel accept:valid generation:0], @"Duplicate/busy request accepted");
    tunnel.busy = NO;
    NSCAssert(![tunnel accept:valid generation:0], @"Duplicate request accepted");
    for (NSString *path in @[@"//evil/status", @"/../status", @"/%2e%2e/status",
                              @"/status?token=x", @"http://evil/", @"/status\n",
                              @"/wda/claim/password", @"/wda/claim/%70assword", @"/wda/claim/configure", @"/wda/claim/pairing"]) {
      NSMutableDictionary *message = [valid mutableCopy];
      message[@"id"] = @"abcdef0123456789abcdef0123456789";
      message[@"path"] = path;
      NSCAssert(![tunnel accept:message generation:0], @"Unsafe path accepted");
    }
    NSMutableDictionary *expired = [valid mutableCopy];
    expired[@"id"] = @"abcdef0123456789abcdef0123456789";
    expired[@"expires"] = @(NSDate.date.timeIntervalSince1970 - 1);
    NSCAssert(![tunnel accept:expired generation:0], @"Expired request accepted");
    expired[@"expires"] = @(NSDate.date.timeIntervalSince1970 + 3600);
    NSCAssert(![tunnel accept:expired generation:0], @"Future deadline accepted");
    NSData *header = [@"--BoundaryString\r\nContent-Type: image/jpeg\r\nContent-Length: 5\r\n\r\n" dataUsingEncoding:NSASCIIStringEncoding];
    const uint8_t jpegA[] = {0xff, 0xd8, 'A', 0xff, 0xd9};
    const uint8_t jpegB[] = {0xff, 0xd8, 'B', 0xff, 0xd9};
    tunnel.videoBuffer = [header mutableCopy];
    [tunnel.videoBuffer appendBytes:jpegA length:2];
    NSCAssert([tunnel parseVideo] && !tunnel.videoLatest, @"Partial JPEG was published");
    [tunnel.videoBuffer appendBytes:jpegA + 2 length:3];
    [tunnel.videoBuffer appendData:header];
    [tunnel.videoBuffer appendBytes:jpegB length:sizeof(jpegB)];
    NSCAssert([tunnel parseVideo] && tunnel.videoLatest.length == 13 && tunnel.videoBuffer.length == 0,
              @"MJPEG frames were not parsed");
    NSCAssert(((const uint8_t *)tunnel.videoLatest.bytes)[10] == 'B', @"Latest frame did not replace the old frame");
    puts("Native tunnel request gate and latest MJPEG frame checks passed");
  }
  return 0;
}
