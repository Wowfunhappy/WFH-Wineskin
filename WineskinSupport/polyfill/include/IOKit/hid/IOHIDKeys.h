/* IOHIDKeys.h with the transport values later SDKs name: the strings HID devices report in their
   Transport property, which 10.9's report as well. */
#include_next <IOKit/hid/IOHIDKeys.h>

#if !defined(WINESKIN_IOHIDKEYS_H) && !defined(kIOHIDTransportUSBValue)
#define WINESKIN_IOHIDKEYS_H
#define kIOHIDTransportUSBValue                 "USB"
#define kIOHIDTransportBluetoothValue           "Bluetooth"
#define kIOHIDTransportBluetoothLowEnergyValue  "Bluetooth Low Energy"
#define kIOHIDTransportAIDBValue                "AID"
#define kIOHIDTransportI2CValue                 "I2C"
#define kIOHIDTransportSPIValue                 "SPI"
#define kIOHIDTransportSerialValue              "Serial"
#define kIOHIDTransportIAPValue                 "iAP"
#define kIOHIDTransportAirPlayValue             "AirPlay"
#define kIOHIDTransportSPUValue                 "SPU"
#endif
