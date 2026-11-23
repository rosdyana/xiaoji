# Graphics: the guest GPU stack and how the engine provides it

Status: research and design (2026-11-23, engine at cf5b4fc). No engine code was written for this document.
The question comes from B4f (`docs/boot.md`, end): `DisplayManagerService` blocks in
`SurfaceControl.nativeGetCompositionDataspaces` because SurfaceFlinger is not running, and SurfaceFlinger
needs an EGL/GLES implementation, a composer and an allocator.

Sources. AOSP is `build-host:~/xiaoji-aosp` (android13-release), read only; paths below are relative to it.
"Local" paths are in this repo. Line numbers are from the files as read on 2026-11-23.

## Summary

- **The emulator stack in our image, `rootfs-xiaoji`,** is the goldfish-opengl guest libraries
  (`libEGL_emulation` and the GLES encoders) talking to the emulator's host renderer (gfxstream/emugl,
  `device/generic/vulkan-cereal`). Buffers come from minigbm on a virtio-gpu DRM render node. The composer is
  `hwcomposer.ranchu` (HWC2 `HostComposer`), which composes on the host with `rcCompose*` and scans out through
  KMS on `/dev/dri/card0`. Because minigbm is used, the GL transport has to be one of the three virtio-gpu
  transports (`virtio-gpu`, `virtio-gpu-pipe` or `virtio-gpu-asg`). The host chooses which one through
  `ro.boot.qemu.gltransport*`, and the emulator binary that sets it is not in the tree.
- **Google's prebuilt image, `build/rootfs`, has the older goldfish stack:** ranchu gralloc (allocator@3.0 and
  mapper@3.0-ranchu) on `/dev/goldfish_pipe` and `/dev/goldfish_address_space`, with no DRM at all. This is the
  stack the B4f probe exercised.
- **Under the engine, none of the selecting properties are set.** `ro.hardware` is `xiaoji`
  (`engine/props.c:380`), so `init.ranchu.rc` is never imported and `hwcomposer.ranchu` is never found. All three
  probe failures follow from that, plus the missing devices.
- **Recommendation: option A, in its "goldfish" form (A2).** The engine plays the emulator's host. It
  implements three small character devices (`goldfish_pipe`, `goldfish_address_space`, `goldfish_sync`), the
  renderControl protocol, and GLES decoders generated from the specs in the tree. Behind the decoders sits one
  backend, written in C against GLES 3.0. Natively it links a host GLES (ANGLE or Mesa) so frames can be captured
  headless. For wasm, emscripten compiles the same calls to WebGL2.
  - **Guest side:** keep the goldfish-opengl libraries unchanged, but switch our product's gralloc from minigbm
    to ranchu, which is exactly what `build/rootfs` ships.
  - **Effort:** about 4 agent-weeks to boot completion with a null renderer, about 6 to 7 to phase 1 (frames
    captured headless), and about 8 to 11 in total to phase 2 (frames in the browser canvas).
- **Option B, ANGLE on SwiftShader in the guest,** is in the tree and has an arm64 precedent (the `fvp` product).
  It needs a DRM device emulation anyway, and our estimate is 0.5 to 5 fps for full-screen redraws under the
  translator. It is a fallback, not the plan.
- **Option C, custom guest libraries,** gives somewhat cheaper calls, but costs more effort and more risk than A.
  The wire encoding it saves can be mostly recovered inside A, using the ASG shared-memory transport the guest
  libraries already contain.

## 1. The stack found

### 1.1 What the product builds

`device/xiaoji/xiaoji_arm64.mk:25-27` inherits the arm64 emulator vendor side unchanged
(`arm64-vendor.mk`, `emulator64_vendor.mk`, `emulator64_arm64/device.mk`). The graphics packages come from
`device/generic/goldfish/vendor.mk`:

| Lines | Packages |
|---|---|
| 38-41 | `vulkan.ranchu`, `libOpenglCodecCommon`, `libOpenglSystemCommon` |
| 48-51 | `android.hardware.graphics.composer@2.4-service`, `android.hardware.graphics.allocator@4.0-service.minigbm`, `android.hardware.graphics.mapper@4.0-impl.minigbm`, `hwcomposer.ranchu` |
| 77-88 | (unless `BUILD_EMULATOR_OPENGL=false`) `libGLESv1_CM_emulation`, `lib_renderControl_enc`, `libEGL_emulation`, `libGLESv2_enc`, `libvulkan_enc`, `libGLESv2_emulation`, `libGLESv1_enc`, `libEGL_angle`, `libGLESv1_CM_angle`, `libGLESv2_angle` |

Local check of `build/rootfs-xiaoji/vendor`:

- `lib64/egl/` has `libEGL_emulation`, `libGLESv{1_CM,2}_emulation` and the full ANGLE set. `libGLESv2_angle.so`
  (7.7 MB) **is present**, which is not the case in `build/rootfs`.
- `lib64/` has `libGLESv1_enc`, `libGLESv2_enc`, `lib_renderControl_enc`, `libOpenglSystemCommon`,
  `libminigbm_gralloc`, `libdrm` and `libvulkan_enc`.
- `lib64/hw/` has `hwcomposer.ranchu.so`, `gralloc.default.so`, `vulkan.ranchu.so` and
  `android.hardware.graphics.mapper@4.0-impl.minigbm.so`.
- There is no `vulkan.pastel` (SwiftShader).

Google's `build/rootfs` instead has `vendor/bin/hw/android.hardware.graphics.allocator@3.0-service` and
`vendor/lib64/hw/android.hardware.graphics.mapper@3.0-impl-ranchu.so`. Those are the goldfish-opengl ranchu
gralloc HALs (`device/generic/goldfish-opengl/system/hals/Android.mk:20,59`).

### 1.2 Properties that select it

All of these are set by the emulator, either on the kernel command line (bootconfig `androidboot.*` becomes
`ro.boot.*`) or by `init.ranchu.rc`:

