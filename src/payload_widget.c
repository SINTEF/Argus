// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 SINTEF Digital
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "payload_widget.h"
#include <dji_widget.h>
#include <dji_logger.h>
#include <dji_platform.h>
#include <utils/util_misc.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PAYLOAD_WIDGET_CH4_INDEX 0
#define PAYLOAD_WIDGET_WIND_DIRECTION_INDEX 1
#define PAYLOAD_WIDGET_TEMPERATURE_INDEX 2
#define PAYLOAD_WIDGET_COUNT 3
#define WIND_AVERAGE_SAMPLES 3
#define WIND_MOUNTING_OFFSET_DEG 45.0f

static T_DjiMutexHandle s_payloadWidgetMutex;
static uint64_t ch4_last_seen;
static float ch4_ppm;
static float wind_direction_deg;
static float temperature_degc;
static float wind_direction_history[WIND_AVERAGE_SAMPLES];
static unsigned wind_direction_count;
static T_DjiTaskHandle s_widgetTask;
/* Accessed under s_payloadWidgetMutex; SDK polling does not prove RC receipt. */
static unsigned s_getValueCalls[PAYLOAD_WIDGET_COUNT];
static unsigned s_sendAttempts;

uint64_t get_timestamp_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static T_DjiReturnCode PayloadWidget_SetValue(E_DjiWidgetType widgetType,
                                              uint32_t index,
                                              int32_t value,
                                              void *userData)
{
    USER_UTIL_UNUSED(widgetType);
    USER_UTIL_UNUSED(index);
    USER_UTIL_UNUSED(value);
    USER_UTIL_UNUSED(userData);

	// USER_LOG_INFO("[PR] %s()\n", __func__);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode PayloadWidget_GetValue(E_DjiWidgetType widgetType,
                                              uint32_t index,
                                              int32_t *value,
                                              void *userData)
{
    T_DjiOsalHandler *osal = DjiPlatform_GetOsalHandler();
    USER_UTIL_UNUSED(userData);

	// validate data
    if (!value ||
		widgetType != DJI_WIDGET_TYPE_INT_INPUT_BOX ||
        index >= PAYLOAD_WIDGET_COUNT) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
	}

    osal->MutexLock(s_payloadWidgetMutex);

    double scaled = 0;
    switch (index) {
    case PAYLOAD_WIDGET_CH4_INDEX: scaled = (double)ch4_ppm * 1000.0; break;
    case PAYLOAD_WIDGET_WIND_DIRECTION_INDEX: scaled = wind_direction_deg; break;
    case PAYLOAD_WIDGET_TEMPERATURE_INDEX: scaled = (double)temperature_degc * 100.0; break;
    }
    if (!isfinite(scaled)) {
		scaled = 0;
	}
    if (scaled > INT32_MAX) {
		scaled = INT32_MAX;
	}
    if (scaled < INT32_MIN) {
		scaled = INT32_MIN;
	}
    *value = (int32_t)llround(scaled);
    ++s_getValueCalls[index];

    osal->MutexUnlock(s_payloadWidgetMutex);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static const T_DjiWidgetHandlerListItem s_payloadWidgetHandlers[] = {
    { PAYLOAD_WIDGET_CH4_INDEX, DJI_WIDGET_TYPE_INT_INPUT_BOX,
      PayloadWidget_SetValue, PayloadWidget_GetValue, NULL },
    { PAYLOAD_WIDGET_WIND_DIRECTION_INDEX, DJI_WIDGET_TYPE_INT_INPUT_BOX,
      PayloadWidget_SetValue, PayloadWidget_GetValue, NULL },
    { PAYLOAD_WIDGET_TEMPERATURE_INDEX, DJI_WIDGET_TYPE_INT_INPUT_BOX,
      PayloadWidget_SetValue, PayloadWidget_GetValue, NULL },
};

T_DjiReturnCode PayloadWidget_Init(const char *widgetConfigDirPath)
{
    T_DjiReturnCode rc;
    T_DjiOsalHandler *osal = DjiPlatform_GetOsalHandler();

    rc = osal->MutexCreate(&s_payloadWidgetMutex);
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        return rc;
    }

    rc = DjiWidget_Init();
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("DjiWidget_Init failed: 0x%08llX", (unsigned long long)rc);
        return rc;
    }

    USER_LOG_INFO("[Widget] UI config directory: %s", widgetConfigDirPath);
    rc = DjiWidget_RegDefaultUiConfigByDirPath(widgetConfigDirPath);
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("DjiWidget_RegDefaultUiConfigByDirPath failed: 0x%08llX", (unsigned long long)rc);
        return rc;
    }

    rc = DjiWidget_RegUiConfigByDirPath(DJI_MOBILE_APP_LANGUAGE_ENGLISH,
                                        DJI_MOBILE_APP_SCREEN_TYPE_BIG_SCREEN,
                                        widgetConfigDirPath);
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("DjiWidget_RegUiConfigByDirPath failed: 0x%08llX", (unsigned long long)rc);
        return rc;
    }

    rc = DjiWidget_RegHandlerList(
        s_payloadWidgetHandlers,
        sizeof(s_payloadWidgetHandlers) / sizeof(s_payloadWidgetHandlers[0]));
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        USER_LOG_ERROR("DjiWidget_RegHandlerList failed: 0x%08llX", (unsigned long long)rc);
        return rc;
    }

	// Make sure everything is zero'd (should be for static, but play it safe)
	ch4_last_seen = 0;
    memset(s_getValueCalls, 0, sizeof(s_getValueCalls));
    s_sendAttempts = 0;
	ch4_ppm   = 0;
    wind_direction_deg = 0;
    temperature_degc = 0;
    memset(wind_direction_history, 0, sizeof(wind_direction_history));
    wind_direction_count = 0;

	USER_LOG_INFO("[PR] %s() init'd OK\n", __func__);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

