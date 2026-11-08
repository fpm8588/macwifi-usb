#pragma once
#include <stdint.h>
typedef uint8_t UInt8; typedef uint16_t UInt16; typedef uint32_t UInt32; typedef int32_t SInt32;
typedef const void* CFTypeRef; typedef struct __CFDict* CFMutableDictionaryRef; typedef const struct __CFNum* CFNumberRef;
typedef const struct __CFStr* CFStringRef; typedef const struct __CFAlloc* CFAllocatorRef; typedef const struct __CFUUID* CFUUIDRef;
typedef struct { UInt8 b[16]; } CFUUIDBytes; typedef int CFNumberType;
extern CFAllocatorRef kCFAllocatorDefault; enum { kCFNumberSInt32Type = 3 };
CFNumberRef CFNumberCreate(CFAllocatorRef, CFNumberType, const void*);
void CFDictionarySetValue(CFMutableDictionaryRef, const void*, const void*);
void CFRelease(CFTypeRef); CFStringRef __CFStr(const char*);
#define CFSTR(x) __CFStr(x)
CFUUIDBytes CFUUIDGetUUIDBytes(CFUUIDRef);
typedef int32_t HRESULT; typedef void* LPVOID;
#define S_OK 0
typedef long CFIndex; typedef unsigned long CFTypeID; typedef uint32_t CFStringEncoding;
enum { kCFStringEncodingUTF8 = 0x08000100 };
CFTypeID CFGetTypeID(CFTypeRef); CFTypeID CFNumberGetTypeID(void); CFTypeID CFStringGetTypeID(void);
int CFNumberGetValue(CFNumberRef, CFNumberType, void*); int CFStringGetCString(CFStringRef, char*, CFIndex, CFStringEncoding);
