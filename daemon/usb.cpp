/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/usb/functionfs.h>
#include <sys/eventfd.h>

#include <algorithm>
#include <array>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <asyncio/AsyncIO.h>

#include <android-base/logging.h>
#include <android-base/macros.h>
#include <android-base/parsebool.h>
#include <android-base/properties.h>
#include <android-base/thread_annotations.h>

#include "adb_unique_fd.h"
#include "adb_utils.h"
#include "apacket_reader.h"
#include "daemon/property_monitor.h"
#include "daemon/usb_ffs.h"
#include "sysdeps/chrono.h"
#include "transfer_id.h"
#include "transport.h"
#include "types.h"

using android::base::StringPrintf;

// Not all USB controllers support operations larger than 16k, so don't go above that.
// Also, each submitted operation does an allocation in the kernel of that size, so we want to
// minimize our queue depth while still maintaining a deep enough queue to keep the USB stack fed.
static constexpr size_t kUsbReadQueueDepth = 8;
static constexpr size_t kUsbReadSize = 16384;

static constexpr size_t kUsbWriteQueueDepth = 8;
static constexpr size_t kUsbWriteSize = 16384;

static const char* to_string(enum usb_functionfs_event_type type) {
    switch (type) {
        case FUNCTIONFS_BIND:
            return "FUNCTIONFS_BIND";
        case FUNCTIONFS_UNBIND:
            return "FUNCTIONFS_UNBIND";
        case FUNCTIONFS_ENABLE:
            return "FUNCTIONFS_ENABLE";
        case FUNCTIONFS_DISABLE:
            return "FUNCTIONFS_DISABLE";
        case FUNCTIONFS_SETUP:
            return "FUNCTIONFS_SETUP";
        case FUNCTIONFS_SUSPEND:
            return "FUNCTIONFS_SUSPEND";
        case FUNCTIONFS_RESUME:
            return "FUNCTIONFS_RESUME";
    }
}

template <class Payload>
struct IoBlock {
    bool pending = false;
    struct iocb control = {};
    Payload payload;

    TransferId id() const { return TransferId::from_value(control.aio_data); }
};

using IoReadBlock = IoBlock<Block>;
using IoWriteBlock = IoBlock<std::shared_ptr<Block>>;

struct ScopedAioContext {
    ScopedAioContext() = default;
    ~ScopedAioContext() { reset(); }

    ScopedAioContext(ScopedAioContext&& move) { reset(move.release()); }
    ScopedAioContext(const ScopedAioContext& copy) = delete;

    ScopedAioContext& operator=(ScopedAioContext&& move) {
        reset(move.release());
        return *this;
    }
    ScopedAioContext& operator=(const ScopedAioContext& copy) = delete;

    static ScopedAioContext Create(size_t max_events) {
        aio_context_t ctx = 0;
        if (io_setup(max_events, &ctx) != 0) {
            PLOG(FATAL) << "failed to create aio_context_t";
        }
        ScopedAioContext result;
        result.reset(ctx);
        return result;
    }

    aio_context_t release() {
        aio_context_t result = context_;
        context_ = 0;
        return result;
    }

    void reset(aio_context_t new_context = 0) {
        if (context_ != 0) {
            io_destroy(context_);
        }

        context_ = new_context;
    }

    aio_context_t get() { return context_; }

  private:
    aio_context_t context_ = 0;
};

struct UsbFfsConnection : public Connection {
    UsbFfsConnection(unique_fd control, unique_fd read, unique_fd write,
                     std::promise<void> destruction_notifier)
        : worker_started_(false),
          stopped_(false),
          destruction_notifier_(std::move(destruction_notifier)),
          control_fd_(std::move(control)),
          read_fd_(std::move(read)),
          write_fd_(std::move(write)) {
        VLOG(USB) << "UsbFfsConnection constructed";
        worker_event_fd_.reset(eventfd(0, EFD_CLOEXEC));
        if (worker_event_fd_ == -1) {
            PLOG(FATAL) << "failed to create eventfd";
        }

        monitor_event_fd_.reset(eventfd(0, EFD_CLOEXEC));
        if (monitor_event_fd_ == -1) {
            PLOG(FATAL) << "failed to create eventfd";
        }

        aio_context_ = ScopedAioContext::Create(kUsbReadQueueDepth + kUsbWriteQueueDepth);
    }

