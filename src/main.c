// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 SINTEF Digital
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Portions derived from the DJI Payload SDK sample code,
// Copyright (c) 2021 DJI, under the MIT License (see Payload-SDK/LICENSE.txt).

/* Includes ------------------------------------------------------------------*/
#include <dji_platform.h>
#include <dji_logger.h>
#include <dji_core.h>
#include <utils/util_misc.h>
#include <dji_widget.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>

#include <power_management/test_power_management.h>
#include <fc_subscription/test_fc_subscription.h>

#include <time_sync/test_time_sync.h>
#include <monitor/sys_monitor.h>
#include <osal/osal.h>
#include <osal/osal_fs.h>
#include <osal/osal_socket.h>

// Platform stuff
#include <hal/hal_i2c.h>
#include <hal/hal_uart.h>
#include <dji_aircraft_info.h>
#include <dji_sdk_config.h>

// Payload info
#include "dji_sdk_app_info.h"


// Serial link interface
#include "sensor_reader.h"
#include "payload_widget.h"

/* Private constants ---------------------------------------------------------*/
#define DJI_LOG_PATH                    "Logs/DJI"
#define DJI_LOG_INDEX_FILE_NAME         "Logs/latest"
#define DJI_LOG_FOLDER_NAME             "Logs"
#define DJI_LOG_PATH_MAX_SIZE           (128)
#define DJI_LOG_FOLDER_NAME_MAX_SIZE    (32)
#define DJI_LOG_MAX_COUNT               (10)
#define DJI_SYSTEM_CMD_STR_MAX_SIZE     (64)
#define DJI_SYSTEM_RESULT_STR_MAX_SIZE  (128)

/* Private types -------------------------------------------------------------*/
typedef struct {
    pid_t tid;
    char name[16];
    float pcpu;
} T_ThreadAttribute;

/* Private values -------------------------------------------------------------*/
static FILE *s_djiLogFile;
static FILE *s_djiLogFileCnt;
static pthread_t s_monitorThread = 0;

/* Private functions declaration ---------------------------------------------*/
static T_DjiReturnCode DjiUser_PrepareSystemEnvironment(void);
static T_DjiReturnCode DjiUser_CleanSystemEnvironment(void);
static T_DjiReturnCode DjiUser_FillInUserInfo(T_DjiUserInfo *userInfo);
static T_DjiReturnCode DjiUser_PrintConsole(const uint8_t *data, uint16_t dataLen);
static T_DjiReturnCode DjiUser_LocalWrite(const uint8_t *data, uint16_t dataLen);
static T_DjiReturnCode DjiUser_LocalWriteFsInit(const char *path);
static void *DjiUser_MonitorTask(void *argument);
static void DjiUser_NormalExitHandler(int signalNum);

/*
 * Callback functions for data we subscribe to
 */
struct drone_state {
	dji_f64_t pitch, yaw, roll;
	dji_f64_t lon, lat, alt; // lat/lon: 10^-7 degrees, alt: mm
	dji_f64_t vacc, hacc; // Vertical/Horizontal accuracy (mm)
	uint16_t numsats;
	uint32_t gps_date, gps_time;
	uint64_t pos_time_ms; // last time of pos update
	uint64_t update_time_ms; /* last time display was updated */
};
static struct drone_state *state;

T_DjiReturnCode enable_dji_subscriptions(void);

/*
 * The base of this file is copied from
 * samples/sample_c/platform/linux/raspberry_pi/application/main.c
 */
