[markdown]
# Model:Cycles 6CH — Companion VST3 / AU Plugin

Free, open-source companion audio plugin (**VST3** & **AU**) for the **Elektron Model:Cycles 6-Channel USB Audio mod** (*Modded-Cycles* / *ms-multi-output*).

### Why does this plugin exist?
In 6-channel USB Audio mode (`6CH`), the Model:Cycles outputs its 6 mono stems **pre-mixer**. Because of this, turning the `VOLUME + DIST` knob down to `0` removes the overdrive, but the dry stem volume never drops to silence.

**Model:Cycles 6CH** solves this in any DAW by listening to the Model:Cycles MIDI CC messages over the same USB cable and recreating the hardware volume, mute, and pan behavior right on each track:

* **CC 7 (`VOLUME + DIST`)**:
  * `0 – 64` smoothly attenuates the stem from `-∞ dB` (complete silence) to `0 dB` (unity).
  * `64 – 127` stays locked at `0 dB`, letting the hardware overdrive in the stem come through cleanly without double-boosting.
* **CC 94 (`MUTE`)**: Smooth, click-free track mute.
* **CC 10 (`PAN`)**: Center-unity stereo pan (`64 = Center`) converting each mono stem to stereo.
* **CC 95 (`LEVEL / DATA`)**: Optional toggleable attenuation (`CC95 ON / OFF`).
* **Zero-routing "Direct USB MIDI" mode**: Select your Model:Cycles directly inside the plugin header to bypass DAW MIDI routing completely (all 6 instances safely share one hardware MIDI port), or use standard **DAW MIDI** mode if you want to record MIDI automation.

---

## Download & Installation

Grab the latest ZIP for your OS from the **[Releases](../../releases)** page.

### macOS (Apple Silicon & Intel — Universal)
1. Unzip `ModelCycles-6CH-macOS-Universal.zip`.
2. Copy the plugin bundles to your system plugin folders:
   * **VST3** (REAPER, Ableton Live, Cubase, Bitwig, FL Studio, Studio One):
     `/Library/Audio/Plug-Ins/VST3/ModelCycles 6CH.vst3`
   * **AU** (Logic Pro, GarageBand):
     `/Library/Audio/Plug-Ins/Components/ModelCycles 6CH.component`
3. **First-time macOS security note (Unsigned Open-Source Plugin):**
   Because this free plugin is not notarized with a paid Apple Developer certificate, macOS Gatekeeper may block it on first scan. Run this command in **Terminal** after copying the files:
   ```bash
   xattr -cr "/Library/Audio/Plug-Ins/VST3/ModelCycles 6CH.vst3"
   xattr -cr "/Library/Audio/Plug-Ins/Components/ModelCycles 6CH.component"
   ```
4. Rescan plugins in your DAW.

### Windows (64-bit)
1. Unzip `ModelCycles-6CH-Windows-x64.zip`.
2. Copy `ModelCycles 6CH.vst3` into:
   `C:\Program Files\Common Files\VST3\`
3. Rescan plugins in your DAW.

---

## Quick Setup

### 1. On your Model:Cycles
1. Press **SETTINGS → MIDI → PORTS** → set **`OUT TO`** to **`USB`** (or **`M+U`**).
2. Press **SETTINGS → MIDI → FILTER** → set **`ENCS`** to **`I+E`**.
3. Press **SETTINGS → MIDI → OUT CHAN** → make sure **T1–T6** are set to MIDI channels **1–6**.
4. For each track **T1–T6**, press **`FUNC + TRACK`** (`TRACK MENU`) and enable **`MOUT`** *(note: `MOUT` is saved per pattern)*.

### 2. In your DAW
1. Create 6 audio tracks receiving **Input 1** through **Input 6** from the Model:Cycles.
2. Insert **ModelCycles 6CH** as the first effect on each track.
3. Click **`T1`**, **`T2`**, **`T3`**, **`T4`**, **`T5`**, **`T6`** respectively on each instance.
4. Choose your MIDI input mode in the top-right dropdown:
   * **Easiest (Recommended — works in Logic Pro, Ableton, REAPER, FL Studio, etc.):** Select **`Direct: Elektron Model:Cycles`** in the dropdown. *(On Windows, disable the Model:Cycles MIDI input inside your DAW preferences so the plugin can open the single-client port directly).*
   * **DAW MIDI Mode:** Leave the dropdown on **`DAW MIDI`** and route MIDI channels 1–6 from your DAW to each track (best if you want to record MIDI automation inside the DAW).

> **Note on Hardware Limitations:** The Model:Cycles only outputs MIDI CC for physical knob turns and mutes. Parameter Locks (P-Locks) and LFOs assigned to `DIST` do not send MIDI CC out.

---

## Building from Source

Requires **CMake 3.22+** and a C++17 compiler (Xcode on macOS, Visual Studio on Windows). JUCE is fetched automatically by CMake.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

## License

This project uses the [JUCE](https://juce.com/) framework under the **GNU Affero General Public License v3.0 (AGPL-3.0)**. See the `LICENSE` file for full details.
