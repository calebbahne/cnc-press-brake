This file defines how Codex should read, create, and modify Arduino sketches in this project.

## Sketch layout

- Each Arduino sketch is kept in its own folder.
- The main `.ino` file must have the same name as its containing folder. For example:

  ```text
  Testing/
    hello_world/
      hello_world.ino
  ```

- Place new, focused temporary hardware tests under `Testing/<test_name>/<test_name>.ino`.
- Keep the production firmware separate from individual tests when it is introduced.
- Do not rename or move a sketch folder without renaming its main `.ino` file to match.

## How to read an `.ino` sketch

Arduino preprocesses `.ino` files before compiling them as C++. Read the sketch in this order:

1. Libraries and constants: `#include`, pin assignments, timing values, and configuration.
2. Global state: position counters, homing state, serial buffers, and fault flags.
3. `setup()`: runs once after power-up/reset; initializes pins, serial communication, and safe startup state.
4. `loop()`: runs continuously; should service serial commands, limits, motion, and status without unnecessary blocking.
5. Helper functions: motion, homing, command parsing, and safety checks.

`delay()` is acceptable in small isolated bring-up tests such as `hello_world.ino`. Avoid it in motion-control firmware, because it prevents responsive serial handling and limit-switch monitoring.

## Serial protocol direction

The React UI is responsible for operator interaction. The ESP is responsible for executing accepted commands, real-time motion, homing, limit monitoring, and safety behavior. Once the firmware accepts a command, it must continue safely even if the UI becomes unresponsive.

When defining commands, use newline-delimited text commands with explicit acknowledgements, errors, and status messages unless the project later adopts a documented alternative protocol.

## Change workflow

Before modifying a sketch:

1. Read the entire target sketch and any referenced project documentation.
2. Identify the board, pins, driver enable polarity, limit wiring, and expected safe state.
3. Keep a test narrowly scoped: one hardware behavior per test sketch where possible.
4. Include clear serial output for setup, inputs, commanded actions, and faults.
5. Review changes for blocking calls, uncontrolled motion, and missing limit checks.
