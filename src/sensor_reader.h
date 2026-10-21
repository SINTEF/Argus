// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 SINTEF Digital
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <stdint.h>
#include <termios.h>
#include <stdbool.h>

#define SENSOR_TIMESTAMP_MAX 64


struct sr_payload {
    char time_stamp[SENSOR_TIMESTAMP_MAX]; /* Time Stamp */
    int inlet_number;            /* Inlet Number */
    double p_mbars;              /* P (mbars) */
    double t0_degc;              /* T0 (degC) */
    double t5_degc;              /* T5 (degC) */
    double tgas_degc;            /* Tgas(degC) */
    double laser_pid_readout;    /* Laser PID Readout */

    double det_pid_readout;      /* Det PID Readout */
    double win0_fit0;            /* win0Fit0 */
    double win0_fit1;            /* win0Fit1 */
    double win0_fit2;            /* win0Fit2 */
    double win0_fit3;            /* win0Fit3 */

    double win0_fit4;            /* win0Fit4 */
    double win0_fit5;            /* win0Fit5 */
    double win0_fit6;            /* win0Fit6 */
    double win0_fit7;            /* win0Fit7 */
    double win0_fit8;            /* win0Fit8 */

    double win0_fit9;            /* win0Fit9 */
    double win1_fit0;            /* win1Fit0 */
    double win1_fit1;            /* win1Fit1 */
    double win1_fit2;            /* win1Fit2 */
    double win1_fit3;            /* win1Fit3 */

    double win1_fit4;            /* win1Fit4 */
    double win1_fit5;            /* win1Fit5 */
    double win1_fit6;            /* win1Fit6 */
    double det_bkgd;             /* Det Bkgd */
    double ramp_ampl;            /* Ramp Ampl */

    double ch4_ppm_wet;          /* CH4 (ppm)-Wet */
    double ch4_ppm;              /* CH4 (ppm) */
    double h2o_ppm;              /* H2O (ppm) */
    double c2h6_ppb_wet;         /* C2H6 (ppb)-Wet */
    double c2h6_ppb;             /* C2H6 (ppb) */

    double r;                    /* R */
    double c2_c1;                /* C2/C1 */
    double battery_charge_v;     /* Battery Charge (V) */
    double power_input_mv;       /* Power Input (mV) */
    double current_ma;           /* Current (mA) */

    double soc_percent;          /* SOC (%) */
    double battery_t_degc;       /* Battery T (degC) */
    double fet_t_degc;           /* FET T (degC) */

    int64_t gps_time;            /* GPS Time */

    double latitude;             /* Latitude */
    double longitude;            /* Longitude */
    double altitude_m;           /* Alt. (m) */
    double co2_ppm;              /* CO2 (ppm) */
    double u_m_sec;              /* U (m/sec) */

    double v_m_sec;              /* V (m/sec) */
    double w_m_sec;              /* W (m/sec) */
    double tanem_degc;           /* Tanem (degC) */
    double dir_deg;              /* Dir (deg) */
    double speed_m_sec;          /* Speed (m/sec) */

    double rh_percent;           /* RH(%) */
    double pressure;             /* Pressure */
    double compass_deg;          /* Compass (deg) */

	/* Datafields from Drone */
	double dji_pitch, dji_yaw, dji_roll;
	double dji_lon, dji_lat, dji_alt;
	double dji_vacc, dji_hacc; 	/* Vertical/horizontal accuracy (mm) */
	uint16_t dji_numsats;
	uint32_t dji_gps_date, dji_gps_time;
};

/* Loads the field mapping from config_path at startup. */
struct sr_cont * sr_open(const char *device, speed_t baudrate, const char *logfile, const char *log_postfix, const char *config_path);
void sr_close(struct sr_cont *);
bool sr_recv(struct sr_cont *);
/* Clears the whole payload, then maps the last complete received CSV line. */
bool sr_parse(struct sr_cont *sr, struct sr_payload *);
void sr_log_line(struct sr_cont *sr, struct sr_payload *sample);
