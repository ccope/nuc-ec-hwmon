# nuc-ec-hwmon

A Linux hwmon driver for the Intel NUC embedded controller. It publishes the
chassis fan tachometer and the EC's temperature channels as an ordinary hwmon
device, so `sensors` shows them and node-exporter's hwmon collector exports
`node_hwmon_fan_rpm_rpm` / `node_hwmon_temp_celsius` with nothing running as
root on a timer.

It **never writes to the EC** — there is no fan control in it.

Nothing on this machine exposes a fan through hwmon by default: `/sys/class/hwmon`
holds only `acpitz`, `coretemp`, `nvme`, `iwlwifi` and two USB-C PSY chips, the
three ACPI fans are ACPI 1.0 on/off devices with no `_FST`, and the Super I/O
window is relocated to `0x164E` where no in-tree driver probes.

It reads through `ec_read()`, the kernel's own ACPI EC path, so there is no
`ioremap`, no `/dev/mem` and no physical address anywhere in it, and the ACPI EC
driver serialises against AML's own EC traffic. It binds to the ACPI device
`INTC1036` (`\_SB.NSSS`) — the natural attach point, and what Intel's own
Windows driver binds to.

## Building and installing

Shipped as `nuc-ec-hwmon-dkms`. DKMS is what makes it survive `apt upgrade`;
without it the sensors vanish at the next kernel bump.

```bash
dpkg-buildpackage -b -us -uc
sudo apt install ../nuc-ec-hwmon-dkms_*.deb
```

The module source lands in `/usr/src/nuc-ec-hwmon-<version>/`, DKMS builds it
against the running kernel, `postinst` modprobes it, and
`/usr/lib/modules-load.d/` loads it at boot.

This stays a Debian package rather than moving to Chef with the rest of the
host config: it is compiled code with a kernel-version dependency, and DKMS
rebuild-on-kernel-upgrade plus clean removal are exactly what a package gives
you and a converge does not.

**The version lives in three places** and they must agree: the
`usr/src/nuc-ec-hwmon-<version>/` paths in `debian/install`, `PACKAGE_VERSION`
in `dkms.conf`, and `MODULE_VERSION()` in the `.c` (cosmetic, but keep it
honest). `postinst`/`prerm` derive it from the installed directory, so they need
no edit.

## Checking it bound

```bash
sensors nuc_ec-*
dkms status nuc-ec-hwmon
dmesg | grep nuc_ec_hwmon      # says which layout it chose
```

Expect a `nuc_ec` chip with `CPU fan` in RPM and five temperature channels. Fan
channels are hidden for slots reporting type 0 (not connected), so a one-fan
machine shows one fan.

Temperatures appear **only** on a board listed in the driver's DMI table,
because unlike the fan block they were never corroborated against vendor code —
and even there only `temp1` is confidently identified. The other four move with
temperature but lag, so they are labelled by their EC address rather than given
invented names. Do not read more into them than that.

## Related documents

- [`UPSTREAMING_TODO.md`](UPSTREAMING_TODO.md) — why this is not yet in a state
  the kernel would take, and what each blocker needs.
- [`FAN_PROFILES.md`](FAN_PROFILES.md) — how fan *mode* selection would work via
  `platform_profile`, and why per-fan pwm control is deliberately out of scope.
  Nothing there is implemented.

---

## How the register layout was found

node-exporter's hwmon collector is already enabled and would export a fan for
free, but out of the box nothing on this machine exposes one to the kernel.

- `/sys/class/hwmon` holds only `acpitz`, `coretemp`, `nvme`, `iwlwifi` and two
  USB-C PSY chips. No `fan*_input` anywhere.
- The three ACPI fans (`PNP0C0B`) are ACPI 1.0 on/off devices — `max_state` is
  1 and there is no `_FST` — so they carry no speed at all, only a state.
- The Super I/O window is relocated to `0x164E` (it shows up as `PNP0C02:01` in
  a root `/proc/ioports`), and no in-tree driver probes there: `nct6775` and
  `it87` only look at `0x2E`/`0x4E`. `modprobe nct6683` — the one driver
  carrying an explicit Intel-NUC customer ID — returns `No such device`.

The data is in the ACPI EC's RAM, and the kernel can already reach it: the
`PNP0C09` device and its `0x62`/`0x66` ports are right there in `/proc/ioports`
with Linux's EC driver bound to them. (An earlier version of this section
claimed otherwise, reasoning from the absence of an `EmbeddedControl` operation
region in the ACPI tables. That was wrong — an operation region is only how
*AML* reaches the EC, and has no bearing on what `ec_read()` or `ec_sys` can
do.)

EC RAM identifies itself — bytes `0x00`–`0x05` are the ASCII `NUC_EC` — and the
fan block is six 3-byte records starting at `0x1C`:

```
[0] fan type (0 = not connected)   [1] speed, high byte   [2] speed, low byte
```

That layout was derived here by measurement and then **confirmed against
Intel's own code**. Sampling the whole window across an idle →
all-threads-busy → cooldown cycle, one value moved like a fan:

| phase | CPU (coretemp) | tachometer |
|---|---|---|
| idle | 74–79 °C | 2996, 2971, 2998 |
| loaded | 95–100 °C | 4507, 4526, 4522 |
| cooling | 89 → 81 °C | 4529 → 4202 → 3969 |

Intel's NUC Software Studio Service parses the same buffer identically —
`type = Data[i*3]`, `speed = Data[i*3+2] + Data[i*3+1] * 256`, with
`MAX_FAN_COUNT = 6` and `FAN_SPEED_LENGTH = 18` — and its `FanType` enumeration
(`1 = CPUFan`, `2 = dGPUFan`, `3/4 = SystemFan1/2`, `5 = LiquidCoolerFanPump`,
`7 = CPUFan2`) supplies the channel labels. On this board only record 0 is
populated, type 1; the other five read type 0 and the driver hides them.

