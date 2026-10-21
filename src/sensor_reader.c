// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 SINTEF Digital
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "sensor_reader.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <dji_logger.h>


enum sr_type { SR_STRING, SR_INT, SR_INT64, SR_DOUBLE };

struct sr_field {
    const char *name;
    enum sr_type type;
    size_t offset;
};

#define FIELD(name, type) { #name, type, offsetof(struct sr_payload, name) }
static const struct sr_field payload_fields[] = {
    FIELD(time_stamp, SR_STRING),
    FIELD(inlet_number, SR_INT),
    FIELD(p_mbars, SR_DOUBLE),
    FIELD(t0_degc, SR_DOUBLE),
    FIELD(t5_degc, SR_DOUBLE),
    FIELD(tgas_degc, SR_DOUBLE),
    FIELD(laser_pid_readout, SR_DOUBLE),
    FIELD(det_pid_readout, SR_DOUBLE),
    FIELD(win0_fit0, SR_DOUBLE),
    FIELD(win0_fit1, SR_DOUBLE),
    FIELD(win0_fit2, SR_DOUBLE),
    FIELD(win0_fit3, SR_DOUBLE),
    FIELD(win0_fit4, SR_DOUBLE),
    FIELD(win0_fit5, SR_DOUBLE),
    FIELD(win0_fit6, SR_DOUBLE),
    FIELD(win0_fit7, SR_DOUBLE),
    FIELD(win0_fit8, SR_DOUBLE),
    FIELD(win0_fit9, SR_DOUBLE),
    FIELD(win1_fit0, SR_DOUBLE),
    FIELD(win1_fit1, SR_DOUBLE),
    FIELD(win1_fit2, SR_DOUBLE),
    FIELD(win1_fit3, SR_DOUBLE),
    FIELD(win1_fit4, SR_DOUBLE),
    FIELD(win1_fit5, SR_DOUBLE),
    FIELD(win1_fit6, SR_DOUBLE),
    FIELD(det_bkgd, SR_DOUBLE),
    FIELD(ramp_ampl, SR_DOUBLE),
    FIELD(ch4_ppm_wet, SR_DOUBLE),
    FIELD(ch4_ppm, SR_DOUBLE),
    FIELD(h2o_ppm, SR_DOUBLE),
    FIELD(c2h6_ppb_wet, SR_DOUBLE),
    FIELD(c2h6_ppb, SR_DOUBLE),
    FIELD(r, SR_DOUBLE),
    FIELD(c2_c1, SR_DOUBLE),
    FIELD(battery_charge_v, SR_DOUBLE),
    FIELD(power_input_mv, SR_DOUBLE),
    FIELD(current_ma, SR_DOUBLE),
    FIELD(soc_percent, SR_DOUBLE),
    FIELD(battery_t_degc, SR_DOUBLE),
    FIELD(fet_t_degc, SR_DOUBLE),
    FIELD(gps_time, SR_INT64),
    FIELD(latitude, SR_DOUBLE),
    FIELD(longitude, SR_DOUBLE),
    FIELD(altitude_m, SR_DOUBLE),
    FIELD(co2_ppm, SR_DOUBLE),
    FIELD(u_m_sec, SR_DOUBLE),
    FIELD(v_m_sec, SR_DOUBLE),
    FIELD(w_m_sec, SR_DOUBLE),
    FIELD(tanem_degc, SR_DOUBLE),
    FIELD(dir_deg, SR_DOUBLE),
    FIELD(speed_m_sec, SR_DOUBLE),
    FIELD(rh_percent, SR_DOUBLE),
    FIELD(pressure, SR_DOUBLE),
    FIELD(compass_deg, SR_DOUBLE),
};
#undef FIELD
#define SR_FIELD_COUNT (sizeof(payload_fields) / sizeof(payload_fields[0]))

struct sr_mapping {
    size_t column;
    const struct sr_field *field;
};

struct sr_cont {
	char device[257];
	int baud;
	int fd;
	struct sr_mapping mappings[SR_FIELD_COUNT];
	size_t mapping_count;
	bool line_ready;
	char buffer[4096];
	char logfile[257];
	FILE *log;
};

struct sr_cont sr = {
	.device = {0},
	.baud = 0,
	.fd = -1,

