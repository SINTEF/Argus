# Argus Widget

The project-owned `src/payload_widget.c` registers the directory containing
`widget_config.json` directly: `$ARGUS_DATA_DIR/en_big_screen`, by default
`/etc/argus/en_big_screen`. Both the default
UI and English big-screen UI use that directory. It must contain valid JSON.

After `DjiCore_ApplicationStart()` succeeds, `PayloadWidget_Start()` creates a
one-second display task. It sends `Waiting for sensor data` until the first sample,
then CH4, the sample age in seconds, wind direction in degrees, and Temperature
in degrees Celsius. Refresh does not depend on GPS
callbacks or serial reads completing. Send errors and recovery are logged; a
successful SDK return confirms acceptance by the SDK, not rendering on the RC.

On the first send and every ten sends, diagnostics log the outgoing text, send
result, SDK getter-call counts, and floating-window channel state (busy flag and
bandwidth limit/before/after flow control in bytes/s). Getter calls show SDK
polling, not confirmation that Pilot received the values. Channel statistics
likewise do not confirm rendering. Open the payload settings to check whether
the `CH4 ppm x1000`, `Wind direction (deg)`, and `Temperature` controls appear independently of the floating
window. The main-interface action-bar widget list is intentionally empty.

The three integer settings widgets have matching handlers: index 0 is CH4 scaled
by 1000, index 1 is wind direction rounded to whole degrees, and index 2 is
Temperature scaled by 100 to retain two decimal places (see its `degC x100` hint).
Setting these values from Pilot does not alter sensor measurements. The floating
window displays CH4 directly in ppm and Temperature directly in degrees Celsius.
Wind direction comes from `sr_payload.dir_deg`; Temperature comes from
`sr_payload.tanem_degc`. The current sensor config maps these to zero-based columns
21 and 23 respectively. Missing values follow the reader's zero-fill behavior.
Wind direction is corrected for the 45-degree anemometer mounting offset and
displayed as a plain three-sample (approximately three-second) rolling average.

## Deploy and inspect

Build the executable, then deploy the executable, UI configuration and sensor
mapping with the installer (see [INSTALL_TARGET.md](INSTALL_TARGET.md)):

```bash
./scripts/install_target.sh rp
ssh rp 'journalctl -u sensor-payload.service -b -n 100 --no-pager'
```

Startup fails before widget initialization if `$ARGUS_DATA_DIR/sensor-fields.conf`
or the sensor device cannot be opened.

Expected logs include:

- `Using config directory /etc/argus`
- `[Widget] UI config directory: /etc/argus/en_big_screen`
- `PayloadWidget_Init() init'd OK`
- `[Widget] Floating-window message accepted by SDK`

A `DjiWidget_Reg... failed` message points to UI registration. A
`[Widget] Floating-window send failed` message includes the SDK return code for
link/API diagnosis. If the RC remains blank despite successful SDK sends, verify
the deployed JSON and reopen the payload panel in Pilot; collect the startup logs
and RC/Pilot version for further diagnosis.

## Local checks

```bash
python3 -m json.tool src/en_big_screen/widget_config.json
cmake --build build-rpi4-aarch64 -j4
cc -std=gnu99 -Wall -Wextra -Werror -Itests/stubs \
  -IPayload-SDK/psdk_lib/include \
  -IPayload-SDK/samples/sample_c/module_sample \
  tests/payload_widget_test.c -o /tmp/payload_widget_test -lm
/tmp/payload_widget_test
```

The standalone test uses mocked SDK calls to check registration, startup text,
formatted readings, numeric getters and send-error propagation. RC rendering
requires verification on the aircraft/controller.
