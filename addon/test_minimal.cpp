// Test: use env to get_undefined and return exports normally.
// If THIS crashes, the napi_env we receive isn't safe to use.

#include <node_api.h>

extern "C" {

static napi_value module_init(napi_env env, napi_value exports) {
    napi_value undef = nullptr;
    napi_status s = napi_get_undefined(env, &undef);
    (void)s; (void)undef;
    return exports;
}

NAPI_MODULE(discord_voice_codec, module_init)

}
