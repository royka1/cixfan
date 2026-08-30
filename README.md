# cixfan — quiet fan control for CIX Sky1 boards booted with ACPI

Restores devicetree-style numeric fan control on a CIX Sky1 board
(Orange Pi 6 Plus / "CIX Phecda Board") running with ACPI instead of a
devicetree, and drives it from a quiet temperature curve.

Tested on Armbian, kernel 7.1.8-edge-cix-p1, aarch64.

## The problem

Under devicetree the fan was driven by Linux: a `pwm-fan` cooling device on
top of `google,cros-ec-pwm`, with a `cooling-levels` curve in the DT.

Under ACPI that path is gone. The firmware exposes only three fan profiles,
through the `CIXHA024` ACPI device:

| ACPI method | via | effect |
|---|---|---|
| `SFAT` | SCMI `SFMD(1)` | automatic — idles the fan at **40 % duty** |
| `SFMT` | SCMI `SFMD(0)` | mute — fan off, no thermal management |
| `SFPF` | SCMI `SFMD(2)` | performance — full speed |

The automatic profile's 40 % floor is what makes the board loud at idle.
There is no quieter firmware profile: "mute" really is off.

## How this driver works

The EC is a Chrome-EC protocol-v3 device at I²C address 0x76, on the
controller at 0x04070000. That controller is **not** exposed to Linux under
ACPI — AML owns it through an `OperationRegion` inside `\_SB.EC0` and speaks
to the EC via the serialized `\_SB.EC0.TRAS` method. Binding an I²C driver
to it would race with AML.

So the driver does not touch the I²C controller. At probe it installs one
extra ACPI method, `\_SB.EC0.XRAW`, with `acpi_install_method()`. That method
is a thin raw passthrough over the firmware's own `TRAS` transport:

```asl
Method (\_SB.EC0.XRAW, 2, Serialized)
{
    Name (RBUF, Buffer (0x40) {})
    Name (ROUT, Package (0x02) { Ones, Zero })
    ROUT [Zero] = \_SB.EC0.TRAS (Arg0, SizeOf (Arg0), RBUF, Arg1)
    ROUT [One] = RBUF
    Return (ROUT)
}
```

Everything else — packet building, checksums, the curve — is plain C.

### The three things that make numeric control work

1. **`EC_CMD_THERMAL_AUTO_FAN_CTRL` (0x0052) profile byte `0` = manual.**
   The firmware only ever uses 1 (auto), 2 (mute) and 4 (performance).
   Profile 0 is undocumented and is the one that matters: without it the EC's
   own control loop overwrites any duty you write within about two seconds,
   so a write looks like a silent no-op even though the EC returns success.

2. **`EC_CMD_PWM_SET_DUTY` (0x0025) takes a percent, 0..100** — not the
   0..65535 range a stock Chrome EC uses. `pwm_type` = generic (0), index 0.
   Read it back with `EC_CMD_PWM_GET_DUTY` (0x0026).

3. **This EC keeps the `u16` fields of `ec_host_request` and
   `ec_host_response` big-endian**, unlike a stock Chrome EC. The platform's
   own AML builds its packets the same way, which is how you can tell.

Request layout on the wire (the platform AML uses exactly this):

```
[0]    0xDA          EC_COMMAND_PROTOCOL_3
[1]    0x03          struct_version
[2]    checksum      0x100 - (sum of [1..end] excluding [2]) & 0xFF
[3..4] command       big-endian u16
[5]    command_version
[6]    reserved
[7..8] data_len      big-endian u16
[9..]  data
```

Response layout:

```
[0]     EC result byte
[1]     packet length, which TRAS verifies == 10 + response data length
[2..9]  ec_host_response (result at [4..5], big-endian)
[10..]  data
```

## Fan curve

Polled every 2 s off the hottest of the `TZB0`, `TZB1`, `TZM0`, `TZM1` and
`TZGT` thermal zones, with 3 °C of hysteresis on the way down:

| SoC temperature | duty |
|---:|---:|
| below 58 °C | 15 % |
| 58 °C | 20 % |
| 64 °C | 28 % |
| 70 °C | 40 % |
| 76 °C | 55 % |
| 81 °C | 70 % |
| 86 °C | 100 % |

