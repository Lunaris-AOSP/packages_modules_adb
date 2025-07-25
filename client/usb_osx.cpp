/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define TRACE_TAG USB

#include "sysdeps.h"

#include "client/usb.h"

#include <CoreFoundation/CoreFoundation.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOMessage.h>
#include <mach/mach_port.h>

#include <inttypes.h>
#include <stdio.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/thread_annotations.h>

#include "adb.h"
#include "transport.h"

using namespace std::chrono_literals;

struct usb_handle
{
    UInt8 bulkIn;
    UInt8 bulkOut;
    IOUSBInterfaceInterface550** interface;
    unsigned int zero_mask;
    size_t max_packet_size;

    // For garbage collecting disconnected devices.
    bool mark;
    std::string devpath;
    std::atomic<bool> dead;

    usb_handle()
        : bulkIn(0),
          bulkOut(0),
          interface(nullptr),
          zero_mask(0),
          max_packet_size(0),
          mark(false),
          dead(false) {}
};

static std::atomic<bool> usb_inited_flag;

static auto& g_usb_handles_mutex = *new std::mutex();
static auto& g_usb_handles = *new std::vector<std::unique_ptr<usb_handle>>();

static bool IsKnownDevice(const std::string& devpath) {
    std::lock_guard<std::mutex> lock_guard(g_usb_handles_mutex);
    for (auto& usb : g_usb_handles) {
        if (usb->devpath == devpath) {
            // Set mark flag to indicate this device is still alive.
            usb->mark = true;
            return true;
        }
    }
    return false;
}

static void usb_kick_locked(usb_handle* handle);

static void KickDisconnectedDevices() {
    std::lock_guard<std::mutex> lock_guard(g_usb_handles_mutex);
    for (auto& usb : g_usb_handles) {
        if (!usb->mark) {
            usb_kick_locked(usb.get());
        } else {
            usb->mark = false;
        }
    }
}

static void AddDevice(std::unique_ptr<usb_handle> handle) {
    handle->mark = true;
    std::lock_guard<std::mutex> lock(g_usb_handles_mutex);
    g_usb_handles.push_back(std::move(handle));
}

static void AndroidInterfaceAdded(io_iterator_t iterator);
static std::unique_ptr<usb_handle> CheckInterface(IOUSBInterfaceInterface550** iface, UInt16 vendor,
                                                  UInt16 product);

// Flag-guarded (using host env variable) feature that turns on
// the ability to clear the device-side endpoint also before
// starting. See public bug https://issuetracker.google.com/issues/37055927
// for historical context.
static bool clear_endpoints() {
    static const char* env(getenv("ADB_OSX_USB_CLEAR_ENDPOINTS"));
    static bool result = env && strcmp("1", env) == 0;
    return result;
}

static bool FindUSBDevices() {
    // Create the matching dictionary to find the Android device's adb interface.
    CFMutableDictionaryRef matchingDict = IOServiceMatching(kIOUSBInterfaceClassName);
    if (!matchingDict) {
        LOG(ERROR) << "couldn't create USB matching dictionary";
        return false;
    }
    // Create an iterator for all I/O Registry objects that match the dictionary.
    io_iterator_t iter = 0;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matchingDict, &iter);
    if (kr != KERN_SUCCESS) {
        LOG(ERROR) << "failed to get matching services";
        return false;
    }
    // Iterate over all matching objects.
    AndroidInterfaceAdded(iter);
    IOObjectRelease(iter);
    return true;
}

