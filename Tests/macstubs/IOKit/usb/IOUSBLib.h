#pragma once
#include <IOKit/IOCFPlugIn.h>
#define kIOUSBDeviceClassName "IOUSBDevice"
#define kUSBVendorID "idVendor"
#define kUSBProductID "idProduct"
extern CFUUIDRef kIOUSBDeviceUserClientTypeID, kIOUSBDeviceInterfaceID320;
enum { kUSBDeviceSpeedLow, kUSBDeviceSpeedFull, kUSBDeviceSpeedHigh };
typedef struct { UInt8 bLength, bDescriptorType; UInt16 wTotalLength; UInt8 bNumInterfaces; UInt8 bConfigurationValue; } IOUSBConfigurationDescriptor, *IOUSBConfigurationDescriptorPtr;
#define USBToHostWord(x) (x)
typedef struct { UInt8 bmRequestType, bRequest; UInt16 wValue, wIndex, wLength; void* pData; UInt32 wLenDone, noDataTimeout, completionTimeout; } IOUSBDevRequestTO;
enum { kIOUSBFindInterfaceDontCare = 0xFFFF };
typedef struct { UInt16 bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, bAlternateSetting; } IOUSBFindInterfaceRequest;
typedef struct IOUSBDeviceInterface320 {
  void* _r; HRESULT (*QueryInterface)(void*, CFUUIDBytes, LPVOID*); UInt32 (*AddRef)(void*); UInt32 (*Release)(void*);
  IOReturn (*USBDeviceOpen)(void*); IOReturn (*USBDeviceClose)(void*);
  IOReturn (*GetDeviceVendor)(void*, UInt16*); IOReturn (*GetDeviceProduct)(void*, UInt16*); IOReturn (*GetDeviceReleaseNumber)(void*, UInt16*);
  IOReturn (*GetLocationID)(void*, UInt32*); IOReturn (*GetDeviceSpeed)(void*, UInt8*);
  IOReturn (*GetConfigurationDescriptorPtr)(void*, UInt8, IOUSBConfigurationDescriptorPtr*);
  IOReturn (*DeviceRequestTO)(void*, IOUSBDevRequestTO*); IOReturn (*USBDeviceReEnumerate)(void*, UInt32); IOReturn (*GetConfiguration)(void*, UInt8*); IOReturn (*SetConfiguration)(void*, UInt8); IOReturn (*CreateInterfaceIterator)(void*, IOUSBFindInterfaceRequest*, io_iterator_t*);
} IOUSBDeviceInterface320;
extern CFUUIDRef kIOUSBInterfaceUserClientTypeID, kIOUSBInterfaceInterfaceID300;
enum { kUSBOut = 0, kUSBIn = 1, kUSBBulk = 2 };
#define kIOUSBPipeStalled ((IOReturn)0xe000404f)
#define kIOUSBTransactionTimeout ((IOReturn)0xe0004051)
typedef struct IOUSBInterfaceInterface300 {
  void* _r; HRESULT (*QueryInterface)(void*, CFUUIDBytes, LPVOID*); UInt32 (*AddRef)(void*); UInt32 (*Release)(void*);
  IOReturn (*USBInterfaceOpen)(void*); IOReturn (*USBInterfaceClose)(void*);
  IOReturn (*GetInterfaceClass)(void*, UInt8*); IOReturn (*GetNumEndpoints)(void*, UInt8*);
  IOReturn (*GetPipeProperties)(void*, UInt8, UInt8*, UInt8*, UInt8*, UInt16*, UInt8*);
  IOReturn (*WritePipeTO)(void*, UInt8, void*, UInt32, UInt32, UInt32);
  IOReturn (*ReadPipeTO)(void*, UInt8, void*, UInt32*, UInt32, UInt32);
  IOReturn (*ClearPipeStallBothEnds)(void*, UInt8);
} IOUSBInterfaceInterface300;
