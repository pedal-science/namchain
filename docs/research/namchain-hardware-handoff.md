# Pedal Build - hardware design discussion notes and handoff

Context brief for picking up the idea of making a real pedal build. Everything here was established by reading the SparkFun schematic directly or decided deliberately; where
something is unverified it says so.

---

## Hardware

| Part | Notes |
|---|---|
| `tib` | ASUS Tinker Board, RK3288 Cortex-A17, **armhf / 32-bit** (no aarch64). This is no longer in production but a similarly powerful Raspberry Pi or workalike would likely be a suitable drop-in, especially as namchain and nampi have already proved themselves on a Pi 500. |
| Codec | SparkFun WM8960 Audio Codec Breakout, schematic v1.0 (Eagle 9.4.2, Jan 2023). Purchased but not yet arrived. Note: This product has been retired so I may need to find a suitable replacement if I want to reproduce this design. |
| Control MCU | Raspberry Pi Pico 2 or similar (purchased but not yet arrived: RP2350-USB-A Mini Development Board RP2350A Dual Cortex-M33 RISC-V 150MHz 520KB SRAM 2MB Flash) |
| Front end | Hi-Z buffer, to be built |
| Power | Cioks DC7: 7 isolated outlets at 6 W each (660 mA @ 9 V); USB-A 1 A (may be 5 W, according to cioks website) on v1, USB-C 15 W on v2; AUX 24 V @ 2 A |

**Topology: mono in, stereo out.** NAM input is mono; the chain may split into two
channels running different kernels, and those are sometimes kept as stereo rather
than recombined.

---

## Established from the SparkFun schematic

These were read off the drawing, not assumed.

### Clocking

- **Y1 is a 24 MHz oscillator**, VCC *and* EN both hard-tied to 3.3 V, so it runs
  permanently. Output goes through R10 (10 Ω) into MCLK, pin 11.
- **MCLK is not brought out to the header.** It cannot be supplied externally.
- Consequence: the codec's converters are clocked from this local oscillator.
  If `tib` were I2S master, its LRCLK and the codec's sample rate would come from
  independent references and drift. **The codec must be I2S master.**

### Inputs

Signal path from the header pad inward:

```
pad → 100 Ω series → [47 nF to GND] → 1 µF series → codec pin
```

- Coupling caps are **already fitted** (1 µF). Do not add more.
- The 47 nF shunt sits on the *source* side of the coupling cap, so the filter
  corner is `1 / (2π (Z_source + 100) × 47 nF)`:

| Source impedance | −3 dB |
|---|---|
| ~0 Ω | 34 kHz |
| 100 Ω | 17 kHz |
| 470 Ω | 5.9 kHz |
| 1 kΩ | 3.1 kHz |

- Component mapping (**verify against silkscreen before desoldering**):

| Input | Series R | Shunt C | Coupling C |
|---|---|---|---|
| LINPUT1 | R4 | C22 | C19 |
| LINPUT2 | R6 | C21 | C20 |
| LINPUT3 | R1 | C27 | C23 |
| RINPUT1 | R7 | C28 | C24 |
| RINPUT2 | R8 | C29 | C25 |
| RINPUT3 | R9 | C30 | C26 |

- If the buffer's output impedance pushes the corner too low, swap the relevant
  47 nF for **4.7 nF** (≈31 kHz with a 1 kΩ source). Don't remove them entirely.

### Outputs

- HP_L (pin 31), HP_R (pin 29) and OUT3 (pin 30) go **straight to the header with
  no coupling capacitors**. This is the capless configuration: OUT3 is a driven
  pseudo-ground, not ground.
- Tying an output sleeve to system ground without a series capacitor **shorts the
  OUT3 buffer**.
- There are no true line outputs on this chip. Speaker outputs (SPEAKER_LP/LN/RP/RN
  via 220 Ω ferrites L1–L4 and 1 nF C15–C18) are class-D and unused.

### Power

- `3.3V` header pin feeds **DCVDD, DBVDD and Y1**. The oscillator's jitter therefore
  tracks whatever supplies this rail.
- `VIN` (3.7–5.5 V) → C1/C31 → **U3, XC6222B331MR-G** (3.3 V, 700 mA out, **6 V max in**)
  → AVDD via the **AVDD-ISO** jumper. R18 is a 100 k pull-up on U3's EN, and EN is
  broken out on J4 pin 3.
- **SPKVDD jumper** selects 3.3 V or VIN; leaving both open means the class-D amp is
  unpowered and SPKVDD can be supplied at the header.

### Other

- I2C address **0x1A**, with 2.2 kΩ pull-ups already on the board.
- All six digital lines have **10 Ω series damping** (R10–R17).
- VMID (27) and MICBIAS (1) each have 4.7 µF to ground.
- **J4**: BCLK, DACLRC, DACDAT, ADCLRC, ADCDAT, SCL, SDA, 3.3V, SPKVDD, VIN, EN, GND
- **J5**: RINPUT3/2/1, LINPUT1/2/3, MICBIAS, HP_L, OUT3, HP_R, AVDD, SPK_LP/LN/RP/RN, GND

---

## Design decisions

### Clocking and buses

