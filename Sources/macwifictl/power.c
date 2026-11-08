// power.c — see power.h. Original code (MIT). macOS only.

#include "power.h"

#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <dispatch/dispatch.h>
#include <stdio.h>

static io_connect_t gRootPort;
static void (*gWillSleep)(void);
static void (*gDidWake)(void);

static void on_power(void *refcon, io_service_t service, natural_t type, void *arg) {
    (void)refcon;
    (void)service;
    switch (type) {
        case kIOMessageCanSystemSleep:          // idle sleep: never object
            IOAllowPowerChange(gRootPort, (long)arg);
            break;
        case kIOMessageSystemWillSleep:         // sleep is coming: shut down cleanly, then let it happen
            printf("[macwifid] the Mac is going to sleep\n");
            fflush(stdout);
            if (gWillSleep) gWillSleep();
            IOAllowPowerChange(gRootPort, (long)arg);
            break;
        case kIOMessageSystemWillNotSleep:      // a sleep that was announced was cancelled
        case kIOMessageSystemHasPoweredOn:      // awake again (USB devices are back)
            printf("[macwifid] the Mac is awake\n");
            fflush(stdout);
            if (gDidWake) gDidWake();
            break;
        default:
            break;
    }
}

int power_watch_start(void (*will_sleep)(void), void (*did_wake)(void)) {
    gWillSleep = will_sleep;
    gDidWake = did_wake;
    IONotificationPortRef port = NULL;
    io_object_t notifier = 0;
    gRootPort = IORegisterForSystemPower(NULL, &port, on_power, &notifier);
    if (!gRootPort || !port) {
        fprintf(stderr, "[macwifid] couldn't register for sleep/wake notifications\n");
        return -1;
    }
    IONotificationPortSetDispatchQueue(port, dispatch_queue_create("io.github.fpm8588.macwifid.power", NULL));
    return 0;
}