static void
AndroidInterfaceAdded(io_iterator_t iterator)
{
    kern_return_t            kr;
    io_service_t             usbDevice;
    io_service_t             usbInterface;
    IOCFPlugInInterface      **plugInInterface = NULL;
    IOUSBInterfaceInterface500  **iface = NULL;
    IOUSBDeviceInterface500  **dev = NULL;
    HRESULT                  result;
    SInt32                   score;
    uint32_t                 locationId;
    UInt8                    if_class, subclass, protocol;
    UInt16                   vendor;
    UInt16                   product;
    UInt8                    serialIndex;
    char                     serial[256];
    std::string devpath;

    while ((usbInterface = IOIteratorNext(iterator))) {
        //* Create an intermediate interface plugin
        kr = IOCreatePlugInInterfaceForService(usbInterface,
                                               kIOUSBInterfaceUserClientTypeID,
                                               kIOCFPlugInInterfaceID,
                                               &plugInInterface, &score);
        IOObjectRelease(usbInterface);
        if ((kIOReturnSuccess != kr) || (!plugInInterface)) {
            LOG(ERROR) << "Unable to create an interface plug-in (" << std::hex << kr << ")";
            continue;
        }

        //* This gets us the interface object
        result = (*plugInInterface)->QueryInterface(
            plugInInterface,
            CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500), (LPVOID*)&iface);
        //* We only needed the plugin to get the interface, so discard it
        (*plugInInterface)->Release(plugInInterface);
        if (result || !iface) {
            LOG(ERROR) << "Couldn't query the interface (" << std::hex << result << ")";
            continue;
        }

        kr = (*iface)->GetInterfaceClass(iface, &if_class);
        kr = (*iface)->GetInterfaceSubClass(iface, &subclass);
        kr = (*iface)->GetInterfaceProtocol(iface, &protocol);
        if (!is_adb_interface(if_class, subclass, protocol)) {
            // Ignore non-ADB devices (interface with incorrect
            // class/subclass/protocol).
            (*iface)->Release(iface);
            continue;
        }

        //* this gets us an ioservice, with which we will find the actual
        //* device; after getting a plugin, and querying the interface, of
        //* course.
        //* Gotta love OS X
        kr = (*iface)->GetDevice(iface, &usbDevice);
        if (kIOReturnSuccess != kr || !usbDevice) {
            LOG(ERROR) << "Couldn't grab device from interface (" << std::hex << kr << ")";
            (*iface)->Release(iface);
            continue;
        }

        plugInInterface = NULL;
        score = 0;
        //* create an intermediate device plugin
        kr = IOCreatePlugInInterfaceForService(usbDevice,
                                               kIOUSBDeviceUserClientTypeID,
                                               kIOCFPlugInInterfaceID,
                                               &plugInInterface, &score);
        //* only needed this to find the plugin
        (void)IOObjectRelease(usbDevice);
        if ((kIOReturnSuccess != kr) || (!plugInInterface)) {
            LOG(ERROR) << "Unable to create a device plug-in (" << std::hex << kr << ")";
            (*iface)->Release(iface);
            continue;
        }

        result = (*plugInInterface)->QueryInterface(plugInInterface,
            CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID*)&dev);
        //* only needed this to query the plugin
        (*plugInInterface)->Release(plugInInterface);
        if (result || !dev) {
            LOG(ERROR) << "Couldn't create a device interface (" << std::hex << result << ")";
            (*iface)->Release(iface);
            continue;
        }

        //* Now after all that, we actually have a ref to the device and
        //* the interface that matched our criteria
        kr = (*dev)->GetDeviceVendor(dev, &vendor);
        kr = (*dev)->GetDeviceProduct(dev, &product);
        kr = (*dev)->GetLocationID(dev, &locationId);
        if (kr == KERN_SUCCESS) {
            devpath = android::base::StringPrintf("usb:%" PRIu32 "X", locationId);
            if (IsKnownDevice(devpath)) {
                (*dev)->Release(dev);
                (*iface)->Release(iface);
                continue;
            }
        }
        kr = (*dev)->USBGetSerialNumberStringIndex(dev, &serialIndex);

        if (serialIndex > 0) {
            IOUSBDevRequest req;
            UInt16          buffer[256];
            UInt16          languages[128];

            memset(languages, 0, sizeof(languages));

            req.bmRequestType =
                    USBmakebmRequestType(kUSBIn, kUSBStandard, kUSBDevice);
            req.bRequest = kUSBRqGetDescriptor;
            req.wValue = (kUSBStringDesc << 8) | 0;
            req.wIndex = 0;
            req.pData = languages;
            req.wLength = sizeof(languages);
            kr = (*dev)->DeviceRequest(dev, &req);

            if (kr == kIOReturnSuccess && req.wLenDone > 0) {

                int langCount = (req.wLenDone - 2) / 2, lang;

                for (lang = 1; lang <= langCount; lang++) {
                    memset(buffer, 0, sizeof(buffer));
                    memset(&req, 0, sizeof(req));

                    req.bmRequestType =
                            USBmakebmRequestType(kUSBIn, kUSBStandard, kUSBDevice);
                    req.bRequest = kUSBRqGetDescriptor;
                    req.wValue = (kUSBStringDesc << 8) | serialIndex;
                    req.wIndex = languages[lang];
                    req.pData = buffer;
                    req.wLength = sizeof(buffer);
                    kr = (*dev)->DeviceRequest(dev, &req);

                    if (kr == kIOReturnSuccess && req.wLenDone > 0) {
                        int i, count;

                        // skip first word, and copy the rest to the serial string,
                        // changing shorts to bytes.
                        count = (req.wLenDone - 1) / 2;
                        for (i = 0; i < count; i++)
                                serial[i] = buffer[i + 1];
                        serial[i] = 0;
                        break;
                    }
                }
            }
        }

        (*dev)->Release(dev);

        VLOG(USB) << android::base::StringPrintf("Found vid=%04x pid=%04x serial=%s\n",
                        vendor, product, serial);
        if (devpath.empty()) {
            devpath = serial;
        }
        if (IsKnownDevice(devpath)) {
            (*iface)->USBInterfaceClose(iface);
            (*iface)->Release(iface);
            continue;
        }

        if (!transport_server_owns_device(devpath, serial)) {
            // We aren't allowed to communicate with this device. Don't open this device.
            D("ignoring device: not owned by this server dev_path: '%s', serial: '%s'",
              devpath.c_str(), serial);
            continue;
        }

        std::unique_ptr<usb_handle> handle =
            CheckInterface((IOUSBInterfaceInterface550**)iface, vendor, product);
        if (handle == nullptr) {
            LOG(ERROR) << "Could not find device interface";
            (*iface)->Release(iface);
            continue;
        }
        handle->devpath = devpath;
        usb_handle* handle_p = handle.get();
        VLOG(USB) << "Add usb device " << serial;
        LOG(INFO) << "reported max packet size for " << serial << " is " << handle->max_packet_size;
        AddDevice(std::move(handle));
        register_usb_transport(reinterpret_cast<::usb_handle*>(handle_p), serial, devpath.c_str(),
                               1);
    }
}