    ~UsbFfsConnection() {
        VLOG(USB) << "UsbFfsConnection being destroyed";
        Stop();
        monitor_thread_.join();

        // We need to explicitly close our file descriptors before we notify our destruction,
        // because the thread listening on the future will immediately try to reopen the endpoint.
        aio_context_.reset();
        control_fd_.reset();
        read_fd_.reset();
        write_fd_.reset();

        destruction_notifier_.set_value();
    }

    virtual bool Write(std::unique_ptr<apacket> packet) override final {
        VLOG(USB) << "USB write: " << dump_header(&packet->msg);
        auto header = std::make_shared<Block>(sizeof(packet->msg));
        memcpy(header->data(), &packet->msg, sizeof(packet->msg));

        std::lock_guard<std::mutex> lock(write_mutex_);
        write_requests_.push_back(
                CreateWriteBlock(std::move(header), 0, sizeof(packet->msg), next_write_id_++));
        if (!packet->payload.empty()) {
            // The kernel attempts to allocate a contiguous block of memory for each write,
            // which can fail if the write is large and the kernel heap is fragmented.
            // Split large writes into smaller chunks to avoid this.
            auto payload = std::make_shared<Block>(std::move(packet->payload));
            size_t offset = 0;
            size_t len = payload->size();

            while (len > 0) {
                size_t write_size = std::min(kUsbWriteSize, len);
                write_requests_.push_back(
                        CreateWriteBlock(payload, offset, write_size, next_write_id_++));
                len -= write_size;
                offset += write_size;
            }
        }

        // Wake up the worker thread to submit writes.
        uint64_t notify = 1;
        ssize_t rc = adb_write(worker_event_fd_.get(), &notify, sizeof(notify));
        if (rc < 0) {
            PLOG(FATAL) << "failed to notify worker eventfd to submit writes";
        }

        return true;
    }

    virtual bool Start() override final {
        StartMonitor();
        return true;
    }

    virtual void Stop() override final {
        if (stopped_.exchange(true)) {
            return;
        }
        stopped_ = true;
        uint64_t notify = 1;
        ssize_t rc = adb_write(worker_event_fd_.get(), &notify, sizeof(notify));
        if (rc < 0) {
            PLOG(FATAL) << "failed to notify worker eventfd to stop UsbFfsConnection";
        }
        CHECK_EQ(static_cast<size_t>(rc), sizeof(notify));

        rc = adb_write(monitor_event_fd_.get(), &notify, sizeof(notify));
        if (rc < 0) {
            PLOG(FATAL) << "failed to notify monitor eventfd to stop UsbFfsConnection";
        }

        CHECK_EQ(static_cast<size_t>(rc), sizeof(notify));
    }

    virtual bool DoTlsHandshake(RSA* key, std::string* auth_key) override final {
        // TODO: support TLS for usb connections.
        LOG(FATAL) << "Not supported yet.";
        return false;
    }

