#pragma once
#include <CoreFoundation/CoreFoundation.h>
typedef int kern_return_t; typedef kern_return_t IOReturn; typedef UInt32 io_service_t; typedef UInt32 mach_port_t;
#define KERN_SUCCESS 0
#define kIOReturnSuccess 0
#define kIOReturnExclusiveAccess ((IOReturn)0xe00002c5)
extern const mach_port_t kIOMainPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char*);
io_service_t IOServiceGetMatchingService(mach_port_t, CFMutableDictionaryRef);
kern_return_t IOObjectRelease(io_service_t);
typedef UInt32 io_iterator_t; typedef UInt32 io_registry_entry_t;
#define kIOReturnError ((IOReturn)0xe00002bc)
#define kIOReturnNoDevice ((IOReturn)0xe00002c0)
#define kIOReturnNotResponding ((IOReturn)0xe00002ed)
kern_return_t IOServiceGetMatchingServices(mach_port_t, CFMutableDictionaryRef, io_iterator_t*);
io_service_t IOIteratorNext(io_iterator_t);
CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t, CFStringRef, CFAllocatorRef, UInt32);
kern_return_t IOObjectRetain(io_service_t);
typedef UInt32 IOOptionBits; typedef const char *io_name_t_; 
#define kIOServicePlane "IOService"
enum { kIORegistryIterateRecursively = 1 };
CFTypeRef IORegistryEntrySearchCFProperty(io_registry_entry_t, const char*, CFStringRef, CFAllocatorRef, IOOptionBits);
#define kIOReturnTimeout ((IOReturn)0xe00002d6)
