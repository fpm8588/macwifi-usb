// MacWifiAudio.c — a Core Audio output device, "UGREEN Bluetooth", for headphones connected through the adapter.
// Original code (GPL-2.0), written against Apple's AudioServerPlugIn.h interface.
//
// It's an AudioServerPlugIn (a user-space HAL driver loaded by coreaudiod, no kext, no paid signing): one device,
// one stereo output stream at 44.1 kHz, one volume control. What apps play into it is mixed by Core Audio and handed
// to DoIOOperation (WriteMix); we apply the volume, convert to 16-bit and send it to macwifid on 127.0.0.1 in
// small UDP datagrams (Sources/Shared/pcmring.h). macwifid buffers it, encodes SBC and streams it to the headphones.
// When nothing is connected the audio is simply dropped by macwifid.
//
// The device clock is the host clock (mach_absolute_time) at the nominal rate, which is also what macwifid paces
// the Bluetooth stream by, so the two never drift apart.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../Sources/Shared/pcmring.h"

enum { kObjPlugIn = kAudioObjectPlugInObject, kObjDevice = 2, kObjStream = 3, kObjVolume = 4 };

#define kDeviceUID      "io.github.fpm8588.macwifi.bluetooth"
#define kModelUID       "io.github.fpm8588.macwifi.bluetooth.model"
#define kDeviceName     "UGREEN Bluetooth"
#define kManufacturer   "macwifi-usb"
#define kRate           44100.0
#define kChannels       2
#define kPeriodFrames   16384            // zero time stamp period
#define kLatencyFrames  8820             // ~200 ms: jitter buffer + SBC packets + the radio (for A/V sync)
#define kMinDb          (-64.0)

static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gMu = PTHREAD_MUTEX_INITIALIZER;
static UInt32 gRefs = 1;
static UInt32 gIOClients;
static Float64 gHostTicksPerFrame;
static UInt64 gAnchorHost, gNumberTimeStamps;
static _Atomic float gScalar = 0.75f;    // volume slider position, 0..1
static int gSock = -1;
static uint32_t gSeq;

// ---------------------------------------------------------------- volume curve

static Float32 scalar_to_db(Float32 s) {
    if (s <= 0) return (Float32)kMinDb;
    Float32 g = s * s;                   // a squared curve feels even across the slider
    Float32 db = 20.0f * log10f(g);
    return db < kMinDb ? (Float32)kMinDb : db > 0 ? 0 : db;
}
static Float32 db_to_scalar(Float32 db) {
    if (db <= kMinDb) return 0;
    if (db > 0) db = 0;
    return sqrtf(powf(10.0f, db / 20.0f));
}

// ---------------------------------------------------------------- IUnknown

static HRESULT QueryInterface(void *drv, REFIID uuid, LPVOID *out);
static ULONG AddRef(void *drv);
static ULONG Release(void *drv);
static OSStatus Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host);
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci,
                             AudioObjectID *out) { (void)d; (void)desc; (void)ci; (void)out; return kAudioHardwareUnsupportedOperationError; }
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id) { (void)d; (void)id; return kAudioHardwareUnsupportedOperationError; }
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *ci) {
    (void)d; (void)ci;
    return id == kObjDevice ? 0 : kAudioHardwareBadObjectError;
}
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *ci) {
    (void)d; (void)ci;
    return id == kObjDevice ? 0 : kAudioHardwareBadObjectError;
}
static OSStatus PerformConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 a, void *i) { (void)d; (void)id; (void)a; (void)i; return 0; }
static OSStatus AbortConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 a, void *i) { (void)d; (void)id; (void)a; (void)i; return 0; }
static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a);
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *out);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a,
                                UInt32 qs, const void *q, UInt32 in, UInt32 *outSize, void *out);