| Property | Set by | Read by | Effect |
|---|---|---|---|
| `ro.hardware` = `ranchu` | `androidboot.hardware` | `system/core/rootdir/init.rc:10` (`import /vendor/etc/init/hw/init.${ro.hardware}.rc`); `hardware/libhardware/hardware.c:59-64` (HAL variant keys: `ro.hardware`, `ro.product.board`, `ro.board.platform`, `ro.arch`) | imports `init.ranchu.rc`; makes `hw_get_module("hwcomposer")` find `hwcomposer.ranchu.so` |
| `ro.hardware.gralloc` = `minigbm` | `init.ranchu.rc:14` (early-init) | `goldfish-opengl/system/OpenglSystemCommon/HostConnection.cpp:146-155` | gralloc helper type, used only in the virtio-gpu transports (below) |
| `ro.hardware.vulkan` = `ranchu` | `init.ranchu.rc:13` | libvulkan HAL loader; `hwc2/Device.cpp:56-58` (`pastel` selects `GuestComposer`) | `vulkan.ranchu` (the goldfish Vulkan encoder) |
| `ro.hardware.egl` = `${ro.boot.hardwareegl:-emulation}` | `init.ranchu.rc:80` (on boot) | `frameworks/native/opengl/libs/EGL/Loader.cpp:139-144,238-250` | `libEGL_emulation.so` and friends |
| `ro.opengles.version` = `${ro.boot.opengles.version}` | `init.ranchu.rc:84` | `PackageManagerService.java:5023`, `ActivityTaskManagerService.java:818`, `GpuStats.cpp:211` | the advertised GLES version (`reqGlEsVersion` feature); the actual version comes from the host's `GL_EXTENSIONS` (`egl.cpp:220,936-938`, `ANDROID_EMU_gles_max_version_3_x`) |
| `ro.boot.qemu.gltransport.name`, then `ro.boot.qemu.gltransport`, then `ro.boot.hardware.gltransport` | the emulator (not in the tree) | `HostConnection.cpp:104-131` | `pipe` (also the default when unset), `asg`, `virtio-gpu`, `virtio-gpu-pipe`, `virtio-gpu-asg`, `tcp` |
| `ro.boot.qemu.gltransport.drawFlushInterval` | the emulator | `HostConnection.cpp:139` | flush batching (default 800) |
| `ro.vendor.hwcomposer.mode` = `noop` | not set on the emulator | `hwc2/Common.cpp` (`IsNoOpMode`) | `NoOpComposer` with a fixed 720x1280 at 30 Hz display (`DisplayFinder.cpp:207-221`, "only used for Gem5 bring-up") |
| `ro.boot.qemu.vsync` | the emulator | `hwc2/DisplayFinder.cpp:35` | vsync rate when DRM does not report one |
| `debug.renderengine.backend` | unset (default) | `SurfaceFlinger.cpp:745` | `gles` selects the legacy `GLESRenderEngine`; the default is Skia GL (`libs/renderengine/RenderEngine.cpp`, `RenderEngine.h:96-101`) |
| `ro.kernel.qemu.gles` / `ro.boot.qemu.gles` | legacy | only `goldfish-opengl/system/gralloc/gralloc_old.cpp:1590` (old `gralloc.goldfish`, not built here) | **no effect on this stack** |
| `qemu.sf.lcd_density` | qemu-props | `goldfish-opengl/system/egl/egl.cpp:1386` | display DPI fallback |

The emulator's feature list for arm64 images (`device/generic/goldfish/data/etc/advancedFeatures.ini.arm`,
copied by `arm64-vendor.mk:35`) includes `HostComposition` (line 9), `GLDirectMem` (16) and `Minigbm` (28).
`hardware-properties.ini:267-272` in vulkan-cereal gives `hw.gltransport` the default `pipe`
(enum `tcp, pipe, virtio-gpu, asg`).

**The transport is not pinned down by the tree.** What the tree does fix:

- The `QEMU_PIPE`, `ASG` and `TCP` branches of `HostConnection::connect` hard-code `GRALLOC_TYPE_RANCHU`
  (`HostConnection.cpp:467-516`).
- The `VIRTIO_GPU` branch hard-codes `GRALLOC_TYPE_MINIGBM` (`:522-541`).
- The `VIRTIO_GPU_PIPE` and `VIRTIO_GPU_ADDRESS_SPACE` branches honour `ro.hardware.gralloc=minigbm` (`:543-595`).
- `VIRTIO_GPU` is compiled in (`OpenglSystemCommon/Android.mk:39`, `egl/Android.mk:27`).

So with `Minigbm = on` the emulator must pass `virtio-gpu`, `virtio-gpu-pipe` or `virtio-gpu-asg`. For comparison,
Cuttlefish's gfxstream mode passes `androidboot.hardware.gltransport=virtio-gpu-asg`
(`device/google/cuttlefish/host/libs/vm_manager/crosvm_manager.cpp:84-92`).

### 1.3 Composer, gralloc and transport in `rootfs-xiaoji` (on the emulator)

