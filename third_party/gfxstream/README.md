# gfxstream protocol specs

The `.in`, `.attrib`, `.types` and `.addon` files under `gles1/`, `gles2/` and `renderControl/` are copied unchanged from
gfxstream's `codegen/` directory:

- Source: https://android.googlesource.com/platform/hardware/google/gfxstream
- Commit: d047a57228332d995d36600792fa9ccc26cf8ae6 (2025-03-26)
- License: Apache-2.0 (`LICENSE`, copied from the same commit; the project is marked `MODULE_LICENSE_APACHE2`)

`tools/gen-glproto.py` generates the engine's wire decoders from them. This upstream copy is newer than the
Android 13 encoders in the guest: it appends 2 renderControl calls and 12 GLES2 calls at the ends of the lists, and
every opcode the guest uses is unchanged (`tools/fetch-remote-protocols.sh` checks all 788 against the guest headers).
