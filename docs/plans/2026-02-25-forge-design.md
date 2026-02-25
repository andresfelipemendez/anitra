# Forge — Separate Builder/Watcher Process

> Status: Implemented (2026-02-25)

## Goal

Extract the file watching and compilation responsibilities from the engine (core.cpp) into a `build.exe watch` mode. The engine should never touch the compiler. Two processes running side by side: forge builds, engine runs.

## Current State

`core.cpp` does everything: watches `src/` via `ReadDirectoryChangesW`, calls TCC to compile `engine.dll`, copies the DLL, and reloads it. The engine IS the builder. This violates the architecture described in BUILD.md and HOTRELOAD.md.

## Desired State

```
Terminal 1:  build.exe watch      <- forge: watches src/, compiles DLL
Terminal 2:  AnitraEngine.exe     <- engine: waits for signal, loads DLL
```

## Architecture

### Forge (build.c `watch` target)

A persistent process that:

1. Watches `src/engine/` for `.c` and `.h` file changes via `ReadDirectoryChangesW`
2. On change: debounce 50ms, then TCC compile `engine.dll`
3. On success: copy `engine.dll` -> `engine_copy.dll`, then `SetEvent(HOTRELOAD_EVENT_NAME)`
4. On failure: print error to terminal, do NOT overwrite DLL, keep watching
5. Loop forever

The forge process owns:
- Source file watching (ReadDirectoryChangesW)
- TCC compilation command
- DLL copy (engine.dll -> engine_copy.dll)
- Named event signaling

### Engine (core.cpp simplified)

The engine's hot-reload path becomes:

1. On startup: wait for named event, load `engine_copy.dll`
2. In game loop: check if reload flag is set
3. On reload: unload old DLL, load `engine_copy.dll`, reinit

The engine does NOT:
- Know where source files are
- Invoke the compiler
- Watch source directories
- Copy the DLL

### Communication Protocol

```
Forge                                Engine
  |                                    |
  +- detect file change                |
  +- debounce 50ms                     |
  +- TCC compile -> engine.dll         |
  +- copy engine.dll -> engine_copy.dll|
  +- SetEvent(HOTRELOAD_EVENT_NAME) -->+- WaitForSingleObject
  |                                    +- unload old DLL
  |                                    +- load engine_copy.dll
  |                                    +- call init_engine()
  |                                    +- resume game loop
```

### Error Handling

- Compile failure: forge prints TCC stderr, does NOT touch engine.dll or engine_copy.dll. Engine keeps running old code.
- Forge not running: engine starts without forge, loads whatever engine_copy.dll exists on disk. No crash if forge is absent.
- Engine not running: forge compiles happily, DLL sits on disk waiting.

## What Gets Removed from core.cpp

- `compile_dll()` function
- `watch_src_directory()` function
- `begin_watch_src_directory()` function
- `<filesystem>`, `<iostream>`, `<thread>` includes
- The source-watching thread

## What Gets Added to build.c

- `watch` target in main() dispatch
- `watch_and_rebuild()` function: ReadDirectoryChangesW loop + debounce + compile + copy + signal
- Reuses existing `find_msvc_tools()` for TCC path detection
- HOTRELOAD_EVENT_NAME constant (shared with core.cpp via a header or hardcoded string)

## What Stays in core.cpp

- Named event listener thread (WaitForSingleObject)
- DLL loading (loadlibrary/getfunction)
- Function pointer assignment
- Game loop with reload check
- Tracy integration
