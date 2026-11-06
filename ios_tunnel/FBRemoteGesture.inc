#if !TARGET_OS_TV && !TARGET_OS_WATCH
+ (NSArray *)remoteGeometryForApplication:(XCUIApplication *)application
{
  NSString *bundle = application.bundleID;
  UIInterfaceOrientation orientation = application.interfaceOrientation;
  if (!bundle.length || orientation == UIInterfaceOrientationUnknown) return nil;
  CGSize size = FBAdjustDimensionsForApplication(application.wdFrame.size, orientation);
  if (size.width <= 0 || size.height <= 0) return nil;
  return @[@(size.width), @(size.height), @(orientation), bundle];
}

+ (id<FBResponsePayload>)handleRemoteGeometry:(FBRouteRequest *)request
{
  if (XCUIDevice.sharedDevice.fb_isScreenLocked) return FBResponseWithObject(@{@"blocked": @"locked"});
  NSArray *geometry = [self remoteGeometryForApplication:request.session.activeApplication];
  return FBResponseWithObject(geometry ? @{@"geometry": geometry} : @{@"blocked": @"changed"});
}

+ (id<FBResponsePayload>)handleRemoteGesture:(FBRouteRequest *)request
{
  id stamp = request.arguments[@"capturedAt"];
  id expected = request.arguments[@"geometry"];
  if (![stamp isKindOfClass:NSNumber.class] || ![expected isKindOfClass:NSArray.class]) {
    return FBResponseWithObject(@{@"blocked": @"invalid"});
  }
  NSTimeInterval captureTime = [stamp doubleValue];
  NSTimeInterval age = NSDate.date.timeIntervalSince1970 - captureTime;
  if (!(age >= -1 && age <= 5)) return FBResponseWithObject(@{@"blocked": @"expired"});
  if (XCUIDevice.sharedDevice.fb_isScreenLocked) return FBResponseWithObject(@{@"blocked": @"locked"});
  XCUIApplication *application = request.session.activeApplication;
  if (![[self remoteGeometryForApplication:application] isEqual:expected]) {
    return FBResponseWithObject(@{@"blocked": @"changed"});
  }
  if (NSDate.date.timeIntervalSince1970 - captureTime > 5) return FBResponseWithObject(@{@"blocked": @"expired"});
  return [self handlePerformW3CTouchActions:request];
}
#endif
