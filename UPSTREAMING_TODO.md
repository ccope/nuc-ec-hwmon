# Upstreaming TODO

The driver works on this machine and is versioned 1.0 on that basis. It is
**not** in a state the kernel would take, and the gap is mostly scope rather
than quality. This is the list, and the reason each item is still open.

Two things were already done because they were worth doing regardless of
upstreaming: the one-second sample cache (a `sensors` run cost 23 EC
transactions before it), and removing a dead macro.

## Blockers

### 1. Untested code paths

Layouts `21` and `bc`, and the whole `wmi_probe` path, have never executed on
real hardware and cannot be here — this board is layout `22` and resolves via
DMI before either is consulted. "We don't merge code nobody has tested" is a
standard and correct response, and a module parameter defaulting to off is not
a mitigation upstream; it is an admission.

**To resolve:** find someone with an ECNUC21 or ECBC board (a NUC whose BIOS
string contains `EBTGL`, `BCTGL` or `HBADL`) willing to confirm a reading, or
submit with only layout `22` and let the others land when hardware appears.

### 2. `layout=` as a module parameter

Selecting a *hardware* layout by module parameter is what DMI quirk tables
exist to avoid. Expect "if you can't detect it, don't claim to support it."

**To resolve:** drop the parameter for a submission. It stays here because it
is the only way to try an unlisted board without recompiling.

### 3. Speculative temperature channels

`temp2`–`temp5` are labelled `EC 0x0F`, `EC 0x11`, `EC 0x13`, `EC 0x15`. They
track temperature but nothing identifies them, and hwmon channels are expected
to mean something. Only `temp1` has a real claim: it follows the coretemp
package reading closely.

**To resolve:** submit `temp1` only, or identify the rest. Note they are
already gated to DMI-matched boards, so the exposure is contained.

## Required work, not yet done

### 4. Documentation

A new hwmon driver needs `Documentation/hwmon/nuc-ec-hwmon.rst` and a
MAINTAINERS entry. The material exists in `../README.md` and the source header
comment; it needs reformatting, not researching.

### 5. Decide the subsystem

`drivers/hwmon` or `drivers/platform/x86`. The sensors argue for hwmon; the
ACPI binding, the DMI quirking and any future `platform_profile` support argue
for platform-x86. Worth asking on the list before writing a patch.

## Open questions a reviewer will raise

### 6. `ec_read()`

It targets the boot EC global and does not honour any ASL mutex the firmware
may expect around EC access. New callers attract scrutiny. There is no better
option — the alternatives (an operation region, a vendor WMI method for fan
speed) do not exist on this platform — but the answer needs to be ready.

### 7. Claiming ACPI HID `INTC1036`

Taking an ACPI ID upstream means owning it, and this one is generic (`_CID
PNP0C02`) and is what Intel's own Windows driver binds to for RAPL. This was a
real collision risk; it is much weaker now that the NUC line is discontinued
and no further Intel driver is expected. Worth stating plainly in the
submission rather than leaving a reviewer to wonder.

## Legal, before any submission

The constants — offsets, the `NUC_EC` signature, the WMI GUID and method 1537,
the fan-type enumeration — are interface facts, which is the well-trodden
ground of interoperability reverse engineering. The exposure is narrower:

- **Paraphrase, don't reproduce.** The source currently says the WMI detection
  is "transcribed branch-for-branch from Intel's `GetCurrentECVersion()`" and
  quotes their parsing. Honest, and right for review, but describe the
  *interface* rather than reproduce decompiled expression.
- **Lead with the measurement.** Layout `22` was derived by sampling this
  machine across an idle → load → cooldown cycle *before* any vendor code was
  opened; the vendor code only corroborated it. That is close to clean-room and
  should be said. Layouts `21`/`bc`, the GUID and the method ID have no such
  independent derivation — which is the same set of things item 1 says are
  untested. The legally weakest parts and the technically weakest parts are the
  same parts, and dropping them fixes both.
- **The DCO sign-off** certifies the right to submit under GPL. Comfortable for
  measured facts; less so for copied structure.

## The shape of an actual submission

Layout `22` only, one DMI entry, fans plus `temp1`, no module parameters, no
WMI, with caching and documentation. Everything stripped is what makes this
copy useful *here*, where risk that upstream cannot accept is fine to carry.
