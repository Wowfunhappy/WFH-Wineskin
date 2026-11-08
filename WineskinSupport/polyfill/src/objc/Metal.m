/*
 * Metal (10.11) on 10.9, a Mac without a Metal device: no system default device, and no devices.
 */
#import <Metal/Metal.h>

id<MTLDevice> MTLCreateSystemDefaultDevice(void)
{
    return nil;
}

NSArray *MTLCopyAllDevices(void)
{
    return [[NSArray alloc] init];
}