| Piece | What runs | Device nodes |
|---|---|---|
| Composer | `composer@2.4-service`, the passthrough service. It loads HWC via `hw_get_module` and falls back to the gralloc module plus `HWC2OnFbAdapter` (`hardware/interfaces/graphics/composer/2.1/utils/passthrough/include/composer-passthrough/2.1/HwcLoader.h:62-65,126-132`). On the emulator it loads `hwcomposer.ranchu` = `goldfish-opengl/system/hwc2`. `Device::init` (`Device.cpp:74-110`) picks `HostComposer` (because the Vulkan driver is not `pastel`), and with minigbm it initialises `DrmPresenter` on `/dev/dri/card0`. `HostComposer` composes on the host with `rcComposeWithoutPost` / `rcComposeAsyncWithoutPost` (`HostComposer.cpp:692-704`). The result goes out through KMS atomic commit (`flushToDisplay`, `:729`), and the retire fence comes from the DRM commit. Without minigbm it would use `rcCompose` plus a `goldfish_sync` fence (`goldfish_sync_queue_work`, `:734`), or `rcFBPost` for client composition (`:760-764`, `post` at `:774-781`). | `/dev/dri/card0` (KMS: `drmModeGetResources`, `GetConnector`, `GetCrtc`, `GetPlaneResources`, `GetPlane`, `ObjectGetProperties`, `GetProperty`, `GetPropertyBlob`, `CreatePropertyBlob`, `AddFB2`, `RmFB`, `AtomicCommit` with `ALLOW_MODESET`, `drmSetClientCap(ATOMIC, UNIVERSAL_PLANES)`, `PrimeFDToHandle`, `GEM_CLOSE`; plus a `NETLINK_KOBJECT_UEVENT` socket for hotplug; `DrmPresenter.cpp`, 655 lines). Display modes come from renderControl (`rcGetFBParam`, `rcGetFBDisplayConfigs*`; `DisplayFinder.cpp`) unless the device is Cuttlefish. |
| Gralloc | `allocator@4.0-service.minigbm` + `mapper@4.0-impl.minigbm` (`external/minigbm/cros_gralloc`). `init_try_nodes` opens `/dev/dri/renderD128..` and then `card0..` (`cros_gralloc_driver.cc:97-130`), and picks the backend from the DRM driver name (`drv.c:58-93`, `virtio_gpu`). | `/dev/dri/renderD128`. ioctls: `DRM_IOCTL_VERSION`, `VIRTGPU_GETPARAM` (10 params, `virtgpu.c:25-50`), `VIRTGPU_GET_CAPS`, `VIRTGPU_RESOURCE_CREATE` or `RESOURCE_CREATE_BLOB`, `VIRTGPU_MAP` + `mmap` on the DRM fd, `TRANSFER_TO/FROM_HOST`, `VIRTGPU_WAIT`, `RESOURCE_INFO`, `PRIME_*`, `GEM_CLOSE`. **With `3D_FEATURES`=0 it falls back to dumb buffers** (`virtgpu_virgl.c:765-776`: `MODE_CREATE_DUMB`, `MODE_MAP_DUMB`, `mmap`, `drv_helpers.c:362,468`). |
| GL | `libEGL_emulation` with the encoders. A per-process `HostConnection` (`HostConnection.cpp:462-595`) uses either `VirtioGpuPipeStream` (a pipe over virtio-gpu `EXECBUFFER`) or the virtio-gpu ASG stream (`virtgpuOpen(capset)`; capset `CAPSET_GFXSTREAM 3`; `RESOURCE_CREATE_BLOB` for the ring, `VIRTGPU_MAP`, `EXECBUFFER` for pings, `VIRTGPU_WAIT`; `goldfish_address_space_android.impl:550-790`). | `/dev/dri/renderD128` |
| Other nodes in `vendor/etc/ueventd.rc:4-11` | `/dev/goldfish_pipe`, `/dev/goldfish_sync`, `/dev/goldfish_address_space`, `/dev/dri/card0`, `controlD64`, `renderD128` | |

`ShouldUseGuestComposer` (`Device.cpp:56-58`) means the same `hwcomposer.ranchu` composes on the CPU with libyuv
(`GuestComposer.cpp`, 1166 lines) when `ro.hardware.vulkan=pastel`. It still needs `DrmPresenter`, and outside
Cuttlefish it still needs renderControl to find displays.

### 1.4 The goldfish stack (`build/rootfs`, and the A2 target)

This is the stack with no DRM. `HostConnection` in `QEMU_PIPE` mode (gltransport unset or `pipe`) or in `ASG`
mode, with `GRALLOC_TYPE_RANCHU`.

**`/dev/goldfish_pipe`** (alias `/dev/qemu_pipe`, `init.ranchu.rc:32-33`)

- `open(O_RDWR|O_NONBLOCK)`, then `write("pipe:<name>\0")` selects the service (`shared/qemupipe/qemu_pipe_guest.cpp:138-160`).
- After that the fd is a byte stream with `read`, `write` and `poll`. No ioctls and no mmap are used by the
  graphics code. On an `opengles` stream, the first 4 bytes the guest sends are a zero `clientFlags` word
  (`HostConnection.cpp:615-619`), and commands follow.
- Graphics services:
  - `opengles`: the renderControl and GLES streams (`QemuPipeStream.cpp:67`).
  - `GLProcessPipe`: write int32 `100`, read a uint64 per-process id (`ProcessPipe.cpp:156-172`). Closing it
    tells the host the process died.
  - `refcount`: written one uint32 color-buffer handle. Closing it releases the buffer (`allocator3.cpp:376-398`).
- Graphics pipes never go over vsock (`qemu_pipe_guest.cpp:38-43,102-120`). Other services (`qemud:*`,
  `logcat`, ...) try vsock port 5000 first.
- Command framing (`vulkan-cereal/protocols/generic-apigen/README`): each packet is
  `uint32 opcode, uint32 packet_len, args...`. Pointer arguments are inlined as `uint32 size` plus bytes, per
  the `.attrib` `len`/`dir` rules. Replies (out buffers, then the return value) are read back synchronously only
  for calls that have outputs or a return value.

**`/dev/goldfish_address_space`**: ioctl magic `'G'`, all `_IOWR` (`goldfish_address_space_android.impl:56-62`).

- The ioctls: `ALLOCATE_BLOCK`(10) `{size, offset, phys_addr}`, `DEALLOCATE_BLOCK`(11) `u64`,
  `PING`(12) `struct address_space_ping`, `CLAIM_SHARED`(13) `{offset, size}`, `UNCLAIM_SHARED`(14) `u64`.
- The first `PING` sets the subdevice type (`:89-96`): `Graphics`=0 (ASG), `Media`=1,
  `HostMemoryAllocator`=5, `SharedSlotsHostMemoryAllocator`=6, `VirtioGpuGraphics`=10
  (`goldfish_address_space.h:46-53`).
- mmap semantics: `mmap(fd, offset)` at an offset returned by `ALLOCATE_BLOCK`, or claimed with `CLAIM_SHARED`,
  maps memory that the host also sees (`GoldfishAddressSpaceBlock::mmap`, `:258-280`). The guest and host share
  it coherently and with no explicit transfer.
- The ranchu allocator gets every gralloc buffer's CPU backing this way (`allocator3.cpp:362-410`). It picks
  the subdevice from the host feature `ANDROID_EMU_has_shared_slots_host_memory_allocator`
  (`allocator3.cpp:362-363`; constructor at `goldfish_address_space_android.impl:352-357`):
  - With the feature, it uses subdevice 6: `PING(ALLOCATE_ID)` returns an offset, then `CLAIM_SHARED`
    (`:374-390`).
  - Without it, it uses subdevice 5: `ALLOCATE_BLOCK`, then `PING(ALLOCATE_ID)` with that offset (`:391-410`).
  - Either way, the block is then `mmap`ed. Every mapper user maps the same fd and offset again
    (`mapper3.cpp:238-255`).
- The host color buffer is always created with `rcCreateColorBufferDMA` (`allocator3.cpp:384`), whatever
  features are advertised.