- **Codec is I2S master; `tib` is I2S slave.** Non-negotiable, see above.
- **`tib` owns the I2C bus.** The ASoC driver caches WM8960 register state. The Pico
  must never write codec registers — it talks to `tib` over UART and `tib` applies
  changes through ALSA mixer controls.

### Jumpers

- **AVDD-ISO: cut.** AVDD fed externally from a dedicated LDO.
- **SPKVDD: both open.** Class-D amplifier unpowered — it's a switching noise source
  next to the ADC inputs and serves no purpose here.
- **VIN: unconnected.** With AVDD-ISO cut and SPKVDD open it feeds nothing.

### Input stage

One jack, **permanently 1 MΩ**. A pedal output is low-impedance and is perfectly
happy driving 1 MΩ, so impedance never needs switching — only level does.

- The front-panel switch is a **gain** switch, labelled **Inst / Line**, not a
  "Hi-Z" switch. Putting a mechanical contact at the highest-impedance node would
  invite hum and contact noise.
- OPA1642 (JFET input), 9 V single supply, 4.5 V bias from a 1 M/1 M divider
  decoupled with 10 µF, 1 M input resistor to that bias.
- Non-inverting: ×10 for guitar, ×1 for pedal output. This lands both sources in
  roughly the same window so the codec PGA sits near 0 dB rather than supplying
  26 dB of gain and its own noise.
- ~100 Ω series on the way out for stability into the board's 47 nF.

### Output stage

Each of HP_L and HP_R: **10 µF series + 100 Ω + 1 MΩ bleed to ground** → TS jack.
OUT3 unused and **disabled in the register configuration** — don't leave the driver
enabled as a return path with nothing connected.

### Power

Two inputs, deliberately:

- **5 V / 3 A** for `tib` — own USB-C brick, or the DC7 v2's USB-C. The v1's USB-A
  at 1 A is insufficient.
- **9 V centre-negative** from one isolated Cioks outlet → the buffer directly, and
  → a low-noise LDO → 3.3 V → AVDD.

The point is that the analogue supply never carries `tib`'s current. Hum comes from
current in shared ground paths; this arrangement means the two meet only at the
**star ground point**, where PSU 0 V, codec GND, buffer GND and jack sleeves are
joined at one place.

A single-cable alternative exists — 24 V AUX at 2 A into an onboard buck for 5 V
plus a linear regulator for 9 V — but it puts a 48 W-capable switcher inside the
enclosure and gives up the isolation. Rejected for the first build.

---

## Software work outstanding

1. **Device tree.** Write a `simple-audio-card` node: WM8960 codec at 0x1A, with
   `bitclock-master` and `frame-master` on the codec DAI. The mainline `wm8960`
   ASoC driver is available, but there is **no ready-made overlay** — the Pi's
   `wm8960-soundcard` overlay does not apply here. On Armbian use
   `armbian-add-overlay`.
2. **Verify the I2S wiring** on this Tinker Board revision: which signals the
   40-pin header actually exposes, and whether the RK3288 side provides separate
   TX and RX frame clocks (the board breaks out ADCLRC and DACLRC separately, and
   the WM8960 can alternatively slave the ADC to DACLRC).
3. **ALSA mixer configuration.** ALC **off** — it pumps audibly on guitar dynamics.
   3D enhancement off. Simplest possible analogue routing. PGA near 0 dB, boost
   mixer at 0 dB (the boost stages are noisier than the PGA).
4. **Audio host.** JACK or PipeWire plus the NAM LV2 plugin. PiPedal and similar
   turnkey stacks assume a Raspberry Pi.
5. **Pico firmware.** Footswitches, rotary encoder, OLED, status LEDs, expression
   pedal ADC, MIDI. Define the UART protocol to `tib`.
6. **Boot-time bypass.** ~30 s of boot before audio passes. A relay held in hard
   bypass until userspace signals ready, with the Pico holding the relay. This is
   the difference between a gig-usable pedal and a desk toy.

---

## Open questions

- **Pico ↔ `tib` link**: UART chosen to avoid a USB gadget stack and an enumeration
  dependency at boot. Revisit if the Pico should also present as a USB MIDI device
  to a host computer.
- **Codec 3V3 source**: currently `tib`'s header. A second LDO off the 9 V would take
  the whole codec board — including the 24 MHz oscillator — off `tib`'s rails.
  Defensible addition; left out of the first pass.
- **Pico power**: shown from `tib`, which means the control surface is dead during
  boot. Its own 5 V would let it show a splash and hold the bypass relay.
- **Enclosure, panel layout, bypass scheme** — not yet designed.

---

## What not to assume

- This is **not** a Raspberry Pi. Pi-specific overlays, `config.txt` entries and
  HAT conventions do not apply.
- This is **not** aarch64. Anything assuming 64-bit ARM needs checking.
- The codec board is **not** stock — AVDD-ISO is cut, and the input shunt caps may
  have been changed.

---

## References

- Block diagram: `docs/diagrams/` (draw.io source, two-layer: blocks and connections,
  with non-component junction squares on the connection layer)
- SparkFun WM8960 Audio Codec Breakout schematic v1.0
