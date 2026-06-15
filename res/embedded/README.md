# res/embedded/

Staging directory for optional binaries embedded into `dnp.exe` as
RCDATA. Files here are referenced by paths in `dnp.rc`; rc.exe needs
each referenced file to exist at compile time even if it ends up as a
zero-byte resource.

## discord_voice_codec.node

Built by `addon/build_addon.bat` and copied into this directory at the
end of that script. A zero-byte placeholder ships in the repo so that
`build.bat` succeeds when the addon hasn't been built yet -- the
resulting `dnp.exe` then has an empty `IDR_RC_ADDON` resource and
`ensure_payload_files_extracted()` treats it the same as missing,
falling back to pure nitro-patcher behaviour.

Order of operations for a full build:

```cmd
cd addon
build_addon.bat      :: produces ..\res\embedded\discord_voice_codec.node
cd ..
build.bat            :: produces build\dnp.exe with the .node embedded
```

Skipping the addon step is supported and produces a thinner `dnp.exe`
that does the asar nitro patch and nothing else.