  private:
    void StartMonitor() {
        // This is a bit of a mess.
        // It's possible for io_submit to end up blocking, if we call it as the endpoint
        // becomes disabled. Work around this by having a monitor thread to listen for functionfs
        // lifecycle events. If we notice an error condition (either we've become disabled, or we
        // were never enabled in the first place), we send interruption signals to the worker thread
        // until it dies, and then report failure to the transport via HandleError, which will
        // eventually result in the transport being destroyed, which will result in UsbFfsConnection
        // being destroyed, which unblocks the open thread and restarts this entire process.
        static std::once_flag handler_once;
        std::call_once(handler_once, []() { signal(kInterruptionSignal, [](int) {}); });

        monitor_thread_ = std::thread([this]() {
            adb_thread_setname("UsbFfs-monitor");
            VLOG(USB) << "UsbFfs-monitor thread spawned";

            bool bound = false;
            bool enabled = false;
            bool running = true;
            while (running) {
                adb_pollfd pfd[2] = {
                  { .fd = control_fd_.get(), .events = POLLIN, .revents = 0 },
                  { .fd = monitor_event_fd_.get(), .events = POLLIN, .revents = 0 },
                };

                // If we don't see our first bind within a second, try again.
                int timeout_ms = bound ? -1 : 1000;

                int rc = TEMP_FAILURE_RETRY(adb_poll(pfd, 2, timeout_ms));
                if (rc == -1) {
                    PLOG(FATAL) << "poll on USB control fd failed";
                } else if (rc == 0) {
                    LOG(WARNING) << "timed out while waiting for FUNCTIONFS_BIND, trying again";
                    break;
                }

                if (pfd[1].revents) {
                    // We were told to die.
                    break;
                }

                struct usb_functionfs_event event;
                rc = TEMP_FAILURE_RETRY(adb_read(control_fd_.get(), &event, sizeof(event)));
                if (rc == -1) {
                    PLOG(FATAL) << "failed to read functionfs event";
                } else if (rc == 0) {
                    LOG(WARNING) << "hit EOF on functionfs control fd";
                    break;
                } else if (rc != sizeof(event)) {
                    LOG(FATAL) << "read functionfs event of unexpected size, expected "
                               << sizeof(event) << ", got " << rc;
                }

                VLOG(USB) << "USB event: "
                          << to_string(static_cast<usb_functionfs_event_type>(event.type));

                switch (event.type) {
                    case FUNCTIONFS_BIND:
                        if (bound) {
                            LOG(WARNING) << "received FUNCTIONFS_BIND while already bound?";
                            running = false;
                            break;
                        }

                        if (enabled) {
                            LOG(WARNING) << "received FUNCTIONFS_BIND while already enabled?";
                            running = false;
                            break;
                        }

                        bound = true;
                        break;

                    case FUNCTIONFS_ENABLE:
                        if (!bound) {
                            LOG(WARNING) << "received FUNCTIONFS_ENABLE while not bound?";
                            running = false;
                            break;
                        }

                        if (enabled) {
                            LOG(WARNING) << "received FUNCTIONFS_ENABLE while already enabled?";
                            running = false;
                            break;
                        }

                        enabled = true;
                        StartWorker();
                        break;

                    case FUNCTIONFS_DISABLE:
                        if (!bound) {
                            LOG(WARNING) << "received FUNCTIONFS_DISABLE while not bound?";
                        }

                        if (!enabled) {
                            LOG(WARNING) << "received FUNCTIONFS_DISABLE while not enabled?";
                        }

                        enabled = false;
                        running = false;
                        break;

                    case FUNCTIONFS_UNBIND:
                        if (enabled) {
                            LOG(WARNING) << "received FUNCTIONFS_UNBIND while still enabled?";
                        }

                        if (!bound) {
                            LOG(WARNING) << "received FUNCTIONFS_UNBIND when not bound?";
                        }

                        bound = false;
                        running = false;
                        break;

                    case FUNCTIONFS_SETUP: {
                        VLOG(USB) << "received FUNCTIONFS_SETUP control transfer: bRequestType = "
                                  << static_cast<int>(event.u.setup.bRequestType)
                                  << ", bRequest = " << static_cast<int>(event.u.setup.bRequest)
                                  << ", wValue = " << static_cast<int>(event.u.setup.wValue)
                                  << ", wIndex = " << static_cast<int>(event.u.setup.wIndex)
                                  << ", wLength = " << static_cast<int>(event.u.setup.wLength);

                        if ((event.u.setup.bRequestType & USB_DIR_IN)) {
                            VLOG(USB) << "acking device-to-host control transfer";
                            ssize_t rc = adb_write(control_fd_.get(), "", 0);
                            if (rc != 0) {
                                PLOG(ERROR) << "failed to write empty packet to host";
                                break;
                            }
                        } else {
                            std::string buf;
                            buf.resize(event.u.setup.wLength + 1);

                            ssize_t rc = adb_read(control_fd_.get(), buf.data(), buf.size());
                            if (rc != event.u.setup.wLength) {
                                LOG(ERROR)
                                        << "read " << rc
                                        << " bytes when trying to read control request, expected "
                                        << event.u.setup.wLength;
                            }

                            VLOG(USB) << "control request contents: " << buf;
                            break;
                        }
                    }
                }
            }

            StopWorker();
            HandleError("monitor thread finished");
        });
    }

