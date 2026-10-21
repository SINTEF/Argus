// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 SINTEF Digital
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Exported constants --------------------------------------------------------*/
// DJI application credentials (USER_APP_NAME, USER_APP_ID, USER_APP_KEY,
// USER_APP_LICENSE, USER_DEVELOPER_ACCOUNT) live in app_info.h, which is not
// tracked in git. Copy app_info.h.example to app_info.h and fill it in.
#include "app_info.h"

#define USER_BAUD_RATE              "230400"

// Sensor serial device settings
// #define SENSOR_SERIAL_DEVICE        "/dev/pts/3"
#define SENSOR_SERIAL_DEVICE        "/dev/serial/by-id/usb-FTDI_TTL232R-3V3_FTENBB2G-if00-port0"
#define SENSOR_SERIAL_BAUDRATE      B115200
#define SENSOR_SERIAL_LOGDIR       "/var/log/argus/"

// Configuration directory (field mapping and widget UI). Set at build time with
// -DARGUS_DATA_DIR=PATH; the ARGUS_DATA_DIR environment variable overrides it.
#ifndef ARGUS_DATA_DIR
#define ARGUS_DATA_DIR              "/etc/argus"
#endif

// Default field mapping in ARGUS_DATA_DIR; the ARGUS_SENSOR_FIELDS environment
// variable selects another file by full path.
#define SENSOR_FIELDS_FILE          "sensor-fields.conf"
#define WIDGET_CONFIG_SUBDIR        "en_big_screen"

#define CONFIG_HARDWARE_CONNECTION DJI_USE_ONLY_UART
/* Exported types ------------------------------------------------------------*/
const char *SP_SerialNumber="ARGUS_42";

/* Exported functions --------------------------------------------------------*/

#ifdef __cplusplus
}
#endif
