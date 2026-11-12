# Resident Evil Revelations 2 – Dual Windows

> [!CAUTION]
> ## ⚠️ STILL IN DEVELOPMENT: EXPECT MANY BUGS ⚠️
> **This mod is NOT finished.** It is an early, experimental build and **it can have MANY bugs**:
> - **freezes** (especially on loading screens, after moving/resizing windows, or after minimizing);
> - **crashes**;
> - **input problems** (wrong controller on a player, mouse stops working, co-op not starting).
>
> Use it at your own risk. **Back up your saves** before playing. Please report every bug in [Issues](../../issues) with your log file.

## Full mouse + keyboard and controller support on separate screens

**Two people can play Resident Evil Revelations 2 local co-op at the same time: one on mouse + keyboard, the other on a controller, each on their own window or screen.**

| | Player A | Player B |
|---|---|---|
| Device | Mouse + keyboard | Controller |
| Screen | Display 1 or 2 (you choose) | The other display |
| Move, aim, shoot, melee | ✅ independent | ✅ independent |
| Pick up items, open doors, climb ladders | ✅ own device | ✅ own device |
| Button prompts | Keyboard keys | Controller buttons |

- Each device controls **only** the player on its own display.
- Each screen shows the **correct button prompts** for its device.
- **F1** swaps characters. Each person keeps their own device and their own screen.
- A **controls dashboard** at game start lets you drag devices onto Display 1 or Display 2. Your last layout is saved.

