# DiscordNitroPatcher

Small Windows tool that patches the local Discord client so screenshare / Go Live
runs at higher quality (up to 1080p60, ~25 Mbps) without a Nitro subscription.

It works by editing Discord's `app.asar` to load a small JavaScript file that
raises the video bitrate and framerate caps and flips the client-side premium
flag. Everything happens locally, nothing is sent anywhere.

## How it works

Discord ships its app code in `resources/app.asar`. The patcher unpacks that,
adds a loader script, and repacks it. A wrapper is put on the Discord shortcut
and the startup Run key so the patch gets re-applied automatically after Discord
updates itself (Discord overwrites the asar on every update).

The whole thing is one `dnp.exe`. There is no background process.

## Build

Needs Visual Studio Build Tools (C++ / MSVC). Then just run:

```
build.bat
```

Output is `build\dnp.exe`.

## Usage

Run `dnp.exe`. The control panel shows the current state and has buttons to:

- Install / apply the patch and launch Discord
- Reapply the patch (after a Discord update)
- Open the log
- Uninstall

Uninstall closes Discord, restores the original `app.asar`, and removes the
shortcut and registry changes.

## Notes

- Only tested on Windows 10/11 with the stable Discord build.
- This changes quality limits on your own machine. Whether other people can
  actually receive the higher quality still depends on Discord's servers.
- Use at your own risk. Modifying the client is against Discord's ToS, so don't
  do anything dumb with it.