- ASG (`gltransport=asg`): subdevice `Graphics` is set up through `PING` with `ASG_GET_RING`=0,
  `ASG_GET_BUFFER`=1, `ASG_SET_VERSION`=2, `ASG_NOTIFY_AVAILABLE`=3 and `ASG_GET_CONFIG`=4. The ring and the
  transfer buffer are then claimed and `mmap`ed (`address_space_graphics_types.h:52-66,337-359`,
  `AddressSpaceStream.cpp:44-95`). Commands are written straight into shared memory; a ping is needed only when
  the host state is `NEED_NOTIFY`.

**`/dev/goldfish_sync`**: magic `'@'`.

- The ioctls are `QUEUE_WORK` (`_IOWR 0`, `goldfish_sync_ioctl_info {host_glsync, host_syncthread, fence_fd
  out}`) and `SIGNAL` (`_IOWR 1`) (`system/egl/goldfish_sync.h:57-66`).
- `QUEUE_WORK` returns a sync-file fd that the host signals once the GL fence created by `rcCreateSyncKHR`
  completes.
- It backs `EGL_ANDROID_native_fence_sync` in `libEGL_emulation` and the HostComposer retire fences.

### 1.5 What happened under the engine (the B4f probe), attributed per image

The probe (`build/tests/boot/b4f-sf.rc`) ran on **`build/rootfs`** (allocator@3.0 ranchu, no `libGLESv2_angle`).
The engine sets `ro.hardware=xiaoji` and `ro.boot.hardware=xiaoji` (`engine/props.c:380-381`) and boots
`image/xiaoji.rc`. As a result:

- **`init.ranchu.rc` is never imported** (`init.rc:10` looks for `init.xiaoji.rc`), so `ro.hardware.egl`,
  `ro.hardware.gralloc`, `ro.hardware.vulkan` and `ro.opengles.version` are all unset.
- **libEGL:** with `ro.hardware.egl` and `ro.board.platform` empty, both suffixed lookups are skipped
  (`Loader.cpp:238-250`). The exact unsuffixed lookup (`:258`) finds no `libEGL.so`. The non-exact fallback
  (`:261-262`, `attempt_to_load_system_driver(cnx, nullptr, false)` at `:600`) then picks the first
  `libEGL*` in the vendor egl directory, which was `libEGL_angle.so`. The log line is `load_driver(...)` (`:471`).
  It failed on the missing `libGLESv2_angle.so`, and the fatal error is at `:276-278`.
  With `ro.hardware.egl=emulation` set, it would load `libEGL_emulation`, which would then fail to open
  `pipe:opengles`.
- **Composer:** `hw_get_module("hwcomposer")` tries `hwcomposer.xiaoji`, `hwcomposer.goldfish_arm64`
  (`ro.product.board`), the empty `ro.board.platform`, and `ro.arch`, then falls back to the gralloc module
  (`gralloc.default`), and `framebuffer_open` fails on `/dev/graphics/fb0` / `/dev/fb0` (`HwcLoader.h:62-65,128`).
  `hwcomposer.ranchu.so` exists in both images but is never looked up. `ro.hardware.hwcomposer=ranchu` would
  select it (`hardware.c:220`).
- **Allocator:** the ranchu allocator@3.0 opens `/dev/goldfish_pipe` (the `refcount` pipe and the host
  connection), which the engine does not have.
- **Prediction for `rootfs-xiaoji`** (not run): ANGLE now loads, its Vulkan backend loads `vulkan.ranchu`
  (only if `ro.hardware.vulkan` is set), and that needs the host. The minigbm allocator finds no `/dev/dri` node
  and fails to initialise. The composer fails as above.

## 2. What phase 1 needs from GLES

- **SurfaceFlinger always creates a RenderEngine, and every backend is GL.** `GLESRenderEngine` requires EGL,
  ES 2.0 at minimum (ES 3.0 when background blur is on), `EGL_KHR_surfaceless_context` or a pbuffer, and it
  uses native fence sync when available (`libs/renderengine/gl/GLESRenderEngine.cpp:252-321,526-593`). There is
  no software RenderEngine in Android 13. `debug.renderengine.backend=gles` (`SurfaceFlinger.cpp:745`) avoids
  Skia in SurfaceFlinger, which makes for a smaller GL call set during bring-up.
- **`system_server` draws in software on low-RAM devices.** `ThreadedRenderer.initForSystemProcess`
  (`ThreadedRenderer.java:215-224`) disables HWUI when `!isHighEndGfx()`, and `ro.config.low_ram=true` is set in
  our product. It is called only from `ActivityThread.systemMain` (`ActivityThread.java:7654`). Line 220 is the
  only place in `core/java` that clears `sRendererEnabled`.
- **Apps, including persistent ones like SystemUI and Launcher, still use HWUI GL (Skia GL).**
  `ViewRootImpl.enableHardwareAcceleration` (`ViewRootImpl.java:1554`) enables it whenever `sRendererEnabled` is
  set. Its comment about persistent processes on low-end devices describes older behaviour. So a working EGL plus GLES is needed
  for any UI, and boot completion needs at least SurfaceFlinger plus SystemUI and Launcher not crashing in EGL.
- **The guest encoder already deals with external textures.** `GL2Encoder` rewrites `samplerExternalOES` to
  `sampler2D` in shader source (`GL2Encoder.cpp:1673-1700`) and maps `GL_TEXTURE_EXTERNAL_OES` onto 2D textures.
  EGLImages from gralloc buffers become host color-buffer images (`rcCreateClientImage`, `egl.cpp`;
  `GL2Encoder.cpp:2190-2263`). The host never sees external textures, which matters for WebGL2.

Cheap composer paths for bring-up:

1. **`NoOpComposer`** (`ro.vendor.hwcomposer.mode=noop`, `ro.hardware.hwcomposer=ranchu`): needs no device and
   no renderControl, has a fixed 720x1280 display, and drops frames. Usable for "SurfaceFlinger starts" only.
2. **`HostComposer` with no host-composition feature** (`HostComposer.cpp:471`, `fallBackToClient`):
   SurfaceFlinger composes everything with GLES and HWC calls `rcFBPost` on the client target. That one call is
   the frame-capture point. This is the A2 choice.
3. **The fb0 adapter with `gralloc.default`:** needs an fbdev emulation (`FBIOGET_VSCREENINFO` and friends plus
   mmap). It is CPU only and pairs with option B. It is legacy and gives no fences.
