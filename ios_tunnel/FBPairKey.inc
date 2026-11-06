#import <Foundation/Foundation.h>
#import <Security/Security.h>

static NSDictionary *FBPairCredentialQuery(void)
{
  return @{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
           (__bridge id)kSecAttrService: @"WalletClick.PhoneEnrollment.v1",
           (__bridge id)kSecAttrAccount: @"relay"};
}

static NSDictionary *FBPairCredentials(void)
{
  NSMutableDictionary *query = [FBPairCredentialQuery() mutableCopy];
  query[(__bridge id)kSecReturnData] = @YES;
  CFTypeRef result = NULL;
  if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &result) != errSecSuccess) return nil;
  NSData *data = CFBridgingRelease(result);
  id value = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:NULL error:nil];
  return [value isKindOfClass:NSDictionary.class] ? value : nil;
}

static inline BOOL FBPairSave(NSDictionary *value)
{
  NSData *data = [NSPropertyListSerialization dataWithPropertyList:value format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
  if (!data) return NO;
  NSMutableDictionary *query = [FBPairCredentialQuery() mutableCopy];
  NSDictionary *fields = @{(__bridge id)kSecValueData: data};
  OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query, (__bridge CFDictionaryRef)fields);
  if (status == errSecItemNotFound) {
    [query addEntriesFromDictionary:fields];
    query[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleWhenUnlockedThisDeviceOnly;
    status = SecItemAdd((__bridge CFDictionaryRef)query, NULL);
  }
  return status == errSecSuccess;
}

static SecKeyRef FBClaimStartKey(BOOL create) CF_RETURNS_RETAINED
{
  NSData *tag = [@"WalletClick.PhoneClaim.Start.v1" dataUsingEncoding:NSUTF8StringEncoding];
  NSDictionary *query = @{(__bridge id)kSecClass: (__bridge id)kSecClassKey,
                          (__bridge id)kSecAttrApplicationTag: tag,
                          (__bridge id)kSecAttrKeyType: (__bridge id)kSecAttrKeyTypeRSA,
                          (__bridge id)kSecAttrKeyClass: (__bridge id)kSecAttrKeyClassPrivate,
                          (__bridge id)kSecReturnRef: @YES};
  CFTypeRef value = NULL;
  OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &value);
  if (status == errSecSuccess) return (SecKeyRef)value;
  if (!create || status != errSecItemNotFound) return NULL;
  NSDictionary *attributes = @{(__bridge id)kSecAttrKeyType: (__bridge id)kSecAttrKeyTypeRSA,
      (__bridge id)kSecAttrKeySizeInBits: @4096,
      (__bridge id)kSecPrivateKeyAttrs: @{(__bridge id)kSecAttrIsPermanent: @YES,
          (__bridge id)kSecAttrApplicationTag: tag,
          (__bridge id)kSecAttrAccessible: (__bridge id)kSecAttrAccessibleWhenUnlockedThisDeviceOnly}};
  return SecKeyCreateRandomKey((__bridge CFDictionaryRef)attributes, NULL);
}

static NSString *FBPairPublicKey(void)
{
  SecKeyRef secret = FBClaimStartKey(YES);
  if (!secret) return nil;
  SecKeyRef publicKey = SecKeyCopyPublicKey(secret);
  CFRelease(secret);
  if (!publicKey) return nil;
  NSData *data = CFBridgingRelease(SecKeyCopyExternalRepresentation(publicKey, NULL));
  CFRelease(publicKey);
  if (!data.length) return nil;
  return [data base64EncodedStringWithOptions:(NSDataBase64EncodingOptions)0];
}
