# SDTracker: A Modern Evolution of the Classic Handheld Tracker


SDTracker bridges the gap between the legendary Little Piggy Tracker (LGPT) workflow and the capabilities of modern portable computing. Specifically optimized for the Steam Deck, it retains the intuitive, button-mapped sequencing of its predecessor while removing the hardware limitations of the past.

Key Enhancements:

    Expanded Sequencing: 16-track polyphony (doubling the original 8-track limit).

    Broad Format Support: Integrated FFmpeg support allows for nearly universal sample compatibility.

    Professional Audio Routing: Dedicated Mixer page for real-time level management and spatial positioning.

    Expanded Phrase Editor: 4 command columns per phrase step (up from 2), allowing complex per-step automation without needing tables.

    Plugin Integration: Support for LV2 plugins and Windows VST3 plugins (via bundled yabridge + Proton), allowing for professional-grade synthesis and effects within the tracker environment.

    LV2 Effects: A dedicated Effects page (to the right of the Instrument page, accessed via the 'E' legend shortcut) provides 16 effect slots for loading LV2 audio effect plugins. Effects are applied to channels using the FXSN command on the Phrase or Table pages.

        FXSN aabb — aa = effect slot (0-F), bb = wet/dry mix (00-FF)

    Effects support full parameter editing with real value display and scale point labels. Effect configurations are saved and restored with the project.

    Automated Workspace: On first launch, the software generates a streamlined directory structure in the user's Documents/SDTracker folder for easy management. After that use the options in the Projects page to install the factory samples as well as my Mutated Instruments plugin.


Building (local) ⚙️

To build the project on Linux (x86_64) run this from the `projects` directory:

    make PLATFORM=X64 -j1

Or from the repository root you can run:

    make -C projects PLATFORM=X64 -j1

This is the recommended invocation for local builds.


In Steam search for SDTracker control layout in community to correctly map your controls.




<img width="550" height="272" alt="image" src="https://github.com/user-attachments/assets/0b00a1d6-16e3-4cda-b206-c3c75236b8a5" />

<img width="550" height="272" alt="image" src="https://github.com/user-attachments/assets/9cc5d3ff-fce2-4935-92a8-356b7d3509f9" />

<img width="550" height="272" alt="image" src="https://github.com/user-attachments/assets/8f0bb008-fc26-4c2a-be48-0aec7d03e610" />


---

## Navigation

SDTracker uses a page-based navigation system. A small guide map in the bottom-right corner shows the current page highlighted. Pages are arranged in a grid and you move between them using button combos:

```
 P G          P = Project   G = Groove
 S C P I E    S = Song      C = Chain     P = Phrase   I = Instrument   E = Effect
 M   T T      M = Mixer     T = Table (under Phrase or Instrument)
```

### Moving Between Pages

| From | Action | Destination |
|------|--------|-------------|
| Any  | **R + Up** | Project |
| Song | **R + Down** | Mixer |
| Mixer | **R + Up** | Song |
| Song | **R + Right** (on a chain) | Chain |
| Chain | **R + Right** (on a phrase) | Phrase |
| Chain | **R + Left** | Song |
| Phrase | **R + Right** (on a note/instr) | Instrument |
| Phrase | **R + Left** | Chain |
| Instrument | **R + Left** | Phrase |
| Instrument | **R + Right** | Effect |
| Phrase | **R + Down** | Table |
| Table | **R + Up** | Phrase |
| Song | **R + Left** (on row 0) | Groove |

### Playback Controls

| Action | Description |
|--------|-------------|
| **Start** | Play from current position |
| **R + Start** | Stop playback |
| **L + Start** | Live mode — play current row |

### Selection & Clipboard (Song / Chain / Phrase)

| Action | Description |
|--------|-------------|
| **B** (held) + **D-Pad** | Start/extend selection |
| **B** (release) | Copy selection |
| **A** | Paste clipboard |
| **L + B** | Select entire row |
| **L + B + B** | Select entire screen |
| **L + A** | Cut selection |

### Mute & Solo (Song / Chain / Phrase)

| Action | Description |
|--------|-------------|
| **R + B** | Toggle mute on cursor channel |
| **R + A** | Toggle solo on cursor channel |
| **L + R** | Un-mute all channels |

---

## Mixer Page

Navigate to the Mixer from the Song page with **R + Down**. Return to Song with **R + Up**.

### Layout (top to bottom)