// Used to clear both the endpoints before starting.
// When adb quits, we might clear the host endpoint but not the device.
// So we make sure both sides are clear before starting up.
// Returns true if:
//      - the feature is disabled (OSX/host only)
//      - the feature is enabled and successfully clears both endpoints
// Returns false otherwise (if an error is encountered)
static bool ClearPipeStallBothEnds(IOUSBInterfaceInterface550** interface, UInt8 bulkEp) {
    // If feature-disabled, (silently) bypass clearing both
    // endpoints (including device-side).
    if (!clear_endpoints()) {
        return true;
    }

    IOReturn rc = (*interface)->ClearPipeStallBothEnds(interface, bulkEp);
    if (rc != kIOReturnSuccess) {
        LOG(ERROR) << "Could not clear pipe stall both ends: " << std::hex << rc;
        return false;
    }
    return true;
}

static std::string darwinErrorToString(IOReturn result) {
    switch (result) {
        case kIOReturnSuccess:
            return "no error";
        case kIOReturnNotOpen:
            return "device not opened for exclusive access";
        case kIOReturnNoDevice:
            return "no connection to an IOService";
        case kIOUSBNoAsyncPortErr:
            return "no async port has been opened for interface";
        case kIOReturnExclusiveAccess:
            return "another process has device opened for exclusive access";
        case kIOUSBPipeStalled:
#if defined(kUSBHostReturnPipeStalled)
        case kUSBHostReturnPipeStalled:
#endif
            return "pipe is stalled";
        case kIOReturnError:
            return "could not establish a connection to the Darwin kernel";
        case kIOUSBTransactionTimeout:
            return "transaction timed out";
        case kIOReturnBadArgument:
            return "invalid argument";
        case kIOReturnAborted:
            return "transaction aborted";
        case kIOReturnNotResponding:
            return "device not responding";
        case kIOReturnOverrun:
            return "data overrun";
        case kIOReturnCannotWire:
            return "physical memory can not be wired down";
        case kIOReturnNoResources:
            return "out of resources";
        case kIOUSBHighSpeedSplitError:
            return "high speed split error";
        case kIOUSBUnknownPipeErr:
            return "pipe ref not recognized";
        default:
            return std::format("unknown error ({:#x})", result);
    }
}

