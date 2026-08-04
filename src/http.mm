#import <Foundation/Foundation.h>

#include "http.hpp"

#include <algorithm>
#include <cctype>

namespace mpxadrv {
namespace {

bool hasCaseInsensitivePrefix(const std::string& value, const char* prefix) {
  const std::size_t length = std::strlen(prefix);
  if (value.size() < length) {
    return false;
  }
  for (std::size_t index = 0; index < length; ++index) {
    if (std::tolower(static_cast<unsigned char>(value[index])) !=
        std::tolower(static_cast<unsigned char>(prefix[index]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool isHttpUrl(const std::string& value) {
  return hasCaseInsensitivePrefix(value, "http://") ||
         hasCaseInsensitivePrefix(value, "https://");
}

std::vector<std::uint8_t> fetchUrl(
    const std::string& url,
    std::optional<std::pair<std::size_t, std::size_t>> byteRange) {
  @autoreleasepool {
    NSString* urlString =
        [NSString stringWithUTF8String:url.c_str()];
    if (urlString == nil) {
      throw HttpError("invalid URL: " + url);
    }
    NSURL* nsUrl = [NSURL URLWithString:urlString];
    if (nsUrl == nil) {
      throw HttpError("invalid URL: " + url);
    }

    NSMutableURLRequest* request =
        [NSMutableURLRequest requestWithURL:nsUrl];
    [request setHTTPMethod:@"GET"];
    [request setTimeoutInterval:120.0];
    if (byteRange) {
      NSString* rangeValue = [NSString
          stringWithFormat:@"bytes=%zu-%zu", byteRange->first, byteRange->second];
      [request setValue:rangeValue forHTTPHeaderField:@"Range"];
    }

    __block NSData* responseData = nil;
    __block NSError* responseError = nil;
    __block NSInteger statusCode = 0;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

    NSURLSessionDataTask* task = [[NSURLSession sharedSession]
        dataTaskWithRequest:request
          completionHandler:^(NSData* data, NSURLResponse* response,
                              NSError* error) {
            responseError = error;
            responseData = data;
            if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
              statusCode =
                  static_cast<NSInteger>(
                      [(NSHTTPURLResponse*)response statusCode]);
            }
            dispatch_semaphore_signal(semaphore);
          }];
    [task resume];
    dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);

    if (responseError != nil) {
      const char* message =
          responseError.localizedDescription.UTF8String != nullptr
              ? responseError.localizedDescription.UTF8String
              : "network request failed";
      throw HttpError(std::string("HTTP fetch failed for ") + url + ": " +
                      message);
    }

    const bool ranged = byteRange.has_value();
    if (statusCode < 200 || statusCode >= 300) {
      if (!(ranged && statusCode == 206)) {
        throw HttpError("HTTP " + std::to_string(statusCode) + " for " + url);
      }
    }
    if (responseData == nil || responseData.length == 0) {
      throw HttpError("empty HTTP response for " + url);
    }

    const auto* bytes = static_cast<const std::uint8_t*>(responseData.bytes);
    return std::vector<std::uint8_t>(bytes, bytes + responseData.length);
  }
}

}  // namespace mpxadrv
