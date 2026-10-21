# RaspberryPi 4B Wi-Fi

## Inputs

You need:

```text
SSID:       <your_ssid>
Passphrase: <your_passphrase>
Country:    <your two-letter country code, e.g. NO, US, GB>
```

Use quotes around SSIDs or passphrases that contain spaces or shell-special
characters.

## 1. Set The WLAN Country

The Wi-Fi radio may stay disabled until the wireless country is configured,
especially for 5 GHz networks.

Interactive:

```bash
sudo raspi-config
```

Then select:

```text
Localisation Options -> WLAN Country
```

Non-interactive example:

```bash
sudo raspi-config nonint do_wifi_country NO
```

Replace `NO` with the country where the Pi is operating.

## 2. Check That Wi-Fi Is Enabled

```bash
nmcli radio wifi
```

If it prints `disabled`, enable Wi-Fi:

```bash
sudo nmcli radio wifi on
```

## 3. Scan For Networks

```bash
nmcli dev wifi list
```

Look for your SSID in the `SSID` column.

## 4. Connect Safely With A Prompt

This avoids putting the passphrase in shell history:

```bash
sudo nmcli --ask dev wifi connect "<your_ssid>"
```

Enter the passphrase when prompted.

For a hidden SSID:

```bash
sudo nmcli --ask dev wifi connect "<your_ssid>" hidden yes
```

## 5. Connect Non-Interactively

If you need a one-shot command and accept that the passphrase may appear in shell
history or process logs, use:

```bash
sudo nmcli dev wifi connect "<your_ssid>" password "<your_passphrase>"
```

For a hidden SSID:

```bash
sudo nmcli dev wifi connect "<your_ssid>" password "<your_passphrase>" hidden yes
```

Alternative using `raspi-config`:

```bash
sudo raspi-config nonint do_wifi_ssid_passphrase "<your_ssid>" "<your_passphrase>"
```

For a hidden SSID with `raspi-config`, pass `1` as the hidden flag:

```bash
sudo raspi-config nonint do_wifi_ssid_passphrase "<your_ssid>" "<your_passphrase>" 1
```

## 6. Restrict Routing To One Subnet

If this Wi-Fi link is only for a device network, configure NetworkManager so it
does not make `wlan0` the default route. The goal is:

```text
only 192.168.8.0/24 -> wlan0
everything else     -> existing default route, e.g. eth0 or another uplink
```

Find the NetworkManager connection name:

```bash
nmcli connection show
```

For most simple Wi-Fi connections, the connection name is the SSID. In the examples
below, replace `<connection_name>` with that name.

Prevent this Wi-Fi connection from becoming the IPv4 or IPv6 default route:

```bash
sudo nmcli connection modify "<connection_name>" ipv4.never-default yes
sudo nmcli connection modify "<connection_name>" ipv6.never-default yes
```

If the Pi receives an address directly on `192.168.8.0/24`, for example
`192.168.8.23/24`, NetworkManager will normally install the connected route
automatically. In that case, the two `never-default` commands above are usually
enough.

If `192.168.8.0/24` is reached through a gateway on the Wi-Fi network, add an
explicit route. Replace `192.168.8.1` with the actual Wi-Fi-side gateway:

```bash
sudo nmcli connection modify "<connection_name>" +ipv4.routes "192.168.8.0/24 192.168.8.1"
```

Apply the routing changes:

```bash
sudo nmcli connection down "<connection_name>"
sudo nmcli connection up "<connection_name>"
```

Temporary command-line version, useful for testing before making the route
persistent:

```bash
sudo ip route replace 192.168.8.0/24 dev wlan0
sudo ip route del default dev wlan0 2>/dev/null || true
```

## 7. Verify The Connection And Routing

Check the active Wi-Fi network:

```bash
nmcli dev wifi list
```

The connected network should have `*` in the `IN-USE` column.

Check IP addressing:

```bash
ip -br addr show wlan0
ip route
```

Confirm traffic to `192.168.8.0/24` goes through `wlan0`:

```bash
ip route get 192.168.8.1
```

The result should include:

```text
dev wlan0
```

## 8. Useful Troubleshooting

Show NetworkManager device state:

```bash
nmcli device status
```

Show saved connections:

```bash
nmcli connection show
```

Bring Wi-Fi back up:

```bash
sudo nmcli device up wlan0
```

Disconnect from Wi-Fi:

```bash
sudo nmcli device disconnect wlan0
```

Drop the wifi

```bash
sudo nmcli connection delete "<your_ssid>"
```