int main(int argc, char **argv)
{
    T_DjiReturnCode returnCode;
    T_DjiUserInfo userInfo;
    T_DjiAircraftInfoBaseInfo aircraftInfoBaseInfo;
    T_DjiAircraftVersion aircraftInfoVersion;
    T_DjiFirmwareVersion firmwareVersion = {
        .majorVersion = 1,
        .minorVersion = 0,
        .modifyVersion = 0,
        .debugVersion = 0,
    };

	// If we pass an argument, it's a timestamp for the logs. It becomes part of
	// the log file name, so it must not contain a path separator.
	char log_postfix[64]={0};
	if (argc > 1) {
		int len = snprintf(log_postfix, sizeof(log_postfix), "-%s", argv[1]);
		if (strchr(argv[1], '/') || len < 0 || (size_t)len >= sizeof(log_postfix)) {
			fprintf(stderr, "Invalid log name suffix '%s': must not contain '/' and be at most %zu characters\n",
				argv[1], sizeof(log_postfix) - 2);
			return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
		}
		printf("Using %s\n", log_postfix);
	}

    // attention: when the program is hand up ctrl-c will generate the coredump file
    signal(SIGTERM, DjiUser_NormalExitHandler);

    /*!< Step 1: Prepare system environment, such as osal, hal uart, console function and so on. */
    returnCode = DjiUser_PrepareSystemEnvironment();
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("Prepare system environment error");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    USER_LOG_INFO("[OK] System environment (e.g. OSAL, HAL) is ready.");

    // Grab data directory from env, if not present, use the default ARGUS_DATA_DIR from 
	const char *data_dir = getenv("ARGUS_DATA_DIR");
	if (!data_dir || !*data_dir)
		data_dir = ARGUS_DATA_DIR;
	char fields_config[PATH_MAX];
	char widget_config_dir[PATH_MAX];
	if (snprintf(fields_config, sizeof(fields_config), "%s/%s", data_dir, SENSOR_FIELDS_FILE) >= (int)sizeof(fields_config) ||
	    snprintf(widget_config_dir, sizeof(widget_config_dir), "%s/%s", data_dir, WIDGET_CONFIG_SUBDIR) >= (int)sizeof(widget_config_dir)) {
		USER_LOG_ERROR("Config directory path too long: %s", data_dir);
		return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
	}
	// A different field mapping (e.g. sensor-fields-strato58.conf) can be selected
	// with ARGUS_SENSOR_FIELDS; the default is $ARGUS_DATA_DIR/sensor-fields.conf.
	const char *fields_override = getenv("ARGUS_SENSOR_FIELDS");
	if (fields_override && *fields_override) {
		if (snprintf(fields_config, sizeof(fields_config), "%s", fields_override) >= (int)sizeof(fields_config)) {
			USER_LOG_ERROR("Sensor field config path too long: %s", fields_override);
			return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
		}
	}
	USER_LOG_INFO("Using config directory %s, sensor fields %s", data_dir, fields_config);

	struct sr_cont *serial = sr_open(SENSOR_SERIAL_DEVICE, SENSOR_SERIAL_BAUDRATE, SENSOR_SERIAL_LOGDIR, log_postfix, fields_config);
	if (!serial) {
		USER_LOG_ERROR("[SR ERROR] Failed opening sensor serial device, aborting program. Verify that %s exists and is accessible.", SENSOR_SERIAL_DEVICE);
		usleep(50000);
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
	}
	USER_LOG_INFO("[SR] OK, opened log");

    // Opening up serial devvice for reading sensor data
    // FIXME: move down to post-drone setup
    USER_LOG_INFO("target baudrate: %s - target UART DEV1=%s DEV2=%s", USER_BAUD_RATE, LINUX_UART_DEV1, LINUX_UART_DEV2);
	usleep(50000);

    /*!< Step 2: Fill your application information in dji_sdk_app_info.h and use this interface to fill it. */
    returnCode = DjiUser_FillInUserInfo(&userInfo);
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("Fill user info error, please check user info config");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    returnCode = DjiCore_SetFirmwareVersion(firmwareVersion);
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("set firmware version error");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    returnCode = DjiCore_SetSerialNumber(SP_SerialNumber);
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("set serial number error");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    USER_LOG_INFO("Initializing Payload SDK core...\n");
    returnCode = DjiCore_Init(&userInfo);
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("Core init error");
        sleep(1);
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    USER_LOG_INFO("DJI Core Init'd OK");

	returnCode = PayloadWidget_Init(widget_config_dir);
	if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
		USER_LOG_ERROR("payload widget init error");
		return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
	}

    returnCode = DjiCore_SetAlias("SensorPayload");
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("set alias error");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    USER_LOG_INFO("Getting aircraft information");
    returnCode = DjiAircraftInfo_GetBaseInfo(&aircraftInfoBaseInfo);
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("get aircraft base info error");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    returnCode = DjiAircraftInfo_GetAircraftVersion(&aircraftInfoVersion);
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("get aircraft version info error");
    } else {
        USER_LOG_INFO("Aircraft version is V%02d.%02d.%02d.%02d",
                aircraftInfoVersion.majorVersion,
                aircraftInfoVersion.minorVersion,
                aircraftInfoVersion.modifyVersion,
                aircraftInfoVersion.debugVersion);
    }

    if (aircraftInfoBaseInfo.mountPosition == DJI_MOUNT_POSITION_EXTENSION_PORT &&
        (aircraftInfoBaseInfo.aircraftType == DJI_AIRCRAFT_TYPE_M300_RTK ||
         aircraftInfoBaseInfo.aircraftType == DJI_AIRCRAFT_TYPE_M350_RTK)) {
        // TODO: something is no support on E-Port of M300/M350 ???

    }

    /*!< Step 5: Tell the DJI Pilot you are ready. */
    returnCode = DjiCore_ApplicationStart();
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("start sdk application error");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    USER_LOG_INFO("[OK] SDK application ready.");

	// Spawn a worker to periodically update the widget text
    returnCode = PayloadWidget_Start();
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("widget refresh task start failed: 0x%08llX", (unsigned long long)returnCode);
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    /*
     * Create the monitor thread to keep tabs on the system.
     */
    if (pthread_create(&s_monitorThread, NULL, DjiUser_MonitorTask, NULL) != 0) {
        USER_LOG_ERROR("create monitor task fail.");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    if (pthread_setname_np(s_monitorThread, "monitor task") != 0) {
        USER_LOG_ERROR("set name for monitor task fail (non-fatal).");
    }
    USER_LOG_INFO("[OK] Monitoring thread started, entering main loop.");

    /*
     * Drone is ready, start poking for input from sensor.
     *
     * We will let the sensor input drive the main loop (which runs at
     * approx. 1 Hz), but add subscription to the needed fields at a
     * rate x3 expected sample rate.
     */
	if (enable_dji_subscriptions() != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
		USER_LOG_ERROR("Failed setting up payload callbacks, aborting");
		return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
	}

    /*
     * Start main loop where we read from the sensor and push data to the drone.
     */
    T_DjiOsalHandler *osalHandler = DjiPlatform_GetOsalHandler();
    int serial_error_retry = 10;
    struct sr_payload pl = {0};

    while (1) {
        /* Read sensor data */
        if (!sr_recv(serial)) {
            USER_LOG_ERROR("[SR ERROR] Failed reading payload");
            usleep(100000); // ratelimit (100ms) in case of error 
            serial_error_retry--;
            if (serial_error_retry <= 0) {
                USER_LOG_ERROR("[SR ERROR] Too many consecutive read errors, aborting");
                return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
            }
            continue;
        }
        serial_error_retry = 10; // reset retry counter on success

        if (!sr_parse(serial, &pl)) {
            USER_LOG_ERROR("[SR ERROR] Failed parsing payload");
            continue;
        }
		/* FIXME: verify updated Drone data */
		if (state) {
			pl.dji_pitch = state->pitch;
			pl.dji_yaw = state->yaw;
			pl.dji_roll = state->roll;
			pl.dji_lon = state->lon;
			pl.dji_lat = state->lat;
			pl.dji_alt = state->alt;
			pl.dji_vacc = state->vacc;
			pl.dji_hacc = state->hacc;
			pl.dji_numsats = state->numsats;
			pl.dji_gps_date = state->gps_date;
			pl.dji_gps_time = state->gps_time;
			sr_log_line(serial, &pl);
		}
        // FIXME: push sensor data to PSDK/RC
		PayloadWidget_UpdateCh4(pl.ch4_ppm);
		PayloadWidget_UpdateTemperature(pl.tanem_degc);
		PayloadWidget_UpdateWindDirection(pl.dir_deg);

        // FIXME: Add drone data to payload and log to file
    }
}

