# dawproject_player

A small C++ player for arrangement audio in `.dawproject` files. It plays through miniaudio and time-stretches with Signalsmith Stretch. Plug-ins, MIDI, sends, and the clip launcher are out of scope. See `docs/SPEC.md`.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/dawplay info song.dawproject
./build/dawplay song.dawproject
./build/dawplay render song.dawproject out.wav --rate 48000 --format f32 --layout stereo
./build/dawplay_tests
```

PCM formats are `f32`, `s32`, `s24`, `s16`, and `u8`. `--layout multi` writes one file channel per assigned hardware output.

Terminal keys: space play/pause, `s` stop, left/right by a bar, `g` jump to a bar, `t` jump to seconds, up/down select a track, `o` cycle that track's hardware output, `p` stretch preset, `d` playback device, `[` `]` cache budget, `f` render format, `l` stereo or multi, `r` render `render.wav`, `q` quit.

## Web

The page needs `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`.

```sh
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web
python3 web/serve.py --directory build-web
```

`web/serve.py` sets those headers. The build copies `index.html` next to `dawplay.js`.
