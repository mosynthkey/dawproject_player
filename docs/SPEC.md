# DAWPROJECT player — specification draft

Status: proposal for review. Items under "Open questions" are not decided.

This is a playback and render tool for `.dawproject` files, not a DAW. It plays arrangement audio and can write the same mix to a file. MIDI, plug-ins, and recording are out of scope.

## Goals

- C++17, small dependency set, low resident memory.
- Pitch-preserving time stretch via Signalsmith Stretch.
- Device I/O and file decode/encode via miniaudio.
- Real-time playback and offline render through one mix graph.
- Native terminal UI with transport only (no clip waveforms).
- The same engine builds to WebAssembly.
- On-screen CPU and memory use.

## Non-goals

- MIDI notes, note expressions, and instrument tracks as sound.
- VST2, VST3, CLAP, AU, and generic plug-in state.
- Recording, editing, and saving a project back out.
- Video.

## What a `.dawproject` file is

A ZIP container. `project.xml` (and `metadata.xml`) are UTF-8 XML. Media paths are chosen by the exporter.

Arrangement audio in the wild is often nested the way Bitwig writes it: an arrangement `Clip` holds inner `Clips`, which hold `Warps`, which hold `Audio` plus at least two `Warp` points. Warp `time` is on the outer timeline (usually beats). `contentTime` is on the inner timeline (usually seconds). Segments between warp points are linear.

Other audio facts that affect playback:

- `Clip` has `time`, `duration`, `playStart`, `playStop`, `loopStart`, `loopEnd`, `enable`, and optional `reference` for linked clips.
- Fades use `fadeInTime` / `fadeOutTime` with `fadeTimeUnit` of beats or seconds. A negative fade-in starts the clip earlier (`time - abs(fadeInTime)`), which is how the format expresses a crossfade.
- `Audio` carries `sampleRate`, `channels`, `duration` (whole file, seconds), and an optional vendor-specific `algorithm` string. Bitwig writes `algorithm="stretch"`.
- `File` is inside the zip unless `external="true"` (then the path is relative to the `.dawproject`, or absolute).
- Transport tempo is a `RealParameter` in bpm, optionally automated with `Points` (`hold` or `linear`). Time signature is separate and can also be automated.
- Mixer: track `Channel` with `Volume`, `Pan`, `Mute`, `Solo`, `destination`, and `audioChannels`. Roles include regular, master, effect, submix, and vca. Sends exist. Built-in EQ, compressor, gate, and limiter exist.
- Clip-level gain, pan, and transpose are expression timelines (`Points` with `expression="gain|pan|transpose"`), not attributes on `Clip`.
- `Scene` / `ClipSlot` is the clip launcher, separate from `Arrangement`.

## How other engines treat stretched clips

Tracktion Engine has two playback paths:

- Proxy: render the whole stretched clip to a float WAV and memory-map it. Playback CPU is then almost zero, but a long clip becomes a second full copy (a 5-minute 48 kHz stereo float file is about 115 MB) and the proxy is stale as soon as the warp changes.
- Real-time (what Waveform prefers while a clip is still live): do not build that proxy. `ReadAheadTimeStretcher` runs the stretcher on a background thread into a FIFO a short distance ahead of the playhead. The audio thread only copies. The source is read from the file, not expanded into a stretched buffer. Seeking and loop wraps re-prime the stretcher. If the ratio is 1 and there is no pitch change, the engine skips the stretcher and only resamples.

Zrythm's cache copies the fully stretched region into an `AudioTimelineDataCache`. That is simple and realtime-safe, and it is the wrong default for a low-memory player.

This player follows the Tracktion read-ahead model and does not follow the proxy or the full-region cache.

## Engine design

One mix graph serves playback and render. Offline render calls the same block function in a pull loop and does not open a device.

### Flatten on load

Parse into a compact model and drop the XML. Flatten nested clips into playable events:

- Each event has an arrangement range, a source file, a warp map (piecewise linear), loop points, fade times, static or automated gain/pan/transpose, and the destination channel.
- Convert musical time to seconds with a tempo map (binary search over integrated breakpoints, not a per-sample table).
- Bar numbers use the time-signature map the same way.
- Skip `loaded="false"` tracks and `enable="false"` clips.
- Linked clips (`reference`) resolve to the referenced timeline.

### Memory

- Do not decode a file into a single buffer, and do not pre-render a stretched proxy.
- A Signalsmith instance exists only for a clip that is audible inside the look-ahead window. A silent project allocates none.
- Source audio stays compressed in the zip (or on disk when `external="true"`). miniaudio decodes on seek into a small block cache (on the order of 1 MB, shared).
- Each active clip keeps a FIFO of stretched float frames, about 100–250 ms. At 48 kHz stereo that is under 100 KB per sounding clip.
- The dominant WASM cost is the zip itself, because the browser has to hold the file. Stretched audio is not the dominant cost.
- Render writes WAV as it goes. The output is not accumulated in RAM.

Signalsmith is stateful. A seek, a loop wrap, or the first block of a clip calls `seek` / `outputSeek` with a pre-roll of about one block plus one interval (`seekLength()`, and `inputLatency()` so output lines up). The decoder must be able to seek backward by that pre-roll.

Per callback block, the local warp slope is `d(contentTime) / d(time)`. `process(input, inputFrames, output, outputFrames)` gets that ratio for the block. Pitch uses `setTransposeSemitones`. When the ratio is 1 and transpose is 0, skip the stretcher. If the source rate and the device rate still differ, resample only.

