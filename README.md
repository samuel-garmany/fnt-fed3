# FED3 firmware v3 for FNT

> [!WARNING]
> **Alpha, and not yet run on hardware.** The logic is tested on a PC (below);
> the bench checklist at the end has not been worked through yet. Only tested
> against the FED 3.2 hardware revision's pinout.

Firmware for the **Feeding Experimentation Device 3 (FED3)** that runs an
experiment on its own. FNT compiles a protocol file into a schedule and uploads
it at the start of a run. From then on the device makes every decision:
- what each poke earns;
- when to dispense;
- which cue to play;
- when to move to the next phase, by time of day, elapsed time or counts.

The schedule and the device's place in it are kept on the SD card, so a crashed,
rebooted or unplugged PC changes nothing for the animals.

The design, the serial protocol and the data file format are specified in
FNT's [`docs/fed3/design.md`](https://github.com/calebvogt/fnt). This firmware
replaces the 14 hard-coded modes of `ClassicFed3withTimeSync`, and nothing in it
is compatible with that version.

## Layout

| Path | What |
| --- | --- |
| `fnt-fed3.ino` | The FED3's hardware: motor, beam sensors, LEDs, buzzer, BNC, SD card, RTC, display, watchdog. Plus `setup()` and `loop()`. |
| `core.cpp`, `core.h` | Everything that decides what happens: parsing schedules, rules and cues; phases; recovery after a reboot; the data file; serial commands. Portable C++, reaching the hardware only through the `Hal` interface in `core.h`. |
| `sketch.yaml` | The board platform and every library version, pinned. |
| `test/` | The same core built for a PC, with a simulated clock, motor and SD card, and the tests that run on it. Arduino ignores it. |

The repository is the sketch, so Arduino needs it in a folder named
`fnt-fed3` (the name of the `.ino`). A GitHub zip unpacks as `fnt-fed3-v3`;
rename the folder before opening it.

## Building and flashing

From the repository folder:

```bash
arduino-cli compile .
arduino-cli board list                      # find the port
arduino-cli upload -p COM5 .                # or /dev/ttyACM0
```

The first build needs the Adafruit board index:

```bash
arduino-cli config add board_manager.additional_urls \
  https://adafruit.github.io/arduino-board-index/package_adafruit_index.json
arduino-cli core update-index
```

On Windows, Arduino IDE 2 includes `arduino-cli.exe` under
`%LOCALAPPDATA%\Programs\arduino-ide\resources\app\lib\backend\resources\`.

**What a fresh card does:** a device with a blank SD card boots into FR1 on the
left poke and logs to `IDLE.CSV` until FNT starts a run. Set its number once
with `ID:<n>` (FNT does this from Setup); there is no boot menu any more.

## Testing on a PC

The device logic is built for the PC and checked against FNT's reference
simulator (`fnt/fed3/device_sim.py`):

```bash
make -C test test                  # FNT's device test suite, run on this C++
python3 test/differential.py       # random scenarios through both; files must match
```

Both expect an fnt checkout at `../fnt`, or wherever `FNT=` points.
- **`make -C test test`** runs the same checks that define the simulator's
  behaviour, against the C++.
- **`differential.py`** plays days of random but reproducible pokes, retrievals,
  reboots, power cuts and FNT commands through both implementations. It
  requires their data files to match row for row.

## Serial access from WSL

USB devices aren't visible inside WSL until they are attached with
[usbipd-win](https://learn.microsoft.com/windows/wsl/connect-usb). The WSL
kernel already has the USB/IP client and the `cdc-acm` serial driver. Once,
from an **administrator** PowerShell:

```powershell
winget install usbipd
usbipd list                          # find the FED3's BUSID, e.g. 1-4
usbipd bind --busid 1-4
```

Then, each time it is plugged in (no admin needed):

```powershell
usbipd attach --wsl --busid 1-4 --auto-attach
```

In WSL it appears as `/dev/ttyACM0`. You may need to add yourself to
`dialout`, or run `sudo chmod a+rw /dev/ttyACM0`.

- **Flash from Windows, not WSL.** Uploading resets the board into its
  bootloader, which comes back as a different USB device. That's easiest with
  `arduino-cli.exe` on Windows.
- **A FED3 attached to WSL is invisible to Windows** until it is detached
  (`usbipd detach --busid 1-4`). Don't attach a device FNT is using on
  Windows.

## Bench checklist

Work through this on a spare FED3, not one in an experiment, before first use.
Each step names what to send and what to expect; a serial monitor at any baud
rate works.
1. **Flash, then boot with a blank card.**
   - Expect `BOOT,fw=3.0,reset=…,clock=…,sched=default,run=-`.
   - The display shows `FED3 #1`, `Default`, and the counts.
