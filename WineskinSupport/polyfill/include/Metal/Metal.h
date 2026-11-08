/*
 * Metal (10.11), which 10.9 does not have: the declarations its callers compile against. 10.9 is a Mac
 * without a Metal device, and src/objc/Metal.m answers as Metal does on one. Images link the stub
 * Metal.framework (polyfill/build/Frameworks) weakly.
 */
#ifndef WINESKIN_METAL_H
#define WINESKIN_METAL_H

#import <Foundation/Foundation.h>

typedef NS_ENUM(NSInteger, MTLGPUFamily) {
    MTLGPUFamilyApple1 = 1001,
    MTLGPUFamilyApple2 = 1002,
    MTLGPUFamilyApple3 = 1003,
    MTLGPUFamilyApple4 = 1004,
    MTLGPUFamilyApple5 = 1005,
    MTLGPUFamilyApple6 = 1006,
    MTLGPUFamilyApple7 = 1007,
    MTLGPUFamilyApple8 = 1008,
    MTLGPUFamilyApple9 = 1009,
    MTLGPUFamilyMac1 = 2001,
    MTLGPUFamilyMac2 = 2002,
    MTLGPUFamilyCommon1 = 3001,
    MTLGPUFamilyCommon2 = 3002,
    MTLGPUFamilyCommon3 = 3003,
    MTLGPUFamilyMetal3 = 5001,
};

typedef NS_ENUM(NSUInteger, MTLPixelFormat) {
    MTLPixelFormatInvalid = 0,
    MTLPixelFormatBGRA8Unorm = 80,
    MTLPixelFormatBGRA8Unorm_sRGB = 81,
    MTLPixelFormatRGBA16Float = 115,
};

@protocol MTLDevice <NSObject>
@property (readonly) NSString *name;
@property (readonly) uint64_t registryID;
@property (readonly, getter=isLowPower) BOOL lowPower;
@property (readonly, getter=isHeadless) BOOL headless;
@property (readonly, getter=isRemovable) BOOL removable;
- (BOOL)supportsFamily:(MTLGPUFamily)gpuFamily;
@end

#ifdef __cplusplus
extern "C" {
#endif
extern id<MTLDevice> MTLCreateSystemDefaultDevice(void);
extern NSArray *MTLCopyAllDevices(void);
#ifdef __cplusplus
}
#endif

#endif