    void StartWorker() {
        CHECK(!worker_started_);
        worker_started_ = true;
        worker_thread_ = std::thread([this]() {
            adb_thread_setname("UsbFfs-worker");
            VLOG(USB) << "UsbFfs-worker thread spawned";

            for (size_t i = 0; i < kUsbReadQueueDepth; ++i) {
                read_requests_[i] = CreateReadBlock(next_read_id_++);
                if (!SubmitRead(&read_requests_[i])) {
                    return;
                }
            }

            while (!stopped_) {
                uint64_t dummy;
                ssize_t rc = adb_read(worker_event_fd_.get(), &dummy, sizeof(dummy));
                if (rc == -1) {
                    PLOG(FATAL) << "failed to read from eventfd";
                } else if (rc == 0) {
                    LOG(FATAL) << "hit EOF on eventfd";
                }

                HandleEvents();

                std::lock_guard<std::mutex> lock(write_mutex_);
                SubmitWrites();
            }
        });
    }

    void StopWorker() {
        if (!worker_started_) {
            return;
        }

        pthread_t worker_thread_handle = worker_thread_.native_handle();
        while (true) {
            int rc = pthread_kill(worker_thread_handle, kInterruptionSignal);
            if (rc != 0) {
                LOG(ERROR) << "failed to send interruption signal to worker: " << strerror(rc);
                break;
            }

            std::this_thread::sleep_for(100ms);

            rc = pthread_kill(worker_thread_handle, 0);
            if (rc == 0) {
                continue;
            } else if (rc == ESRCH) {
                break;
            } else {
                LOG(ERROR) << "failed to send interruption signal to worker: " << strerror(rc);
            }
        }

        worker_thread_.join();
    }

    void PrepareReadBlock(IoReadBlock* block, uint64_t id) {
        block->pending = false;
        if (block->payload.capacity() >= kUsbReadSize) {
            block->payload.resize(kUsbReadSize);
        } else {
            block->payload = Block(kUsbReadSize);
        }
        block->control.aio_data = static_cast<uint64_t>(TransferId::read(id));
        block->control.aio_buf = reinterpret_cast<uintptr_t>(block->payload.data());
        block->control.aio_nbytes = block->payload.size();
    }

    IoReadBlock CreateReadBlock(uint64_t id) {
        IoReadBlock block;
        PrepareReadBlock(&block, id);
        block.control.aio_rw_flags = 0;
        block.control.aio_lio_opcode = IOCB_CMD_PREAD;
        block.control.aio_reqprio = 0;
        block.control.aio_fildes = read_fd_.get();
        block.control.aio_offset = 0;
        block.control.aio_flags = IOCB_FLAG_RESFD;
        block.control.aio_resfd = worker_event_fd_.get();
        return block;
    }

    void HandleEvents() {
        static constexpr size_t kMaxEvents = kUsbReadQueueDepth + kUsbWriteQueueDepth;
        struct io_event events[kMaxEvents];
        struct timespec timeout = {.tv_sec = 0, .tv_nsec = 0};
        int rc = io_getevents(aio_context_.get(), 0, kMaxEvents, events, &timeout);
        if (rc == -1) {
            HandleError(StringPrintf("io_getevents failed while reading: %s", strerror(errno)));
            return;
        }

        for (int event_idx = 0; event_idx < rc; ++event_idx) {
            auto& event = events[event_idx];
            TransferId id = TransferId::from_value(event.data);

            if (event.res < 0) {
                // On initial connection, some clients will send a ClearFeature(HALT) to
                // attempt to resynchronize host and device after the adb server is killed.
                // On newer device kernels, the reads we've already dispatched will be cancelled.
                // Instead of treating this as a failure, which will tear down the interface and
                // lead to the client doing the same thing again, just resubmit if this happens
                // before we've actually read anything.
                if (!connection_started_ && event.res == -EPIPE &&
                    id.direction == TransferDirection::READ) {
                    uint64_t read_idx = id.id % kUsbReadQueueDepth;
                    SubmitRead(&read_requests_[read_idx]);
                    continue;
                } else {
                    std::string error =
                            StringPrintf("%s %" PRIu64 " failed with error %s",
                                         id.direction == TransferDirection::READ ? "read" : "write",
                                         id.id, strerror(-event.res));
                    HandleError(error);
                    return;
                }
            }

            if (id.direction == TransferDirection::READ) {
                connection_started_ = true;
                if (!HandleRead(id, event.res)) {
                    return;
                }
            } else {
                HandleWrite(id);
            }
        }
    }

