/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "adb_unique_fd.h"
#include "socket.h"

#include <unistd.h>

int init_jdwp();
asocket* create_jdwp_service_socket();
asocket* create_jdwp_tracker_service_socket();
asocket* create_app_tracker_service_socket();

// Create a socket pair. Send one end to the debuggable process `jdwp_pid` and
// return the other one.
unique_fd create_jdwp_connection_fd(pid_t jdwp_pid);