	.buffer = {0},
	.logfile = {0},
	.log = NULL,
};
static bool load_config(struct sr_cont *sr, const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        USER_LOG_ERROR("[SR] Cannot open config %s: %s", path, strerror(errno));
        return false;
    }
    sr->mapping_count = 0;
    char line[256];
    unsigned line_number = 0;
    bool ok = true;
    while (fgets(line, sizeof(line), file)) {
        ++line_number;
        if (!strchr(line, '\n') && !feof(file)) { ok = false; break; }
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        char *start = line;
        while (isspace((unsigned char)*start)) ++start;
        if (!*start) continue;
        char column[32], name[64], type[16], extra;
        if (sscanf(start, "%31s %63s %15s %c", column, name, type, &extra) != 3) {
            ok = false; break;
        }
        char *end;
        errno = 0;
        uintmax_t index = strtoumax(column, &end, 10);
        if (!isdigit((unsigned char)column[0]) || *end || errno || index > SIZE_MAX) {
            ok = false; break;
        }
        const struct sr_field *field = NULL;
        for (size_t i = 0; i < SR_FIELD_COUNT; ++i)
            if (!strcmp(name, payload_fields[i].name)) field = &payload_fields[i];
        static const char *types[] = { "string", "int", "int64", "double" };
        if (!field || strcmp(type, types[field->type]) || sr->mapping_count == SR_FIELD_COUNT) {
            ok = false; break;
        }
        for (size_t i = 0; i < sr->mapping_count; ++i)
            if (sr->mappings[i].column == index || sr->mappings[i].field == field) ok = false;
        if (!ok) break;
        sr->mappings[sr->mapping_count++] = (struct sr_mapping){ (size_t)index, field };
    }
    if (ferror(file)) ok = false;
    fclose(file);
    if (!ok) {
        USER_LOG_ERROR("[SR] Invalid config %s at line %u", path, line_number);
        sr->mapping_count = 0;
    }
    return ok;
}


struct sr_cont * sr_open(const char *device, speed_t baud, const char *logpath, const char *log_postfix, const char *config_path)
{
	if (!device || strlen(device) <= 1) {
		USER_LOG_INFO("[SR ERROR] Invalid device name");
		return NULL;
	}
	if (baud != B115200) {
		USER_LOG_INFO("[SR ERROR] Invalid device baudrate");
		return NULL;
	}

    if (!config_path || !*config_path) {
        USER_LOG_ERROR("[SR] Missing sensor field config path");
        return NULL;
    }
    if (!load_config(&sr, config_path)) return NULL;
    sr.line_ready = false;
    sr.logfile[0] = '\0';

	if (logpath && strlen(logpath) > 1) {
		int len = snprintf(sr.logfile, sizeof(sr.logfile), "%s/argus%s.log", logpath, log_postfix);
		if (len < 0 || (size_t)len >= sizeof(sr.logfile)) {
			USER_LOG_ERROR("[SR] Log file path too long: %s/argus%s.log", logpath, log_postfix);
			sr.logfile[0] = '\0';
			return NULL;
		}
		USER_LOG_INFO("[SR] writing to log %s\n", sr.logfile);
	}

	if (strlen(device) >= sizeof(sr.device)) {
		USER_LOG_ERROR("[SR] Device path too long: %s", device);
		return NULL;
	}
	snprintf(sr.device, sizeof(sr.device), "%s", device);
	sr.baud = baud;
	USER_LOG_INFO("[SR] using device=%s", device);


	// open device
	sr.fd = open(sr.device, O_RDONLY | O_NOCTTY);
	if (sr.fd < 0) {
		USER_LOG_INFO("[SR ERROR] Failed openeing serial device");
		return NULL;
	}

	// configure device
    struct termios attrs = {0};
    if (tcgetattr(sr.fd, &attrs) != 0) {
        USER_LOG_INFO("[SR ERROR] tcgetattr failed: %s", strerror(errno));
        goto err_out;
    }
    attrs.c_iflag = 0;
    attrs.c_oflag = 0;
    attrs.c_lflag = 0;
    attrs.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    attrs.c_cflag |= CREAD | CLOCAL | CS8;
    attrs.c_cc[VMIN] = 1;
    attrs.c_cc[VTIME] = 0;

