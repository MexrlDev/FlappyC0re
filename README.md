# FlappyC0re
* [The Big One!](https://youtu.be/VHOCPpjUtD8?si=mlzagng2nm-eBmUX)

---
## Readme
- Require USB for saving (optional)
- Headphones can be used to switch audio to it automatically
- You can exit the payload then resend it and enter the game normally with all your save options and score still saved because the game is deigned to handle it.

---
## Controls

| Button | Function |
| ------ | -------- |
| Cross (X) | Jump (In Game) Select/Resume (Menu/Pause) |
| Circle (O) | Back (In Game Start Only) Back (Menu/Pause) |
| D-Pad | Navigation (menu) / Left Right for SFX to turn volume down and up by 5% each |
| Options button | Pause/Resume (In Game) |

---
## Menu Options
START GAME
DIFFICULTY: EASY / NORMAL / HARD / RACER
BACKGROUND: DAY / NIGHT
VIBRATION: ON / OFF
SFX: ON / 95% / 90% ... 5% / OFF
SCREEN: FULL / 16:9 / 4:3
RESET SCORE (current high score shown)
SAVE: OK / NO SAVEDATA
RESET SETTINGS
CREDITS
EXIT

---
## Setup
1. Get the release zip, or copy the lua/py from the source code from this repo
2. Make sure to edit the python to add your PlayStation or luac0re IP address so payload gets send
2.1 In Pythonica/PyCode for mobile go to SetUp for mobile
3. for pc just start it via going to Python and starting it via shellcode Python3 flappy_launcher.py

---
## How to build it
1. Use YML to build from actions
2. and finally wait till the build completed and download the zip file called `FlappyC0re.zip`

---
## Credits

Game logic ported from [MexrlDev's JS Flappy Bird](https://github.com/MexrlDev/PsVue-Mod/tree/main/Flappy%20Bird). - RIP VUE

Sprites and audio are from
[samuelcust/flappy-bird-assets](https://github.com/samuelcust/flappy-bird-assets)
by Samuel Custodio, MIT licensed. Full notice in `assets/Credits.txt`.

The platform layer follows the pattern from **Doom-PS** (MIT, by MexrlDev);
the Flappy Bird implementation here is written fresh for this port.

Structural ideas for the PS4/PS5 platform layer — the `native_call` trampoline
shape, the `.ps_persist` section trick, and the streamed-shellcode launcher
design — are adapted from **DooMC0re** (GPL-2.0, by EgyDevTeam /
egycnq). See `NOTICE` for details. No DooMC0re source code is copied into
this repository; only the general architecture is reused.

LuaC0re by Gezine. The 8×8 bitmap font is a small public-domain-style
glyph table; the version here was typed out fresh for this port.

## License

Everything in this repository — source code, build scripts, linker
script, Lua launcher, Python launcher, and bundled assets — is
released under the **MIT License**. See `LICENSE`.

    Flappy Bird Luac0re PS4/PS5 port
    Copyright (c) 2026 MexrlDev

    Flappy Bird sprites and audio
    Copyright (c) 2019 Samuel Custodio

Architectural inspiration was drawn from DooMC0re (GPL-2.0),
but no DooMC0re source code is present in this tree. See `NOTICE`.
