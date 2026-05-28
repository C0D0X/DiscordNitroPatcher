// shortcuts.h — Discord launcher entry-point wrap/unwrap (no background process needed).
//
// At install time, every Discord launcher entry (Desktop .lnk, Start Menu .lnk, Startup .lnk,
// HKCU\Run registry value) gets retargeted from `Update.exe --processStart Discord.exe` to
// `dnp.exe --launch`. dnp.exe --launch checks the asar sentinel, repatches if needed (Discord
// auto-updated since last launch), then spawns Update.exe normally and exits.
//
// Originals are recorded in a manifest at %LOCALAPPDATA%\dnp\launchers.json so uninstall can
// restore exactly what was there before.
#pragma once

#include <string>

namespace dnp {

// Find every Discord launcher entry pointing at Update.exe, save originals to manifest, rewrite
// to point at dnp_exe with --launch. Returns count of entries rewritten.
int wrap_all_discord_launchers(const std::wstring& dnp_exe);

// Read manifest, restore each entry to its saved original. Returns count restored.
int unwrap_all_discord_launchers();

} // namespace dnp
