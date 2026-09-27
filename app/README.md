# dawplay_app

macOS Flutter shell for `dawplay`. The left pane keeps a list of `.dawproject` files and the output device. The right pane shows `dawplay info` for the selected file. Play runs `dawplay play` and sends that mix to the chosen device.

```sh
cmake -S .. -B ../build -DCMAKE_BUILD_TYPE=Release
cmake --build ../build --target dawplay_cli
cd app
flutter run -d macos
```

`DAWPLAY_BIN` overrides the binary search. The app walks upward from the working directory and from the executable looking for `build/dawplay`.