static void dumpEndpointProperties(const std::string& label,
                                   const IOUSBEndpointProperties& properties) {
    VLOG(USB) << std::endl << label;
    VLOG(USB) << "    wMaxPacketSize=" << properties.wMaxPacketSize;
    VLOG(USB) << "    bTransferType=" << static_cast<unsigned>(properties.bTransferType);
    VLOG(USB) << "    bDirection=" << static_cast<unsigned>(properties.bDirection);
    VLOG(USB) << "    bAlternateSetting=" << static_cast<unsigned>(properties.bAlternateSetting);
    VLOG(USB) << "    bMult=" << static_cast<unsigned>(properties.bMult);
    VLOG(USB) << "    bMaxBurst=" << static_cast<unsigned>(properties.bMaxBurst);
    VLOG(USB) << "    bEndpointNumber=" << static_cast<unsigned>(properties.bEndpointNumber);
    VLOG(USB) << "    bInterval=" << static_cast<unsigned>(properties.bInterval);
    VLOG(USB) << "    bMaxStreams=" << static_cast<unsigned>(properties.bMaxStreams);
    VLOG(USB) << "    bSyncType=" << static_cast<unsigned>(properties.bSyncType);
    VLOG(USB) << "    bUsageType=" << static_cast<unsigned>(properties.bUsageType);
    VLOG(USB) << "    bVersion=" << static_cast<unsigned>(properties.bVersion);
    VLOG(USB) << "    wBytesPerInterval=" << static_cast<unsigned>(properties.wBytesPerInterval);
}

