# Fan profiles

How fan *mode* selection would be done on this machine, and what it would take.

This documents the `platform_profile` route only. Per-fan manual control —
hwmon's `pwmN` / `pwmN_enable`, driven by lm-sensors' `fancontrol` — is
deliberately out of scope: it moves the thermal control loop into userspace, so
a daemon that dies, hangs, or gets OOM-killed leaves the fan wherever it was
last told. On a headless box with one fan and a sustained 40 W package limit,
handing the curve back to the EC and only *choosing between its curves* is the
right shape. Nothing here is implemented.

## What `platform_profile` is

A small fixed-vocabulary switch for "how should this machine behave
thermally", exposed by the kernel at

```
/sys/firmware/acpi/platform_profile           # current
/sys/firmware/acpi/platform_profile_choices   # what this machine supports
```

and, on kernels with the class-based implementation, per-device under
`/sys/class/platform-profile/platform-profile-N/`.

The vocabulary is closed — a driver picks from `low-power`, `cool`, `quiet`,
`balanced`, `balanced-performance`, `performance`, `max-power` and `custom`,
and advertises only the subset the hardware really has. `thinkpad_acpi`,
`asus-wmi`, `hp-wmi`, `dell-pc` and `amd-pmf` all register one. Desktop
environments drive it directly (GNOME's power panel, `powerprofilesctl`), and
`platform_profile_cycle()` exists for a keyboard hotkey.

**This machine exposes none today** — there is no `/sys/firmware/acpi/platform_profile`,
because no loaded driver registers a handler. So there is nothing to conflict
with, and equally nothing to build on.

## The kernel interface

As of the 7.0 headers on this host:

```c
struct platform_profile_ops {
	int (*probe)(void *drvdata, unsigned long *choices);
	int (*hidden_choices)(void *drvdata, unsigned long *choices);
	int (*profile_get)(struct device *dev, enum platform_profile_option *profile);
	int (*profile_set)(struct device *dev, enum platform_profile_option profile);
};

struct device *devm_platform_profile_register(struct device *dev, const char *name,
					      void *drvdata,
					      const struct platform_profile_ops *ops);
```

`probe()` fills a bitmap of the options this machine actually offers; `get` and
`set` do the obvious. Note this is the newer device-scoped API — older kernels
used a single global handler, so anything written against it needs a version
check if it is ever to build on both.

## How the NUC's modes would map

Intel's `NucFanMode`, recovered from the NUC Software Studio Service:

| `NucFanMode` | value | maps to |
|---|---|---|
| Quiet | 0 | `PLATFORM_PROFILE_QUIET` |
| Balanced | 1 | `PLATFORM_PROFILE_BALANCED` |
| Cool | 2 | `PLATFORM_PROFILE_COOL` |
| Custom | 3 | — not a curve |
| Fixed | 4 | — not a curve |
| Fanless | 5 | — deliberately not offered |

Only the first three are profiles in the `platform_profile` sense. `Fixed` and
`Custom` set a duty cycle rather than selecting a curve, which is `pwmN`
territory; advertising `custom` without any way to set that duty would be a
dead end in sysfs. `Fanless` should never be advertised on a machine whose only
fan cools a part that will happily draw 67 W in a burst.

So a first implementation offers exactly `quiet`, `balanced`, `cool`.

## What would have to be reverse-engineered first

The fan *speed* block is fully decoded. The fan *profile* block is not:

- **Reading the current mode** — Intel goes through IOCTL `0x9C406020`
  (`ReadFanProfileCode`) against `FAN_PROFILE_EC` at EC address `0x5E`, with
  `FAN_PROFILE_WMI_LENGTH` 90 bytes (21 for the `_23` variant). The structure
  inside is unknown.
- **Writing it** — `WriteFanProfileNuc21/22/23` (IOCTLs `0x9C40A018`,
  `0x9C40A01C`, `0x9C40A04C`), one structure layout per EC generation, same as
  the fan block's per-section split.

Both are a decode job on top of what is already done, not a guess away.

## Where it would live

The same module. A single driver may register a hwmon device *and* a platform
profile — `asus-wmi` and `thinkpad_acpi` both do — and
`devm_platform_profile_register()` would attach to the `INTC1036` platform
device the driver already binds. No second module is needed.

What it *does* change is the driver's character: it stops being read-only.
Every caveat about `ec_write()` then applies for real — a torn read is one bad
sample, a torn write leaves the EC in a state the firmware did not put it in,
and there is no undo on a machine with one fan.

## Why this is not implemented

1. The profile structures are not decoded yet.
2. Verifying a write means rebooting into the BIOS fan menu to see whether the
   mode actually took, which is a slow and unpleasant test loop.
3. The payoff is small. The EC's own curve already works, and the effective
   knob for a quieter machine is **lowering PL1** via powercap — 40 W by
   default here — which reduces the heat the fan has to remove instead of
   overriding the fan that removes it. That path is already supported, already
   safe, and needs no new code:

   ```bash
   echo 25000000 | sudo tee /sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw
   ```

## If it is implemented later

- Decode read-only first. Log the profile block and compare against the BIOS
  menu across every mode before a single byte is written.
- Restore firmware control in `remove()`. Leaving a fan under a mode the driver
  set and then unloading is how hardware dies.
- Never advertise a choice that can stop the fan, whatever the EC allows.
- Treat `profile_set` failures as real errors rather than best-effort; a mode
  that silently did not apply is worse than one that refused.