4. **`GuestComposer`:** CPU composition with libyuv, but it needs KMS (`DrmPresenter`), and outside Cuttlefish
   it needs renderControl for display discovery. It pairs with option B.

## 3. Option A: the engine is the emulator's host

### 3.1 Two variants

- **A1, keep minigbm** (the current product). The engine would emulate the virtio-gpu DRM UAPI on `renderD128`
  (about 12 ioctls, blob resources, context init with the gfxstream capset, `EXECBUFFER` carrying gfxstream
  commands, and ASG over virtio-gpu). It would also emulate KMS atomic plus uevent netlink on `card0` for
  `DrmPresenter`. That is a much larger surface, and the gfxstream virtio-gpu command layer comes on top of the
  GL protocol.
- **A2, switch to ranchu gralloc** (recommended). Product change:
  - Replace `allocator@4.0-service.minigbm` and `mapper@4.0-impl.minigbm` with
    `android.hardware.graphics.allocator@3.0-service` and `android.hardware.graphics.mapper@3.0-impl-ranchu`
    (`goldfish-opengl/system/hals`). That is Google's own `build/rootfs` configuration, so the framework accepts
    it in Android 13.
  - Set the properties ourselves in the engine (or in `init.xiaoji.rc`): `ro.hardware.egl=emulation`,
    `ro.hardware.hwcomposer=ranchu`, `ro.hardware.gralloc=ranchu`, `ro.opengles.version=196608`, gltransport
    unset (pipe) at first and later `asg`, and optionally `debug.renderengine.backend=gles`.
  - Leave `ro.hardware.vulkan` unset, so no Vulkan.
  - Devices: `goldfish_pipe`, `goldfish_address_space` (the `HostMemoryAllocator` subdevice, and later
    `Graphics` for ASG) and `goldfish_sync`. No DRM. HostComposer uses `goldfish_sync` for fences when not on
    minigbm (`HostComposer.cpp:715-736`).

### 3.2 Protocol surface (counted)

| Protocol | Spec in tree | Entry points |
|---|---|---|
| renderControl | `vulkan-cereal/protocols/renderControl/renderControl.{in,attrib,types}` (68-line `.in`, 119-line `.attrib`). The guest's generated `renderControl_opcodes.h` has 68 opcodes (`OP_rcGetRendererVersion` .. `OP_rcGetFBDisplayActiveConfig`). The stale `goldfish-opengl/system/renderControl_enc/renderControl.in` lists only 36, so **use vulkan-cereal's**. | 68 |
| GLES 2/3 | `vulkan-cereal/protocols/gles2/gles2.{in,attrib,types}` (1491-line `.attrib`) | 427, of which 54 are emulator-specific (`*Data`, `*Offset`, `*AEMU`, `*DMA`, `*NullAEMU`, for example `glVertexAttribPointerData`, `glDrawElementsOffset`, `glMapBufferRangeAEMU`, `glFenceSyncAEMU`, `glShaderString`, `glGetUniformIndicesAEMU`) |
| GLES 1 | `vulkan-cereal/protocols/gles1/gles1.*` | 293 |

- **The specs can be reused.** `generic-apigen` (`protocols/generic-apigen`, C++) generates both the encoder and
  the decoder from the `.in`/`.attrib`/`.types` triple. The host's generated decoders are
  `gles2_dec.cpp` (9518 lines), `gles1_dec.cpp` (6187) and `renderControl_dec.cpp` (1430).
- **Plan: write a small generator** (Python, about 1000 to 1500 lines) that reads the same three files and emits
  C decode tables for the engine: opcode, argument unpacking, `len`/`dir`/`var_flag` handling, and reply packing.
  apigen's C++ output depends on emugl's `IOStream` and checksum classes, so reusing its output directly is
  awkward. Its spec semantics are what matter.
- **Most of the cost is the hand-written layer, not the tables.** On the host that is `RenderControl.cpp` (1629
  lines), `FrameBuffer.cpp` (3620), `ColorBuffer.cpp` (1131), `GLESv2Decoder.cpp` (990) and `glestranslator/`
  (34882 lines of `.cpp`, GLES on top of desktop GL). WebGL2 is close to ES 3.0, so most of the translator is not
  needed. For comparison, the guest side we keep for free is about 11.7k lines (`GL2Encoder.cpp`,
  `GLESv2Validation.cpp`, `egl/*.cpp`).

**The engine chooses the feature flags.** The host's `GL_EXTENSIONS` string carries `ANDROID_EMU_*` flags (34 in
`OpenglSystemCommon/EmulatorFeatureInfo.h`). Advertise the minimum:

- **Advertise:** `ANDROID_EMU_gles_max_version_3_0`, `ANDROID_EMU_native_sync_v2` (or v3 for
  `rcWaitSyncKHR`) and `ANDROID_EMU_gles_no_host_error` (no `glGetError` round trips).
- **Leave out:**
  - Vulkan, direct_mem, the GL DMA buffer path (`glMapBufferRangeDMA`), async frame commands, host composition,
    YUV cache, multi-configs and virtio_gpu_next. With those left out, the guest never calls the matching
    opcodes.
  - `has_shared_slots_host_memory_allocator`. The allocator then uses the plain subdevice-5 path (1.4).
- **Not controlled by any flag:** `rcCreateColorBufferDMA` is called unconditionally by the allocator, and
  `rcCreateDisplayById` plus `rcSetDisplayPoseDpi` by HostComposer for display 0 (`HostComposer.cpp:348-360`).
  All three are in the subset below.

**Minimum renderControl subset for SurfaceFlinger plus RenderEngine plus HWUI** (estimate, about 36 of 68):

- Version and config queries: `rcGetRendererVersion`, `rcGetEGLVersion`, `rcQueryEGLString`, `rcGetGLString`,
  `rcGetNumConfigs`, `rcGetConfigs`, `rcChooseConfig`, `rcGetFBParam`.
- Contexts and surfaces: `rcCreateContext`, `rcDestroyContext`, `rcCreateWindowSurface`,
  `rcDestroyWindowSurface`, `rcMakeCurrent`.