Measured on an Orange Pi 6 Plus: idle settles around 54 °C at 15 %; six busy
cores settle around 65 °C at 28 %. Firmware auto held about 43 °C at 40 %, so
this trades roughly 10 °C of idle headroom for silence.

## Proving the fan is really ours

Fan profile byte 0 is undocumented. It was found by probing this board's
embedded controller, not from CIX documentation or the published edk2 source.
An EC firmware that numbers its profiles differently could accept every
command and then ignore the duty writes, leaving the driver running a curve it
is not actually driving.

There are two checks, and it is worth being precise about what each one
proves.

**At probe**, the driver reads the current duty, steps it 12 points away
(downwards where possible, to stay quiet), waits 2.5 s and reads it back. If
the EC does not report roughly what was asked for, the firmware profile is
restored and the driver declines to bind — the module stays loaded but idle,
with no hwmon device and no polling:

```
cix_fan_quiet CIXHA024:00: EC ignored the fan duty write: asked for 32%, it reports 60% (was 20%)
cix_fan_quiet CIXHA024:00: not taking the fan over
```

This proves the command path works end to end — right command numbers, right
duty scale, a fan actually on generic PWM channel 0. It does **not** prove
that profile 0 means manual, and no probe-time check can, because this EC does
not touch the fan while its own computed target is unchanged. Measured on this
board: with the EC left in automatic mode, a manual write of 30 % was still
sitting there untouched 45 seconds later, and re-sending the profile does not
force a re-evaluation either. At a stable temperature, automatic and manual
are indistinguishable from the outside.

**So the real check runs continuously.** Every fifth poll — before
re-asserting, since reading straight after a write would only hand back the
value just written — the driver compares the EC's duty with the one it asked
for. Three consecutive disagreements, about 30 s of a fan that is not
following orders, and it gives the fan back:

```
cix_fan_quiet CIXHA024:00: EC keeps overriding the fan: asked 15%, it has 60%
cix_fan_quiet CIXHA024:00: something else is driving the fan, firmware automatic profile restored
```

That is the check that would actually catch a firmware where profile 0 is not
manual, because it is watching at the moments when the difference becomes
visible: when the EC re-evaluates its own target.

Set `skip_probe_check=1` to skip the probe check only. The runtime check
always runs.

## Failsafes

The firmware's automatic profile is restored on:

- module unload, `reboot` and `shutdown`
- suspend (and re-taken on resume)
- five consecutive EC command failures — the EC transport is shared with AML,
  so a single busy transfer is retried rather than treated as fatal
- the EC repeatedly reporting a duty other than the one asked for
- no thermal zone readable
- any zone above `emergency_temp` (default 95 000 m°C)

The driver never uses the firmware's "mute" profile, which would leave the
fan off with no thermal management at all.

## Installing

```sh
sudo apt install ./cixfan-dkms_1.1-1_all.deb
```

It is a DKMS source package, so it rebuilds automatically on kernel upgrades.
It needs the matching kernel headers installed (`linux-headers-edge-cix-p1`
on this board).

The `postinst` deliberately builds for the running kernel only, rather than
using DKMS' build-for-every-kernel pass. This machine carries several stale
`/lib/modules/*/build` trees — `/lib/modules/6.19.4+/build` is a symlink to an
unconfigured `~/linux` source tree — and that pass aborts the whole
installation on the first unusable one. New kernels are still picked up
automatically by `dkms autoinstall`, which runs per-kernel at install time.
If the running kernel has no usable headers, the package installs cleanly and
prints what to run once the headers are there.

The module autoloads from the `acpi:CIXHA024:` modalias, so nothing else is
needed; `/etc/modules-load.d/cixfan.conf` is shipped as a belt-and-braces
second path.

To uninstall:

```sh
sudo apt remove cixfan-dkms
```

which unloads the module and hands the fan back to the firmware profile.

## Using it

The driver registers an hwmon device named `cix_fan_quiet`, visible in
`sensors` and under `/sys/class/hwmon/hwmonN/`:

| attribute | meaning |
|---|---|
| `pwm1_enable` | `0` firmware performance (full speed), `1` manual, `2` quiet curve (default), `3` firmware automatic |
| `pwm1` | 0..255, mapped onto the EC's 0..100 % |
| `temp1_input` | hottest SoC zone, in m°C |