```
 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F MAS   ← Channel / Master labels
 ████████████████████████████████████████████████████████  ← Pixel level meters (10 rows)
 100 100 100 100 100 100 100 100 100 100 100 ...  100    ← Volume %
                  S           M                          ← Mute / Solo indicators
 ~~~~~~~~~ oscilloscope waveform (real-time) ~~~~~~~~~~  ← Pixel oscilloscope
```

- **Level meters** — Per-channel pixel bars with a green → yellow → red gradient mapped to a dB scale (−48 dB to +6 dB). A bright peak-hold marker tracks the loudest recent level. The rightmost column is the master bus.
- **Volume %** — Current volume for each channel (0–100) and master.
- **Mute / Solo indicators** — Shows **M** when a channel is muted or **S** when soloed.
- **Oscilloscope** — A real-time waveform trace of the master output, drawn in a cyan-to-purple gradient.

### Mixer Controls

| Action | Description |
|--------|-------------|
| **Left / Right** | Select channel (last column = Master) |
| **Up / Down** | Increase / decrease volume on the selected channel |
| **A** | Toggle mute on the selected channel (blocked while any channel is soloed) |
| **B** | Toggle solo on the selected channel (mutes all others; press again to unsolo) |
| **Start** | Play / Stop |
| **R + Up** | Return to Song page |

> **Solo lock:** While any channel is soloed, mute toggles are locked out. Unsolo the channel first (press **B** on the soloed channel) to restore normal mute control.

---

## Sample Recorder

The sample recorder lets you capture audio from any connected input device directly into a WAV file and auto-import it into the current instrument.

### Opening the Recorder

1. Navigate to the **Instrument** page.
2. Select the **`record`** action field and press **A** to open the Record Sample dialog.

### Setup Phase

When the dialog opens you'll see:

| Field | Description |
|-------|-------------|
| **Input** | The capture device. Use **Left / Right** to cycle through available audio inputs. |
| **Rate** | Detected native sample rate of the device (read-only). |
| **Chan** | Detected channel count — Mono or Stereo (read-only). |

Use **Up / Down** to move between the input selector and the buttons.

| Button | Action |
|--------|--------|
| **Record** (A) | Start recording |
| **Cancel** (A or B) | Close without recording |

### Recording Phase

While recording you'll see:

- A **level meter** with a green → red gradient and a peak-hold marker so you can monitor input volume in real time.
- A running **time counter** (minutes:seconds).
- **Sample rate, channels, and sample count** info.
- Maximum recording length is **120 seconds**.

Press **A** or **B** to **stop** recording.

### Save Phase

After stopping, you can name and save the recording:

- **Name field** — Use **A** to open the on-screen keyboard, then type a filename.
  - **A** = insert character at cursor
  - **B** = backspace / delete
  - **L / R** = move cursor left / right in the name
  - **D-Pad** = navigate the keyboard grid
  - **Start** or select **OK** = close keyboard

| Button | Action |
|--------|--------|
| **Preview** | Play back the recording to check it before saving |
| **Save** | Write the WAV file to `samplelib/recordings/` and auto-import it into the current instrument |
| **Discard** | Throw away the recording and close |

Use **Left / Right** to move between Preview, Save, and Discard. Press **A** to activate. Press **B** at any time to discard and close.

The saved file is a standard 16-bit PCM WAV at the device's native sample rate and channel count.

---

## Windows VST3 Plugins (via yabridge + Proton)

