# Contributing

## Build

ESP-IDF 5.5.x, target esp32s3.

```sh
cd firmware
./build.sh lite                  # or dual_throttle; output in build_<variant>/
./build.sh dual_throttle --flash # --port /dev/ttyACM0 to pick the port
```

Common settings live in `sdkconfig.defaults`, the variant's six lines in
`sdkconfig.defaults.<variant>`. After changing either, delete
`sdkconfig.<variant>` so the new defaults apply.

## Tests

```sh
cd firmware
cc -O1 -Wall -Wextra -o /tmp/t main/throttle_math.c test/test_throttle_math.c -lm && /tmp/t
cc -Wall -Wextra -Itest/stubs -o /tmp/s main/settings_rules.c test/test_settings.c && /tmp/s
```

CI runs both with ASan/UBSan, builds both variants and checks formatting.

## Style

`clang-format` (LLVM, 2-space, `.clang-format`) on hand-written code. Enable
the hook once: `git config core.hooksPath githooks`. Generated UI in
`firmware/main/ui_*` comes from EEZ Studio (`UI/eez_*`): change the EEZ
project, not the generated C.

## Pull requests

- One change per PR; don't mix refactors with behaviour changes.
- Anything touching throttle, brake, BLE or pairing must fail toward neutral
  and keep braking available. Describe how you tested it on hardware, with the
  wheels off the ground.
- `firmware/main/gb_protocol.h` is owned by gb_receiver; change it there.
- Releases: bump `firmware/main/version.h`, merge, run `firmware/release.sh`.