static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a,
                                UInt32 qs, const void *q, UInt32 n, const void *data);
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client);
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client);
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, Float64 *st, UInt64 *ht, UInt64 *seed);
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace);
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, UInt32 frames,
                                 const AudioServerPlugInIOCycleInfo *ci) { (void)d; (void)id; (void)client; (void)op; (void)frames; (void)ci; return 0; }
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream, UInt32 client, UInt32 op,
                              UInt32 frames, const AudioServerPlugInIOCycleInfo *ci, void *main, void *secondary);
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, UInt32 frames,
                               const AudioServerPlugInIOCycleInfo *ci) { (void)d; (void)id; (void)client; (void)op; (void)frames; (void)ci; return 0; }

static AudioServerPlugInDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformConfigChange, AbortConfigChange, HasProperty, IsPropertySettable, GetPropertyDataSize, GetPropertyData,
    SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation,
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriver = &gInterfacePtr;

// The factory named in Info.plist.
void *MacWifiAudio_Create(CFAllocatorRef alloc, CFUUIDRef type);
void *MacWifiAudio_Create(CFAllocatorRef alloc, CFUUIDRef type) {
    (void)alloc;
    return CFEqual(type, kAudioServerPlugInTypeUUID) ? gDriver : NULL;
}

static HRESULT QueryInterface(void *drv, REFIID uuid, LPVOID *out) {
    if (drv != gDriver || !out) return kAudioHardwareBadObjectError;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gMu);
        gRefs++;
        pthread_mutex_unlock(&gMu);
        *out = gDriver;
        r = S_OK;
    }
    CFRelease(req);
    return r;
}
static ULONG AddRef(void *drv) {
    if (drv != gDriver) return 0;
    pthread_mutex_lock(&gMu);
    ULONG r = ++gRefs;
    pthread_mutex_unlock(&gMu);
    return r;
}
static ULONG Release(void *drv) {
    if (drv != gDriver) return 0;
    pthread_mutex_lock(&gMu);
    ULONG r = gRefs > 0 ? --gRefs : 0;
    pthread_mutex_unlock(&gMu);
    return r;
}

static OSStatus Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host) {
    if (drv != gDriver) return kAudioHardwareBadObjectError;
    gHost = host;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    Float64 ticksPerSecond = 1e9 * (Float64)tb.denom / (Float64)tb.numer;
    gHostTicksPerFrame = ticksPerSecond / kRate;
    return 0;
}

// ---------------------------------------------------------------- properties