/* Private functions definition-----------------------------------------------*/
static T_DjiReturnCode DjiUser_PrepareSystemEnvironment(void)
{
    T_DjiReturnCode returnCode;
    T_DjiOsalHandler osalHandler = {
        .TaskCreate = Osal_TaskCreate,
        .TaskDestroy = Osal_TaskDestroy,
        .TaskSleepMs = Osal_TaskSleepMs,
        .MutexCreate= Osal_MutexCreate,
        .MutexDestroy = Osal_MutexDestroy,
        .MutexLock = Osal_MutexLock,
        .MutexUnlock = Osal_MutexUnlock,
        .SemaphoreCreate = Osal_SemaphoreCreate,
        .SemaphoreDestroy = Osal_SemaphoreDestroy,
        .SemaphoreWait = Osal_SemaphoreWait,
        .SemaphoreTimedWait = Osal_SemaphoreTimedWait,
        .SemaphorePost = Osal_SemaphorePost,
        .Malloc = Osal_Malloc,
        .Free = Osal_Free,
        .GetRandomNum = Osal_GetRandomNum,
        .GetTimeMs = Osal_GetTimeMs,
        .GetTimeUs = Osal_GetTimeUs,
    };

    T_DjiLoggerConsole printConsole = {
        .func = DjiUser_PrintConsole,
        .consoleLevel = DJI_LOGGER_CONSOLE_LOG_LEVEL_INFO,
        .isSupportColor = true,
    };


    T_DjiHalUartHandler uartHandler = {
        .UartInit = HalUart_Init,
        .UartDeInit = HalUart_DeInit,
        .UartWriteData = HalUart_WriteData,
        .UartReadData = HalUart_ReadData,
        .UartGetStatus = HalUart_GetStatus,
        .UartGetDeviceInfo = HalUart_GetDeviceInfo,
    };

    T_DjiFileSystemHandler fileSystemHandler = {
        .FileOpen = Osal_FileOpen,
        .FileClose = Osal_FileClose,
        .FileWrite = Osal_FileWrite,
        .FileRead = Osal_FileRead,
        .FileSync = Osal_FileSync,
        .FileSeek = Osal_FileSeek,
        .DirOpen = Osal_DirOpen,
        .DirClose = Osal_DirClose,
        .DirRead = Osal_DirRead,
        .Mkdir = Osal_Mkdir,
        .Unlink = Osal_Unlink,
        .Rename = Osal_Rename,
        .Stat = Osal_Stat,
    };

    T_DjiSocketHandler socketHandler = {
        .Socket = Osal_Socket,
        .Bind = Osal_Bind,
        .Close = Osal_Close,
        .UdpSendData = Osal_UdpSendData,
        .UdpRecvData = Osal_UdpRecvData,
        .TcpListen = Osal_TcpListen,
        .TcpAccept = Osal_TcpAccept,
        .TcpConnect = Osal_TcpConnect,
        .TcpSendData = Osal_TcpSendData,
        .TcpRecvData = Osal_TcpRecvData,
    };

    T_DjiHalI2cHandler i2CHandler = {
        .I2cInit = HalI2c_Init,
        .I2cDeInit = HalI2c_DeInit,
        .I2cWriteData = HalI2c_WriteData,
        .I2cReadData = HalI2c_ReadData,
    };

    returnCode = DjiPlatform_RegOsalHandler(&osalHandler);
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        printf("register osal handler error");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    if (DjiUser_LocalWriteFsInit(DJI_LOG_PATH) != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        printf("file system init error");
        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
    }

    returnCode = DjiLogger_AddConsole(&printConsole);
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        printf("add printf console error");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    returnCode = DjiPlatform_RegHalI2cHandler(&i2CHandler);
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        printf("register hal i2c handler error");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    USER_LOG_INFO("I2C handler registred ok");
    returnCode = DjiPlatform_RegHalUartHandler(&uartHandler);
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        printf("register hal uart handler error");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    returnCode = DjiPlatform_RegFileSystemHandler(&fileSystemHandler);
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        printf("register osal filesystem handler error");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode DjiUser_CleanSystemEnvironment(void)
{
    T_DjiReturnCode returnCode;
    T_DjiAircraftInfoBaseInfo aircraftInfoBaseInfo;

    returnCode = DjiAircraftInfo_GetBaseInfo(&aircraftInfoBaseInfo);
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("get aircraft base info error");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    returnCode = DjiCore_DeInit();
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        perror("Core deinit failed.");
    }

    return returnCode;
}