### Threads

Native playback: the audio callback only mixes FIFOs, fades, and channel gain/pan. A worker fills the FIFOs.

Offline render: stretch synchronously on the render thread. No worker, larger blocks.

WASM without pthreads: stretch inside the audio callback (or the main-thread audio pump miniaudio uses). No shared-memory worker in the first web build. `presetCheaper` is the realtime configuration so a callback-sized block stays affordable. Render still uses `presetDefault` only if playback and render are allowed to differ; see the open question.

### Mixer

Sum sounding clips into their destination channel, then walk `destination` links to the master. Apply volume, pan, mute, and solo. Solo follows the usual rule: if any channel is soloed, only soloed channels are heard.

Pan is the normalized 0–1 parameter (0.5 is center) mapped to constant-power or linear balance. Volume `unit` may be `linear`, `normalized`, or `decibel`; convert to a linear gain. Mono sources are duplicated to the output bus before pan. Channel counts above the output bus are downmixed.

Fades are applied in the arrangement domain. Overlapping negative fade-ins are independent fades on each clip, not a linked crossfade object.

### Sample rates and files

Decode at the file rate. Stretch in that rate. Resample the clip output to the device rate with miniaudio when they differ.

Decoders enabled: WAV, FLAC, MP3. The project has no single sample rate; the device rate (or the render rate flag) is the output rate.

### UI

Native: a dependency-free ANSI screen, not ncurses.

- Transport state, position in seconds, bars, and beats, and the current tempo.
- Process CPU percent and resident memory.
- Audio callback load (time spent in the callback divided by the buffer duration), separate from process CPU.
- Track names with mute/solo marks. No waveforms and no clip rectangles.

Keys: play/pause, stop, seek by a bar, jump to a bar number, jump to a time in seconds, render, quit.

WASM: the engine is a C API. The page is a small HTML shell with the same commands and the same CPU/memory line. A terminal UI inside the browser is not the web UI. miniaudio's Emscripten backend talks to Web Audio. The first web build does not require AudioWorklet or pthreads.

CPU and memory sources:

- Native: `getrusage` for process CPU, resident set from the OS (`VmRSS` on Linux). Callback load from a clock around the mix.
- WASM: audio callback duty cycle as the CPU figure, and the Emscripten heap size as the memory figure. The browser does not expose process RSS to the module.

### Libraries

Git submodules, not copies:

- `miniaudio` — https://github.com/mackron/miniaudio
- `signalsmith-stretch` — https://github.com/Signalsmith-Audio/signalsmith-stretch  
  The stretch repo vendors `dsp/` in-tree. `cmd/util` is a submodule and is not required to compile the library.
- `miniz` — https://github.com/richgel999/miniz  
  In-memory unzip so WASM does not need a filesystem for the container.

XML is parsed by a small reader in this repo, covering elements, attributes, and text for the subset above. No pugixml unless real-world files prove that insufficient.

Build: CMake. Native executable plus an Emscripten target that exports the C API and emits the HTML shell.

## Playback scope this draft assumes

Included:

- Arrangement only (scenes ignored).
- Nested clips, loops, warp maps with any number of points, fades including negative fade-in overlap.
- Tempo and time-signature maps, including linear automation, because bar jumps and beat timelines need them.
- Static channel volume, pan, mute, solo, master, and submix routing via `destination`.
- Clip expression automation for gain, pan, and transpose, and channel volume/pan/mute automation, because those change the audio.
- Stereo output. Sources with more channels are downmixed.
- Embedded WAV, FLAC, and MP3. External files on native only.
- `algorithm` missing or `stretch`: Signalsmith when the ratio is not 1 or transpose is not 0. Any other algorithm string: still Signalsmith (pitch-preserving), not varispeed.
- Linear amplitude fades.
- Render to 32-bit float WAV at a chosen rate (default 48000), streamed. On WASM the result is a download.
- Realtime stretcher preset `presetCheaper`. Render uses the same preset so the file matches playback.

Excluded until a later decision:

- Scenes and clip slots.
- Sends, VCA, effect-role processing.
- Built-in EQ, compressor, gate, limiter.
- Varispeed / repitch algorithms.
- Channel counts above stereo as a first-class bus.

## Open questions

Defaults above are what will be built if you do not change them.

1. Is ignoring the clip launcher (`Scene` / `ClipSlot`) correct?
2. Should tempo, time signature, volume, pan, mute, and clip gain/pan/transpose automation all be audible in the first version? The alternative is static parameter values only, with tempo still read as a single bpm.
3. Are sends, VCA, and the built-in EQ / compressor / gate / limiter correctly out of scope?
4. When `algorithm` is present and is not `stretch`, should playback stay pitch-preserving? Varispeed would change pitch with the ratio and would not use Signalsmith.
5. Are linear fades acceptable, or do you want equal-power fades?
6. Is a stereo mix with downmix of extra source channels enough?
7. Is float32 WAV at 48 kHz (overridable) the right render target, including a browser download?
8. Is the UI split right: ANSI TUI on native, minimal HTML on WASM, jump by bar and by seconds?
9. Should render use `presetCheaper` as well, so it matches realtime, or `presetDefault` for a higher-quality bounce?
10. Is external media native-only, with WASM limited to files inside the zip?
11. Is a pthread-free WASM build acceptable for the first web version (stretch on the audio callback, higher latency, no AudioWorklet yet)?
