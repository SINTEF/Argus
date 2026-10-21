#!/bin/bash
# Configure the Raspberry Pi USB gadget as a DJI PSDK virtual serial port.
# Replace any existing pi4 gadget and expose the local port as /dev/ttyGS0.
set -e

GADGET=/sys/kernel/config/usb_gadget/pi4

modprobe libcomposite

# Remove old gadget if present
if [ -d "$GADGET" ]; then
    if [ -f "$GADGET/UDC" ]; then
        echo "" > "$GADGET/UDC" || true
    fi
    rm -f "$GADGET/configs/c.1/acm.gs0" || true
    rmdir "$GADGET/functions/acm.gs0" 2>/dev/null || true
    rmdir "$GADGET/configs/c.1/strings/0x409" 2>/dev/null || true
    rmdir "$GADGET/configs/c.1" 2>/dev/null || true
    rmdir "$GADGET/strings/0x409" 2>/dev/null || true
    rmdir "$GADGET" 2>/dev/null || true
fi

mkdir -p "$GADGET"
cd "$GADGET"

# DJI VCOM identity
echo 0x2ca3 > idVendor
echo 0xf002 > idProduct

echo 0x0200 > bcdUSB
echo 0x0001 > bcdDevice

mkdir -p strings/0x409
echo "psdk-rpi4b" > strings/0x409/serialnumber
echo "raspberry"   > strings/0x409/manufacturer
echo "PSDK VCOM"   > strings/0x409/product

mkdir -p configs/c.1/strings/0x409
echo "PSDK VCOM" > configs/c.1/strings/0x409/configuration
echo 250 > configs/c.1/MaxPower

mkdir -p functions/acm.gs0
ln -s functions/acm.gs0 configs/c.1/

UDC=$(ls /sys/class/udc | head -n1)

if [ -z "$UDC" ]; then
    echo "No USB Device Controller found"
    exit 1
fi

echo "$UDC" > UDC

echo "PSDK VCOM enabled"
echo "Device should be available locally as /dev/ttyGS0"