	// Set input and output baud rate
    if (cfsetispeed(&attrs, baud) != 0 || cfsetospeed(&attrs, baud) != 0) {
        USER_LOG_INFO("[SR ERROR] failed to set baud rate: %s\n", strerror(errno));
        goto err_out;
    }

    if (tcsetattr(sr.fd, TCSANOW, &attrs) != 0) {
        USER_LOG_INFO("[SR ERROR] tcsetattr failed: %s\n", strerror(errno));
        goto err_out;
    }

	/* all OK */
	USER_LOG_INFO("[SR] Opened %s OK", sr.device);
	return &sr;

err_out:
	sr_close(&sr);
	return NULL;
}

void sr_close(struct sr_cont *sr)
{
	if (!sr)
		return;
	if (sr->fd == -1)
		return;
	close(sr->fd);
	sr->fd = -1;
	sr->baud = 0;
	if (sr->log) {
		fclose(sr->log);
		sr->log = NULL;
	}

	memset(sr->buffer, 0, 4096);
}

bool sr_recv(struct sr_cont *sr)
{
    if (!sr) return false;
    sr->line_ready = false;
    sr->buffer[0] = '\0';
    size_t length = 0;
    bool overflow = false;
    for (;;) {
        char ch;
        ssize_t count = read(sr->fd, &ch, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            if (count < 0) USER_LOG_ERROR("[SR] serial read failed: %s", strerror(errno));
            return false; /* Never parse a partial line after EOF/disconnection. */
        }
        if (ch == '\n') {
            sr->buffer[length] = '\0';
            if (overflow) {
                USER_LOG_ERROR("[SR] Discarding oversized or invalid sensor line");
                return false;
            }
            sr->line_ready = true;
            return true;
        }
        if (ch == '\r') continue;
        if (ch == '\0' || length + 1 >= sizeof(sr->buffer)) overflow = true;
        if (!overflow) sr->buffer[length++] = ch;
    }
}

static void convert_field(char *token, const struct sr_field *field, struct sr_payload *sample)
{
    while (isspace((unsigned char)*token)) ++token;
    char *tail = token + strlen(token);
    while (tail > token && isspace((unsigned char)tail[-1])) *--tail = '\0';
    if (!*token || !strcmp(token, "No Data")) return;
    void *dest = (char *)sample + field->offset;
    if (field->type == SR_STRING) {
        if (strlen(token) < SENSOR_TIMESTAMP_MAX) strcpy(dest, token);
        return;
    }
    char *end;
    errno = 0;
    if (field->type == SR_DOUBLE) {
        double value = strtod(token, &end);
        if (end != token && !*end && !errno && isfinite(value)) *(double *)dest = value;
    } else {
        intmax_t value = strtoimax(token, &end, 10);
        // Accept an all-zero fraction, e.g. GPS time written as "1790438592.000".
        if (*end == '.' && end != token) {
            char *fraction = end + 1;
            while (*fraction == '0') fraction++;
            if (!*fraction) end = fraction;
        }
        if (end == token || *end || errno) return;
        if (field->type == SR_INT && value >= INT_MIN && value <= INT_MAX)
            *(int *)dest = (int)value;
        else if (field->type == SR_INT64 && value >= INT64_MIN && value <= INT64_MAX)
            *(int64_t *)dest = (int64_t)value;
    }
}

bool sr_parse(struct sr_cont *sr, struct sr_payload *sample)
{
    if (!sample) return false;
    memset(sample, 0, sizeof(*sample));
    if (!sr || !sr->line_ready) return false;
    sr->line_ready = false;
    char *token = sr->buffer;
    size_t column = 0;
    do {
        char *comma = strchr(token, ',');
        if (comma) *comma = '\0';
        for (size_t i = 0; i < sr->mapping_count; ++i) {
            if (sr->mappings[i].column == column) {
                convert_field(token, sr->mappings[i].field, sample);
                break;
            }
        }
        ++column;
        token = comma ? comma + 1 : NULL;
    } while (token);
    return true;
}

