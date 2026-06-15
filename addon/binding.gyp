{
  # node-gyp build config for the Discord-side runtime addon.
  #
  # Output: discord_voice_codec.node — a Win32 DLL named with .node extension
  # so Electron's require() loads it via process.dlopen(). Filename is chosen
  # to blend with Discord's own native modules (discord_voice, discord_krisp,
  # discord_overlay2, discord_rpc, discord_utils). When dropped into Discord's
  # modules/ subdir, it looks like one more first-party Discord binary.
  #
  # Build:
  #   1) npm install              (pulls node-addon-api headers)
  #   2) build_addon.bat          (configure + build against Electron headers)
  #
  # The Electron version is detected at build time from the installed Discord
  # app dir; see build_addon.bat. NAPI v8 was chosen so the same .node binary
  # runs across the Electron range Discord ships (28..38+) without rebuild.

  "targets": [{
    "target_name": "discord_voice_codec",

    "sources": [
      "binding.cpp",
      "rc_core.cpp",
      "d3d_canvas.cpp"
    ],

    "include_dirs": [
      # node-addon-api header-only wrapper around node_api.h.
      "<!@(node -p \"require('node-addon-api').include\")",
      # Shared wire protocol (proto::Header / proto::Scene / ...).
      "../../common"
    ],

    "defines": [
      # Pin NAPI surface to v8 — covers Node 12+ / Electron 14+. A single
      # built binary then runs across every Discord Electron release in the
      # foreseeable window without ABI rebuild.
      "NAPI_VERSION=8",
      "NAPI_CPP_EXCEPTIONS",
      # Quiet the standard "deprecated POSIX names" noise from winsock2.
      "_CRT_SECURE_NO_WARNINGS",
      # Reduce <windows.h> surface (no GDI fluff, no MFC noise).
      "WIN32_LEAN_AND_MEAN",
      "NOMINMAX"
    ],

    # Win32 user-mode libraries. All standard imports — nothing exotic on
    # the import table, which keeps `dumpbin /imports` audit clean.
    "libraries": [
      "-ld3d11.lib",     # ID3D11Device, swap chain
      "-ldxgi.lib",      # DXGI surface QueryInterface for D2D interop
      "-ld2d1.lib",      # Direct2D primitives (lines, ellipses, rects)
      "-ldwrite.lib",    # DirectWrite text rendering
      "-lws2_32.lib",    # Winsock2 UDP socket
      "-luser32.lib",    # FindWindowA, GetAsyncKeyState
      "-lkernel32.lib"   # threads, atomics, time
    ],

    "msvs_settings": {
      "VCCLCompilerTool": {
        # /MT — static CRT, no extra DLL imports.
        "RuntimeLibrary": 0,
        # /GL — whole-program optimization, pairs with /LTCG below.
        "WholeProgramOptimization": "true",
        "ExceptionHandling": 1,
        "BufferSecurityCheck": "false",
        "AdditionalOptions": [
          "/std:c++17",
          "/utf-8",
          "/O2",
          "/Ob2",
          "/Oi",
          "/GR-",
          "/W3"
        ]
      },
      "VCLinkerTool": {
        # /LTCG /OPT:REF /OPT:ICF — squeeze unreferenced code, fold
        # duplicate COMDATs. Smaller binary, smaller signature surface.
        "LinkTimeCodeGeneration": 1,
        "OptimizeReferences": 2,
        "EnableCOMDATFolding": 2,
        # /DEBUG:NONE in release — never emit PDB path into the headers.
        "GenerateDebugInformation": "false",
        "RandomizedBaseAddress": 2,        # /DYNAMICBASE
        "DataExecutionPrevention": 2,      # /NXCOMPAT
        "ImageHasSafeExceptionHandlers": "true",
        "AdditionalOptions": [
          "/HIGHENTROPYVA"
        ]
      }
    }
  }]
}