//* TODO: simplify this further since we only register to get ADB interface
//* subclass+protocol events
static std::unique_ptr<usb_handle> CheckInterface(IOUSBInterfaceInterface550** interface,
                                                  UInt16 vendor, UInt16 product) {
    std::unique_ptr<usb_handle> handle;
    IOReturn kr;
    UInt8 interfaceNumEndpoints, interfaceClass, interfaceSubClass, interfaceProtocol;
    UInt8 endpoint;

    //* Now open the interface.  This will cause the pipes associated with
    //* the endpoints in the interface descriptor to be instantiated
    kr = (*interface)->USBInterfaceOpen(interface);
    if (kr != kIOReturnSuccess) {
        LOG(ERROR) << "Could not open interface: " << std::hex << kr;
        return NULL;
    }

    //* Get the number of endpoints associated with this interface
    kr = (*interface)->GetNumEndpoints(interface, &interfaceNumEndpoints);
    if (kr != kIOReturnSuccess) {
        LOG(ERROR) << "Unable to get number of endpoints: " << std::hex << kr;
        goto err_get_num_ep;
    }

    //* Get interface class, subclass and protocol
    if ((*interface)->GetInterfaceClass(interface, &interfaceClass) != kIOReturnSuccess ||
            (*interface)->GetInterfaceSubClass(interface, &interfaceSubClass) != kIOReturnSuccess ||
            (*interface)->GetInterfaceProtocol(interface, &interfaceProtocol) != kIOReturnSuccess) {
            LOG(ERROR) << "Unable to get interface class, subclass and protocol";
            goto err_get_interface_class;
    }

    //* check to make sure interface class, subclass and protocol match ADB
    //* avoid opening mass storage endpoints
    if (!is_adb_interface(interfaceClass, interfaceSubClass, interfaceProtocol)) {
        goto err_bad_adb_interface;
    }

    handle.reset(new usb_handle);
    if (handle == nullptr) {
        goto err_bad_adb_interface;
    }

    //* Iterate over the endpoints for this interface and find the first
    //* bulk in/out pipes available.  These will be our read/write pipes.
    for (endpoint = 1; endpoint <= interfaceNumEndpoints; ++endpoint) {
        VLOG(USB) << std::endl << "Inspecting endpoint " << static_cast<unsigned>(endpoint);
        IOUSBEndpointProperties properties = {.bVersion = kUSBEndpointPropertiesVersion3};

        // We only call GetPipePropertiesV3 so it populates the IOUSBEndpointProperties field
        // needed for GetEndpointPropertiesV3. We don't use wMaxPacketSize returned here
        // because it is the FULL maxPacketSize which includes burst and mul.
        kr = (*interface)->GetPipePropertiesV3(interface, endpoint, &properties);
        if (kr != kIOReturnSuccess) {
            LOG(ERROR) << "GetPipePropertiesV3 error : " << darwinErrorToString(kr);
            goto err_get_pipe_props;
        }
        dumpEndpointProperties("GetPipePropertiesV3 values", properties);

        // GetEndpointPropertiesV3 needs IOUSBEndpointProperties fields bVersion, bAlternateSetting,
        // bDirection, and bEndPointNumber to be set before calling. This was done by
        // GetPipePropertiesV3.
        kr = (*interface)->GetEndpointPropertiesV3(interface, &properties);
        if (kr != kIOReturnSuccess) {
            LOG(ERROR) << "GetEndpointPropertiesV3 error : " << darwinErrorToString(kr);
            goto err_get_pipe_props;
        }
        dumpEndpointProperties("GetEndpointPropertiesV3 values", properties);

        if (properties.bTransferType != kUSBBulk) {
            continue;
        }

        if (properties.bDirection == kUSBIn) {
            handle->bulkIn = endpoint;

            if (!ClearPipeStallBothEnds(interface, handle->bulkIn)) {
                goto err_get_pipe_props;
            }
        }

        if (properties.bDirection == kUSBOut) {
            handle->bulkOut = endpoint;
            handle->zero_mask = properties.wMaxPacketSize - 1;
            handle->max_packet_size = properties.wMaxPacketSize;

            if (!ClearPipeStallBothEnds(interface, handle->bulkOut)) {
                goto err_get_pipe_props;
            }
        }
    }

    handle->interface = interface;
    return handle;

err_get_pipe_props:
err_bad_adb_interface:
err_get_interface_class:
err_get_num_ep:
    (*interface)->USBInterfaceClose(interface);
    return nullptr;
}

std::mutex& operate_device_lock = *new std::mutex();

static void RunLoopThread() {
    adb_thread_setname("RunLoop");

    VLOG(USB) << "RunLoopThread started";
    while (true) {
        {
            std::lock_guard<std::mutex> lock_guard(operate_device_lock);
            FindUSBDevices();
            KickDisconnectedDevices();
        }
        // Signal the parent that we are running
        usb_inited_flag = true;
        std::this_thread::sleep_for(1s);
    }
    VLOG(USB) << "RunLoopThread done";
}