Download the ready-to-use mod from **[Releases](../../releases/latest)** or from **[Nexus Mods](https://www.nexusmods.com/residentevilrevelations2/mods/229)**. You don't need to compile anything.

> [!WARNING]
> **Known issue: the game may freeze when minimized.**
> Minimizing the game during loading screens or certain gameplay sections may make it stop responding, and you then have to close and restart it. **This is not fixed yet.**
> Avoid minimizing during loading screens and, when possible, during gameplay.
>
> **Help us fix it:** since v0.6.1 the mod saves where a frozen game got stuck. Wait about 15 seconds before closing the frozen game, then attach `scripts\ResidentEvilRevelations2.DualWindows.log` and `scripts\ResidentEvilRevelations2.DualWindows.hang1.dmp` to an [issue](../../issues).

## What it does

Revelations 2's local co-op squeezes both players into an uneven split screen. Dual Windows shows each player's view in its own 16:9 window. Each window can be resized, filled on a monitor, or moved to another monitor. This works well with two monitors, a TV, or playing with a friend over Parsec.

- One 16:9 window per player, each with its own size (1280×720 up to 3840×2160)
- Player HUD fitted to each window; subtitles and menus shared in both
- Cutscenes shown full-frame in both windows
- Chest lock puzzle and healing gauge use the centered single-player layout

## Requirements

- Steam version of the game (the installer checks the exact executable)
- [FusionFix](https://github.com/ThirteenAG/WidescreenFixesPack/tree/master/source/ResidentEvilRevelations2.FusionFix) by ThirteenAG (provides the ASI loader, `dinput8.dll`). The installer checks for the FusionFix build this mod was made with.
- Game in windowed mode at a 16:9 resolution (the installer sets windowed mode)
- XInput (Xbox-type) controller

## Install

1. Install FusionFix in the game folder, next to `rerev2.exe` (Steam: **Manage → Browse local files**).
2. Start the game once and quit normally, so it creates its settings file.
3. Download `RE2-DualWindows-v0.6.1.zip` from [Releases](../../releases/latest), extract it to its own folder and run **Install.cmd**.
4. Start the game through Steam. The controls dashboard opens first: drag **Mouse + keyboard** onto one display and your **controller** onto the other, then press **Start game**.

## Hotkeys (with the game active)

| Key | Action |
|---|---|
| F1 | Swap characters (devices stay on their screens) |
| F2 | Start co-op during a level |
| Ctrl+1 / Ctrl+2 | Fill or restore a player's window |
| Ctrl+Shift+1 / 2 | Cycle output sizes |
| Ctrl+Alt+1 / 2 | Move a window to the next monitor |
| F7 | Show the full game frame in both windows (if a cutscene looks wrong) |
| F8 | Hide or show the player windows |

## Uninstall

Close the game and run **Uninstall_DualWindows.cmd** (in the release ZIP, and in the game's `scripts` folder after install).

- It removes only Dual Windows files, never game files, FusionFix, saves or other mods.
- It asks before deleting your saved layout, logs or backups.
- It can restore your previous fullscreen setting, and it refuses to run while the game is open.

`Restore.cmd` only disables the mod, without deleting anything.

## Build from source

The mod is a 32-bit DLL (`.asi`) loaded by FusionFix's ASI loader.

**Option A: Zig (no Visual Studio needed).** Install [Zig 0.13.0](https://ziglang.org/download/), then:

```bat
cd source
build-zig.cmd "C:\path\to\zig.exe"
```

**Option B: Visual Studio 2022 or newer (C++ desktop workload).** Open `source\build.cmd`, fix the `vcvars32.bat` path if your Visual Studio is in another folder, then run it.

Both produce:
- `ResidentEvilRevelations2.DualWindows.asi`: the mod. Copy it to `<game>\scripts\`, or put it next to `Install.ps1` and run `Install.cmd`.
- `DualWindowsTest.exe`: an offline self-test. Run it from `source`; exit code 0 means PASS. It checks the input routing rules (both dashboard layouts, F1 swaps) and the window/graphics pipeline with a dummy Direct3D 9 device. It does not start the game.

The only third-party code is [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause), included in `source/minhook`.

## How it works (short tour)

| File | What it does |
|---|---|
| `source/DualWindows.cpp` | Entry point. Direct3D 9 hooks that copy each player's viewport into its own window, window management and hotkeys, F1 swap, and per-player input mode hooks (movement, actions, interactions, button prompts) |
| `source/InputSetup.h` | The controls dashboard (plain Win32/GDI), the saved layout, and XInput routing: which physical controller feeds which in-game player, based on what each display shows |
| `source/HudFix.h` | Fits the player HUD to each 16:9 window |
| `source/SharedGui.h` | Draws shared menus and subtitles into both windows |
| `source/MovieFrames.h` | Shows prerecorded cutscenes full-frame |
| `source/Diagnostics.h` | Startup/monitor logging and the freeze watchdog (logs the stuck thread's stack and writes a small dump) |
| `source/DualWindowsTest.cpp` | Offline test harness |

The game itself only lets mouse + keyboard drive one character. The mod switches the game's input mode per player while that player's input, actions and button icons are processed. Mouse + keyboard is active only for the player on its display, and controllers are routed to the player shown on theirs. Comments in the source name the game functions involved, by address in the Steam executable.

The source contains no game code or game assets. You need your own copy of the game.

## Contributing

- **Bugs:** open an [issue](../../issues). Attach `scripts\ResidentEvilRevelations2.DualWindows.log` (and any `.hang1.dmp` file) from the game folder and say which dashboard layout you used.
- **Pull requests** are welcome. Please run `DualWindowsTest.exe` before submitting, and keep changes small and focused.

## Credits

- **Manerodawd** ([Nexus profile](https://www.nexusmods.com/profile/Manerodawd)): co-author, made about 70% of this project
- **maiki797**: co-author
- **ThirteenAG**: FusionFix and Ultimate ASI Loader
- **Tsuda Kageyu**: MinHook

## License

The mod's own code is released under the [MIT License](LICENSE). MinHook keeps its own BSD 2-Clause license ([source/minhook/LICENSE.txt](source/minhook/LICENSE.txt)). See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Resident Evil Revelations 2 is a trademark of Capcom. This is an unofficial fan project, not affiliated with or endorsed by Capcom.
