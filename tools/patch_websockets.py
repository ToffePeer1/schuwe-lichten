"""PlatformIO pre-build script: fix the WebSocketsServer read timeout on Arduino core 2.x.

links2004/WebSockets 2.7.3 calls client->tcp->setTimeout(WEBSOCKETS_TCP_TIMEOUT) (5000) for new server
clients, but on arduino-esp32 2.x WiFiClient::setTimeout() takes SECONDS. A client that sends half a
handshake line and then vanishes would block loop() for 5000 s (no web or serial control; DMX keeps
running). The library already handles this for its client side (NETWORK_SET_TIMEOUT_IN_SECONDS); this
script applies the same fix to the server. Idempotent; fails the build if the library changed.
"""

from pathlib import Path

Import("env")  # noqa: F821 (provided by PlatformIO/SCons)

MARKER = "// dmx-controller patch: setTimeout takes seconds on core 2.x"
ORIGINAL = "            client->tcp->setTimeout(WEBSOCKETS_TCP_TIMEOUT);\n"
PATCHED = (
    f"            {MARKER}\n"
    "#if defined(NETWORK_SET_TIMEOUT_IN_SECONDS)\n"
    "            client->tcp->setTimeout(WEBSOCKETS_TCP_TIMEOUT / 1000);\n"
    "#else\n"
    "            client->tcp->setTimeout(WEBSOCKETS_TCP_TIMEOUT);\n"
    "#endif\n"
)

libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV")  # noqa: F821
target = libdeps / "WebSockets" / "src" / "WebSocketsServer.cpp"

if target.exists():
    source = target.read_text(encoding="utf-8")
    if MARKER not in source:
        if source.count(ORIGINAL) != 1:
            raise RuntimeError(f"patch_websockets.py: expected line not found once in {target}; re-check the fix")
        target.write_text(source.replace(ORIGINAL, PATCHED), encoding="utf-8")
        print("patch_websockets.py: patched WebSocketsServer read timeout (seconds on core 2.x)")