void usb_cleanup() NO_THREAD_SAFETY_ANALYSIS {
    VLOG(USB) << "Macos usb_cleanup";
    // Wait until usb operations in RunLoopThread finish, and prevent further operations.
    operate_device_lock.lock();
    close_usb_devices();
}

void usb_init() {
    static bool initialized = false;
    if (!initialized) {
        usb_inited_flag = false;

        std::thread(RunLoopThread).detach();

        // Wait for initialization to finish
        while (!usb_inited_flag) {
            std::this_thread::sleep_for(100ms);
        }

        adb_notify_device_scan_complete();
        initialized = true;
    }
}

int usb_write(usb_handle *handle, const void *buf, int len)
{
    IOReturn    result;

    if (!len)
        return 0;

    if (!handle || handle->dead)
        return -1;

    if (NULL == handle->interface) {
        LOG(ERROR) << "usb_write interface was null";
        return -1;
    }

    if (0 == handle->bulkOut) {
        LOG(ERROR) << "bulkOut endpoint not assigned";
        return -1;
    }

    result =
        (*handle->interface)->WritePipe(handle->interface, handle->bulkOut, (void *)buf, len);

    if ((result == 0) && (handle->zero_mask)) {
        /* we need 0-markers and our transfer */
        if(!(len & handle->zero_mask)) {
            result =
                (*handle->interface)->WritePipe(
                        handle->interface, handle->bulkOut, (void *)buf, 0);
        }
    }

    if (!result)
        return len;

    LOG(ERROR) << "usb_write failed with status: " << std::hex << result;
    return -1;
}

int usb_read(usb_handle *handle, void *buf, int len)
{
    IOReturn result;
    UInt32  numBytes = len;

    if (!len) {
        return 0;
    }

    if (!handle || handle->dead) {
        return -1;
    }

    if (NULL == handle->interface) {
        LOG(ERROR) << "usb_read interface was null";
        return -1;
    }

    if (0 == handle->bulkIn) {
        LOG(ERROR) << "bulkIn endpoint not assigned";
        return -1;
    }

    result = (*handle->interface)->ReadPipe(handle->interface, handle->bulkIn, buf, &numBytes);

    if (kIOUSBPipeStalled == result) {
        LOG(ERROR) << "Pipe stalled, clearing stall.\n";
        (*handle->interface)->ClearPipeStall(handle->interface, handle->bulkIn);
        result = (*handle->interface)->ReadPipe(handle->interface, handle->bulkIn, buf, &numBytes);
    }

    if (kIOReturnSuccess == result)
        return numBytes;
    else {
        LOG(ERROR) << "usb_read failed with status: " << std::hex << result;
    }

    return -1;
}

int usb_close(usb_handle *handle)
{
    std::lock_guard<std::mutex> lock(g_usb_handles_mutex);
    for (auto it = g_usb_handles.begin(); it != g_usb_handles.end(); ++it) {
        if ((*it).get() == handle) {
            g_usb_handles.erase(it);
            break;
        }
    }
    return 0;
}

void usb_reset(usb_handle* handle) {
    // Unimplemented on OS X.
    usb_kick(handle);
}

static void usb_kick_locked(usb_handle *handle)
{
    LOG(INFO) << "Kicking handle";
    /* release the interface */
    if (!handle)
        return;

    if (!handle->dead)
    {
        handle->dead = true;
        (*handle->interface)->USBInterfaceClose(handle->interface);
        (*handle->interface)->Release(handle->interface);
    }
}

void usb_kick(usb_handle *handle) {
    // Use the lock to avoid multiple thread kicking the device at the same time.
    std::lock_guard<std::mutex> lock_guard(g_usb_handles_mutex);
    usb_kick_locked(handle);
}

size_t usb_get_max_packet_size(usb_handle* handle) {
    return handle->max_packet_size;
}
