# 26 — DDJ-FLX6 MIDI Surface & 4-Channel Mapping

Technical specification and mapping reference for the **Pioneer DJ DDJ-FLX6 / DDJ-FLX6-GT** control surface in `knobshim.so` (`map_flx6.c`).

---

## 1. Overview of the DDJ-FLX6

The Pioneer DDJ-FLX6 is a 4-channel DJ controller featuring full-sized jog wheels with track position display, Merge FX rotaries, dedicated Beat FX slots, and performance pads.

In this port, `map_flx6.c` translates the DDJ-FLX6 hardware inputs (MIDI Notes and Control Changes) into native `rbp` (XDJ-RX3) internal keycodes via `KeyManager::asEventCode` and `MixerEngine` calls.

### Hardware Comparison: FLX6 vs FLX4 vs RX3

| Feature | DDJ-FLX4 | DDJ-FLX6 | XDJ-RX3 Hardware |
|---|---|---|---|
| **Channels** | 2 Decks (CH1, CH2) | 4 Channels (CH1, CH2, CH3, CH4) | 2 Decks (Deck 1, Deck 2) + Master |
| **Jog Wheels** | Compact (approx. 128 PPR) | Full Size (720 PPR, Vinyl Touch) | Full Size with CDJ LCD Display |
| **Jog Mode** | Relative CC 0x40 | Relative CC 0x40 + Touch Note | Optical encoder + Platter switch |
| **FX Architecture** | Single FX selector knob | 6 Beat FX Slot Racks + Merge FX | 14-position rotary selector |
| **Sound Color FX** | Knobs only (fixed Filter) | 4 CFX Quick Select Slots + Knobs | 6 Dedicated CFX Hardware Buttons |
| **Faders** | 2 Upfaders + Crossfader | 4 Upfaders + Crossfader | 2 Channel Faders + Magvel Crossfader |
| **Tempo Range** | 14-bit CC MSB/LSB | 14-bit CC MSB/LSB | 14-bit AD Converter |

---

## 2. Channel Assignment & Deck Mirroring

`rbp` is natively a 2-player engine (Deck 1 and Deck 2). On the 4-channel DDJ-FLX6:
- **Physical Channel 1**: Assigned to **Deck 1** (Player 0).
- **Physical Channel 2**: Assigned to **Deck 2** (Player 1).
- **Physical Channel 3**: Mirrored to **Deck 1** controls (allows multi-layer mixing).
- **Physical Channel 4**: Mirrored to **Deck 2** controls.
- **Mixer Strips (Trim, EQ High, Mid, Low, Color FX, Fader)**:
  - Channels 1 & 3 feed Deck 1 DSP / EQ path.
  - Channels 2 & 4 feed Deck 2 DSP / EQ path.

---

## 3. Beat FX Subsystem (6 Virtual Slots)

The DDJ-FLX6 features two FX banks (FX Rack 1 on Left, FX Rack 2 on Right), each with 3 slot buttons and 3 parameter knobs:

```
FX Rack 1 (Left Deck)           FX Rack 2 (Right Deck)
┌───────┬───────┬───────┐       ┌───────┬───────┬───────┐
│ Slot 1│ Slot 2│ Slot 3│       │ Slot 4│ Slot 5│ Slot 6│
│ Delay │ Echo  │Spiral │       │Reverb │Flanger│ Roll  │
└───────┴───────┴───────┘       └───────┴───────┴───────┘
```

### Slot Configurations
```c
/* 6 slots on DDJ-FLX6:
 * Slots 0..2: FX Rack 1 (ch 4, 0x94), Notes 71..73, CCs 2, 4, 6
 * Slots 3..5: FX Rack 2 (ch 5, 0x95), Notes 71..73, CCs 2, 4, 6 */
static struct flx6_bfx_slot flx6_slots[6] = {
     { 0, 0, 64, 0 },  /* FX1-1: Delay,   Deck 1 (Note 71, CC 2) */
     { 1, 0, 64, 0 },  /* FX1-2: Echo,    Deck 1 (Note 71, CC 4) */
     { 3, 0, 64, 0 },  /* FX1-3: Spiral,  Deck 1 (Note 71, CC 6) */
     { 4, 1, 64, 0 },  /* FX2-1: Reverb,  Deck 2 (Note 71, CC 2) */
     { 6, 1, 64, 0 },  /* FX2-2: Flanger, Deck 2 (Note 71, CC 4) */
     { 9, 1, 64, 0 },  /* FX2-3: Roll,    Deck 2 (Note 71, CC 6) */
};
```

