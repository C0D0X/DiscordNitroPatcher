// Stub binding -- the in-process route is parked. The overlay runtime
// now lives inside dnp.exe instead of inside Discord's main process.
// This stub stays in place so shim_main.js / dnp_loader.js can still
// see a .node module if they look for one, but it does nothing.

#include <node_api.h>

extern "C" {

static napi_value module_init(napi_env env, napi_value exports) {
    return exports;
}

NAPI_MODULE(discord_voice_codec, module_init)

}
