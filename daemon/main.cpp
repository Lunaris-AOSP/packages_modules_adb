/*
 * Copyright (C) 2015 The Android Open Source Project
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

#define TRACE_TAG ADB

#include "sysdeps.h"

#include <errno.h>
#include <getopt.h>
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/capability.h>
#include <sys/prctl.h>

#include <memory>
#include <vector>

#include <android-base/logging.h>
#include <android-base/macros.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#if defined(__ANDROID__)
#include <libminijail.h>
#include <log/log_properties.h>
#include <scoped_minijail.h>

#include <private/android_filesystem_config.h>
#include "selinux/android.h"
#endif

#include "adb.h"
#include "adb_auth.h"
#include "adb_listeners.h"
#include "adb_utils.h"
#include "adb_wifi.h"
#include "socket_spec.h"
#include "tradeinmode.h"
#include "transport.h"

#include "daemon/jdwp_service.h"
#include "daemon/mdns.h"
#include "daemon/transport_daemon.h"
#include "daemon/watchdog.h"

#if defined(__ANDROID__)
static const char* root_seclabel = nullptr;
static const char* tim_seclabel = nullptr;

static bool should_drop_privileges() {
    // The properties that affect `adb root` and `adb unroot` are ro.secure and
    // ro.debuggable. In this context the names don't make the expected behavior
    // particularly obvious.
    //
    // ro.debuggable:
    //   Allowed to become root, but not necessarily the default. Set to 1 on
    //   eng and userdebug builds.
    //
    // ro.secure:
    //   Drop privileges by default. Set to 1 on userdebug and user builds.
    bool ro_secure = android::base::GetBoolProperty("ro.secure", true);
    bool ro_debuggable = __android_log_is_debuggable();

    // Drop privileges if ro.secure is set...
    bool drop = ro_secure;

    // ... except "adb root" lets you keep privileges in a debuggable build.
    std::string prop = android::base::GetProperty("service.adb.root", "");
    bool adb_root = (prop == "1");
    bool adb_unroot = (prop == "0");
    if (ro_debuggable && adb_root) {
        drop = false;
    }
    // ... and "adb unroot" lets you explicitly drop privileges.
    if (adb_unroot) {
        drop = true;
    }

    return drop;
}

static void drop_privileges() {
    ScopedMinijail jail(minijail_new());

    // Add extra groups:
    // AID_ADB to access the USB driver
    // AID_LOG to read system logs (adb logcat)
    // AID_INPUT to diagnose input issues (getevent)
    // AID_INET to diagnose network issues (ping)
    // AID_NET_BT and AID_NET_BT_ADMIN to diagnose bluetooth (hcidump)
    // AID_SDCARD_R to allow reading from the SD card
    // AID_SDCARD_RW to allow writing to the SD card
    // AID_NET_BW_STATS to read out qtaguid statistics
    // AID_READPROC for reading /proc entries across UID boundaries
    // AID_UHID for using 'hid' command to read/write to /dev/uhid
    // AID_EXT_DATA_RW for writing to /sdcard/Android/data (devices without sdcardfs)
    // AID_EXT_OBB_RW for writing to /sdcard/Android/obb (devices without sdcardfs)
    // AID_READTRACEFS for reading tracefs entries
    gid_t groups[] = {AID_ADB,          AID_LOG,          AID_INPUT,    AID_INET,
                      AID_NET_BT,       AID_NET_BT_ADMIN, AID_SDCARD_R, AID_SDCARD_RW,
                      AID_NET_BW_STATS, AID_READPROC,     AID_UHID,     AID_EXT_DATA_RW,
                      AID_EXT_OBB_RW,   AID_READTRACEFS};
    minijail_set_supplementary_gids(jail.get(), arraysize(groups), groups);

    // Don't run as root if running in secure mode.
    if (should_drop_privileges()) {
        const bool should_drop_caps = !__android_log_is_debuggable();

        if (should_drop_caps) {
            // CAP_SETUI and CAP_SETGID are required for change_uid and change_gid calls below.
            // CAP_SYS_NICE needs to be in the bounding set of adbd for sh spawned from `adb shell`
            // to also have it in the bounding set. This in turn is required to be able to launch
            // VMs from shell (e.g. adb shell /apex/com.android.virt/bin/vm run-microdroid).
            // Full fork+execve chain looks like this:
            //   adbd (CapBnd: CAP_SYS_NICE) -> /system/bin/sh (CapBnd: CAP_SYS_NICE) ->
            //   /apex/com.android.virt/bin/vm (CapBnd: CAP_SYS_NICE) ->
            //   virtmngr (CapBnd: CAP_SYS_NICE) -> crosvm (CapEff: CAP_SYS_NICE).
            // Note: the adbd will drop it's effective capabilities several lines below, while the
            // /system/bin/sh process spawned from adbd will run as non-root uid, hence won't be
            // able to use the CAP_SYS_NICE capability in the first place.
            minijail_use_caps(jail.get(), CAP_TO_MASK(CAP_SETUID) | CAP_TO_MASK(CAP_SETGID) |
                                                  CAP_TO_MASK(CAP_SYS_NICE));
        }

        minijail_change_gid(jail.get(), AID_SHELL);
        minijail_change_uid(jail.get(), AID_SHELL);
        // minijail_enter() will abort if any priv-dropping step fails.
        minijail_enter(jail.get());

        // Whenever ambient capabilities are being used, minijail cannot
        // simultaneously drop the bounding capability set to just
        // CAP_SETUID|CAP_SETGID while clearing the inheritable, effective,
        // and permitted sets. So we need to do that in two steps.
        using ScopedCaps =
            std::unique_ptr<std::remove_pointer<cap_t>::type, std::function<void(cap_t)>>;
        ScopedCaps caps(cap_get_proc(), &cap_free);
        if (cap_clear_flag(caps.get(), CAP_INHERITABLE) == -1) {
            PLOG(FATAL) << "cap_clear_flag(INHERITABLE) failed";
        }
        if (cap_clear_flag(caps.get(), CAP_EFFECTIVE) == -1) {
            PLOG(FATAL) << "cap_clear_flag(EFFECTIVE) failed";
        }
        if (cap_clear_flag(caps.get(), CAP_PERMITTED) == -1) {
            PLOG(FATAL) << "cap_clear_flag(PEMITTED) failed";
        }
        if (cap_set_proc(caps.get()) != 0) {
            PLOG(FATAL) << "cap_set_proc() failed";
        }

        if (should_enter_tradeinmode()) {
            enter_tradeinmode(tim_seclabel);
            auth_required = false;
        } else if (is_in_tradein_evaluation_mode()) {
            auth_required = false;
        }
    } else {
        // minijail_enter() will abort if any priv-dropping step fails.
        minijail_enter(jail.get());

        if (root_seclabel != nullptr) {
            if (selinux_android_setcon(root_seclabel) < 0) {
                // If we failed to become root, don't try again to avoid a
                // restart loop.
                android::base::SetProperty("service.adb.root", "0");
                LOG(FATAL) << "Could not set SELinux context";
            }
        }
    }
}
#endif

static void setup_adb(const std::vector<std::string>& addrs) {
#if defined(__ANDROID__)
    // Get the first valid port from addrs and setup mDNS.
    int port = -1;
    std::string error;
    for (const auto& addr : addrs) {
        port = get_host_socket_spec_port(addr, &error);
        if (port != -1) {
            break;
        }
    }
    if (port == -1) {
        port = DEFAULT_ADB_LOCAL_TRANSPORT_PORT;
    }
    LOG(INFO) << "Setup mdns on port= " << port;
    setup_mdns(port);
#endif
    for (const auto& addr : addrs) {
        LOG(INFO) << "adbd listening on " << addr;
        init_transport_socket_server(addr);
    }
}

int adbd_main() {
    umask(0);

    signal(SIGPIPE, SIG_IGN);

    // We need to call this even if auth isn't enabled because the file
    // descriptor will always be open.
    adbd_cloexec_auth_socket();

#if defined(__ANDROID__)
    bool device_unlocked = android::base::GetProperty("ro.boot.verifiedbootstate", "") == "orange";
    if (device_unlocked || __android_log_is_debuggable()) {
        // If we're on userdebug/eng or the device is unlocked, permit no-authentication.
        auth_required = android::base::GetBoolProperty("ro.adb.secure", false);
#if defined(__ANDROID_RECOVERY__)
        auth_required &= android::base::GetBoolProperty("ro.adb.secure.recovery", true);
#endif
    }
#endif

    // Our external storage path may be different than apps, since
    // we aren't able to bind mount after dropping root.
    const char* adb_external_storage = getenv("ADB_EXTERNAL_STORAGE");
    if (adb_external_storage != nullptr) {
        setenv("EXTERNAL_STORAGE", adb_external_storage, 1);
    } else {
        D("Warning: ADB_EXTERNAL_STORAGE is not set.  Leaving EXTERNAL_STORAGE"
          " unchanged.\n");
    }

#if defined(__ANDROID__)
    drop_privileges();
#endif

#if defined(__ANDROID__)
    // A thread gets spawned as a side-effect of initializing the watchdog, so it needs to happen
    // after we drop privileges.
    watchdog::Initialize();
#endif

    // adbd_auth_init will spawn a thread, so we need to defer it until after selinux transitions.
    adbd_auth_init();

    bool is_usb = false;

#if defined(__ANDROID__)
    if (access(USB_FFS_ADB_EP0, F_OK) == 0) {
        // Listen on USB.
        usb_init();
        is_usb = true;
    }
#endif

    // If one of these properties is set, also listen on that port.
    // If one of the properties isn't set and we couldn't listen on usb, listen
    // on the default port.
    std::vector<std::string> addrs;
    std::string prop_addr = android::base::GetProperty("service.adb.listen_addrs", "");
    if (prop_addr.empty()) {
        std::string prop_port = android::base::GetProperty("service.adb.tcp.port", "");
        if (prop_port.empty()) {
            prop_port = android::base::GetProperty("persist.adb.tcp.port", "");
        }

#if !defined(__ANDROID__)
        if (prop_port.empty() && getenv("ADBD_PORT")) {
            prop_port = getenv("ADBD_PORT");
        }
#endif

        int port;
        if (sscanf(prop_port.c_str(), "%d", &port) == 1 && port > 0) {
            D("using tcp port=%d", port);
            // Listen on TCP and VSOCK port specified by service.adb.tcp.port property.
            addrs.push_back(android::base::StringPrintf("tcp:%d", port));
            addrs.push_back(android::base::StringPrintf("vsock:%d", port));
            setup_adb(addrs);
        } else if (!is_usb) {
            // Listen on default port.
            addrs.push_back(
                    android::base::StringPrintf("tcp:%d", DEFAULT_ADB_LOCAL_TRANSPORT_PORT));
            addrs.push_back(
                    android::base::StringPrintf("vsock:%d", DEFAULT_ADB_LOCAL_TRANSPORT_PORT));
            setup_adb(addrs);
        }
    } else {
        addrs = android::base::Split(prop_addr, ",");
        setup_adb(addrs);
    }

    LOG(INFO) << "adbd started";

    D("adbd_main(): pre init_jdwp()");
    init_jdwp();
    D("adbd_main(): post init_jdwp()");

    D("Event loop starting");
    fdevent_loop();

    return 0;
}

int main(int argc, char** argv) {
#if defined(__BIONIC__)
    // Set M_DECAY_TIME so that our allocations aren't immediately purged on free.
    mallopt(M_DECAY_TIME, 1);
#endif

    while (true) {
        static struct option opts[] = {
                {"root_seclabel", required_argument, nullptr, 's'},
                {"tim_seclabel", required_argument, nullptr, 't'},
                {"device_banner", required_argument, nullptr, 'b'},
                {"version", no_argument, nullptr, 'v'},
                {"logpostfsdata", no_argument, nullptr, 'l'},
                {nullptr, no_argument, nullptr, 0},
        };

        int option_index = 0;
        int c = getopt_long(argc, argv, "", opts, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
#if defined(__ANDROID__)
            case 's':
                root_seclabel = optarg;
                break;
            case 't':
                tim_seclabel = optarg;
                break;
#endif
            case 'b':
                adb_device_banner = optarg;
                break;
            case 'v':
                printf("Android Debug Bridge Daemon version %d.%d.%d\n", ADB_VERSION_MAJOR,
                       ADB_VERSION_MINOR, ADB_SERVER_VERSION);
                return 0;
            case 'l':
                LOG(ERROR) << "post-fs-data triggered";
                return 0;
            default:
                // getopt already prints "adbd: invalid option -- %c" for us.
                return 1;
        }
    }

    close_stdin();

    adb_trace_init(argv);

    D("Handling main()");
    return adbd_main();
}