Bytes `0x0E`, `0x0F`, `0x11`, `0x13` and `0x15` track temperature; everything
else is flat. Those five remain empirical — only `temp1` is confidently
identified, and the rest are labelled by their EC address rather than given
invented names.

`host/nuc-ec-hwmon/` turns this into an ordinary hwmon device. It reads through
`ec_read()`, the kernel's own ACPI EC path, so there is no `ioremap`, no
`/dev/mem` and no physical address anywhere in it, and the ACPI EC driver
serialises against AML's own EC traffic for us. `sensors` shows it, and
node-exporter's hwmon collector exports `node_hwmon_fan_rpm_rpm` and
`node_hwmon_temp_celsius` with nothing running as root on a timer. It never
writes to the EC — there is no fan control in it.

It binds to the ACPI device `INTC1036` (`\_SB.NSSS`), the natural attach point
for this hardware — Intel's own Windows driver binds to `ACPI\INTC1036` too.
That device's `_CRS` declares a memory-mapped alias of the same bytes,
`Memory32Fixed(ReadWrite, 0xFE410400, 0xFF)`. Reading that alias through
`/dev/mem` from a root systemd timer was the first thing that worked here; it
has since been deleted, because the EC path is better in every respect and
there is no reason to keep a second way of publishing the same fan.

**The fan block is not in the same place on every NUC**, so the driver carries
Intel's own variants rather than this board's offsets alone. Its service
enumerates `ECNUC21`, `ECNUC22`, `ECNUC23`, `ECBC` and `ECElement`, and its
kernel driver assembles the 18 bytes differently per "EC section":

| Layout | Used by | Assembled from |
|---|---|---|
| `21` | ECNUC21 | 9 bytes at `0x1A`, then 9 at `0x37` |
| `22` | ECNUC22, ECNUC23 | 18 bytes at `0x1C` — **this board** |
| `bc` | ECBC | the `21` read with the first byte dropped and a zero appended |
| — | ECElement | no fan path at all |

Transcribing those gave a free cross-check. Layout `21`'s first record works
out to `type` at `0x1A`, high byte at `0x1B`, low at `0x1C` — exactly the
`FAN_SPEED_HIGH_ADDR` / `FAN_SPEED_LOW_ADDR` constants sitting unused in
Intel's assembly, which is a satisfying way to find out the transcription is
right.

Two service builds were disassembled, the general `NSSServiceInstaller
1.17.38.0` and a model-specific `PAH-` build, and they agree exactly: same
section offsets in the kernel driver, same `[type, hi, lo]` parsing, same
`Skip(1)`-and-pad trick for `BC`. The older PAH build simply defines less — no
`ECNUC23` (which shares layout `22` anyway) and no `CPUFan2` type — so there is
no fourth layout hiding in it.

Layout selection follows Intel too, in four steps: `layout=21`, `22` or `bc` on
the command line if given; then the driver's DMI table; then Intel's
BIOS-string rules (`EBTGL` → 21, `BCTGL` → BC, `HBADL` → 21); then Intel's
last-resort WMI `PlatformCapabilityReport` call, **only** with `wmi_probe=1`.
Unknown boards get a clear refusal naming the override rather than a guess.

That last step is implemented but off by default, which is a deliberate split
between "transcribed" and "trusted". The WMI details came out of the firmware's
binary MOF —

```
[WMI, guid("{8C5DA44C-CDC3-46b3-8619-4E26D34390B7}")] class CISD_WMI {
  [WmiMethodId(1537), Description("PlatformCapabilityReport")]
      void PlatformCapabilityReport([in] uint32 Cmd, [out] CISD_Package Data); }
```

— so the GUID, the method ID and the reply format are certain, and the bit
tests are transcribed branch-for-branch from Intel's `GetCurrentECVersion()`,
including the quirk that bit 1 set without bit 0 reaches the `ECElement` test
rather than `ECNUC21`. What is *not* certain is the result on any machine other
than this one, and here steps 2 and 3 answer first, so the path never runs
unless asked for. Turning it on where it is wrong means silently publishing the
wrong bytes as fan speeds, which is worse than not binding at all — so it waits
for a board to check it against.

The driver reaches WMI the modern way: the GUID-string `wmi_evaluate_method()`
is gone from current kernels, so it registers a small `wmi_driver` for that
GUID and calls `wmidev_evaluate_method()` on the bound device. That happens
before the platform driver is registered, and driver registration binds
already-present devices synchronously, so the WMI device is settled before any
platform probe runs and no `-EPROBE_DEFER` dance is needed. One side effect
worth knowing: the module now has a `depends: wmi` whether or not the flag is
set.

Two things are checked regardless of how the layout was chosen. The `NUC_EC`
signature must be at EC address 0, so an EC that repurposes that space makes
the sensors *disappear* rather than report whatever now lives there; and values
are bounds-checked for absurdity (RPM ≤ 15000, temperature ≤ 127 °C). Neither
is a proof: the signature says the region is still the EC, not that the fields
inside it are still where they were.

The temperature channels are the weaker half of this and are treated as such.
They come only from local measurement, with no vendor code to corroborate them,
so the driver publishes them **only** on a board in its DMI table — a board
matched by BIOS string gets fans and no temperatures.

Installing it needs root — see [`host/README.md`](host/README.md).

```bash
kubectl -n vm port-forward ds/node-exporter 9100:9100 &
curl -s localhost:9100/metrics | grep '^node_hwmon_fan'
```

