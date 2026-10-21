// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 SINTEF Digital
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <dji_widget.h>

T_DjiReturnCode PayloadWidget_Init(const char *widgetConfigDirPath);
/* Start periodic display updates after DjiCore_ApplicationStart(). */
T_DjiReturnCode PayloadWidget_Start(void);
void PayloadWidget_UpdateCh4(float ch4_ppm);
void PayloadWidget_UpdateTemperature(float temperature_degc);
void PayloadWidget_UpdateWindDirection(float direction_deg);
T_DjiReturnCode PayloadWidget_ShowLatestText(void);
