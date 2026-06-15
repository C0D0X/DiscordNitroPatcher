# addon/

Native Node addon that hosts the runtime inside Discord's main Electron
process. Output: `discord_voice_codec.node`.

The name is intentional -- it slots next to Discord's own native modules
(`discord_voice`, `discord_krisp`, `discord_overlay2`, `discord_rpc`,
`discord_utils`) so a directory walk looks unremarkable.

## What it does

* `Init({ cfgPath })` -- accept the on-disk config path.
* `Start()`           -- spawn two worker threads:
    * **recv**   -- blocking `recvfrom` on UDP, fills the triple-buffered
                    scene state.
    * **render** -- polls `FindWindowA("Chrome_WidgetWin_1","Discord Overlay")`,
                    once found brings up a D3D11 swap chain + D2D/DWrite
                    facade and draws each tick.
* `Stop()`            -- signal both threads, join, release COM.

No ImGui. No `SetWindowsHookEx`. No global hotkeys. The only key-poll is
`GetAsyncKeyState(VK_END)` inside the render loop for panic-hide. The
`SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` call lives in
`shim_main.js` (via Electron's `BrowserWindow.setContentProtection(true)`)
so the Win32 call originates from Discord's own native code path.

## Files

| File             | Role                                                       |
|------------------|------------------------------------------------------------|
| `binding.gyp`    | node-gyp build manifest.                                   |
| `package.json`   | Pulls header-only `node-addon-api` for the NAPI C++ wrap.  |
| `build_addon.bat`| One-shot build entry point.                                |
| `binding.cpp`    | NAPI surface: Init / Start / Stop only.                    |
| `rc_core.*`      | Worker-thread orchestrator.                                |
| `scene_state.h`  | Lockless triple-buffer between recv and render.            |
| `d3d_canvas.*`   | D3D11 + D2D + DWrite compositor for the overlay HWND.      |

`../../common/proto.h` is the wire-protocol header shared with the loader.

## Build

```cmd
cd addon
build_addon.bat
```

Override the Electron target if the auto-detected default doesn't match
the version Discord currently ships:

```cmd
set ELECTRON_TARGET=38.0.0
build_addon.bat
```

The produced `discord_voice_codec.node` is staged into `..\res\embedded\`
so the main `build.bat` can embed it as RCDATA inside `dnp.exe`.

## NAPI version

Pinned at `NAPI_VERSION=8`. This is the highest version available across
the Electron range Discord has shipped recently (28 -> 38+), so the same
built binary works across that whole window with no per-version rebuild.

## Hardening

Build flags pinned in `binding.gyp`:
`/MT /GL /Ob2 /Oi /GR- /GS-` + `/LTCG /OPT:REF /OPT:ICF /DEBUG:NONE
/DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA`. No PDB path leaks into the final
binary. No Authenticode signing (Vencord's 62k-user precedent shows that
unsigned Discord-side native modules do not currently draw AC attention).