- Color buffers: `rcCreateColorBuffer`, `rcCreateColorBufferDMA`, `rcOpenColorBuffer2`, `rcCloseColorBuffer`,
  `rcSetWindowColorBuffer`, `rcFlushWindowColorBuffer`, `rcBindTexture`, `rcBindRenderbuffer`,
  `rcColorBufferCacheFlush`, `rcReadColorBuffer`, `rcUpdateColorBuffer`.
- Images, frames, displays and checksums: `rcCreateClientImage`, `rcDestroyClientImage`, `rcFBPost`,
  `rcFBSetSwapInterval`, `rcCreateDisplayById`, `rcSetDisplayPoseDpi`, `rcSelectChecksumHelper`.
- Syncs and the process id: `rcCreateSyncKHR`, `rcClientWaitSyncKHR`, `rcWaitSyncKHR`, `rcDestroySyncKHR`,
  `rcIsSyncSignaled`, `rcSetPuid`.

The GLES subset used by `GLESRenderEngine` plus Skia GL is roughly 150 of the 427 GLES 2/3 opcodes. GLES 1 can
be stubbed: no `CM` calls happen during boot, and only GLES1 apps use them.

### 3.3 The backend: one C layer, three targets

The decoders call a backend interface: color buffers, contexts, surfaces, the GLES calls and syncs.

- **null**: allocates names, answers `glGet*` from a table, compiles shaders as "ok" and drops draws. Boot only.
- **host GLES**: the same C code linked natively against a GLES 3.0 implementation (ANGLE on macOS, Mesa
  llvmpipe on the Linux builder). Frames are read back on `rcFBPost` and written as PNG files. This is phase 1's
  capture path.
- **WebGL2**: the same C code built with emscripten (`-sMAX_WEBGL_VERSION=2 -sFULL_ES3`). Emscripten's GL layer
  maps the GLES 3.0 calls to WebGL2 and emulates client-side arrays and `glMapBufferRange`.
  - **Where the context lives:** in the engine's worker, on an OffscreenCanvas the worker creates itself
    (`new OffscreenCanvas(w, h)`).
  - **Presenting.** The engine runs `callMain` synchronously in one worker (`web/worker.js`; no pthreads or
    Asyncify in the `Makefile`), so the worker never returns to its event loop and implicit presentation never
    happens. Present explicitly on `rcFBPost`: `transferToImageBitmap()`, then `postMessage` (worker to page,
    which is delivered without the worker yielding) to the page's `ImageBitmapRenderingContext`. To verify
    early.
  - **Synchronous queries.** Calls such as `glReadPixels` and `glGetShaderiv` are ordinary synchronous calls,
    since the context is in the same worker. That is a real advantage over a design where the GPU lives on
    another thread.

### 3.4 GLES 3.x versus WebGL2 gaps (these apply to options A and C alike)