static AudioStreamBasicDescription format(void) {
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * kChannels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4 * kChannels;
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 32;
    return f;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a) {
    (void)pid;
    if (d != gDriver || !a) return false;
    UInt32 sz = 0;
    return GetPropertyDataSize(d, id, pid, a, 0, NULL, &sz) == 0;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a,
                                   Boolean *out) {
    (void)pid;
    if (d != gDriver || !a || !out) return kAudioHardwareIllegalOperationError;
    UInt32 sz = 0;
    if (GetPropertyDataSize(d, id, pid, a, 0, NULL, &sz) != 0) return kAudioHardwareUnknownPropertyError;
    *out = (id == kObjVolume && (a->mSelector == kAudioLevelControlPropertyScalarValue ||
                                 a->mSelector == kAudioLevelControlPropertyDecibelValue)) ||
           (id == kObjDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate) ||
           (id == kObjStream && (a->mSelector == kAudioStreamPropertyVirtualFormat ||
                                 a->mSelector == kAudioStreamPropertyPhysicalFormat));
    return 0;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *out) {
    (void)pid; (void)qs; (void)q;
    if (d != gDriver || !a || !out) return kAudioHardwareIllegalOperationError;
    UInt32 sel = a->mSelector;
    switch (id) {
    case kObjPlugIn:
        switch (sel) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *out = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyManufacturer: case kAudioPlugInPropertyResourceBundle: *out = sizeof(CFStringRef); return 0;
        case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyDeviceList: *out = sizeof(AudioObjectID); return 0;
        case kAudioPlugInPropertyTranslateUIDToDevice: *out = sizeof(AudioObjectID); return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kObjDevice:
        switch (sel) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *out = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner: case kAudioDevicePropertyRelatedDevices: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID: *out = sizeof(CFStringRef); return 0;
        case kAudioObjectPropertyOwnedObjects:
            *out = a->mScope == kAudioObjectPropertyScopeInput ? 0 : 2 * sizeof(AudioObjectID); return 0;
        case kAudioDevicePropertyStreams:
            *out = a->mScope == kAudioObjectPropertyScopeInput ? 0 : sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyControlList: *out = sizeof(AudioObjectID); return 0;
        case kAudioDevicePropertyTransportType: case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyIsHidden: case kAudioDevicePropertyZeroTimeStampPeriod:
            *out = sizeof(UInt32); return 0;
        case kAudioDevicePropertyNominalSampleRate: *out = sizeof(Float64); return 0;
        case kAudioDevicePropertyAvailableNominalSampleRates: *out = sizeof(AudioValueRange); return 0;
        case kAudioDevicePropertyPreferredChannelsForStereo: *out = 2 * sizeof(UInt32); return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kObjStream:
        switch (sel) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *out = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyOwnedObjects: *out = 0; return 0;
        case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency: *out = sizeof(UInt32); return 0;
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
            *out = sizeof(AudioStreamBasicDescription); return 0;
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            *out = sizeof(AudioStreamRangedDescription); return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kObjVolume:
        switch (sel) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *out = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyOwnedObjects: *out = 0; return 0;
        case kAudioControlPropertyScope: *out = sizeof(AudioObjectPropertyScope); return 0;
        case kAudioControlPropertyElement: *out = sizeof(AudioObjectPropertyElement); return 0;
        case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyConvertScalarToDecibels: case kAudioLevelControlPropertyConvertDecibelsToScalar:
            *out = sizeof(Float32); return 0;
        case kAudioLevelControlPropertyDecibelRange: *out = sizeof(AudioValueRange); return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    default:
        return kAudioHardwareBadObjectError;
    }
}

