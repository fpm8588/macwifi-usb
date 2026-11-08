#pragma once
#include <IOKit/IOKitLib.h>
typedef struct IOCFPlugInInterface { void* _r; HRESULT (*QueryInterface)(void*, CFUUIDBytes, LPVOID*); } IOCFPlugInInterface;
extern CFUUIDRef kIOCFPlugInInterfaceID;
kern_return_t IOCreatePlugInInterfaceForService(io_service_t, CFUUIDRef, CFUUIDRef, IOCFPlugInInterface***, SInt32*);
kern_return_t IODestroyPlugInInterface(IOCFPlugInInterface**);
