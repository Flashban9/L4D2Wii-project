# UNDEAD RUSH - a low poly zombie shooter for the Wii

Fight your way down a street full of zombies and reach the safe room at the end.
Created by Flashban9. An original game, inspired by co-op zombie horde shooters.

## Controls (Wiimote + Nunchuk)
| Control | Action |
|---|---|
| Nunchuk stick | Move |
| Wiimote pointer | Aim (pointing at the screen edge turns the camera) |
| B | Fire |
| A | Reload |
| C | Sprint |
| Z / D-pad up | Next weapon |
| D-pad down | Previous weapon |
| HOME | Back to the Wii menu |

Without a Nunchuk the D-pad moves you (hold the Wiimote sideways), 1 = sprint, +/- = switch weapon.

## Gameplay
- Idle zombies wake up when you get close or when you shoot. Triggers further down the street unleash hordes.
- Three zombie types: walker, fast runner and the heavy brute.
- Pick up the SMG and the shotgun, ammo crates and medkits on your way.
- Reach the glowing safe room at the end of the street to win.

## Install
Copy the folder `apps/undeadrush` to `SD:/apps/undeadrush`, then start it from the Homebrew Channel.

## Build
- **GitHub Actions:** upload this repository to GitHub. The workflow builds `boot.dol` and packages the ready-to-copy
  SD card folder (*Actions > latest run > Artifacts > UndeadRush-Wii*).
- **Docker:** `docker run --rm -v "$PWD":/project -w /project devkitpro/devkitppc make install`
- **devkitPro:** install devkitPPC + libogc, then `make install`.

## Project layout
- `source/game.c`, `source/game.h` - platform independent game logic (level, enemies, weapons, path finding)
- `source/main.c` - Wii code (GX rendering, HUD, input, synthesised sound effects)
- `test/host_sim.c` - headless test with a bot, runs on a PC: `gcc -O2 -o host_sim test/host_sim.c source/game.c -lm && ./host_sim 10 0`

No game assets are needed: all graphics are low poly shapes drawn in code and all sounds are generated at start-up.