void sr_log_line(struct sr_cont *sr, struct sr_payload *sample)
{
	if (!sr || !sr->logfile[0] || !sample)
		return;
	if (!sr->log) {
		sr->log = fopen(sr->logfile, "w+");
		if (!sr->log) {
			USER_LOG_INFO("[SR ERROR] Failed opening log file %s: %s", sr->logfile, strerror(errno));
			return;
		}
		fprintf(sr->log, "timestamp,inlet_number,p_mbars,t0_degc,t5_degc,tgas_degc,laser_pid_readout,det_pid_readout,win0_fit0,win0_fit1,win0_fit2,win0_fit3,win0_fit4,win0_fit5,win0_fit6,win0_fit7,win0_fit8,win0_fit9,win1_fit0,win1_fit1,win1_fit2,win1_fit3,win1_fit4,win1_fit5,win1_fit6,det_bkgd,ramp_ampl,ch4_ppm_wet,ch4_ppm,h2o_ppm,c2h6_ppb_wet,c2h6_ppb,r,c2_c1,battery_charge_v,power_input_mv,current_ma,soc_percent,battery_t_degc,fet_t_degc,gps_time,latitude,longitude,altitude_m,co2_ppm,u_m_sec,v_m_sec,w_m_sec,tanem_degc,dir_deg,speed_m_sec,rh_percent,pressure,compass_deg,dji_roll,dji_pitch,dji_yaw,dji_lon,dji_lat,dji_alt,dji_numsats,dji_gps_date,dji_gps_time\n");
	}
	fprintf(sr->log, "%s,%d", sample->time_stamp, sample->inlet_number);
	fprintf(sr->log, ",%f,%f,%f,%f,%f", sample->p_mbars, sample->t0_degc, sample->t5_degc, sample->tgas_degc, sample->laser_pid_readout);
	fprintf(sr->log, ",%f,%f,%f,%f,%f", sample->det_pid_readout, sample->win0_fit0, sample->win0_fit1, sample->win0_fit2, sample->win0_fit3);
	fprintf(sr->log, ",%f,%f,%f,%f,%f", sample->win0_fit4, sample->win0_fit5, sample->win0_fit6, sample->win0_fit7, sample->win0_fit8);
	fprintf(sr->log, ",%f,%f,%f,%f,%f", sample->win0_fit9, sample->win1_fit0, sample->win1_fit1, sample->win1_fit2, sample->win1_fit3);
	fprintf(sr->log, ",%f,%f,%f,%f,%f", sample->win1_fit4, sample->win1_fit5, sample->win1_fit6, sample->det_bkgd, sample->ramp_ampl);
	fprintf(sr->log, ",%f,%f,%f,%f,%f", sample->ch4_ppm_wet, sample->ch4_ppm, sample->h2o_ppm, sample->c2h6_ppb_wet, sample->c2h6_ppb);
	fprintf(sr->log, ",%f,%f,%f,%f,%f", sample->r, sample->c2_c1, sample->battery_charge_v, sample->power_input_mv, sample->current_ma);
	fprintf(sr->log, ",%f,%f,%f", sample->soc_percent, sample->battery_t_degc, sample->fet_t_degc);
	fprintf(sr->log, ",%" PRId64, sample->gps_time);
	fprintf(sr->log, ",%f,%f,%f,%f,%f", sample->latitude, sample->longitude, sample->altitude_m, sample->co2_ppm, sample->u_m_sec);
	fprintf(sr->log, ",%f,%f,%f,%f,%f", sample->v_m_sec, sample->w_m_sec, sample->tanem_degc, sample->dir_deg, sample->speed_m_sec);
	fprintf(sr->log, ",%f,%f,%f", sample->rh_percent, sample->pressure, sample->compass_deg);
	// DJI components
	// dji_roll,dji_pitch,dji_yaw,dji_lon,dji_lat,dji_alt,dji_numsats,dji_gps_date,dji_gps_time
	fprintf(sr->log, ",%f,%f,%f", sample->dji_roll, sample->dji_pitch, sample->dji_yaw);
	fprintf(sr->log, ",%f,%f,%f", sample->dji_lon, sample->dji_lat, sample->dji_alt);
	fprintf(sr->log, ",%u,%u,%u", sample->dji_numsats, sample->dji_gps_date, sample->dji_gps_time);

	fprintf(sr->log, "\n");
	fflush(sr->log);
}