| Gap | Handling |
|---|---|
| WebGL contexts cannot share objects, but guest EGL contexts share within share groups, across many processes | One WebGL2 context for everything. Guest object names are mapped per share group, and per-context state is shadowed and re-applied on `rcMakeCurrent`. This is the largest single piece (about 2k lines) and the main correctness risk. |
| External textures and EGLImage | Already reduced to 2D textures by the guest encoder (section 2). A host color buffer is a WebGL texture; an EGLImage of it aliases the same texture. YUV buffers (camera, video) need host-side conversion. Defer. |
| ES 3.1 and 3.2 (compute, SSBO, image load/store, indirect draws, separate programs) | Not in WebGL2. Advertise 3.0 (`gles_max_version_3_0`, `ro.opengles.version=196608`). HWUI and SurfaceFlinger do not need 3.1. |
| Fences | WebGL2's `clientWaitSync` timeout is effectively 0 (`MAX_CLIENT_WAIT_TIMEOUT_WEBGL`). All guest GL is serialized into one context, so treat a guest fence as signaled once the commands before it have been issued, or after `gl.finish` when a readback needs it. Signal the `goldfish_sync` fds from the same point. |
| `glMapBufferRange` | Not in WebGL2. The encoder's `glMapBufferRangeAEMU` / `glUnmapBufferAEMU` already carry explicit data, so map them to `bufferSubData` and `getBufferSubData`. |
| BGRA formats, ETC2/ASTC textures, 3-channel and luminance formats, `GL_HALF_FLOAT_OES` enums | Swizzle or convert on upload. ETC2 is decoded on the host (the emulator's translator does the same). |
| GLSL ES dialect | ES 100 and 300 es shaders pass through. WebGL's validator is stricter: identifier length, `#extension GL_OES_EGL_image_external` (already removed by the encoder), and so on. Expect a small rewrite pass. |
| Context loss | Rebuild from the shadow state, or reload. Defer. |

### 3.5 Effort for A2 (engine lines, agent-weeks)

| Piece | Engine lines (estimate) | Agent-weeks |
|---|---|---|
| `goldfish_pipe` (services `opengles`, `GLProcessPipe`, `refcount`; poll and wake integration with `sched`) | 500 | 0.5-1 |
| `goldfish_address_space` (`HostMemoryAllocator` on memfd-style engine pages; later `Graphics`/ASG) | 350 (+300 for ASG) | 0.5 (+0.5) |
| `goldfish_sync` (sync-file fds) | 250 | 0.5 |
| Decoder generator plus generated tables | 1.2k (generator) + about 12k generated | 1 |
| renderControl (configs, contexts, surfaces, color buffers, FB post, syncs) | 2.5k | 1-1.5 |
| null backend | 1.5k | 0.5 |
| host-GLES backend: context virtualization, name maps, formats, readback and PNG | 5-7k | 3-4 |
| WebGL2 build, OffscreenCanvas, presenting, WebGL validation fixes | 1k | 1-2 |
| **Boot completes** (devices without ASG, generator, renderControl, null backend) | about 5k + generated | **about 4** |
| **Phase 1** (the above, plus the host-GLES backend far enough to render SurfaceFlinger, SystemUI and Launcher and capture frames) | about 9k + generated | **6-7** |
| **Phase 2** (the rest of the backend, WebGL2) | about 12k + generated in total | **8-11 total** (2-4 after phase 1) |

The engine is 22.3k lines today. `binder.c` (2.1k) is the closest precedent for a device with ioctls and mmap.

**Test oracle.** The builder VM can build the real gfxstream host (`vulkan-cereal` builds with CMake; it was not
tried). It can then decode recorded `opengles` streams as a reference for the engine's decoder. A run of the real
emulator is a second oracle for whole boots.

### 3.6 Expected browser performance (A)

The cost is guest CPU plus call overhead, not the GPU:

- The guest encoder runs as translated guest code.
- Each GL call is decoded in wasm and becomes one wasm-to-JS WebGL call (on the order of 0.1 to 1 µs).
- Uploads (glyph atlases, bitmaps) are copied out of guest memory.

SurfaceFlinger's client composition is tens of calls per frame plus one texture per layer, already resident.
**Composition at 30 to 60 fps is plausible.** App frames depend on HWUI and Skia recording in the guest (a
translated CPU workload; the engine runs about 60 to 105 M guest instructions per second,
`docs/spike-0.md:339`, `docs/boot.md:1043`). **Our guess for an animating Launcher or SystemUI is 10 to 30 fps**,
to be measured. ASG (shared memory, with a ping only when the host sleeps) removes the pipe copies and most of
the `write` syscalls.

## 4. Option B: ANGLE on SwiftShader inside the guest

**In the tree.** `external/swiftshader/src/Android.bp:212` builds `vulkan.pastel` (a vendor module) with LLVM 10
Reactor (`libLLVM10_swiftshader`). The AArch64 target is in its sources
(`third_party/llvm-10.0/Android.bp:891`). ANGLE is built from source with only `-DANGLE_ENABLE_VULKAN`
(`external/angle/Android.bp`), so ANGLE here means ANGLE on Vulkan, and Vulkan here would mean SwiftShader.
`vulkan.pastel` is not in `out/` for either product, so **it is in the tree but its build is not verified.**

**Precedents:**

- **`fvp`** (arm64, QEMU or FVP, `device/generic/goldfish/fvp.mk:59-71,108-111`):
  - EGL and Vulkan: `libEGL_angle` + `vulkan.pastel`.
  - Gralloc and composer: `gralloc.minigbm`, `hwcomposer.drm_minigbm` (`external/drm_hwcomposer`),
    `composer@2.1-service`.
  - Properties: `ro.hardware.egl=angle`, `ro.hardware.vulkan=pastel` and
    `PRODUCT_REQUIRES_INSECURE_EXECMEM_FOR_SWIFTSHADER := true`.
  - `fvpbase/init.qemu.rc:5-6` sets gralloc minigbm and hwcomposer `drm_minigbm`.
  - The display is a 2D `virtio-gpu-pci` (`fvpbase/run_qemu:31`).
- **Cuttlefish `guest_swiftshader`** (`crosvm_manager.cpp:66-73`, `shared/device.mk:212-227,400-420`):
  minigbm, `egl=angle`, `vulkan=pastel`, `opengles.version=196609`, and composer3 ranchu or drm_hwcomposer.

**Composer and gralloc without a GPU device.** Every in-tree choice needs DRM:

- minigbm opens a render or card node (`cros_gralloc_driver.cc:97-130`). On a virtio-gpu that reports no 3D,
  it uses dumb buffers.
- drm_hwcomposer and `DrmPresenter` need KMS.

So option B still needs engine work:

- **(a)** A "virtio_gpu 2D" DRM node. `DRM_IOCTL_VERSION` returns `virtio_gpu`, `VIRTGPU_GETPARAM` returns 0 or
  `EINVAL`, plus `MODE_CREATE_DUMB`, `MODE_MAP_DUMB`, `DESTROY_DUMB`, `PRIME_HANDLE_TO_FD`, `PRIME_FD_TO_HANDLE`
  and `GEM_CLOSE`, with mmap backed by memfd pages. About 600 to 800 lines.
- **(b)** For display, KMS on `card0` for drm_hwcomposer: resources, connector, crtc, plane, properties, blobs,
  `ADDFB2`, atomic commit, vblank and page-flip events, and a uevent netlink. About 1 to 1.5k lines. The engine
  presents the scanout buffer with a CPU blit (`putImageData` or `texImage2D`).
- **(c)** Or `NoOpComposer` for "boots, no frames".

A memfd or ashmem gralloc would mean writing a new HAL (that is option C's gralloc).

**Engine risks:**

- Reactor JIT-compiles AArch64 at runtime into writable executable memory (execmem), so the engine must handle
  freshly written code. Its translator detects self-modifying code and invalidates blocks (`engine/jit.c:2555`).
- The JIT output is NEON-heavy, and `simd.c` covers only a subset, so expect SIGILLs to chase.
- marl worker threads give no parallelism, because all guest threads share one worker.
- LLVM compiles every pipeline and shader variant at run time with no persistent cache. That is about 10 to 50 ms
  each natively, so seconds each under the engine, which lands on boot and on first frames.
- The libraries are about 30 MB (ANGLE 7.7 MB plus SwiftShader with LLVM). That is acceptable with lazy chunks.

**Expected cost, an estimate rather than a measurement.** A full-screen 1080x1920 composite in SwiftShader is a
few milliseconds natively on one core. The translator is about 5 times slower than native on general code (ART
hello, 1.9 s against 0.39 s) and far slower on SIMD-heavy loops that go through C helpers, so assume 20 to 50
times. With no threading, that gives **100 ms to 1 s per full redraw, or 0.5 to 5 fps** for SurfaceFlinger plus
an app frame, and worse with blur. This misses the target ("guest GLES rendered through WebGL2"). It would only
be useful as a bring-up path or a no-WebGL fallback.

**Effort:** about 2 to 4 agent-weeks to the first frame (product change and build, DRM 2D, KMS or NoOp, chasing
JIT and NEON issues), then performance work with a low ceiling.

## 5. Option C: custom xiaoji guest libraries

The parts:

- **`libEGL_xiaoji` / `libGLESv2_xiaoji`:** each GL call becomes an engine "hypercall" (a syscall or ioctl
  carrying the function id and arguments). The engine can read guest memory directly, so there is no wire
  encoding: client arrays, pixel data and shader strings are read in place at the call.
- **A memfd-backed gralloc:** allocator AIDL V1 plus a mapper@4.0 passthrough, including gralloc4 metadata.
- **A small composer HAL:** client composition only, with each client target handed to the engine through a
  device.

| | A2 | C |
|---|---|---|
| Guest code to write | none (goldfish-opengl, tested across Android releases) | EGL (configs, window surfaces over `ANativeWindow`, pbuffers, EGLImage from `AHardwareBuffer`, native fence sync, extension strings that HWUI and Skia probe), GLES stubs (about 300, generated from `gl.xml`), gralloc4 (about 1.5-2k lines; compare minigbm's mapper4 and goldfish's 676-line `mapper3.cpp`), composer: about 8-12k guest lines in all, plus sepolicy, VINTF and AOSP rebuilds on the remote |
| Engine code | devices, decoders, renderControl, backend | hypercall dispatch and the same backend (context virtualization, WebGL2 gaps). No decoder, but the guest-library ABI has to be kept stable. |
| Per-call cost | encode in the guest, copy through the pipe, decode | about 1 trap per call, zero-copy. Perhaps 1.5 to 2 times less GL overhead. With ASG, A's copies shrink to one write into shared memory. |
| Risk | protocol fidelity (68 + 427 opcodes, flags); the oracle exists (the gfxstream host) | EGL and gralloc correctness in front of the whole framework; no oracle; a maintenance burden on every AOSP update |
| Effort | 8-11 weeks to phase 2 | about 10-14 weeks to phase 2 |

The backend dominates both, and it is the same work. C trades protocol fidelity for guest-side maintenance and
saves per-call overhead that is not the bottleneck (section 3.6). Revisit C only if profiles show encoding as a
major cost after ASG.

## 6. Reference points

- **Twoyi / Twoyi Web:** not determined. The public repository (`github.com/twoyi/twoyi`) describes the app as "a
  UI rendering engine", and its ROM source is not published. A web search found nothing on how it renders. The
  project's earlier note that Twoyi Web maps GLES to WebGL2 has no recorded source.
- **Redroid:** its documentation (`remote-android/redroid-doc`) documents `androidboot.redroid_gpu_mode` =
  `auto|host|guest` ("guest: use software rendering", "host: use GPU accelerated rendering") and
  `androidboot.redroid_gpu_node`. It does not name the libraries (Mesa, SwiftShader, gralloc), so the mechanism
  is not determined from the documentation.
- **In-tree precedents** (stronger evidence than either): `fvp` and Cuttlefish `guest_swiftshader` for option B;
  the emulator's goldfish stack (`build/rootfs`) for option A2; Cuttlefish gfxstream (`virtio-gpu-asg` plus
  minigbm) for A1.

## 7. Recommendation and phased plan

Choose **A2**: the engine as the emulator host over the goldfish devices, with the goldfish-opengl guest
libraries and ranchu gralloc unchanged, and a single C backend on GLES 3.0 (host GLES natively, WebGL2 in the
browser).

The reasons:

- It reuses the most tested guest code.
- Its device surface is the smallest: three character devices, about 10 ioctls in total, one mmap kind, and no
  DRM.
- It has a real oracle (the gfxstream host).
- It gives a WebGL2 path in which the GPU does the rendering.

### Phase 0 (about 0.5 week): configuration

- In the product, swap minigbm allocator and mapper for `allocator@3.0-service` plus `mapper@3.0-impl-ranchu`,
  and keep `hwcomposer.ranchu` and the `libEGL_emulation` set. Until that build exists, phase 1 can start on
  `build/rootfs`, which already has this stack.
- Set the properties in the engine or `image/xiaoji.rc`: `ro.hardware.egl=emulation`,
  `ro.hardware.hwcomposer=ranchu`, `ro.hardware.gralloc=ranchu`, `ro.opengles.version=196608`, a display size
  and density, `debug.renderengine.backend=gles` (bring-up only), and gltransport unset (pipe).
- Start `surfaceflinger`, the composer and the allocator from the image's own rc blocks, as in the B4f probe.

### Phase 1 (about 6-7 agent-weeks; boot completion at about 4): SurfaceFlinger starts and boot completes, frames captured headless

1. Implement `goldfish_pipe` (`opengles`, `GLProcessPipe`, `refcount`), `goldfish_address_space`
   (`HostMemoryAllocator`) and `goldfish_sync`, each with a freestanding guest test in `tests/guest/`.
2. Build the generator and the renderControl, GLES2 and GLES1 decode tables, then renderControl (the subset in
   3.2) with the null backend. Check: `nativeGetCompositionDataspaces` returns, `startOtherServices` runs, and
   `sys.boot_completed=1`.
3. Write the host-GLES backend natively (ANGLE on macOS, Mesa llvmpipe on the builder) and dump the color buffer
   on each `rcFBPost` to PNG. Check: a captured frame shows Launcher or SystemUI.
4. Decoder oracle: replay recorded `opengles` streams into the real gfxstream host on the builder and compare
   replies and rendered color buffers.

### Phase 2 (2-4 more agent-weeks, 8-11 in total): frames visible in the browser canvas via WebGL2

1. Build the backend with emscripten WebGL2 on an OffscreenCanvas in the engine worker, and present each
   `rcFBPost` with `transferToImageBitmap` plus `postMessage`.
2. Close the WebGL2 gaps (3.4): context virtualization across share groups, formats, fences and
   `MapBufferRange`; then drop `debug.renderengine.backend=gles` so SurfaceFlinger uses Skia GL.
3. Measure frame times on AC power with Low Power Mode off (the benchmark rule).

### Phase 3 (optional, by profile)

- The ASG transport (`gltransport=asg`, the `Graphics` subdevice).
- Host composition (`rcCompose`) to skip SurfaceFlinger's GL pass.
- A YUV path for camera and video.
- GLES1.

**Keep option B in reserve** as a no-WebGL fallback. Its DRM 2D and KMS emulation would also be the
presentation path for option C. The only phase-1 shortcut it offers, `NoOpComposer` without a GL host, cannot
boot SystemUI, because HWUI needs EGL anyway.

## Open questions

- The emulator's exact `gltransport` value on arm64 with `Minigbm = on` (the emulator source is not in the tree).
  It does not affect A2.
- Whether emscripten's GL layer runs in a non-pthread worker with an OffscreenCanvas `Module.canvas` exactly as
  assumed. Verify early in phase 2.
- Whether `vulkan.pastel` builds for `xiaoji_arm64` (option B only). Not built in this study.
- The real HWUI call volume and frame cost under the engine. Measure in phase 1 with the host-GLES backend.
- Input in the browser. The engine worker never yields, so it cannot receive `postMessage`. Input needs a
  SharedArrayBuffer ring (cross-origin isolation) polled by the engine, or periodic yielding. There is no input
  path in the repository yet.
