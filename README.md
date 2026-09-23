## Credits

Game logic ported from [MexrlDev's JS Flappy Bird](https://github.com/MexrlDev/PsVue-Mod/tree/main/Flappy%20Bird).

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
