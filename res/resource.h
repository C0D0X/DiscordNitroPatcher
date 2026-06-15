// resource.h — IDs for embedded payload resources.
#pragma once

#define IDR_DNP_LOADER     101
#define IDR_SHIM_MAIN      102
#define IDR_SHIM_RENDERER  103
// Optional native runtime. Only present in the binary when
// res/embedded/discord_voice_codec.node was staged by addon/build_addon.bat
// before the resource compile. dnp.rc gates the embed on file existence.
#define IDR_RC_ADDON       104
