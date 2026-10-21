#!/bin/bash
set -eu
exec /usr/bin/SensorPayload $(date +%F_%H_%M_%S)
