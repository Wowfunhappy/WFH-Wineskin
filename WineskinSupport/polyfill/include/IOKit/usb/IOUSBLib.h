/*
 * IOUSBLib.h with the USB host names the 10.11 SDK adds (IOUSBHostFamilyDefinitions.h). The matching
 * properties are the USB descriptor fields 10.9's USBSpec.h names as well; 10.9 has no IOUSBHostInterface
 * class, so nothing conforms to it.
 */
#include_next <IOKit/usb/IOUSBLib.h>

#if !defined(WINESKIN_IOUSBHOST_H) && MAC_OS_X_VERSION_MAX_ALLOWED < 101100
#define WINESKIN_IOUSBHOST_H

#define kIOUSBHostDeviceClassName                   "IOUSBHostDevice"
#define kIOUSBHostInterfaceClassName                "IOUSBHostInterface"

#define kUSBHostMatchingPropertyVendorID            "idVendor"
#define kUSBHostMatchingPropertyProductID           "idProduct"
#define kUSBHostMatchingPropertyDeviceReleaseNumber "bcdDevice"
#define kUSBHostMatchingPropertyDeviceClass         "bDeviceClass"
#define kUSBHostMatchingPropertyDeviceSubClass      "bDeviceSubClass"
#define kUSBHostMatchingPropertyDeviceProtocol      "bDeviceProtocol"
#define kUSBHostMatchingPropertyConfigurationValue  "bConfigurationValue"
#define kUSBHostMatchingPropertyInterfaceNumber     "bInterfaceNumber"
#define kUSBHostMatchingPropertyInterfaceClass      "bInterfaceClass"
#define kUSBHostMatchingPropertyInterfaceSubClass   "bInterfaceSubClass"
#define kUSBHostMatchingPropertyInterfaceProtocol   "bInterfaceProtocol"

#endif
