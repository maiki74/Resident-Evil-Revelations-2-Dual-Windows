# Changelog

## v0.6.2: co-op input fixes, F3 mouse sensitivity, freeze fixes

### Highlights
- **Each player's menus now take only that player's devices.** Mouse + keyboard no longer moves or uses items in the controller player's inventory.
- **The mouse cursor reaches the whole inventory and the key-binding menu** (before, only a small area on the left worked).
- **One player aiming no longer blocks the other player's inventory and healing.**
- **New F3 panel: mouse camera sensitivity** (0.10x to 3.00x, steps of 0.01, keyboard only).
- **The herb (healing item) count no longer sits on top of the weapon box.**
- **Several causes of loading-screen freezes fixed** (found in freeze dumps).

### Co-op input
- Fixed: the mouse + keyboard player's keys and mouse clicks also moved, selected and used items in the controller player's inventory. Now each player's menus (inventory, key items, item pickup, weapon select, map, pause menu, weapon upgrade, puzzles) take only that player's own devices. The mouse + keyboard player keeps moving and aiming while the other player's inventory is open. Follows F1.
- Fixed: the mouse cursor in the inventory and the key-binding menu only reached a small area on the left, and the first key-binding rows could not be clicked. The game read the cursor through its own hidden window, which does not match the player windows. The cursor now follows the mouse + keyboard player's window: every point of that window reaches the same point of the menu. Follows F1.
- Fixed: while one player aimed, the other player could not open the inventory (and so could not heal from it). The game checked the FIRST character's aim instead of the player pressing the button. Now each player's own aim/state decides. All other game rules stay.
- Fixed: when the controller player opened their inventory, the mouse player lost the camera and a mouse cursor appeared. The game now checks the mouse + keyboard player's own menus (follows F1).
- Fixed: a controller placed on the same display as mouse + keyboard did nothing. Now that player switches freely: press anything on the controller to use it, touch the mouse or keyboard to go back. Button prompts, menus and interactions follow. The other display is never affected.
- Note: a pause menu opened by the controller player answers only that player's controller.

### F3: mouse camera sensitivity
- **F3** opens a small panel on the mouse + keyboard player's screen.
- **Left/Right arrows or A/D** change the sensitivity by **0.01** per press (hold to repeat). **R** resets to 1.00. **Esc** or **F3** closes it.
- Range 0.10x to 3.00x. Applies at once. Saved as `MouseCameraSensitivity` in the `[Mouse]` section of `scripts\ResidentEvilRevelations2.DualWindows.ini`.
- It changes only the mouse camera and aiming (not the menu cursor, not controllers).
- The panel is keyboard only: it never takes the focus or the mouse from the game. While it is open the mouse + keyboard player's camera and keys are held; the controller player keeps playing.
- Works only when Mouse + keyboard is placed on a display in the controls dashboard.

### HUD
- Fixed: in co-op the herb (healing item) count sat on top of the weapon and sub-item box. It is now drawn a little higher in each player window. Nothing else on the HUD moves.

### Freezes and startup
- Fixed (cause found in a hang dump): after Ctrl+1/2, Ctrl+Alt+1/2, minimizing or Alt+Tab, the game repainted from its main thread during loading screens while its render thread kept drawing, and the two blocked each other inside Direct3D. The game's repaint draw is now skipped while the player windows are shown, and the player windows are drawn by only one thread at a time.
- Each player window now keeps ONE Direct3D swap chain for the whole game. Resizing, filling, moving or minimizing no longer destroys or recreates anything. Recreating them preceded every loading-screen freeze and an NVIDIA driver crash in the dumps.
- The player windows are presented past the Steam and Discord overlay hooks (the overlays still work on the game itself).
- Fixed: "ERR09 : Unsupported function" at startup when the game's resolution is taller than the desktop (for example 3840x2160 on 1920x1080 monitors). The graphics device now starts at the configured resolution.

### Diagnostics and tests
- The freeze watchdog also catches a stalled render thread (no frame for 15 s).
- The log shows: every Start press and the game's pad slots; every Direct3D error with its code; failed device resets with all settings; each menu's owner and which devices it takes; menu cursor mapping; inventory button decisions; aim changes; quick heal presses; F3 panel changes.
- New offline tests: concurrent presents while resizing, no swap-chain recreation, menu cursor corners and edges, sensitivity scaling and 0.01 steps, inventory gate choice.

### Tested
- In-game: per-player menus, menu cursor, key-binding menu, aiming vs inventory/healing.
- Offline only (not yet confirmed in-game): the herb count position and the keyboard-only F3 panel.
- The minimize freeze is not confirmed fixed yet. If the game still freezes, wait about 15 seconds, then send the log and the `.hang1.dmp` file.
## v0.6.1: freeze diagnostics (two monitors and minimize)

- New: the log now records every monitor and graphics adapter, which monitor the game and each player window use, swap-chain and device-reset results, and the last startup stage reached.
- New: freeze watchdog. If a game window stops responding, the mod logs the stuck thread's call stack and saves one small dump file (`ResidentEvilRevelations2.DualWindows.hang1.dmp`) next to the log. It only watches; it never touches the game's windows.
- Tested: started with two monitors (1920x1080 each), one player window on each monitor, no freeze in that run.
- Known issue (not fixed): the game may still freeze when minimized, and some earlier two-monitor starts froze on the first loading screen. The cause is not confirmed yet. If it happens, please send the log and the .dmp file.
- No gameplay, input or window behavior changed from v0.6.0.

## v0.6.0: MAJOR UPDATE, independent mouse + keyboard and controller for dual-screen co-op

### Highlight
- **One player on mouse + keyboard, the other on a controller, at the same time, each on their own screen.** Each device now controls only the player on its own display.
- Both players can move, aim, attack, pick up items, open doors, climb ladders and interact with their own device.
- Each screen shows the correct button prompts for its device.

### New
- **Controls dashboard** at game start: drag mouse + keyboard and controllers onto Display 1 or Display 2.
- Your last layout is saved. Press Start game with nothing placed to reuse it.
- Controllers show their real names and light up when pressed.
- **Uninstall_DualWindows.cmd**: removes only DualWindows files and asks before deleting settings, logs or backups.

### Fixed
- F1 character swap now keeps each person on their own device and screen (before, F1 could leave the controller without a character).
- Pickups, doors and ladders now respond to the correct player's device.
- Button prompts no longer show keyboard keys on the controller player's screen.
- The "Display 1" window now opens on Windows display 1 with two monitors.

### Known issue (not fixed)
- **The game may freeze if minimized** during loading screens or some gameplay sections. Avoid minimizing. If it stops responding, close and restart the game.

## v0.5.0
- Healing gauge centered in both player windows (both of the game's healing gauge screens).

## v0.4.x
- One 16:9 window per player, HUD fitting, shared subtitles and menus, full-frame cutscenes, chest puzzle fix, F1 character swap.
