# DAWPROJECT player — specification

Status: review decisions are locked. Arrangement only. Sends, VCA, and built-in devices are unsupported.

This is a playback and render tool for `.dawproject` files, not a DAW. It plays arrangement audio and can write the same mix to a file. MIDI, plug-ins, and recording are out of scope.

## Goals

- C++17, small dependency set, low resident memory.
- Pitch-preserving time stretch via Signalsmith Stretch.
- Device I/O and file decode/encode via miniaudio.
- Real-time playback and offline render share one mix implementation. Each runs on its own engine instance and its own thread.
- Native terminal UI with transport only (no clip waveforms).
- The same engine builds to WebAssembly, with threads.
- On-screen CPU and memory use.

## Non-goals

- MIDI notes, note expressions, and instrument tracks as sound.
- VST2, VST3, CLAP, AU, and generic plug-in state.
- Recording, editing, and saving a project back out.
- Video.
- Clip launcher playback (`Scene` / `ClipSlot`). The arrangement is the song.
- Sends, VCA channels, and built-in EQ / compressor / gate / limiter. See below for why.

## What a `.dawproject` file is

A ZIP container. `project.xml` (and `metadata.xml`) are UTF-8 XML. Media paths are chosen by the exporter.

Arrangement audio in the wild is often nested the way Bitwig writes it: an arrangement `Clip` holds inner `Clips`, which hold `Warps`, which hold `Audio` plus at least two `Warp` points. Warp `time` is on the outer timeline (usually beats). `contentTime` is on the inner timeline (usually seconds). Segments between warp points are linear.

Other audio facts that affect playback:

- `Clip` has `time`, `duration`, `playStart`, `playStop`, `loopStart`, `loopEnd`, `enable`, and optional `reference` for linked clips.
- Fades use `fadeInTime` / `fadeOutTime` with `fadeTimeUnit` of beats or seconds. A negative fade-in starts the clip earlier (`time - abs(fadeInTime)`), which is how the format expresses a crossfade. The format does not store a fade curve.
- `Audio` carries `sampleRate`, `channels`, `duration` (whole file, seconds), and an optional vendor-specific `algorithm` string. The official example uses `algorithm="stretch"`. The schema has no enum of other values.
- `File` is inside the zip unless `external="true"` (then the path is relative to the `.dawproject`, or absolute).
- Transport tempo is a `RealParameter` in bpm, with `Points` automation (`hold` or `linear`). Time signature is a separate parameter and can also be automated.
- Mixer: track `Channel` with `Volume`, `Pan`, `Mute`, `Solo`, `destination`, and `audioChannels`. Roles include regular, master, effect, submix, and vca.
- Clip-level gain, pan, and transpose are expression timelines (`Points` with `expression="gain|pan|transpose"`).
- `Scene` / `ClipSlot` is a separate clip-launcher grid. It is not the arrangement.

### Clip launcher

A Scene is one column of a session grid (the Ableton / Bitwig clip launcher). A ClipSlot is one cell: a track plus a scene, holding a clip that loops until the user launches something else. Playing a scene starts every armed slot in that column together. That is a different transport from "play the arrangement from bar 1". The Bitwig example file stores an empty `<Scenes/>`. This player reads the arrangement and does not launch scenes.

### Sends, VCA, built-in devices

These are not a shared DSP algorithm.

- A send is a copy of a channel, pre or post fader, at a level, routed to another channel (usually an effect return). It is a routing edge plus a gain.
- A VCA channel has no audio of its own. Its fader multiplies the gain of other channels.
- `Equalizer`, `Compressor`, `NoiseGate`, and `Limiter` are a shared parameter list (threshold, ratio, attack, release, EQ band type / frequency / Q / gain, and so on) so a project can move those settings without a plug-in. The format does not define the filter shape, knee, or lookahead. Bitwig and Cubase will not sound the same from those numbers. This player does not invent that DSP.

## How other engines treat stretched clips

Tracktion Engine has two playback paths:

- Proxy: render the whole stretched clip to a float WAV and memory-map it. Playback CPU is then almost zero, but a long clip becomes a second full copy (a 5-minute 48 kHz stereo float file is about 115 MB) and the proxy is stale as soon as the warp changes.
- Real-time (what Waveform prefers while a clip is still live): do not build that proxy. `ReadAheadTimeStretcher` runs the stretcher on a background thread into a FIFO a short distance ahead of the playhead. The audio thread only copies. The source is read from the file, not expanded into a stretched buffer. Seeking and loop wraps re-prime the stretcher. If the ratio is 1 and there is no pitch change, the engine skips the stretcher and only resamples.

Zrythm's cache copies the fully stretched region into an `AudioTimelineDataCache`. That is simple and realtime-safe, and it is the wrong default for a low-memory player.

This player follows the Tracktion read-ahead model and does not follow the proxy or the full-region cache.

Tracktion's stored default for a clip fade-in and fade-out is `AudioFadeCurve::linear`. Overlapping crossfades use a pair of convex sine gains (`CrossfadeLevels`), which keeps the power roughly constant through the overlap. This player does the same: a lone fade is linear amplitude, and a negative fade-in overlap uses equal-power sine.