static T_DjiReturnCode DjiUser_FillInUserInfo(T_DjiUserInfo *userInfo)
{
    if (userInfo == NULL) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }

    memset(userInfo->appName, 0, sizeof(userInfo->appName));
    memset(userInfo->appId, 0, sizeof(userInfo->appId));
    memset(userInfo->appKey, 0, sizeof(userInfo->appKey));
    memset(userInfo->appLicense, 0, sizeof(userInfo->appLicense));
    memset(userInfo->developerAccount, 0, sizeof(userInfo->developerAccount));
    memset(userInfo->baudRate, 0, sizeof(userInfo->baudRate));

    if (strlen(USER_APP_NAME) >= sizeof(userInfo->appName) ||
        strlen(USER_APP_ID) > sizeof(userInfo->appId) ||
        /* strlen(USER_APP_KEY) > sizeof(userInfo->appKey) || */
        /* strlen(USER_APP_LICENSE) > sizeof(userInfo->appLicense) || */
        strlen(USER_DEVELOPER_ACCOUNT) >= sizeof(userInfo->developerAccount) ||
        strlen(USER_BAUD_RATE) > sizeof(userInfo->baudRate)) {
        USER_LOG_ERROR("Length of user information string is beyond limit. Please check.");
        sleep(1);
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }

    if (!strcmp(USER_APP_NAME, "your_app_name") ||
        !strcmp(USER_APP_ID, "your_app_id") ||
        !strcmp(USER_APP_KEY, "your_app_key") ||
        !strcmp(USER_APP_LICENSE, "your_app_license") ||
        !strcmp(USER_DEVELOPER_ACCOUNT, "your_developer_account") ||
        !strcmp(USER_BAUD_RATE, "your_baud_rate")) {
        USER_LOG_ERROR(
            "Please fill in your DJI application information in src/app_info.h (see src/app_info.h.example).");
        sleep(1);
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }

    strncpy(userInfo->appName, USER_APP_NAME, sizeof(userInfo->appName) - 1);
    memcpy(userInfo->appId, USER_APP_ID, USER_UTIL_MIN(sizeof(userInfo->appId), strlen(USER_APP_ID)));
    memcpy(userInfo->appKey, USER_APP_KEY, USER_UTIL_MIN(sizeof(userInfo->appKey), strlen(USER_APP_KEY)));
    memcpy(userInfo->appLicense, USER_APP_LICENSE,
           USER_UTIL_MIN(sizeof(userInfo->appLicense), strlen(USER_APP_LICENSE)));
    memcpy(userInfo->baudRate, USER_BAUD_RATE, USER_UTIL_MIN(sizeof(userInfo->baudRate), strlen(USER_BAUD_RATE)));
    strncpy(userInfo->developerAccount, USER_DEVELOPER_ACCOUNT, sizeof(userInfo->developerAccount) - 1);

    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode DjiUser_PrintConsole(const uint8_t *data, uint16_t dataLen)
{
    USER_UTIL_UNUSED(dataLen);

    printf("%s", data);

    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode DjiUser_LocalWrite(const uint8_t *data, uint16_t dataLen)
{
    uint32_t realLen;

    if (s_djiLogFile == NULL) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
    }

    realLen = fwrite(data, 1, dataLen, s_djiLogFile);
    fflush(s_djiLogFile);
    if (realLen == dataLen) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    } else {
        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
    }
}

