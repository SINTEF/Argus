# Serial CSV Emulator

Create a virtual serial device and stream rows from a CSV file. The first CSV
row is treated as a header and is not sent.

## Run

```bash
./serial_csv_emulator.py --csv serial-stream.csv --rate 100
```

The emulator prints the device path on startup:

```text
Emulated serial port: /dev/pts/0
```

Open that path from your application as the serial device. Stop the emulator
with `Ctrl+C`.

For startup synchronization, use:

```bash
./serial_csv_emulator.py --csv serial-stream.csv --rate 100 --wait
```

`--wait` prints the device path first, then waits until another process opens
it before sending rows.

## Options

- `--csv PATH`: CSV file to stream.
- `--rate MS`: delay between rows in milliseconds.
- `--wait`: wait for another process to open the virtual serial device.
- `--live`: replace the CSV timestamp with the local send time.
- `--fix-time`: use the first CSV timestamp plus elapsed emulator runtime.
- `--limit N`: stop after sending `N` rows.
- `--loop`: restart from the first data row after reaching EOF.

`--live` and `--fix-time` are mutually exclusive.

## Inspect The Stream

In another terminal, use the printed device path:

```bash
cat /dev/pts/0
```

For raw terminal settings:

```bash
stty -F /dev/pts/0 raw -echo
cat /dev/pts/0
```

Save a copy while printing:

```bash
tee captured.csv < /dev/pts/0
```

## Timestamp Output

The first field is emitted as:

```text
DD/MM/YYYY HH:MM:SS.<ms>
```

Default mode converts the source timestamp through epoch milliseconds and
formats it as UTC. `--fix-time` does the same after adding elapsed runtime.
`--live` uses the local wall-clock time when each row is sent.

## Requirements

- Linux with `/dev/pts`
- Python 3