void PayloadWidget_UpdateCh4(float _ch4_ppm)
{
    T_DjiOsalHandler *osal = DjiPlatform_GetOsalHandler();
    osal->MutexLock(s_payloadWidgetMutex);
	ch4_ppm = _ch4_ppm;
	ch4_last_seen = get_timestamp_ns();
    osal->MutexUnlock(s_payloadWidgetMutex);
}

void PayloadWidget_UpdateTemperature(float temperature)
{
    T_DjiOsalHandler *osal = DjiPlatform_GetOsalHandler();

    osal->MutexLock(s_payloadWidgetMutex);
	temperature_degc = temperature;
    osal->MutexUnlock(s_payloadWidgetMutex);
}

void PayloadWidget_UpdateWindDirection(float direction_deg)
{
    T_DjiOsalHandler *osal = DjiPlatform_GetOsalHandler();
    float corrected_deg;
    float average_deg;

    if (!isfinite(direction_deg)) {
        return;
    }

	// Do an averaging of the direction
    corrected_deg = (direction_deg + WIND_MOUNTING_OFFSET_DEG);
	if (corrected_deg > 360.0)
		corrected_deg -= 360;
	wind_direction_history[wind_direction_count % WIND_AVERAGE_SAMPLES] = corrected_deg;
	++wind_direction_count;
	for (int i = 0; i < WIND_AVERAGE_SAMPLES; i++) {
		average_deg += wind_direction_history[i];
	}
	average_deg = average_deg / (wind_direction_count < WIND_AVERAGE_SAMPLES ? wind_direction_count : WIND_AVERAGE_SAMPLES);
    osal->MutexLock(s_payloadWidgetMutex);
    wind_direction_deg = average_deg;
    osal->MutexUnlock(s_payloadWidgetMutex);
}

T_DjiReturnCode PayloadWidget_ShowLatestText(void)
{
    T_DjiOsalHandler *osal = DjiPlatform_GetOsalHandler();
    char text[DJI_WIDGET_FLOATING_WINDOW_MSG_MAX_LEN];
    osal->MutexLock(s_payloadWidgetMutex);
    if (!ch4_last_seen) {
        snprintf(text, sizeof(text), "Waiting for sensor data");
    } else {
        double diff_sec = (double)(get_timestamp_ns() - ch4_last_seen) / 1e9;
        snprintf(text, sizeof(text), "CH4: %.3f ppm (%.1f sec)\nWind direction: %.0f deg\nTemperature: %.2f C",
                 ch4_ppm, diff_sec, wind_direction_deg, temperature_degc);
    }
    unsigned attempt = ++s_sendAttempts;
    unsigned gets[PAYLOAD_WIDGET_COUNT];
    memcpy(gets, s_getValueCalls, sizeof(gets));
    osal->MutexUnlock(s_payloadWidgetMutex);
    T_DjiReturnCode rc = DjiWidgetFloatingWindow_ShowMessage(text);
    /* Periodic diagnostics verify ongoing sends, not only the first success. */
    if (attempt == 1 || attempt % 10 == 0) {
        char printable[sizeof(text)];
        snprintf(printable, sizeof(printable), "%s", text);
        for (char *p = printable; *p; ++p) if (*p == '\n') *p = '|';
        USER_LOG_INFO("[Widget] send=%u rc=0x%08llX sdk_gets=%u/%u/%u text=[%s]",
                      attempt, (unsigned long long)rc, gets[0], gets[1], gets[2], printable);
        T_DjiDataChannelState channel = {0};
        T_DjiReturnCode state_rc = DjiWidgetFloatingWindow_GetChannelState(&channel);
        if (state_rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            USER_LOG_INFO("[Widget] channel busy=%d limit=%ld before=%ld after=%ld B/s",
                          channel.busyState, (long)channel.realtimeBandwidthLimit,
                          (long)channel.realtimeBandwidthBeforeFlowController,
                          (long)channel.realtimeBandwidthAfterFlowController);
        } else {
            USER_LOG_ERROR("[Widget] channel state unavailable: 0x%08llX",
                           (unsigned long long)state_rc);
        }
    }
    return rc;
}

static void *PayloadWidget_Task(void *argument)
{
    USER_UTIL_UNUSED(argument);
    T_DjiOsalHandler *osal = DjiPlatform_GetOsalHandler();
    T_DjiReturnCode previous = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    bool first = true;
    for (;;) {
        T_DjiReturnCode rc = PayloadWidget_ShowLatestText();
        if (first || rc != previous) {
            if (rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS)
                USER_LOG_INFO("[Widget] Floating-window message accepted by SDK");
            else
                USER_LOG_ERROR("[Widget] Floating-window send failed: 0x%08llX", (unsigned long long)rc);
        }
        first = false;
        previous = rc;
        osal->TaskSleepMs(1000);
    }
    return NULL;
}

T_DjiReturnCode PayloadWidget_Start(void)
{
    return DjiPlatform_GetOsalHandler()->TaskCreate("payload_widget", PayloadWidget_Task,
                                                   2048, NULL, &s_widgetTask);
}