    bool HandleRead(TransferId id, int64_t size) {
        uint64_t read_idx = id.id % kUsbReadQueueDepth;
        IoReadBlock* block = &read_requests_[read_idx];
        block->pending = false;
        VLOG(USB) << "HandleRead, resizing from " << block->payload.size() << " to " << size;
        block->payload.resize(size);

        // Notification for completed reads can be received out of order.
        if (block->id().id != needed_read_id_) {
            VLOG(USB) << "read " << block->id().id << " completed while waiting for "
                      << needed_read_id_;
            return true;
        }

        for (uint64_t id = needed_read_id_;; ++id) {
            size_t read_idx = id % kUsbReadQueueDepth;
            IoReadBlock* current_block = &read_requests_[read_idx];
            if (current_block->pending) {
                break;
            }
            if (!ProcessRead(current_block)) {
                return false;
            }
            ++needed_read_id_;
        }

        return true;
    }

    bool ProcessRead(IoReadBlock* block) {
        if (!block->payload.empty()) {
            if (packet_reader_.add_bytes(std::move(block->payload)) != APacketReader::OK) {
                HandleError("Error while reading USB block");
                return false;
            }

            auto packets = packet_reader_.get_packets();
            for (auto& p : packets) {
                if (p->msg.command == A_CNXN) {
                    CancelWrites();
                }
                transport_->HandleRead(std::move(p));
            }

            block->payload.clear();
        }

        PrepareReadBlock(block, block->id().id + kUsbReadQueueDepth);
        SubmitRead(block);
        return true;
    }

    bool SubmitRead(IoReadBlock* block) {
        block->pending = true;
        struct iocb* iocb = &block->control;
        if (io_submit(aio_context_.get(), 1, &iocb) != 1) {
            HandleError(StringPrintf("failed to submit read: %s", strerror(errno)));
            return false;
        }

        return true;
    }

    void HandleWrite(TransferId id) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        auto it =
                std::find_if(write_requests_.begin(), write_requests_.end(), [id](const auto& req) {
                    return static_cast<uint64_t>(req.id()) == static_cast<uint64_t>(id);
                });
        CHECK(it != write_requests_.end());

