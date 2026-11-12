# Changelog

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
