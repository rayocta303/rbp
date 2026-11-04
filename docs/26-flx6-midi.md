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

## 2. Dynamic 4-Deck Switching & Exclusive Mixer Routing

`rbp` is natively a 2-player engine (`PLAYER_0` / Deck 1 / Left and `PLAYER_1` / Deck 2 / Right). On the 4-channel DDJ-FLX6, hardware deck buttons toggle active control layers:
- **Left Side Layer (Deck 1 vs Deck 3)**:
  - Default: **Deck 1** active -> controls Player 1 (`sch = 1`).
  - Pressing `DECK 3` (`N_DECK_TOGGLE` Note 114): switches control to **Deck 3** -> controls Player 1 (`sch = 1`).
  - **Exclusive Mixer Strips**:
    - When Deck 1 is active: Channel 1 mixer strip (Trim, EQ High/Mid/Low, Fader, Cue, Color FX 1) and Deck 1 jog/pads/pitch control Player 1. Deck 3 strip and controls are **parked / disabled** (clean standalone 1-to-1 control).
    - When Deck 3 is active: Channel 3 mixer strip (Trim, EQ High/Mid/Low, Fader, Cue, Color FX 3) and Deck 3 jog/pads/pitch control Player 1. Deck 1 strip and controls are **parked / disabled**.
- **Right Side Layer (Deck 2 vs Deck 4)**:
  - Default: **Deck 2** active -> controls Player 2 (`sch = 2`).
  - Pressing `DECK 4` (`N_DECK_TOGGLE` Note 114): switches control to **Deck 4** -> controls Player 2 (`sch = 2`).
  - **Exclusive Mixer Strips**:
    - When Deck 2 is active: Channel 2 mixer strip (Trim, EQ High/Mid/Low, Fader, Cue, Color FX 2) and Deck 2 jog/pads/pitch control Player 2. Deck 4 strip and controls are **parked / disabled**.
    - When Deck 4 is active: Channel 4 mixer strip (Trim, EQ High/Mid/Low, Fader, Cue, Color FX 4) and Deck 4 jog/pads/pitch control Player 2. Deck 2 strip and controls are **parked / disabled**.
- **Jog Ring Illumination (Ch 11 / 0xBB, CC 0..3)**:
  - DDJ-FLX6 controls 72 jog ring LEDs per deck via MIDI channel 11 (`0xBB`):
    - Deck 1: `CC 0`
    - Deck 2: `CC 1`
    - Deck 3: `CC 2`
    - Deck 4: `CC 3`
  - When switching layers (e.g. Deck 1 -> Deck 3), the old deck's jog ring is zeroed (`midi_cc(11, old_cc, 0)`), and the active rotation signal is routed to the new deck's CC address.
- **DeckState Feedback LED (Note 60)**:
  - The hardware `DECK` button illuminates with Note 60 on the currently active layer and turns off on the parked layer.
- **Master Deck LED State (Note 92)**:
  - Synchronized via `rbp_led.c` (`n_master = 92`) and routed through `deck_ch_fn` to the active channel on both sides.
- **Channel Level Meters (CC 2)**:
  - Dynamically routed to the currently active mixer channel strip via `meter_ch_fn`, keeping inactive strip meters dark.
- **LED & VU Cache Invalidation on Switch**:
  - `rbp_led_invalidate_deck(side)` and `rbp_vu_invalidate_channel(side + 1)` force an instant re-transmission of all transport LEDs (Play, Cue, Sync, Keylock, Pad modes, CUE) and VU meters to the newly active deck.
- **Instant Strip Knob Transfer & Boot Seeding**:
  - When switching deck layers, the physical positions of Trim, High/Mid/Low EQ, Fader, and Color FX on the destination strip are immediately transmitted to the RBP Player (`sch = side + 1`).
  - At system boot (`flx6_startup`), all channel strips, master level, crossfader, and headphone controls are pre-seeded into RBP memory and restored from `/run/rblive4/flx6_knobs.state`, preventing silence or uninitialized states without requiring manual knob wiggling.

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

### Realtime Beat FX Panel Sync from GUI `rbp`
The Beat FX panel on the DDJ-FLX6 receives two-way realtime synchronization from `rbp`:
1. **Channel Selection LEDs**:
   - `rbp_beatfx_channel()` queries the active channel in `MixerEngine`.
   - Notes 28 (CH1), 29 (CH2), 30 (CH3), 31 (CH4), and 20 (Master) on `CH_FX1` and `CH_FX2` are illuminated dynamically according to the active channel and current deck layer.
2. **Beat Fraction Parameter (`CC 100`)**:
   - `rbp_beatfx_numer()` and `rbp_beatfx_denom()` read fraction parameters directly from `rbp`.
   - `flx6_beat_to_cc100()` maps fractions (1/4 to 32) to `CC 100` on both FX banks whenever changed on the touchscreen.
3. **Beat FX ON/OFF Status**:
   - `rbp_beatfx_on()` monitors the DSP toggle state, blinking the active slot LED at 250 ms intervals when ON, and turning it OFF when disabled.
4. **Beat FX Type Sync**:
   - `rbp_beatfx_type()` keeps the active slot type synchronized if changed via touchscreen rotary.

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
| **Browse Push** | Load selected track (di Browse); Buka Browser (`K_BACK`, di Waveform) | Open SOURCE screen (`K_SOURCE`) |
| **SHIFT + LOAD 1** | Toggle Track Filter side drawer (`K_TRACKFILTER`) | — |
| **SHIFT + LOAD 2** | Toggle Shortcut popup | — |
| **SHIFT + LOAD 3** | Toggle Menu Utility / Settings (`K_MENU` held 500ms / `K_BACK`) | — |

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