Example — pin the fan at roughly 25 % by hand:

```sh
h=$(grep -l cix_fan_quiet /sys/class/hwmon/hwmon*/name | xargs dirname)
echo 64 | sudo tee $h/pwm1
echo 1  | sudo tee $h/pwm1_enable
```

Module parameters, all writable at runtime under
`/sys/module/cixfan/parameters/`:

| parameter | default | meaning |
|---|---|---|
| `poll_ms` | 2000 | curve polling interval |
| `emergency_temp` | 95000 | m°C above which the firmware takes over |
| `hyst` | 3000 | m°C of downshift hysteresis |
| `skip_probe_check` | 0 | skip the probe-time duty check (read-only; the runtime check still runs) |

## Changing the curve

Edit `cix_curve[]` in `cixfan.c`, then rebuild the package (below) or rebuild
in place:

```sh
sudo cp cixfan.c /usr/src/cixfan-1.1/
sudo dkms build   -m cixfan -v 1.1 --force
sudo dkms install -m cixfan -v 1.1 --force
sudo modprobe -r cixfan && sudo modprobe cixfan
```

## Diagnostics

`/sys/kernel/debug/cixfan/cmd` is a raw EC passthrough, useful for exploring
the EC command space:

```sh
# read the current fan duty: EC_CMD_PWM_GET_DUTY, version 0,
# 2 response data bytes, params pwm_type=0 index=0
echo '0026 00 02 0000' | sudo tee /sys/kernel/debug/cixfan/cmd
sudo cat /sys/kernel/debug/cixfan/cmd
# -> cmd=0x0026 ver=0 ret=0 data=000f
```

Write format is `<command hex> <version hex> <response data length hex>
[data hex]`; the driver adds the 0xDA prefix, header and checksum. A `ret`
of `-121` (`EREMOTEIO`) means `TRAS` itself failed, usually because the
response length did not match what the EC returned.

## Building the package

```sh
./build-deb.sh          # produces ./cixfan-dkms_1.1-1_all.deb
```

Needs `dpkg-deb` and `fakeroot` only — no root, and no kernel build at
package time. `acpi/cix_ec_raw.hex` is generated from
`acpi/cix-ec-raw.asl` and checked in, so DKMS does not need `iasl` on the
target; after editing the ASL run `make aml` (needs `acpica-tools`) to
regenerate it.

## Files

| path | what |
|---|---|
| `cixfan.c` | the driver |
| `acpi/cix-ec-raw.asl` | the installed ACPI passthrough method |
| `acpi/cix_ec_raw.hex` | its compiled AML, as a C array |
| `dkms.conf`, `Makefile` | DKMS plumbing |
| `packaging/` | Debian control files |
| `build-deb.sh` | builds the .deb |

## Hardware support

Developed and tested on exactly one machine: a CIX Phecda Board (Orange Pi
6 Plus), Armbian, kernel 7.1.8-edge-cix-p1 aarch64. It also builds against
6.18.4 and 6.19.0-rc8. It needs a kernel new enough for the `void`
`platform_driver::remove` signature and will not compile on older ones; the
exact floor has not been established.

`CIXHA024` also appears on other CIX Sky1 boards, but there is no evidence
their EC firmware behaves identically — in particular that profile byte 0
means manual there. Treat this as a driver for this board that may happen to
work on others, not as a general Sky1 driver.

These all fail safely, leaving the firmware in charge:

| situation | result |
|---|---|
| no `CIXHA024` device | never binds |
| no `\_SB.EC0` or no `TRAS` method | probe fails, nothing touched |
| thermal zones named differently | refuses to load |
| EC rejects the manual profile | probe fails, firmware profile restored |
| EC accepts it but ignores duty writes | caught by the probe check |
| EC accepts control then fights for it | caught by the runtime check, ~30 s |

What is *not* portable is the tuning. A 15 % floor holding 54 °C at idle is a
property of this board's heatsink, not of the SoC. Anyone with different
cooling should expect to retune `cix_curve[]`.

The suspend and resume paths are implemented but have not been exercised.

## Licence

GPL-2.0-only, matching the kernel.