## Engine design

Playback and render call the same mix function. They do not share stretcher state. A render builds a second engine instance from the same project and runs it on a render thread. Both instances read one shared source cache. The stretch quality preset is one global setting and applies to both.

### Flatten on load

Parse into a compact model and drop the XML. Flatten nested clips into playable events:

- Each event has an arrangement range, a source file, a warp map (piecewise linear), loop points, fade times, gain/pan/transpose automation, and the destination channel.
- Tempo and time-signature automation are breakpoint lists. Musical time converts to seconds by binary search over the integrated tempo map. Bar numbers use the time-signature map the same way.
- Channel volume, pan, and mute automation are audible. Clip expression automation for gain, pan, and transpose is audible. Interpolation is `hold` or `linear`, matching the file. An unspecified point interpolation defaults to `hold`, as the format says.
- Skip `loaded="false"` tracks and `enable="false"` clips.
- Linked clips (`reference`) resolve to the referenced timeline.

### Warp algorithm string

The attribute is a free string. The schema lists no values besides calling it vendor-specific. The only value in the format example is `stretch`. Bitwig's own clip UI also has Stretch HD, Raw, and Repitch, but those names are not part of the DAWproject schema and may not be what other DAWs write.

Playback:

- Missing, empty, or `stretch`: Signalsmith when the ratio is not 1 or transpose is not 0. Otherwise copy or resample only.
- Any other string: still Signalsmith, and the UI names the unrecognized value. A later mapping can send something like `repitch` through the resampler with pitch following the ratio. That mapping waits until a real file shows the string.

### Memory

Resident audio is a window around the playhead, plus a hard cap on how many clips are being stretched. Nothing in the player expands a clip into a second full copy.

Tracktion splits readers the same way. Uncompressed audio is memory-mapped, so the OS pages in only the range that is touched. Compressed audio uses a block cache: fixed blocks, a per-file sample budget, and reuse of the oldest blocks (`AudioFileCache`). Their notes on compressed streaming say not to buffer a whole decoded file, and not to read compressed audio on the audio thread. This player uses that split, with one extra rule for ZIP: a deflated entry is not random-access, so native inflates it once to a temp file and then treats it as mapped PCM. That temp file is disk, not a second RAM copy.

What is allowed to occupy memory:

- Project model. XML is parsed into plain structs and then freed. Tempo, time signature, and automation stay as breakpoints, not per-sample tables. File paths are interned. Plug-in state, video, and unused entries are never inflated.
- Source bytes, by access pattern.
  - Native WAV, or a zip entry stored uncompressed: read sample ranges from the file. No private PCM copy.
  - Native deflated audio: inflate once to a temp file, then use the same range reads.
  - FLAC and MP3, and any deflated entry on WASM (there is no separate disk): a single process-wide block cache. Blocks are 16384 frames of int16, after downmix to the channels that clip actually plays (a stereo assignment keeps at most two). The default budget is 8 MiB, shared by every file and by playback and render. Blocks the stretch thread will read inside the current look-ahead are pinned. Unpinned blocks farthest from the playhead are reused. Clips that reference the same path share blocks. Each file has one decoder, owned by the thread that fills the cache, so two clips can sit at different positions without two decoded copies.
- Stretchers. An instance exists only while its clip overlaps the look-ahead. Ratio 1 and transpose 0 do not create one. Mono files are configured with one channel and duplicated at mix time. The instance is configured at the output sample rate, after any source-rate conversion into the cache, so a 96 kHz or 192 kHz file does not enlarge the FFT. `presetCheaper` at 48 kHz uses a 0.10 s block and a 0.04 s interval. That is on the order of a megabyte of internal state per stereo instance, and it scales with channel count and rate. The pool grows on first use up to 16 instances and is not freed until the project closes, so WASM does not churn a heap that cannot shrink. Each active instance has a 150 ms float FIFO. At 48 kHz stereo that is about 60 KB.
- Mix scratch. One float block per bus that is audible in this callback, reused. Clips do not get their own mix buffers.
- Render output. Written through as WAV. Not held.

The audio thread allocates nothing. Cache misses are filled on the stretch thread. If the budget is already pinned and a read still misses, that clip fades out for the block instead of growing the cache.

WASM holds the uploaded `.dawproject` for the life of the page. That copy is the floor. The player does not extract a second copy of each audio entry next to it. The UI reports the Emscripten heap, which is a high-water mark.

The screen shows five counters: source cache used and budget, stretcher pool bytes, FIFO bytes, project-model bytes, and process RSS (native) or Wasm heap (web). The cache budget is a setting, in mebibytes. Lowering it reuses blocks down to the new cap. It does not change the stretcher pool already created.

Simultaneous render uses the same source cache and its own stretcher pool, so the pool cost can double while a render is running. Sequential render touches a narrow forward window, which keeps the shared cache small if playback is stopped.

Signalsmith is stateful. A seek, a loop wrap, or the first block of a clip calls `seek` / `outputSeek` with a pre-roll (`seekLength()`, and `inputLatency()` so output lines up).