        write_requests_.erase(it);
        size_t outstanding_writes = --writes_submitted_;
        VLOG(USB) << "USB write: reaped, down to " << outstanding_writes;
    }

    IoWriteBlock CreateWriteBlock(std::shared_ptr<Block> payload, size_t offset, size_t len,
                                  uint64_t id) {
        auto block = IoWriteBlock();
        block.payload = std::move(payload);
        block.control.aio_data = static_cast<uint64_t>(TransferId::write(id));
        block.control.aio_rw_flags = 0;
        block.control.aio_lio_opcode = IOCB_CMD_PWRITE;
        block.control.aio_reqprio = 0;
        block.control.aio_fildes = write_fd_.get();
        block.control.aio_buf = reinterpret_cast<uintptr_t>(block.payload->data() + offset);
        block.control.aio_nbytes = len;
        block.control.aio_offset = 0;
        block.control.aio_flags = IOCB_FLAG_RESFD;
        block.control.aio_resfd = worker_event_fd_.get();
        return block;
    }

    IoWriteBlock CreateWriteBlock(Block&& payload, uint64_t id) {
        size_t len = payload.size();
        return CreateWriteBlock(std::make_shared<Block>(std::move(payload)), 0, len, id);
    }

    void SubmitWrites() REQUIRES(write_mutex_) {
        if (writes_submitted_ == kUsbWriteQueueDepth) {
            return;
        }

        ssize_t writes_to_submit = std::min(kUsbWriteQueueDepth - writes_submitted_,
                                            write_requests_.size() - writes_submitted_);
        CHECK_GE(writes_to_submit, 0);
        if (writes_to_submit == 0) {
            return;
        }

        struct iocb* iocbs[kUsbWriteQueueDepth];
        for (int i = 0; i < writes_to_submit; ++i) {
            CHECK(!write_requests_[writes_submitted_ + i].pending);
            write_requests_[writes_submitted_ + i].pending = true;
            iocbs[i] = &write_requests_[writes_submitted_ + i].control;
            VLOG(USB) << "submitting write_request " << static_cast<void*>(iocbs[i]);
        }

        writes_submitted_ += writes_to_submit;

        int rc = io_submit(aio_context_.get(), writes_to_submit, iocbs);
        if (rc == -1) {
            HandleError(StringPrintf("failed to submit write requests: %s", strerror(errno)));
            return;
        } else if (rc != writes_to_submit) {
            LOG(FATAL) << "failed to submit all writes: wanted to submit " << writes_to_submit
                       << ", actually submitted " << rc;
        }
    }

    void CancelWrites() {
        std::lock_guard<std::mutex> lock(write_mutex_);
        for (size_t i = 0; i < writes_submitted_; ++i) {
            struct io_event res;
            if (write_requests_[i].pending == true) {
                VLOG(USB) << "cancelling pending write# " << i;
                io_cancel(aio_context_.get(), &write_requests_[i].control, &res);
            }
        }
    }

    void HandleError(const std::string& error) {
        std::call_once(error_flag_, [&]() {
            if (transport_) {
                transport_->HandleError(error);
            }

            if (!stopped_) {
                Stop();
            }
        });
    }

    std::thread monitor_thread_;

    bool worker_started_;
    std::thread worker_thread_;

    std::atomic<bool> stopped_;
    std::promise<void> destruction_notifier_;
    std::once_flag error_flag_;

    unique_fd worker_event_fd_;
    unique_fd monitor_event_fd_;

    ScopedAioContext aio_context_;
    unique_fd control_fd_;
    unique_fd read_fd_;
    unique_fd write_fd_;

    bool connection_started_ = false;
    APacketReader packet_reader_;

    std::array<IoReadBlock, kUsbReadQueueDepth> read_requests_;
    IOVector read_data_;

    // ID of the next request that we're going to send out.
    size_t next_read_id_ = 0;

    // ID of the next packet we're waiting for.
    size_t needed_read_id_ = 0;

    std::mutex write_mutex_;
    std::deque<IoWriteBlock> write_requests_ GUARDED_BY(write_mutex_);
    size_t next_write_id_ GUARDED_BY(write_mutex_) = 0;
    size_t writes_submitted_ GUARDED_BY(write_mutex_) = 0;

    static constexpr int kInterruptionSignal = SIGUSR1;
};

static void usb_ffs_open_thread() {
    adb_thread_setname("usb ffs open");

    // When the device is acting as a USB host, we'll be unable to bind to the USB gadget on kernels
    // that don't carry a downstream patch to enable that behavior.
    //
    // This property is copied from vendor.sys.usb.adb.disabled by an init.rc script.
    //
    // Note that this property only disables rebinding the USB gadget: setting it while an interface
    // is already bound will do nothing.
    static const char* kPropertyUsbDisabled = "sys.usb.adb.disabled";
    PropertyMonitor prop_mon;
    prop_mon.Add(kPropertyUsbDisabled, [](std::string value) {
        // Return false (i.e. break out of PropertyMonitor::Run) when the property != 1.
        return android::base::ParseBool(value) == android::base::ParseBoolResult::kTrue;
    });

    while (true) {
        unique_fd control;
        unique_fd bulk_out;
        unique_fd bulk_in;
        if (!open_functionfs(&control, &bulk_out, &bulk_in)) {
            std::this_thread::sleep_for(1s);
            continue;
        }

        if (android::base::GetBoolProperty(kPropertyUsbDisabled, false)) {
            VLOG(USB) << "pausing USB due to " << kPropertyUsbDisabled;
            prop_mon.Run();
            VLOG(USB) << "resuming USB";
        }

        atransport* transport = new atransport(kTransportUsb);
        transport->serial = "UsbFfs";
        std::promise<void> destruction_notifier;
        std::future<void> future = destruction_notifier.get_future();
        transport->SetConnection(std::make_unique<UsbFfsConnection>(
                std::move(control), std::move(bulk_out), std::move(bulk_in),
                std::move(destruction_notifier)));
        register_transport(transport);
        future.wait();
    }
}

void usb_init() {
    std::thread(usb_ffs_open_thread).detach();
}