2. **`PING`** → `PONG,id=1,fw=3.1,proto=3`. Then **`ID:7`** → `ID,id=7`; reboot and
   check `#7` and `PONG,id=7`.
3. **Clock.**
   - `TIME:<now in standard time, e.g. 2026-10-20T09:00:00-07:00>` → a
     `TIME,before=…,after=…` line.
   - `STATUS` then shows `clock=ok`.
   - Pull the coin cell with the board unpowered, boot: `clock=unset`.
4. **Pokes.**
   - Poke left: a `ROW` with `event=poke,side=L,outcome=reward`.
   - About a second later a `dispense` row, and the pellet is in the well.
   - Take it: a `retrieve` row with a sensible `retrieval_ms`.
   - Poke right: `outcome=inactive`.
5. **A held beam.**
   - Block the left poke for over a minute.
   - The display and serial must stay responsive throughout.
   - One row with `poke_ms=60000,detail=held`.
6. **Feed.**
   - `FEED` turns the disk at about 14 RPM (a step every 2.1 ms, as the classic
     library paces it) and a pellet drops within a few seconds: a `dispense`
     row with `detail=manual`. A buzz with no movement means the steps are too
     fast for the motor.
7. **Jam.**
   - Empty the hopper and poke left.
   - The motor works through its clearing moves for a minute or two.
   - Then a `jam` row, and `STATUS` shows `jam=1`.
   - Refill: within 10 minutes a `dispense` row with `detail=retry`.
8. **Cues.**
   - `TEST:tone:4000:200` plays a tone.
   - `TEST:flash:bar:0:0:30:0:500` flashes the bar blue, then the bar goes
     dark.
   - `TEST:flash:pokes:0:30:0:0:500` lights both pokes.
   - `LIGHTS:on` lights the bar and both pokes white and leaves them on; a
     `TEST` flash plays over them and they come back. `LIGHTS:off`, a reboot or
     `RUN:BEGIN` turns them off. `STATUS` shows `lights=1` while they're on.
   - `TEST:bnc:10:20:20` gives 20 pulses at 20 Hz on BNC (check with a scope
     or LED).
9. **A schedule from FNT.**
   - Upload example 1 from the design doc with `fnt-fed compile`.
   - Then `SCHED:COMMIT,<crc>` and `RUN:BEGIN,TEST1,<now>,<crc>`.
   - Both pokes now reward. After 50 rewards a `phase` row reads `FR3`.
10. **Power cut.**
   - Mid-run, unplug USB and battery for a minute, then reconnect.
   - `BOOT,…,run=TEST1`.
   - `STATUS` shows the same phase and counts, and `seq` carries on.
11. **Watchdog.** Hard to provoke on purpose. Note any `reset=watchdog` seen
    during long runs, since each is a bug to report.
12. **`READ:TEST1,1`**: the whole run replays, then
    `READ_END,run=TEST1,last=<seq>`.
13. **Soak.** Leave it running a 24 h protocol with a daily window. At the end,
    compare the card's `TEST1.CSV` with FNT's copy; they must be identical.

## Credits

Pin assignments, the motor's dispense and jam-clearing movements, and the
hardware this runs on come from the
[FED3 library](https://github.com/KravitzLabDevices/FED3_library) by Lex
Kravitz and Eric Lin (CC BY-SA 3.0). The original FED is Nguyen et al. 2016.