static T_DjiReturnCode DjiUser_LocalWriteFsInit(const char *path)
{
    T_DjiReturnCode djiReturnCode = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    char filePath[DJI_LOG_PATH_MAX_SIZE];
    char systemCmd[DJI_SYSTEM_CMD_STR_MAX_SIZE];
    char folderName[DJI_LOG_FOLDER_NAME_MAX_SIZE];
    time_t currentTime = time(NULL);
    struct tm *localTime = localtime(&currentTime);
    uint16_t logFileIndex = 0;
    uint16_t currentLogFileIndex;
    int ret = -1;

    if (localTime == NULL) {
        printf("Get local time error.\r\n");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    if (access(DJI_LOG_FOLDER_NAME, F_OK) != 0) {
        sprintf(folderName, "mkdir %s", DJI_LOG_FOLDER_NAME);
        ret = system(folderName);
        if (ret != 0) {
            return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
        }
    }

    s_djiLogFileCnt = fopen(DJI_LOG_INDEX_FILE_NAME, "rb+");
    if (s_djiLogFileCnt == NULL) {
        s_djiLogFileCnt = fopen(DJI_LOG_INDEX_FILE_NAME, "wb+");
        if (s_djiLogFileCnt == NULL) {
            return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
        }
    } else {
        ret = fseek(s_djiLogFileCnt, 0, SEEK_SET);
        if (ret != 0) {
            printf("Seek log count file error, ret: %d, errno: %d.\r\n", ret, errno);
            return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
        }

        ret = fread((uint16_t *) &logFileIndex, 1, sizeof(uint16_t), s_djiLogFileCnt);
        if (ret != sizeof(uint16_t)) {
            printf("Read log file index error.\r\n");
        }
    }

    currentLogFileIndex = logFileIndex;
    logFileIndex++;

    ret = fseek(s_djiLogFileCnt, 0, SEEK_SET);
    if (ret != 0) {
        printf("Seek log file error, ret: %d, errno: %d.\r\n", ret, errno);
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    ret = fwrite((uint16_t *) &logFileIndex, 1, sizeof(uint16_t), s_djiLogFileCnt);
    if (ret != sizeof(uint16_t)) {
        printf("Write log file index error.\r\n");
        fclose(s_djiLogFileCnt);
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    fclose(s_djiLogFileCnt);

    sprintf(filePath, "%s_%04d_%04d%02d%02d_%02d-%02d-%02d.log", path, currentLogFileIndex,
            localTime->tm_year + 1900, localTime->tm_mon + 1, localTime->tm_mday,
            localTime->tm_hour, localTime->tm_min, localTime->tm_sec);

    s_djiLogFile = fopen(filePath, "wb+");
    if (s_djiLogFile == NULL) {
        USER_LOG_ERROR("Open filepath time error.");
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }

    if (logFileIndex >= DJI_LOG_MAX_COUNT) {
        sprintf(systemCmd, "rm -rf %s_%04d*.log", path, currentLogFileIndex - DJI_LOG_MAX_COUNT);
        ret = system(systemCmd);
        if (ret != 0) {
            printf("Remove file error, ret:%d.\r\n", ret);
            return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
        }
    }

    return djiReturnCode;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#pragma GCC diagnostic ignored "-Wreturn-type"

static void *DjiUser_MonitorTask(void *argument)
{
    unsigned int i = 0;
    unsigned int threadCount = 0;
    pid_t *tidList = NULL;
    T_ThreadAttribute *threadAttribute = NULL;
    T_DjiOsalHandler *osalHandler = DjiPlatform_GetOsalHandler();

    USER_UTIL_UNUSED(argument);
    printf("Starting monitor task...\n");
    while (1) {
        threadCount = Monitor_GetThreadCountOfProcess(getpid());
        tidList = osalHandler->Malloc(threadCount * sizeof(pid_t));
        if (tidList == NULL) {
            USER_LOG_ERROR("malloc fail.");
            goto delay;
        }
        Monitor_GetTidListOfProcess(getpid(), tidList, threadCount);

        threadAttribute = osalHandler->Malloc(threadCount * sizeof(T_ThreadAttribute));
        if (threadAttribute == NULL) {
            USER_LOG_ERROR("malloc fail.");
            goto freeTidList;
        }
        for (i = 0; i < threadCount; ++i) {
            threadAttribute[i].tid = tidList[i];
        }

        USER_LOG_DEBUG("thread pcpu:");
        USER_LOG_DEBUG("tid\tname\tpcpu");
        for (i = 0; i < threadCount; ++i) {
            threadAttribute[i].pcpu = Monitor_GetPcpuOfThread(getpid(), tidList[i]);
            Monitor_GetNameOfThread(getpid(), tidList[i], threadAttribute[i].name, sizeof(threadAttribute[i].name));
            USER_LOG_DEBUG("%d\t%15s\t%f %%.", threadAttribute[i].tid, threadAttribute[i].name,
                           threadAttribute[i].pcpu);
        }

        USER_LOG_DEBUG("heap used: %d B.", Monitor_GetHeapUsed(getpid()));
        USER_LOG_DEBUG("stack used: %d B.", Monitor_GetStackUsed(getpid()));

        osalHandler->Free(threadAttribute);
        freeTidList:
            osalHandler->Free(tidList);

        delay:
            sleep(10);
    }
}

static void DjiUser_NormalExitHandler(int signalNum)
{
    T_DjiReturnCode returnCode;

    USER_UTIL_UNUSED(signalNum);

    returnCode = DjiUser_CleanSystemEnvironment();
    if (returnCode != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        perror("Clean up system environment failed.");
    }

    exit(0);
}

/* Callbacks */

T_DjiReturnCode dji_quaternion_cb(const uint8_t *data, uint16_t sz, const T_DjiDataTimestamp *ts)
{
	if (!state || !data)
		return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;

    T_DjiFcSubscriptionQuaternion *q = (T_DjiFcSubscriptionQuaternion *) data;
    USER_UTIL_UNUSED(sz);
    USER_UTIL_UNUSED(ts);

	/* Used as described in DJI PSDK tutorial  */
	state->pitch = (dji_f64_t) asinf(-2 * q->q1 * q->q3 + 2 * q->q0 * q->q2) * 57.3;
    state->roll  = (dji_f64_t) atan2f(2 * q->q2 * q->q3 + 2 * q->q0 * q->q1, -2 * q->q1 * q->q1 - 2 * q->q2 * q->q2 + 1) * 57.3;
    state->yaw   = (dji_f64_t) atan2f(2 * q->q1 * q->q2 + 2 * q->q0 * q->q3, -2 * q->q2 * q->q2 - 2 * q->q3 * q->q3 + 1) * 57.3;
	return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode dji_position_cb(const uint8_t *data, uint16_t sz, const T_DjiDataTimestamp *ts)
{
	if (!state || !data || !ts)
		return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    USER_UTIL_UNUSED(sz);

	T_DjiFcSubscriptionGpsPosition * pos = (T_DjiFcSubscriptionGpsPosition *)data;

	state->lon = pos->x;
	state->lat = pos->y;
	state->alt = pos->z;
	state->pos_time_ms = ts->millisecond;
	return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode dji_accuracy_cb(const uint8_t *data, uint16_t sz, const T_DjiDataTimestamp *ts)
{
	if (!state || !data || !ts)
		return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    USER_UTIL_UNUSED(sz);

	T_DjiFcSubscriptionGpsDetails *accuracy = (T_DjiFcSubscriptionGpsDetails *)data;
	state->vacc = accuracy->vacc;
	state->hacc = accuracy->hacc;
	state->numsats = accuracy->totalSatelliteNumberUsed;

	return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode dji_gps_date_cb(const uint8_t *data, uint16_t sz, const T_DjiDataTimestamp *ts)
{
	if (!state || !data)
		return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    USER_UTIL_UNUSED(sz);
	USER_UTIL_UNUSED(ts);
	state->gps_date = *(uint32_t *)data;

	return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode dji_gps_time_cb(const uint8_t *data, uint16_t sz, const T_DjiDataTimestamp *ts)
{
	if (!state || !data)
		return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    USER_UTIL_UNUSED(sz);
	USER_UTIL_UNUSED(ts);
	state->gps_time = *(uint32_t *)data;

	return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode enable_dji_subscriptions(void)
{
	// Allocate memory
    T_DjiOsalHandler *osalHandler = DjiPlatform_GetOsalHandler();
	state = osalHandler->Malloc(sizeof(*state));
	if (!state) {
		USER_LOG_ERROR("Failed allocating memory for drone state container");
		return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
	}
	memset(state, 0, sizeof(*state));

	E_DjiDataSubscriptionTopicFreq freq = DJI_DATA_SUBSCRIPTION_TOPIC_5_HZ;

	// Roll-Pitch-Yaw
    T_DjiReturnCode res = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_QUATERNION, freq, dji_quaternion_cb);
    if (res != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("Subscribe topic quaternion error.");
        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
    }

	// GPS position
	res = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_GPS_POSITION, freq, dji_position_cb);
    if (res != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("Subscribe topic GPS POSITION error.");
        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
    }
	res = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_GPS_DETAILS, freq, dji_accuracy_cb);
    if (res != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("Subscribe topic POSITION ACCURACY error.");
        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
    }

	res = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_GPS_DATE, freq, dji_gps_date_cb);
    if (res != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("Subscribe topic GPS_DATE error.");
        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
    }
	res = DjiFcSubscription_SubscribeTopic(DJI_FC_SUBSCRIPTION_TOPIC_GPS_TIME, freq, dji_gps_time_cb);
    if (res != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("Subscribe topic GPS_TIME error.");
        return DJI_ERROR_SYSTEM_MODULE_CODE_UNKNOWN;
    }
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}


#pragma GCC diagnostic pop

/****************** (C) COPYRIGHT DJI Innovations *****END OF FILE****/
