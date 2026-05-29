// Discord launcher wrapper - rewrite .lnk + Run registry to point to dnp.exe --launch
#pragma once

#include <string>

namespace dnp {

int wrap_all_discord_launchers(const std::wstring& dnp_exe);
int unwrap_all_discord_launchers();

} // namespace dnp