#define NEED(T) do { if (in < sizeof(T)) return kAudioHardwareBadPropertySizeError; *outSize = sizeof(T); } while (0)

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a,
                                UInt32 qs, const void *q, UInt32 in, UInt32 *outSize, void *out) {
    (void)pid;
    if (d != gDriver || !a || !outSize || !out) return kAudioHardwareIllegalOperationError;
    UInt32 sel = a->mSelector;
    switch (id) {
    case kObjPlugIn:
        switch (sel) {
        case kAudioObjectPropertyBaseClass: NEED(AudioClassID); *(AudioClassID *)out = kAudioObjectClassID; return 0;
        case kAudioObjectPropertyClass: NEED(AudioClassID); *(AudioClassID *)out = kAudioPlugInClassID; return 0;
        case kAudioObjectPropertyOwner: NEED(AudioObjectID); *(AudioObjectID *)out = kAudioObjectUnknown; return 0;
        case kAudioObjectPropertyManufacturer: NEED(CFStringRef); *(CFStringRef *)out = CFSTR(kManufacturer); return 0;
        case kAudioPlugInPropertyResourceBundle: NEED(CFStringRef); *(CFStringRef *)out = CFSTR(""); return 0;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            if (in < sizeof(AudioObjectID)) { *outSize = 0; return 0; }
            *(AudioObjectID *)out = kObjDevice;
            *outSize = sizeof(AudioObjectID);
            return 0;
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            NEED(AudioObjectID);
            CFStringRef uid = (qs == sizeof(CFStringRef) && q) ? *(const CFStringRef *)q : NULL;
            *(AudioObjectID *)out = uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo ? kObjDevice
                                                                                                         : kAudioObjectUnknown;
            return 0;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kObjDevice:
        switch (sel) {
        case kAudioObjectPropertyBaseClass: NEED(AudioClassID); *(AudioClassID *)out = kAudioObjectClassID; return 0;
        case kAudioObjectPropertyClass: NEED(AudioClassID); *(AudioClassID *)out = kAudioDeviceClassID; return 0;
        case kAudioObjectPropertyOwner: NEED(AudioObjectID); *(AudioObjectID *)out = kObjPlugIn; return 0;
        case kAudioObjectPropertyName: NEED(CFStringRef); *(CFStringRef *)out = CFSTR(kDeviceName); return 0;
        case kAudioObjectPropertyManufacturer: NEED(CFStringRef); *(CFStringRef *)out = CFSTR(kManufacturer); return 0;
        case kAudioDevicePropertyDeviceUID: NEED(CFStringRef); *(CFStringRef *)out = CFSTR(kDeviceUID); return 0;
        case kAudioDevicePropertyModelUID: NEED(CFStringRef); *(CFStringRef *)out = CFSTR(kModelUID); return 0;
        case kAudioObjectPropertyOwnedObjects: {
            if (a->mScope == kAudioObjectPropertyScopeInput) { *outSize = 0; return 0; }
            AudioObjectID ids[2] = {kObjStream, kObjVolume};
            UInt32 n = in / sizeof(AudioObjectID);
            if (n > 2) n = 2;
            memcpy(out, ids, n * sizeof(AudioObjectID));
            *outSize = n * (UInt32)sizeof(AudioObjectID);
            return 0;
        }
        case kAudioDevicePropertyStreams:
            if (a->mScope == kAudioObjectPropertyScopeInput || in < sizeof(AudioObjectID)) { *outSize = 0; return 0; }
            *(AudioObjectID *)out = kObjStream;
            *outSize = sizeof(AudioObjectID);
            return 0;
        case kAudioObjectPropertyControlList:
            if (in < sizeof(AudioObjectID)) { *outSize = 0; return 0; }
            *(AudioObjectID *)out = kObjVolume;
            *outSize = sizeof(AudioObjectID);
            return 0;
        case kAudioDevicePropertyRelatedDevices: NEED(AudioObjectID); *(AudioObjectID *)out = kObjDevice; return 0;
        case kAudioDevicePropertyTransportType: NEED(UInt32); *(UInt32 *)out = kAudioDeviceTransportTypeBluetooth; return 0;
        case kAudioDevicePropertyClockDomain: NEED(UInt32); *(UInt32 *)out = 0; return 0;
        case kAudioDevicePropertyDeviceIsAlive: NEED(UInt32); *(UInt32 *)out = 1; return 0;
        case kAudioDevicePropertyDeviceIsRunning:
            NEED(UInt32);
            pthread_mutex_lock(&gMu);
            *(UInt32 *)out = gIOClients > 0;
            pthread_mutex_unlock(&gMu);
            return 0;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            NEED(UInt32);
            *(UInt32 *)out = a->mScope != kAudioObjectPropertyScopeInput;   // output only
            return 0;
        case kAudioDevicePropertyLatency:
            NEED(UInt32);
            *(UInt32 *)out = a->mScope == kAudioObjectPropertyScopeInput ? 0 : kLatencyFrames;
            return 0;
        case kAudioDevicePropertySafetyOffset: NEED(UInt32); *(UInt32 *)out = 0; return 0;
        case kAudioDevicePropertyIsHidden: NEED(UInt32); *(UInt32 *)out = 0; return 0;
        case kAudioDevicePropertyZeroTimeStampPeriod: NEED(UInt32); *(UInt32 *)out = kPeriodFrames; return 0;
        case kAudioDevicePropertyNominalSampleRate: NEED(Float64); *(Float64 *)out = kRate; return 0;
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            NEED(AudioValueRange);
            AudioValueRange r = {kRate, kRate};
            *(AudioValueRange *)out = r;
            return 0;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (in < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)out)[0] = 1;
            ((UInt32 *)out)[1] = 2;
            *outSize = 2 * sizeof(UInt32);
            return 0;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kObjStream:
        switch (sel) {
        case kAudioObjectPropertyBaseClass: NEED(AudioClassID); *(AudioClassID *)out = kAudioObjectClassID; return 0;
        case kAudioObjectPropertyClass: NEED(AudioClassID); *(AudioClassID *)out = kAudioStreamClassID; return 0;
        case kAudioObjectPropertyOwner: NEED(AudioObjectID); *(AudioObjectID *)out = kObjDevice; return 0;
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return 0;
        case kAudioStreamPropertyIsActive: NEED(UInt32); *(UInt32 *)out = 1; return 0;
        case kAudioStreamPropertyDirection: NEED(UInt32); *(UInt32 *)out = 0; return 0;            // 0 = output
        case kAudioStreamPropertyTerminalType: NEED(UInt32); *(UInt32 *)out = kAudioStreamTerminalTypeHeadphones; return 0;
        case kAudioStreamPropertyStartingChannel: NEED(UInt32); *(UInt32 *)out = 1; return 0;
        case kAudioStreamPropertyLatency: NEED(UInt32); *(UInt32 *)out = 0; return 0;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            NEED(AudioStreamBasicDescription);
            *(AudioStreamBasicDescription *)out = format();
            return 0;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            if (in < sizeof(AudioStreamRangedDescription)) { *outSize = 0; return 0; }
            AudioStreamRangedDescription r;
            r.mFormat = format();
            r.mSampleRateRange.mMinimum = kRate;
            r.mSampleRateRange.mMaximum = kRate;
            *(AudioStreamRangedDescription *)out = r;
            *outSize = sizeof r;
            return 0;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kObjVolume:
        switch (sel) {
        case kAudioObjectPropertyBaseClass: NEED(AudioClassID); *(AudioClassID *)out = kAudioLevelControlClassID; return 0;
        case kAudioObjectPropertyClass: NEED(AudioClassID); *(AudioClassID *)out = kAudioVolumeControlClassID; return 0;
        case kAudioObjectPropertyOwner: NEED(AudioObjectID); *(AudioObjectID *)out = kObjDevice; return 0;
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return 0;
        case kAudioControlPropertyScope:
            NEED(AudioObjectPropertyScope);
            *(AudioObjectPropertyScope *)out = kAudioObjectPropertyScopeOutput;
            return 0;
        case kAudioControlPropertyElement: NEED(AudioObjectPropertyElement); *(AudioObjectPropertyElement *)out = 0; return 0;
        case kAudioLevelControlPropertyScalarValue: NEED(Float32); *(Float32 *)out = gScalar; return 0;
        case kAudioLevelControlPropertyDecibelValue: NEED(Float32); *(Float32 *)out = scalar_to_db(gScalar); return 0;
        case kAudioLevelControlPropertyDecibelRange: {
            NEED(AudioValueRange);
            AudioValueRange r = {kMinDb, 0.0};
            *(AudioValueRange *)out = r;
            return 0;
        }
        case kAudioLevelControlPropertyConvertScalarToDecibels:
            NEED(Float32);
            *(Float32 *)out = scalar_to_db(*(Float32 *)out);
            return 0;
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            NEED(Float32);
            *(Float32 *)out = db_to_scalar(*(Float32 *)out);
            return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    default:
        return kAudioHardwareBadObjectError;
    }
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a,
                                UInt32 qs, const void *q, UInt32 n, const void *data) {
    (void)pid; (void)qs; (void)q;
    if (d != gDriver || !a || !data) return kAudioHardwareIllegalOperationError;
    if (id == kObjVolume && (a->mSelector == kAudioLevelControlPropertyScalarValue ||
                             a->mSelector == kAudioLevelControlPropertyDecibelValue)) {
        if (n < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
        Float32 v = *(const Float32 *)data;
        Float32 s = a->mSelector == kAudioLevelControlPropertyScalarValue ? v : db_to_scalar(v);
        if (s < 0) s = 0;
        if (s > 1) s = 1;
        gScalar = s;
        AudioObjectPropertyAddress changed[2] = {
            {kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0},
            {kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0},
        };
        if (gHost) gHost->PropertiesChanged(gHost, kObjVolume, 2, changed);
        return 0;
    }
    if (id == kObjDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (n < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return *(const Float64 *)data == kRate ? 0 : kAudioHardwareIllegalOperationError;
    }
    if (id == kObjStream && (a->mSelector == kAudioStreamPropertyVirtualFormat ||
                             a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (n < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription *f = data;
        return (f->mSampleRate == kRate && f->mChannelsPerFrame == kChannels && f->mFormatID == kAudioFormatLinearPCM)
                   ? 0 : kAudioDeviceUnsupportedFormatError;
    }
    return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------- IO

static void open_socket(void) {
    if (gSock >= 0) return;
    gSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (gSock < 0) return;
    fcntl(gSock, F_SETFL, fcntl(gSock, F_GETFL) | O_NONBLOCK);   // never block Core Audio's IO thread
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(MW_PCM_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(gSock, (struct sockaddr *)&sa, sizeof sa) != 0) { close(gSock); gSock = -1; }
}

static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client) {
    (void)client;
    if (d != gDriver || id != kObjDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gMu);
    if (gIOClients++ == 0) {
        gAnchorHost = mach_absolute_time();
        gNumberTimeStamps = 0;
        open_socket();
    }
    pthread_mutex_unlock(&gMu);
    return 0;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client) {
    (void)client;
    if (d != gDriver || id != kObjDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gMu);
    if (gIOClients > 0 && --gIOClients == 0 && gSock >= 0) {
        close(gSock);
        gSock = -1;
    }
    pthread_mutex_unlock(&gMu);
    return 0;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, Float64 *st, UInt64 *ht, UInt64 *seed) {
    (void)client;
    if (d != gDriver || id != kObjDevice) return kAudioHardwareBadObjectError;
    UInt64 now = mach_absolute_time();
    Float64 ticksPerPeriod = gHostTicksPerFrame * kPeriodFrames;
    pthread_mutex_lock(&gMu);
    UInt64 next = gAnchorHost + (UInt64)((Float64)(gNumberTimeStamps + 1) * ticksPerPeriod);
    if (next <= now) gNumberTimeStamps++;
    *st = (Float64)(gNumberTimeStamps * kPeriodFrames);
    *ht = gAnchorHost + (UInt64)((Float64)gNumberTimeStamps * ticksPerPeriod);
    pthread_mutex_unlock(&gMu);
    *seed = 1;
    return 0;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, Boolean *will,
                                  Boolean *inPlace) {
    (void)client;
    if (d != gDriver || id != kObjDevice) return kAudioHardwareBadObjectError;
    *will = op == kAudioServerPlugInIOOperationWriteMix;
    *inPlace = true;
    return 0;
}

static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream, UInt32 client, UInt32 op,
                              UInt32 frames, const AudioServerPlugInIOCycleInfo *ci, void *main, void *secondary) {
    (void)stream; (void)client; (void)ci; (void)secondary;
    if (d != gDriver || id != kObjDevice) return kAudioHardwareBadObjectError;
    if (op != kAudioServerPlugInIOOperationWriteMix || !main || gSock < 0) return 0;
    const float *in = main;
    float s = gScalar;
    float gain = s * s;
    uint8_t dg[sizeof(MwPcmHeader) + MW_PCM_MAX_FRAMES * 4];
    UInt32 done = 0;
    while (done < frames) {
        UInt32 n = frames - done;
        if (n > MW_PCM_MAX_FRAMES) n = MW_PCM_MAX_FRAMES;
        MwPcmHeader h = {MW_PCM_MAGIC, (uint32_t)kRate, gSeq++, (uint16_t)n, 2};
        memcpy(dg, &h, sizeof h);
        int16_t *o = (int16_t *)(void *)(dg + sizeof h);
        for (UInt32 i = 0; i < n * 2; i++) {
            float v = in[(done * 2) + i] * gain * 32767.0f;
            o[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        }
        (void)send(gSock, dg, sizeof h + (size_t)n * 4, 0);   // non-blocking; nobody listening = dropped
        done += n;
    }
    return 0;
}