Per callback block, the local warp slope is `d(contentTime) / d(time)`. `process` gets that ratio. Pitch uses `setTransposeSemitones`.

### Threads

Native and WASM both use threads. The audio callback never stretches.

- Audio thread: mix FIFOs, fades, automation gains, pan, and hardware output routing. Lock-free reads only.
- Stretch thread: fill FIFOs ahead of the playhead. Seek and loop wraps happen here.
- Render thread: a second engine instance, synchronous stretch inside that instance, same preset as playback. The UI keeps running. Playback and render do not share stretchers. They do share the source block cache.

WASM uses Emscripten pthreads (`-pthread`, a pthread pool so `pthread_create` does not have to wait on the browser event loop). The audio callback is an AudioWorklet, which Emscripten runs as a Wasm Worker rather than a pthread, via miniaudio `MA_ENABLE_AUDIO_WORKLETS` (`-sAUDIO_WORKLET -sWASM_WORKERS`). The worklet only reads the FIFO. Stretch and render are pthreads. Shared memory needs `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` on the page. That header pair is the deployment cost. The mix code stays the same as native.

### Mixer and hardware outputs

Sum sounding clips into their destination channel, then walk `destination` links up to the master. Apply volume, pan, mute, and solo, including automation. Solo follows the usual rule: if any channel is soloed, only soloed channels are heard.

Pan is the normalized 0–1 parameter (0.5 is center) mapped with constant-power balance. Volume `unit` may be `linear`, `normalized`, or `decibel`; convert to a linear gain.

The project graph is separate from the sound card. `destination` never names an audio interface. The player adds its own assignment, stored in the session rather than in the `.dawproject`:

- Enumerate playback devices and their output channels with miniaudio.
- Each track and each bus (submix or master) can be assigned to a device output: a starting channel and a width (1 for mono, 2 for a stereo pair).
- Default: master goes to device outputs 1–2. Every other channel follows `destination` and does not touch the device directly.
- When the user assigns a channel to hardware, that post-fader signal is added to those device channels and is not also sent along `destination`. That keeps a stem on outputs 3–4 from doubling through the master.
- The device is opened with enough channels to cover the highest assignment. Sources with more channels than the assignment width are downmixed into that width.

### Fades

A fade that does not overlap another clip is linear amplitude, matching Tracktion's default fade type. A negative fade-in, which exists to overlap the previous clip, uses an equal-power sine on both sides, matching Tracktion `CrossfadeLevels`.

### Sample rates and files

Decode at the file rate. Stretch in that rate. Resample the clip output to the device rate with miniaudio when they differ.

Decoders: WAV, FLAC, MP3. Embedded files on every platform. `external="true"` on native only. WASM ignores external paths and reports them as missing.

### Stretch quality

One global preset, shared by playback and render. The user can switch it from the UI. The choices are Signalsmith `presetCheaper` and `presetDefault`. Default is `presetCheaper`. Switching resets active stretchers. Render started after the switch uses the new preset. A render already running keeps the preset it started with.

### Render

Same mix function as playback, on a render thread and a private engine instance.

miniaudio encodes WAV only. The user can choose, among the WAV settings miniaudio can write:

- PCM format: `f32`, `s32`, `s24`, `s16`, `u8`. Default `f32`.
- Sample rate. Default 48000.
- Layout: stereo master, or a multi-channel file whose channels follow the current hardware assignments (silent channels included so the indexes match the device).

Native writes a path. WASM offers the bytes as a download. Render does not follow the live device clock.

### UI

Native: a dependency-free ANSI screen, not ncurses.

- Transport state, position in seconds, bars, and beats, and the current tempo.
- Process CPU percent, the memory counters from the memory section, and audio callback load (callback time divided by the buffer duration).
- Track and bus names with mute, solo, and hardware output assignment.
- The stretch preset.

Keys: play/pause, stop, seek by a bar, jump to a bar, jump to a time in seconds, change a channel's hardware output, change the stretch preset, start render, quit.

WASM: the engine is a C API. The page is a small HTML shell with the same commands, the same meters, device output picks, and the stretch preset. The page must be served with the COOP and COEP headers above.

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

XML is parsed by a small reader in this repo, covering elements, attributes, and text for the subset above.

Build: CMake. Native executable plus an Emscripten target with pthreads and AudioWorklet.

## In scope

- Arrangement audio, including nested clips, loops, warp maps, fades, and negative fade-in overlaps.
- Tempo, time signature, volume, pan, mute, and clip gain / pan / transpose automation.
- Static and automated channel volume, pan, mute, solo, master, and submix routing via `destination`.
- Per track and per bus assignment to audio-interface outputs.
- Embedded WAV, FLAC, and MP3. External files on native only.
- One stretch preset for playback and render, switchable by the user.
- WAV render with selectable PCM format, sample rate, and stereo or multi-channel layout.
- Threads on native and on WASM.
- A capped source cache, a capped stretcher pool, and no full-file or full-clip audio copies.

## Out of scope

- Scenes and clip slots.
- Sends, VCA, and built-in EQ, compressor, gate, and limiter.
- Plug-ins and MIDI playback.
- Encoding anything other than WAV.