### Auto-Disable on Channel Switch (Anti-Glitch / Anti-Pop)
When switching Beat FX channel routing (`K_BFXCH`):
- If Beat FX is currently active (`rbp_beatfx_on() > 0`), the shim automatically toggles Beat FX **OFF** first (`K_BFX PRESS + RELEASE`).
- This resets the DSP delay/reverb buffer and prevents audio pops and artifacts before the new channel assignment takes effect.

```c
/* Beat FX CH SELECT buttons */
if (changing && rbp_beatfx_on() > 0) {
     send_rx_key(K_BFX, OP_PRESS, CH_GLOBAL, 0);
     send_rx_key(K_BFX, OP_RELEASE, CH_GLOBAL, 0);
     for (int s = 0; s < 6; s++) flx6_slots[s].on = 0;
}
flx6_slots[flx6_active_slot].channel = target_ch;
send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, target_ch);
```

### Merge FX Integration
- **Merge FX Knob (CC 8)**: Mapped to Beat FX Level/Depth (`K_DEPTH`).
- **Merge FX Push (Note 46)**: Toggles Beat FX ON/OFF (`K_BFX`).

---

## 4. Sound Color FX Slots

The FLX6 does not have hardware buttons for selecting Sound Color FX type. To give the user full access to all RX3 Color FX, `map_flx6.c` binds **SHIFT + HEADPHONE CUE 1–4**:

| Combination | Sound Color FX Type | Keycode |
|---|---|---|
| `SHIFT` + `CUE 1` | Filter | `K_FILTER` (0x0211) |
| `SHIFT` + `CUE 2` | Dub Echo | `K_DUBECHO` (0x0213) |
| `SHIFT` + `CUE 3` | Noise | `K_NOISE` (0x0212) |
| `SHIFT` + `CUE 4` | Space | `K_SPACE` (0x0210) |

The dedicated CFX knobs on each channel control parameter depth (`CC 23` for Deck 1, `CC 24` for Deck 2).

---

## 5. Jog Wheel Mechanics (720 PPR)

The DDJ-FLX6 jog wheels have 720 pulses per revolution.
- **Top Surface Platter Touch (Vinyl Mode)**: Note 54 (Deck 1) / Note 54 (Deck 2).
  - Press: `K_JOG_TOUCH` (0x0208) OP_PRESS.
  - Release: `K_JOG_TOUCH` OP_RELEASE.
- **Outer Ring / Pitch Bend**: Relative CC 33 (`0x40` center).
- **Pitch Bend Formula**:
  The rotation speed is converted into revolutions-per-second float and sent via `K_JOG_ROTATE` (`0x4305`) with `OP_ROTATE`.
  Smooth vinyl scratch response is achieved with an idle decay timeout of 120 ms.

---

## 6. Pad Modes & Shift Controls

Each deck features 8 velocity-sensitive pads:

| Pad Button | Normal Mode | With `SHIFT` Pressed |
|---|---|---|
| **Pad 1–8** | Trigger Hot Cue 1–8 (`K_HOTCUE1`..`K_HOTCUE8`) | Delete Hot Cue (`hotcue_delete()`) |
| **Beat Jump (< / >)** | Jump 1 Beat backward / forward | Jump 4 / 8 Beats |
| **Loop In / Out** | Manual 4-Beat Loop / Reloop | Exit Loop / Slot Save |
| **Deck 3/4 Toggle** | Dual Deck Control | Slip Mode Toggle (`K_SLIP`) |
| **Browse Push** | Load selected track | Open SOURCE screen (`K_SOURCE`) |
| **SHIFT + LOAD 1** | Open Utility / Settings (`K_MENU` held 500ms) | — |
| **SHIFT + LOAD 2** | Toggle Shortcut popup | — |
| **SHIFT + LOAD 3** | Toggle MIDI / Device View | — |

---

## 7. Configuration in `rb.conf`

To activate the DDJ-FLX6 mapping on either S905X or Raspberry Pi 4B:
```ini
# ALSA Audio Device for DDJ-FLX6
AUDIO_DEV=hw:CARD=DDJFLX6,DEV=0

# MIDI Surface Mapping for DDJ-FLX6
MIDI_MAP=flx6

# Touchscreen Pointing Mode
POINT_KIND=auto
```