SDTracker can load **Windows VST3 instrument and effect plugins** on the Steam Deck (and any Linux x86_64 system with Steam installed) using [yabridge](https://github.com/robbert-vdh/yabridge) and Valve's Proton. This is fully automatic — no manual Wine or yabridge configuration is required.

### How It Works

On startup SDTracker:

1. Locates Proton (prefers **Proton Experimental**) from your Steam installation.
2. Extracts its bundled copy of **yabridge 5.1.1** to `~/Documents/SDTracker/yabridge/`.
3. Initialises a dedicated Wine prefix at `~/Documents/SDTracker/wineprefix/`.
4. Creates the standard Windows VST3 directory (`Program Files/Common Files/VST3/`) inside the Wine prefix so Windows installers drop plugins in the right place.
5. Scans **all** plugin directories — `~/.vst3/win/` plus the standard Windows install locations inside the prefix — and runs `yabridgectl sync` to create Linux bridge bundles.
6. Copies essential DLLs (DirectX/vkd3d) from Proton's default prefix so plugins with GPU-accelerated GUIs can load.
7. Sets `WINELOADER`, `WINEPREFIX`, `LD_LIBRARY_PATH`, and `PATH` so the yabridge chainloader can find everything at runtime.

After this, bridged plugins appear in the **Import VST3 Instrument** and **Import VST3 Effect** dialogs alongside native Linux plugins.

### Installing Windows VST3 Plugins

#### Method 1: Run the Windows Installer (Recommended)

The easiest way is to run the plugin's Windows installer directly through Wine/Proton. SDTracker's Wine prefix already has the standard `Program Files/Common Files/VST3/` directory, so installers will put the `.vst3` in the right place and any extra files (presets, samples, licence data) will land where the plugin expects them.

```bash
# Find Proton's wine binary
WINELOADER=$(find ~/.steam/steam/steamapps/common -path "*/Proton*/files/bin/wine" | head -1)

# Run the installer into SDTracker's Wine prefix
WINEPREFIX=~/Documents/SDTracker/wineprefix "$WINELOADER" /path/to/PluginInstaller.exe
```

The installed plugin will be discovered automatically on next launch — no extra steps needed.

#### Method 2: Copy Files Manually

You can also drop Windows `.vst3` files (or `.vst3` bundle directories) directly into:

```
~/.vst3/win/
```

Examples:
```
~/.vst3/win/TAL-NoiseMaker.vst3/          ← bundle directory
~/.vst3/win/TAL-NoiseMaker.vst3/Contents/x86_64-win/TAL-NoiseMaker.vst3

~/.vst3/win/fb3300.vst3                    ← flat .vst3 file (also works)
```

Both proper VST3 bundle directories and flat `.vst3` files are supported. The next time SDTracker starts, `yabridgectl sync` will create the corresponding Linux bridge bundles automatically.

#### Scanned Directories

SDTracker automatically scans all of these locations for Windows VST3 plugins:

| Location | Description |
|----------|-------------|
| `~/.vst3/win/` | Manual drop-in directory |
| `{wineprefix}/drive_c/Program Files/Common Files/VST3/` | Standard Windows 64-bit install path |
| `{wineprefix}/drive_c/Program Files (x86)/Common Files/VST3/` | Standard Windows 32-bit install path |

Where `{wineprefix}` is `~/Documents/SDTracker/wineprefix/` (or its `pfx/` subdirectory when using Proton).

### Plugins That Need External Files

Many Windows VST3 plugins ship with presets, sample libraries, or configuration files that live outside the `.vst3` bundle itself. Here's where those files go:

| What the plugin expects | Windows path | Place files here (Linux) |
|---|---|---|
| Files next to the `.vst3` | Same folder as the DLL | `~/.vst3/win/` (alongside the `.vst3`) |
| Companion directories | e.g. `Grand Piano.instruments/` | `~/.vst3/win/Grand Piano.instruments/` (yabridge copies these automatically) |
| User presets / config | `%APPDATA%\VendorName\` | `~/Documents/SDTracker/wineprefix/drive_c/users/steamuser/AppData/Roaming/VendorName/` |
| Local data | `%LOCALAPPDATA%\VendorName\` | `~/Documents/SDTracker/wineprefix/drive_c/users/steamuser/AppData/Local/VendorName/` |
| Shared data | `C:\ProgramData\VendorName\` | `~/Documents/SDTracker/wineprefix/drive_c/ProgramData/VendorName/` |
| Installed programs | `C:\Program Files\VendorName\` | `~/Documents/SDTracker/wineprefix/drive_c/Program Files/VendorName/` |
| User documents | `C:\Users\<user>\Documents\` | `~/Documents/SDTracker/wineprefix/drive_c/users/steamuser/Documents/` |

> **Tip:** Wine's `Z:` drive maps to `/` (the entire Linux filesystem), so plugins can technically access any Linux path. But most plugins use standard Windows paths listed above.

> **Tip:** If a plugin needs external files, the simplest approach is to use **Method 1** (run the Windows installer) — it will place everything in the correct locations automatically.

### Requirements

- **Steam** installed with **Proton Experimental** (downloaded automatically when you first run any Proton game, or install it manually from Steam → Library → Search "Proton Experimental").
- An **x86_64 Linux** system (Steam Deck, desktop Linux, etc.).
- No manual yabridge or Wine installation needed — SDTracker bundles everything.

### Troubleshooting

| Problem | Solution |
|---|---|
| Plugin doesn't appear in the import dialog | Make sure the `.vst3` is in `~/.vst3/win/` and restart SDTracker |
| "Library not found" errors in the log | The Wine prefix may be missing DLLs — delete `~/Documents/SDTracker/wineprefix/` and restart (it will be recreated) |
| Plugin loads but produces no audio | Some plugins need their external resource files installed (see table above). Check the plugin vendor's documentation for required file locations |
| App crashes when loading a plugin | Fixed in current version — yabridge load failures are caught gracefully. If it still crashes, check the terminal output for details |
| "low memory locking limit" warnings | Add `* - memlock unlimited` to `/etc/security/limits.d/99-memlock.conf` and reboot |

---

## Command Reference

Commands are entered in the phrase and table editors. Each command uses the format **`CMD:aabb`** where `aa` is typically a speed or modifier parameter and `bb` is the target value. All values are hexadecimal.

Commands are processed in two stages: the **Player** handles global/playback commands first, then the remaining commands are forwarded to the active **Instrument** for audio-level processing.

### Instrument Compatibility Key

| Symbol | Meaning |
|--------|---------|
| **S** | Sample Instrument |
| **SF** | SoundFont Instrument |
| **LV2** | LV2 Plugin Instrument |
| **All** | All instrument types |
| **Player** | Handled by the player engine (instrument-independent) |

---

### Playback & Flow Control

#### KILL — Kill Note
**Format:** `KILL:--bb`  
**Support:** Player  
Stops the note playing on the current channel after `bb` ticks. Use `KILL:0000` to cut immediately on the next tick. The tick counter decrements each table tick, so higher values give a longer fade-out window before the hard stop.

#### STOP — Stop Playback
**Format:** `STOP:0000`  
**Support:** Player  
Immediately stops song playback. In **Song mode**, stops the entire song. In **Live mode**, stops the current channel only. Takes no parameters.

#### HOP — Hop to Position
**Format:** `HOP:aabb`  
**Support:** Player  
Jumps playback to step `bb` in the current phrase.

- `aa` = loop count: how many times to execute the hop before falling through.
  - `00` = infinite loop (always jumps).
  - `01`–`FE` = jump that many times, then continue past the HOP on the next encounter.
- `bb` = target step (`00`–`0F`).

Example: `HOP:0308` jumps to step 08 three times, then on the fourth encounter continues past without hopping.

#### DLAY — Delay Note
**Format:** `DLAY:--bb`  
**Support:** Player  
Delays the triggering of the note on the current step by `bb` ticks. The note data is held and only played after the specified number of ticks have elapsed.

#### TABL — Trigger Table
**Format:** `TABL:--bb`  
**Support:** Player  
Activates table `bb` on the current channel. Tables are independent sequencer lanes that run commands at a faster rate (controlled by the table tick ratio). Useful for creating rapid arpeggios, filter sweeps, or other automated modulations.

#### GROV — Set Groove
**Format:** `GROV:aabb`  
**Support:** Player  
Sets groove pattern `bb` on the current channel. If `aa` is non-zero, the groove is applied to **all** channels simultaneously. Grooves define variable tick lengths per step to create swing or shuffle feels.

#### RAND — Random Probability
**Format:** `RAND:--bb`  
**Support:** Player  
Sets the probability that the current note will play.

- `bb` = chance out of 256.
  - `00` = note never plays.
  - `80` = ~50% chance.
  - `FF` = note always plays (guaranteed).

When the probability check fails, the note is killed. Useful for creating generative, evolving patterns.

---

### Volume & Panning

#### VOLM — Volume
**Format:** `VOLM:aabb`  
**Support:** S  
Smoothly ramps the channel volume toward `bb` at speed `aa`.

- `bb` = target volume (`00`–`FF`, where `FF` is maximum).
- `aa` = ramp speed (`00` = instant, higher = slower ramp).

Example: `VOLM:0080` instantly sets volume to 50%. `VOLM:1000` fades to silence slowly.

#### PAN — Panning
**Format:** `PAN:aabb`  
**Support:** S  
Pans the channel toward position `bb` at speed `aa`.

- `bb` = pan position (`00` = hard left, `7F` = center, `FE` = hard right).
- `aa` = ramp speed (`00` = instant, higher = slower).

#### TRML — Tremolo
**Format:** `TRML:aabb`  
**Support:** S, SF, LV2  
Applies a volume LFO (tremolo) to the channel.

- `aa` = oscillation speed (`01`–`FF`, higher = faster wobble).
- `bb` = depth (`00` = off, `FF` = maximum volume swing).

The LFO uses a sine wave. On Sample instruments it's applied as an additive offset to volume at the k-rate update. On SoundFont and LV2 instruments, it's applied per-sample as a multiplicative modulation: `volume × (1.0 + sin(phase) × depth)`.

---

### Pitch & Tuning

#### PTCH — Pitch Slide
**Format:** `PTCH:aabb`  
**Support:** S  
Slides pitch toward `bb` semitones (relative) at speed `aa`.

- `bb` = target pitch offset in semitones (signed: `01`–`7F` = up, `80`–`FF` = down).
- `aa` = ramp speed (`00` = instant, higher = slower glide).

#### LEGA — Legato
**Format:** `LEGA:aabb`  
**Support:** S  
Slides from the **previous note's pitch** to the current note without retriggering. Creates smooth portamento between notes.

- `bb` = semitone offset. If `00`, slides to the last played note on the channel.
- `aa` = slide speed (`00` = instant, higher = slower).

#### PFIN — Pitch Fine Tune
**Format:** `PFIN:aabb`  
**Support:** S  
Fine-tunes pitch by a fractional semitone amount.

- `bb` = fine pitch target (`00`–`FF`, where `80` = no change, `00` = -1 semitone, `FF` = +1 semitone).
- `aa` = ramp speed (`00` = instant, higher = slower).

Useful for subtle detuning, microtuning, or smooth vibrato-like sweeps with precise control.

#### ARPG — Arpeggio
**Format:** `ARPG:abcd`  
**Support:** S  
Rapidly cycles through relative pitch offsets from the base note each tick.

- `a`, `b`, `c`, `d` = semitone offsets (each is a single hex digit, `0`–`F`).

The arpeggiator cycles: base → +a → +b → +c → +d → base → ... Unused trailing values of `0` are skipped. Example: `ARPG:3700` creates a minor chord arpeggio (root, +3, +7).

#### VIBR — Vibrato
**Format:** `VIBR:aabb`  
**Support:** S, SF, LV2  
Applies a pitch LFO (vibrato) to the channel.

- `aa` = oscillation speed (`01`–`FF`, higher = faster).
- `bb` = depth (`00` = off, `FF` = maximum ±4 semitone swing).

Uses a sine wave oscillator. Modulates pitch via `pow(2, sin(phase) × depth × 4/12)`, giving a musically-scaled pitch wobble up to ±4 semitones at full depth.

---

### Filter

#### FLTR — Filter & Resonance (Instant)
**Format:** `FLTR:aabb`  
**Support:** S  
Instantly sets both filter cutoff and resonance simultaneously.

- `aa` = cutoff frequency (`00` = fully closed, `FF` = fully open / bypass).
- `bb` = resonance / Q (`00` = no resonance, `FF` = maximum).

Default (no filter) is `FF00`. This command replaces any active FCUT or FRES ramp.

#### FCUT — Filter Cutoff (Ramp)
**Format:** `FCUT:aabb`  
**Support:** S  
Smoothly ramps the filter cutoff to `bb` at speed `aa`.

- `bb` = target cutoff (`00`–`FF`).
- `aa` = ramp speed (`00` = instant, higher = slower sweep).

#### FRES — Filter Resonance (Ramp)
**Format:** `FRES:aabb`  
**Support:** S  
Smoothly ramps filter resonance to `bb` at speed `aa`.

- `bb` = target resonance (`00`–`FF`).
- `aa` = ramp speed (`00` = instant, higher = slower).

#### LFOF — LFO Filter
**Format:** `LFOF:aabb`  
**Support:** S, SF  
Oscillates the filter cutoff with a sine-wave LFO.

- `aa` = oscillation speed (`01`–`FF`, higher = faster).
- `bb` = depth (`00` = off, `FF` = full cutoff range sweep).

Creates auto-wah and filter wobble effects. On SoundFont instruments, modulation is applied per-sample.

---

### Sample Manipulation

#### LPOF — Loop Offset
**Format:** `LPOF:aabb`  
**Support:** S  
Shifts both the loop start and loop end points by `aabb` samples.

- Positive values shift the loop window forward in the sample.
- Values above `8000` are treated as negative (shift backward).

Useful for scanning through different parts of a looping waveform in real time.

#### PLOF — Play Offset
**Format:** `PLOF:aabb`  
**Support:** S  
Jumps the playback position within the sample.

- `aa` = absolute position jump. The sample is divided into 256 chunks; `aa` jumps to that chunk (`00` = no absolute jump, `01` = near start, `FF` = near end).
- `bb` = relative offset. Signed value (`00`–`7F` = forward, `80`–`FF` = backward) in chunk-sized steps.

Both absolute and relative offsets can be combined in a single command.

#### CRSH — Drive & Crush
**Format:** `CRSH:aa-b`  
**Support:** S  
Applies distortion and bit-crushing effects.

- `aa` = drive amount (`00` = no change, `01`–`FF` = increasing distortion).
- `b` = bit crush (lower nibble only, `0` = no change, `1`–`F` = decreasing bit depth).

Values of `0` for either parameter leave that effect unchanged from its current state.

#### RTRG — Retrigger
**Format:** `RTRG:aabb`  
**Support:** S  
Retriggers the sample in a loop from its current position.

- `bb` = loop length in ticks (number of ticks before each retrigger).
- `aa` = speed/offset applied at each retrigger cycle.

Creates stuttering, glitch, and drum-roll effects. Set `bb` to `00` to disable retriggering.

#### IRTG — Instrument Retrigger
**Format:** `IRTG:aabb`  
**Support:** Player  
Retriggers the current instrument and transposes it.

- `aa` = retrigger speed.
- `bb` = transpose amount.

Unlike RTRG which loops from the current position, IRTG fully retriggers the instrument as if a new note was played.

---

### Feedback (Sample Instrument)

#### FBMX — Feedback Mix
**Format:** `FBMX:aabb`  
**Support:** S  
Ramps the feedback mix level to `bb` at speed `aa`.

- `bb` = target feedback mix amount (`00`–`FF`).
- `aa` = ramp speed (`00` = instant, higher = slower).

Controls how much of the feedback buffer is mixed into the output.

#### FBTN — Feedback Tune
**Format:** `FBTN:aabb`  
**Support:** S  
Ramps the feedback tuning to `bb` at speed `aa`.

- `bb` = target feedback tune (`00`–`FF`).
- `aa` = ramp speed (`00` = instant, higher = slower).

Adjusts the pitch/delay-length of the feedback loop, creating comb-filter and Karplus-Strong style timbral changes.

---

### Effects

#### FXSN — Effect Send
**Format:** `FXSN:aabb`  
**Support:** Player  
Routes the channel's audio through an LV2 effect loaded on the Effects page.

- `aa` = effect slot number (`00`–`0F`, corresponding to the 16 effect slots).
- `bb` = wet/dry mix (`00` = fully dry, `FF` = fully wet).

Use `FXSN:FF00` or any slot ≥ `10` to clear the effect assignment from the channel. Effects must be loaded on the Effects page before they can be assigned.

#### REVB — Reverb
**Format:** `REVB:aabb`  
**Support:** S, LV2  
Configures the built-in reverb send for the channel. Uses a nibble-based format:

- `a` (high nibble of `aa`) = decay amount (`0`–`F`, maps to 0–90% feedback).
- `a` (low nibble of `aa`) = damping (`0`–`F`, maps to 0–85% high-frequency absorption).
- `bb` = send amount (`00`–`FF`, how much signal is sent to the reverb).

Example: `REVB:F080` = maximum decay, no damping, 50% send. `REVB:8840` = medium decay, medium damping, 25% send.

The reverb uses a 6-tap multi-delay with cross-channel diffusion and configurable damping for a rich stereo wash.

---

### MIDI

#### MDCC — MIDI CC
**Format:** `MDCC:aabb`  
**Support:** S  
Sends a MIDI Continuous Controller message.

- `aa` = CC number (`00`–`7F`).
- `bb` = CC value (`00`–`7F`).

Useful for controlling external MIDI hardware or software parameters.

#### MDPG — MIDI Program Change
**Format:** `MDPG:--bb`  
**Support:** S  
Sends a MIDI Program Change message on the current channel.

- `bb` = program number.

#### MVEL — MIDI Velocity
**Format:** `MVEL:--bb`  
**Support:** S  
Sets the MIDI velocity for the current step.

- `bb` = velocity value (`00`–`FF`).

Affects the note-on velocity of MIDI output and can influence sample playback volume on velocity-sensitive instruments.

#### MBNK — MIDI Bank Select
**Format:** `MBNK:aabb`  
**Support:** S  
Sends a MIDI Bank Select message (CC0 + CC32) on the current channel.

- `aa` = Bank Select MSB (CC0, `00`–`7F`).
- `bb` = Bank Select LSB (CC32, `00`–`7F`).

Typically used immediately before an `MDPG` command to select a bank before switching programs. Example: `MBNK:0001` then `MDPG:0005` selects bank 1, program 5.

#### MCAT — MIDI Channel Aftertouch
**Format:** `MCAT:--bb`  
**Support:** S  
Sends a MIDI Channel Aftertouch (Channel Pressure) message.

- `bb` = pressure value (`00`–`7F`).

Applies pressure-based modulation to all notes on the MIDI channel. How it's interpreted depends on the receiving synth's aftertouch routing.

#### MPAT — MIDI Polyphonic Aftertouch
**Format:** `MPAT:aabb`  
**Support:** S  
Sends a MIDI Polyphonic Aftertouch (Key Pressure) message.

- `aa` = note number (`00`–`7F`).
- `bb` = pressure value (`00`–`7F`).

Applies pressure-based modulation to a specific note. Useful for per-note expression on synths that support poly aftertouch.

---

### Tempo

#### TMPO — Tempo
**Format:** `TMPO:--bb`  
**Support:** Player  
Sets the global playback tempo.

- `bb` = tempo in BPM (hex). Valid range: 41–399 BPM (hex `29`–`018F`).

Example: `TMPO:0078` sets tempo to 120 BPM. `TMPO:00A0` sets tempo to 160 BPM.

---

### Quick Reference Table

| Command | Format | Description | Support |
|---------|--------|-------------|---------|
| `ARPG` | `abcd` | Arpeggio cycle through pitch offsets | S |
| `CRSH` | `aa-b` | Drive & bit crush | S |
| `DLAY` | `--bb` | Delay note by bb ticks | Player |
| `FBMX` | `aabb` | Feedback mix ramp | S |
| `FBTN` | `aabb` | Feedback tune ramp | S |
| `FCUT` | `aabb` | Filter cutoff ramp | S |
| `FLTR` | `aabb` | Instant filter cutoff + resonance | S |
| `FRES` | `aabb` | Filter resonance ramp | S |
| `FXSN` | `aabb` | Route to LV2 effect slot | Player |
| `GROV` | `aabb` | Set groove pattern | Player |
| `HOP`  | `aabb` | Hop to phrase position | Player |
| `IRTG` | `aabb` | Instrument retrigger + transpose | Player |
| `KILL` | `--bb` | Kill note after bb ticks | Player |
| `LEGA` | `aabb` | Legato/portamento slide | S |
| `LFOF` | `aabb` | LFO filter modulation | S, SF |
| `LPOF` | `aabb` | Loop start/end offset | S |
| `MBNK` | `aabb` | MIDI Bank Select (CC0 + CC32) | S |
| `MCAT` | `--bb` | MIDI Channel Aftertouch | S |
| `MDCC` | `aabb` | Send MIDI CC | S |
| `MDPG` | `--bb` | Send MIDI Program Change | S |
| `MPAT` | `aabb` | MIDI Polyphonic Aftertouch | S |
| `MVEL` | `--bb` | Set MIDI velocity | S |
| `PAN`  | `aabb` | Pan position ramp | S |
| `PFIN` | `aabb` | Pitch fine tune ramp | S |
| `PLOF` | `aabb` | Playback position offset | S |
| `PTCH` | `aabb` | Pitch slide ramp | S |
| `RAND` | `--bb` | Probability note plays | Player |
| `REVB` | `aabb` | Reverb decay/damp/send | S, LV2 |
| `RTRG` | `aabb` | Retrigger from position | S |
| `STOP` | `----` | Stop playback | Player |
| `TABL` | `--bb` | Trigger table | Player |
| `TMPO` | `--bb` | Set tempo (BPM in hex) | Player |
| `TRML` | `aabb` | Tremolo (volume LFO) | S, SF, LV2 |
| `VIBR` | `aabb` | Vibrato (pitch LFO) | S, SF, LV2 |
| `VOLM` | `aabb` | Volume ramp | S |

