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

## G1: phase 0 and the first half of phase 1 (status, 2026-11-23)

Status: **checkpoints 1 to 4 are done on the product tree (`build/rootfs-xiaoji`): SurfaceFlinger starts and is
published, and `DisplayManagerService` completes its constructor (the plan's `nativeGetCompositionDataspaces` step).
Checkpoint 5 is partial: `system_server` reaches `Starting phase 100`, then waits for `idmap2d`, which needs the
service user model the engine lacks: every service runs as root (`docs/boot.md`, B5).** The Google image (`build/rootfs`) does not get as far, because it has no
`vndservicemanager` binary, so its composer never registers.

### Checkpoints

| # | Checkpoint | Result |
|---|---|---|
| 1 | Product rebuilt with ranchu gralloc, rootfs refreshed, tests green | Done. Product build exit 0 on the builder (`~/xiaoji-aosp/xiaoji-build.log`). `build/rootfs-xiaoji` replaced. `make test` 184 passed (default tree, final run). |
| 2 | goldfish_pipe and the renderControl handshake guest test | Done. `tests/guest/goldfish-pipe.c`, expectation `tests/expect/goldfish-pipe.out`. |
| 3 | Generator and null GL backend | Done, with the validation below. The decoders come from the specs; the null backend answers the calls the boot needs. |
| 4 | SurfaceFlinger running and registered | **Done** on the product tree: the `SurfaceFlinger` service is found by servicemanager (`Could not find` never appears), and HWC connects its display. No `service check SurfaceFlinger` smoke test was added; the boot check looks for the same facts in the log. |
| 5 | Boot progress | `StartDisplayManager took to complete: 17ms` at 39.1 s (4.137 G, B5). **Partial**: `system_server` reaches phase 100 and stops at `idmap2d` (the engine runs services as root), and `System now ready` is not reached. |

### What was built (where)

- **Product** (`aosp/device/xiaoji/`, mirrored to the builder with `tools/remote-build.sh`):
  - `xiaoji_arm64.mk`: `PRODUCT_PACKAGES += vndservicemanager`, and the inherit of `xiaoji_emulator64_vendor.mk`.
  - `xiaoji_emulator64_vendor.mk` and `xiaoji_vendor.mk`: forks of `device/generic/goldfish/64bitonly/product/{emulator64_vendor,vendor}.mk`. The only change is the minigbm lines (`allocator@4.0-service.minigbm`, `mapper@4.0-impl.minigbm`), replaced by `allocator@3.0-service` and `mapper@3.0-impl-ranchu`, and a `DEVICE_MANIFEST_FILE` line for the fragment below.
  - `manifest.graphics.xml`: the allocator@3.0 and mapper@3.0 HAL entries. Without it the VINTF check fails: `android.hardware.graphics.mapper` has no provider.
- **Rootfs**: `build/rootfs-xiaoji` (fetched by `tools/fetch-remote-rootfs.sh`, which also regenerates its linker config). The previous tree was removed after the swap.
- **Properties** (`engine/props.c`, `engine_props`): `ro.hardware.egl=emulation`, `ro.hardware.hwcomposer=ranchu`, `ro.hardware.gralloc=ranchu`, `ro.opengles.version=196608`, `debug.renderengine.backend=gles`. No `ro.boot.qemu.gltransport*` (the pipe is the default), and no `ro.hardware.vulkan`. `ro.hardware=xiaoji` is unchanged.
- **Init** (`image/xiaoji.rc`): the `surfaceflinger`, `vendor.hwcomposer-2-4`, `vendor.gralloc-3-0`, `vndservicemanager` and `lmkd` blocks, from the image's own rc files (class and socket as the engine reads them; `task_profiles`, `onrestart`, `user`, `group` and `capabilities` are not modelled by `engine/init.c`, so they are left out).
- **Fixes on the way** (B5): the binder send path chooses a nested call's target from the thread's received requests, as the driver does (`engine/binder.c`); the scalar floating-point three-same group, including `FABD` (`engine/simd.c`, test `tests/guest/fabd-scalar.c` (and `fp-scalar3.c` for the other scalar forms)); the two custom GL calls the spec marks `not_api` (`engine/gl_host.c`).
- **Devices** (`engine/goldfish.c`, `engine/goldfish.h`; wired into `vfs.c`, `vfs.h`, `poll.c`, `sysfs.c` and `syscall.c`):
  - `/dev/goldfish_pipe` (alias `/dev/qemu_pipe`): the services `opengles`, `GLProcessPipe` and `refcount`.
  - `/dev/goldfish_address_space`: the HostMemoryAllocator subdevice. Blocks are ranges of one sparse memfd, mapped by `mmap(fd, offset)`. Without shared slots, CLAIM_SHARED and UNCLAIM_SHARED are not supported (ENOTTY).
  - `/dev/goldfish_sync`: QUEUE_WORK returns a signalled eventfd, so the fence is readable at once. SIGNAL is a no-op.
  - `XIAOJI_GF_TRACE=1` logs each pipe open, request, reply and device call (stderr). It is off by default.
- **GL host** (`engine/gl_host.h`, `engine/gl_host.c`, `engine/gl_null.c`): `gl_host_decode` dispatches by opcode range to the generated decoders. `gl_null.c` is the null backend (below).
- **Generator** (`tools/gen-glproto.py`, `tools/fetch-remote-protocols.sh`, Makefile target `build/gen/gen_report.txt`): reads the three specs and writes `build/gen/` (not checked in; see deviations). `make` runs the fetch when `build/spec` is missing.
- **Tests**: `tests/guest/goldfish-pipe.c` (handshake, GLProcessPipe, refusal of an unknown service, EAGAIN with nothing pending), `tests/guest/goldfish-as.c` (the allocator's ping, allocate, two shared mappings, free, unknown ioctl, and the sync fence). Expectations: `tests/expect/goldfish-pipe.out`, `tests/expect/goldfish-as.out`. Both run as expectation cases (`PROP_CASES`), since the builder VM has no goldfish device to act as an oracle.
- **Removed**: `engine/rc.c` and `engine/rc.h`, the hand-written handshake, replaced by the generated decoder.

### Protocol facts checked against the sources

- **Opcodes**: the generator's numbers for all 788 calls equal the guest's `renderControl_opcodes.h`, `gl2_opcodes.h` and `gl_opcodes.h` (base 10000, 2048 and 1024). `gl_ranges.h` gives the ranges the dispatcher uses.
- **Request sizes**: 512 fixed request sizes equal the `sizeWithoutChecksum` constants of the guest encoders (`renderControl_enc.cpp`, `gl2_enc.cpp`, `gl_enc.cpp`). Zero mismatches. The check is `python3 -I tools/gen-glproto.py ... --check-guest`.
- **Wire rules** (`gles2_dec.cpp`, `glGetShaderiv` and `glShaderSource`): a scalar takes its `.types` width in bytes (`GLboolean` is one byte, as `Unpack<GLboolean,uint8_t>` shows). An `in` or `inout` pointer is a `uint32` length and the bytes. An `out` pointer is a `uint32` length only. The reply is the out bytes in order, then the return value.
- **Pipe** (`qemu_pipe_guest.cpp`, `HostConnection.cpp`): open `O_RDWR|O_NONBLOCK`, write `pipe:<service>\0`. The `opengles` stream starts with a zero `clientFlags` word.
- **renderControl values** (`RenderControl.cpp`): renderer version 1; `rcGetFBParam` (width, height, 72 dpi, 60 fps, swap interval 1); string calls return the length with its NUL, or `-len` when the buffer is too small.
- **EGL configs** (`FbConfig.cpp`): the 34 attributes, in the host's order. The config row is preceded by the attribute-id row.
- **Address space** (`goldfish_address_space_android.impl`): PING with metadata = subdevice (the first ping); ALLOCATE_BLOCK (24 bytes, the size is rounded to pages); PING ALLOCATE_ID (metadata 1) and UNALLOCATE_ID (2); `mmap(fd, offset)`.
- **Sync** (`system/egl/goldfish_sync.h`): `goldfish_sync_ioctl_info` is 24 bytes; the fence fd is at offset 16.

### Deviations from `docs/graphics.md`

1. **EGL_KHR_surfaceless_context is not advertised.** RenderEngine takes the surfaceless path with it and calls `eglMakeCurrent(no surface)`. The guest's `libEGL_emulation` (`egl.cpp:1896`) rejects that with `EGL_BAD_MATCH`, which aborts SurfaceFlinger. Without it, RenderEngine uses a stub pbuffer (`GLESRenderEngine.cpp:288-299`), which works. `EGL_ANDROID_native_fence_sync` is advertised.
2. **GLES1 addon calls are not decoded.** `gles1.addon` (the `*Offset` and `*Data` calls) is numbered by a `gl.in` that is not in the tree: the guest's numbers for them differ from any order the tree gives. Those calls are listed as skipped (12 calls).
3. **DMA calls are not decoded.** `rcUpdateColorBufferDMA` and `rcReadColorBufferDMA` (`var_flag ... DMA`) carry a 64-bit guest address in place of the data. The DMA transport is not advertised, so the guest does not send them. The generator skips them, and the size check excludes them.
4. **Calls decoded and skipped** (`build/gen/gen_report.txt`): 630 decoded, of which 524 are plain and 106 are `flag custom_decoder` (their wire is generic, and the backend supplies the call; the null backend answers some of them, the names, the shader and program queries and `glGetIntegerv`, and the rest by default). 170 skipped: 90 `flag unsupported`, 66 `not_api` (both never sent by the guest), 12 GLES1 addon calls (deviation 2) and 2 DMA calls (deviation 3). No pointer return values and no pointers to pointers occur among them.
5. **The specs are not copied into the repo, and the generated code is not checked in.** `vulkan-cereal`'s `METADATA` says `license_type: BY_EXCEPTION_ONLY` (special licensing), and its `.in`, `.attrib` and `.types` files carry no license header. The plan asked for Apache-2.0 copies with headers, which I could not confirm. `tools/fetch-remote-protocols.sh` reads them from the builder into `build/spec/`, and `make` generates `build/gen/`. **This needs a decision.** The repo has no NOTICE file, so none was updated.
6. **Product mechanics differ from the plan.** `PRODUCT_PACKAGES_REMOVE` does not exist in this tree, and a `filter-out` at the top of `xiaoji_arm64.mk` does nothing: `inherit-product` expands lazily (`build/make/core/node_fns.mk`, `@inherit:` tags). The two goldfish makefiles are therefore forked. The device manifest fragment was needed for the VINTF check.
7. **`vndservicemanager` was added** (not in the plan). The composer and the other vendor HALs look services up on `/dev/vndbinder`, and the product does not inherit `base_vendor.mk`, which lists it (`build/make/target/product/base_vendor.mk:76`). Without it the composer blocks in `getContextObject`.
8. **Display**: 720x1280 at 72 dpi, 60 fps, the B4f NoOp composer's size. One EGL config: RGBA8888, depth 24, stencil 8, window and pbuffer surfaces, ES2 and ES3.
9. **GLES state**: `glGetIntegerv` answers the ES 3.0 limits the compositor reads, from a table, written only where the guest's buffer has room. `glGenTextures` and the other `glGen*` calls return new names. `glCreateShader` and `glCreateProgram` return names. Compile and link report success with no info log. `glShaderString` (the spec's `not_api`, decoded by hand as the host does) is accepted and dropped. Every other GLES call is the generated no-op default and logs once (`xiaoji: gl: <call> answered by the default`).
10. **Advertised extensions** (`GL_EXTENSIONS`): `GL_OES_EGL_image_external_essl3` and the three `ANDROID_EMU_` flags of the design (`gles_max_version_3_0`, `native_sync_v2`, `gles_no_host_error`). Nothing else.

### Null backend: the defaults, and what gets an answer

Answered by `engine/gl_null.c`: `rcGetRendererVersion`, `rcGetFBParam`, `rcGetEGLVersion`, `rcQueryEGLString`, `rcGetGLString`, `rcGetNumConfigs`, `rcGetConfigs`, `rcChooseConfig`, `rcCreateContext`, `rcDestroyContext`, `rcCreateWindowSurface`, `rcDestroyWindowSurface`, `rcMakeCurrent`, `rcCreateColorBuffer`, `rcCreateColorBufferDMA`, `rcCloseColorBuffer`, `rcUpdateColorBuffer`, `rcReadColorBuffer`, `rcCreateClientImage`, `rcFBPost` (counted; the first three frames are logged), `rcCreateSyncKHR`, `rcIsSyncSignaled`, `rcClientWaitSyncKHR`, `rcDestroySyncKHR`, and the GLES state and name calls above. Colour buffers keep their pixels (RGBA8888, unsigned byte; other formats are refused), so `rcUpdateColorBuffer` and `rcReadColorBuffer` copy rows, as the plan asked. Every pointer write is bounded by the wire length.

Still answered by the default in the long product boot (`build/tests/boot/long2.err`, listed once each): `glAttachShader`, `glBindAttribLocation`, `glBindTexture`, `glCompileShader`, `glEnableVertexAttribArray`, `glGetError`, `glGetUniformLocation`, `glLinkProgram`, `glPixelStorei`, `glTexImage2D`, `glUniformMatrix4fv`, `glUseProgram`, `rcCreateDisplayById`, `rcOpenColorBuffer2`, `rcSetDisplayPoseDpi`, `rcSetPuid`, `rcSetWindowColorBuffer`. No frame is posted in these runs.

### Tests (final, on the sources of this step)

- `make test` (default tree, `build/rootfs`): 188 passed, 0 failed.
- `XIAOJI_BOOT=1 make test`: 189 passed, 0 failed. The boot check requires the allocator's registration, SurfaceFlinger's main thread, RenderEngine's GLES start and no `is not decoded` line. On the product tree it also requires `StartDisplayManager took to complete` and no `Since 'SurfaceFlinger' could not be found` line (B5).
- `XIAOJI_JIT=0 make test`: 188 passed, 0 failed.
- `make test-web`: 7 checks passed.
- Product tree, `XIAOJI_ROOT=build/rootfs-xiaoji XIAOJI_BOOT=1 tests/run.sh`: 155 passed, 0 failed. The builder-VM oracle cases are skipped (they compare with the Google image's files), and `binder-driver` is skipped (its binary is only in the Google image).
- New cases in this step: `goldfish-pipe` and `goldfish-as` (expectations, engine and Node), and `fabd-scalar` and `fp-scalar3` (native oracle, engine and Node).
- Warnings: `clang -std=c11 -Wall -Wextra` and `emcc` (one file at a time) report 0 for the changed engine files and the four generated files. The guest tests are clean under `gcc -Wall -Wextra`.
- Runs go through `rtk proxy`: the default output filter truncated some make logs.

### Open

- The composer's hotplug callback and SurfaceFlinger (B5). This is the blocker for checkpoint 4.
- **The rest of the plan's step 5**: the 3.2 subset is answered, but colour buffers store RGBA8888 only, and the rest of the GLES draw path is the no-op default.
- **The service user model** (B5): `idmap2d`, `system_server` and `lmkd` run as root. `System now ready` needs init and the credential checks to model the stock users. `lmkd` exits (no PSI), but ActivityManager retries it and continues.
- **The binder fix has no guest test** with the kernel as oracle (the plan asked for one; B5 explains what it needs).
- **The specs' licensing** (deviation 5), and the NOTICE file.
- **The Google image** (`build/rootfs`): it has no `vndservicemanager`, so its composer 2.4 does not register and SurfaceFlinger does not connect its display. The product tree does this part.



## G2: the WebGL2 backend (web build), the frame path, and the boot's state (status, 2026-11-24)

Status: **the WebGL2 backend runs in headless Chromium. Its context is made on the worker's OffscreenCanvas, RenderEngine
sets up EGL and GLES on it, and its shaders compile and link. No Android frame has been composed in any run: this tree's boot
never draws a window, so SurfaceFlinger has nothing to compose (checkpoint 3 of the plan is not reached).** The frame path is
verified with a synthetic frame: the engine draws with GL calls fed through the wire decoder, posts the frame with `rcFBPost`,
and the boot page shows it on its own canvas. Screenshots: `build/web-shots/glselftest.png` (the synthetic frame, over the boot
screen), `build/web-shots/android-frame.png` (the boot screen when the boot frame check ran).

### What was built (where)

- `engine/gl_webgl.c`: the backend. Compiled only with `XIAOJI_WEB`; the native and Node builds get `gl_host_webgl() == NULL`.
  It copies the null backend's tables (`gl_host_null()`, `engine/gl_null.c`) and replaces the entries it implements (about 120
  GLES 2/3 entries, 22 renderControl entries, and the 6 custom decodes). The synthetic frame (`xj_gl_selftest`, below) is in it.
- `engine/gl_host.c`, `engine/gl_host.h`: the dispatch chooses the backend per request (`gl_host_current()`: the selected one, or
  null when it is not available). `--gl null|webgl` (main.c) selects it; the default is null. The four addon calls the guest
  encoder sends as `*Data` and `*Offset` (`glVertexAttribPointerData`, `glVertexAttribPointerOffset`, `glDrawElementsData`,
  `glDrawElementsOffset`) are decoded by hand, with exact length checks. The generator marks them `not_api`, but the guest sends
  them (`gl2_enc.cpp`), so a client-array draw used to break the stream.
- `engine/goldfish.c`: the `refcount` pipe records the color-buffer handles written to it and releases them when it closes
  (`goldfish_free`), which is the release of the allocator's creation reference (see the color buffers below).
- `engine/display.h`: the one display setting (`XJ_DISPLAY_W`, `XJ_DISPLAY_H`, `XJ_DISPLAY_DPI`), 540x960 at 240 dpi. It has the same
  shape as the input branch's `engine/display.h` (720x1280), which the coordinator reconciles at merge; the size chosen here is 540x960 for speed. `gl_null.c` and `gl_webgl.c` report it through `rcGetFBParam`
  (FB_WIDTH, FB_HEIGHT, FB_XDPI, FB_YDPI), and `props.c` sets `ro.sf.lcd_density` from it. It was 720x1280 at 72 dpi, which is
  also what the native boot reports now. The native log confirms the framework reads it: `RanchuHwc: initializing display:0 with
  config: w:540 h:960 dpi-x:240 dpi-y:240`, and WindowManager's configuration reads 240dpi, 540x960.
- `web/worker.js`: for the boot (`opts.gl === 'webgl'`) sets `self.xjCanvas` (an OffscreenCanvas, 540x960) and `self.xjFrameCtl`
  (a shared Int32Array, see the frame path), and passes `--gl webgl`. The message `glselftest` runs the synthetic frame.
- `web/main.js`, `web/frame.js`, `web/index.html`: the boot page sends `gl: webgl` (or `?gl=null`) and the shared counter. A frame
  is drawn on `#android-screen`, a canvas over the phone frame. The canvas is hidden at each boot start and until the first frame; it
  takes no pointer events, and the bottom bar (with Stop) is layered above it. `window.xb.frames` holds the count, the first and
  last frame times, the size, and the number of distinct colours in a sample.
- `tests/web.mjs`: with `XIAOJI_WEB_FRAMES=1`, `make test-web` runs the synthetic-frame check (`runGlSelfTest`) and the boot's frame
  check (`runFrames`, which waits up to `XN_FRAME_TIMEOUT_MS`, default 900 s). `make test-web` builds the boot's chunk store for it,
  as `XIAOJI_WEB_BOOT=1` does.
- Additions beyond the plan: `--gl` and `?gl=null`, the exported self test (`xj_gl_selftest`, `xj_gl_selftest_alias`,
  `xj_gl_selftest_quad`, `xj_gl_selftest_life`) with the worker's `glselftest` message, the `refcount` release, and the sampler
  objects (below). The display change also applies to the native boot, and `goldfish-pipe`'s expectation changed with it.

### Context multiplexing

- The decoder's context is the pipe stream (`gfpipe_t *`), one per guest thread. `stream_slot()` maps it to the guest EGL context
  made current on it (`rcMakeCurrent`); a stream with none uses a default context.
- A guest context (`gctx_t`) holds what the engine can rebind: its draw and read surfaces, its framebuffer name (0 is the draw
  surface's color buffer), program, VAO, the active texture unit and the texture on each unit (a guest name), viewport and scissor.
  Every GLES call starts with `enter()`: when the stream's context is not the one applied to the WebGL2 context, the tracked state
  is rebound (`apply()`). `rcMakeCurrent` re-applies, so a surface change takes effect at once.
- Not switched (a known gap): the enables (blend, depth, stencil, cull, colour mask), the texture units other than the active one,
  the pixel store state, the sampler bindings, and the scratch buffers for the client-array calls (one per attribute index, shared
  by all contexts). The engine's own calls save and restore the host bindings (`save_host`/`restore_host`), so colour buffer
  uploads and read-backs leave the guest's state as it was.
- Names: a guest object name is the host name, because the host allocates it in `glGen*`. WebGL has one namespace, so every context
  sees every object by name. The share-group argument of `rcCreateContext` is stored but not used: no per-share-group table is
  built, which deviates from the plan. Two guest objects never share a name, so the table is not needed yet.
- Stream slots are never released (64 slots; a stream beyond them uses the default context). A closed pipe keeps its slot.

### Wire data and alignment

- The guest's values sit at packet offsets, so they are not necessarily 4-byte aligned, and emscripten's GL glue reads and writes
  32-bit and float arguments through typed arrays (`HEAP32[p >> 2]` and so on). The backend copies such data into aligned memory
  around the call: uniform vectors and matrices, `glVertexAttrib4fv`, the generated names (`glGen*`, `glDelete*`), and texel data
  when its address is misaligned (`copy_in`, `gen_into`, `delete_from`). Byte data (`glBufferData`, RGBA8 pixels) needs no copy.
- The self test (below) sends its matrix at packet offset 21, so the path is exercised.

### Color buffers, surfaces and aliases

- A color buffer is an RGBA8 texture with its own FBO (`cb_create`). `rcCreateColorBuffer` and `...DMA` create one with one
  reference (RGBA8 only, as in the null backend). `rcOpenColorBuffer` and `rcOpenColorBuffer2` add a reference, and
  `rcCloseColorBuffer` removes one; the buffer is deleted with its last reference.
- **The guest's references** (`system/gralloc/gralloc_30.cpp`, `system/egl/egl.cpp`): the allocator creates a colour buffer and
  writes its handle to a `refcount` pipe, and closing that pipe releases the creation reference (`goldfish.c` does this in
  `goldfish_free`). Each process that imports a buffer opens it (`rcOpenColorBuffer2`) and closes it when it unregisters. So a
  buffer lives while any process holds it, and the creation reference does not fill the 256 slots.
- A window surface holds a color buffer (`rcSetWindowColorBuffer`). A surface without one gets its own, of its size, when first
  drawn to. The guest's framebuffer 0 maps to the draw surface's FBO. A surface's color buffer has no depth or stencil attachment,
  although the EGL config advertises 24 and 8 bits: RenderEngine's composition was the only use checked.
- `rcUpdateColorBuffer` and `rcReadColorBuffer` are `texSubImage2D` and `readPixels` on the color buffer, with unpack and pack
  state set to tight rows, restored after. `rcFlushWindowColorBuffer` is `glFlush`.
- **Gralloc images, as the guest uses them** (`system/GLESv2/gl2.cpp`, `glEGLImageTargetTexture2DOES` with an
  `EGL_NATIVE_BUFFER_ANDROID` image): the guest sends no image call to the host. It calls `rcBindTexture(cb)` on the texture it has
  bound, and `rcBindRenderbuffer(cb)` for a renderbuffer. So `rc_BindTexture` makes the guest name bound on the active unit stand
  for the colour buffer (`tex_alias`; every later bind of the name binds the colour buffer's texture), and `rc_BindRenderbuffer`
  does the same for the bound renderbuffer (`rb_alias`). `glFramebufferRenderbuffer` attaches the colour buffer's texture for such
  a name, because WebGL cannot alias a renderbuffer to a texture.
- **GL_TEXTURE_2D images** (`eglCreateImageKHR` with a texture, `egl.cpp`): `rcCreateClientImage` receives the guest's texture name
  (not a colour buffer handle). `glEGLImageTargetTexture2DOES` then makes the bound name an alias of that texture.
- The external texture target (`GL_TEXTURE_EXTERNAL_OES`) is `GL_TEXTURE_2D` here.

### GLES opcode coverage

- Overridden GLES 2/3 entries: state and enables, buffers, 2D textures, FBOs, renderbuffers, shaders and programs (with the status
  and info-log queries), uniforms and attributes, VAOs, sampler objects, `glDrawArrays`, the client-array and client-index draws,
  `glReadPixels`, `glGetActiveAttrib` and `glGetActiveUniform`, and `glFinish`/`glFlush`. `glGetIntegerv` answers WebGL2's limits,
  except `GL_NUM_EXTENSIONS` (the advertised list, 4), `GL_MAJOR_VERSION` (3), `GL_MINOR_VERSION` (0), and the counts of compressed
  texture formats and shader binary formats (0, so the guest asks for no list).
- Sampler objects matter for HWUI: Skia's `GrGLCaps.cpp` (lines 771 to 779) sets `fUseSamplerObjects` for ES 3.0, so without
  them its textures would use the texture's own state, whose default minification filter needs mipmaps.
- **Not implemented, and on the decoder's skip list** (`build/gen/gen_report.txt`): sync objects (`glFenceSync`,
  `glClientWaitSync`, `glWaitSync`, `glDeleteSync`, `glIsSync`, `glGetSynciv`, and their `AEMU` forms), buffer mapping
  (`glMapBufferRange`, `glUnmapBuffer`, and the `AEMU` and `DMA` forms), the instanced and range draws (`glDrawElementsInstanced`,
  `glDrawRangeElements`), the `*OffsetAEMU` uploads, `glGetCompressedTextureFormats`, and `glGetStringi`. If the guest's encoder
  sends one of these, `gl_host_decode` returns -1, the pipe prints `is not decoded` and is marked broken. Skia's fence-sync use on
  ES 3.0 (`GrGLAssembleWebGLInterfaceAutogen.cpp`) makes `glFenceSync` a likely first one once an app draws; that Skia calls
  it on this path was not traced. Fixing this comes before apps render.
- Still the null answers (no draw, each logged once as `answered by the default`): the GLES 1 calls, the 3D and multisample texture
  and buffer calls, transform feedback, queries, program binaries, and compute and image calls. The browser boot listed these
  defaults: `rcSetPuid`, `rcCreateDisplayById`, `rcOpenColorBuffer2`, `rcSetDisplayPoseDpi`, and (before they were implemented)
  `glGetActiveUniform` and `glGetActiveAttrib`.
- Shaders: the guest sends GLSL ES in `glShaderString`. The backend drops the `#extension GL_OES_EGL_image_external...` lines and reads
  `samplerExternalOES` as `sampler2D`. WebGL2 compiles `#version 100` and `#version 300 es` as they are. A failed compile or link is
  logged with its info log. The browser boot logged no failure.
- Advertised: the null backend's strings, with the renderer `xiaoji WebGL2`. The extension list still names
  `GL_OES_EGL_image_external_essl3`, which WebGL2 does not have. The shader translation makes external textures 2D, so the name stays
  for the framework's shader path; this is an inconsistency.
- HWUI's Skia subset is not established. The synthetic frame exercises RenderEngine's calls and the sampler and alias paths; Skia's
  full set, and its sync use, have not been run.

### The frame path

- `rcFBPost(cb)` is SurfaceFlinger's composed frame. HostComposer reaches it through its fallback to `post`
  (`hwc2/HostComposer.cpp`, when the host advertises no host composition, which this build does not). If the page has taken the
  last frame, the colour buffer is read back (`glReadPixels` on its FBO), the rows are turned top-first, the alpha is set to 255, and
  the bytes go to the page (`xj_frame_send`: a copy, transferred with `postMessage`). Otherwise the frame is dropped before the read,
  and the drop is logged every 300 frames.
- Throttle: a shared Int32Array `[posted, taken]`. The engine posts only when they are equal, and the page adds one to `taken` after
  it draws. The worker is busy in the engine, so it cannot receive an acknowledgement message; the counter is read synchronously
  instead. The engine reads the array through `globalThis`, and a pointer is taken as unsigned (`>>> 0`), since the wasm heap
  addresses reach JavaScript as signed 32-bit values.
- The page draws the frame on `#android-screen` (a 2D canvas, sized to the frame), over the phone frame.

### The synthetic frame (self test)

- `xj_gl_selftest` (web build, run by a `glselftest` worker message from `tests/web.mjs`) makes a context, a surface and a colour
  buffer, and then: a clear; a scissored clear; a quad drawn through the wire decoder (`gl_host_decode`) from hand-built packets,
  with two shaders (`#version 300 es`), a shader-string upload, compile and link, `glVertexAttribPointerData`, a
  `glUniformMatrix4fv` whose matrix sits at packet offset 21, and `glDrawArrays`; a colour buffer's reference counts; a texture
  name bound and attached to a guest FBO through `rcBindTexture`; and `rcFBPost`.
- The checks (all pass, in a headless Chromium run): one frame arrives at 540x960 on `#android-screen`; the clear colour at (10, 10) is
  (26, 128, 230); the scissored colour inside the box and outside the quad, at (115, 800), is (255, 51, 51); the quad is green at
  the centre, read from the frame and from the surface; the alias path reads back (230, 230, 26); the colour buffer's references
  count and the last close deletes; and Stop is still the element at its centre over the canvas.

### Verification (this worktree, 2026-11-24)

- `make` (native), `make wasm-node` and `make wasm` build. `clang -std=c11 -Wall -Wextra` and `emcc -std=c11 -Wall -Wextra` report no
  warning for the changed engine files, without the `-Wno-unused-parameter` of the Makefile's CFLAGS.
- `tests/run.sh` (the last full run, after the refcount, sampler and alignment changes; before the `engine/display.h` rename, which
  kept the values, and the canvas rename, which is web-only): 204 passed, 42 failed. The failures:
  - 28 lines are `no oracle output`: this worktree has no `build/oracle` for the toybox and shell cases (the main checkout has
    them). The `hello-*` oracle files were copied in for the web checks, and those cases pass.
  - `attrs`, `nproc-root`, `pidfd`, `simd` and `threads-many`. These have no source in `tests/guest` in this tree, except `simd` (which
    has one). For `nproc-root-O0`, `threads-many-O0`, `pidfd-O0` and `simd-O0`, the engine at HEAD and this engine give identical
    stdout and stderr in a scratch root. `pidfd`'s syscall 434 and `simd`'s `SSRA` (`0x4f0d1420`) are not implemented at HEAD.
    The `attrs` binary has no source here, so it is from another tree.
  - `goldfish-pipe` (the display setting, 540, 960, 240 dpi) passes for the engine and for Node, with the updated expectation.
- `make test-web` (default: `hello-jit` and `hello-int` against their oracle files): both pass, with stdout and exit codes matching.
- `XIAOJI_WEB_FRAMES=1 XN_FRAME_TIMEOUT_MS=300000 make test-web`: the synthetic-frame checks pass. The boot frame check (`runFrames`)
  fails three of its four checks: no frame arrived in 300 s, so the page's frame is still the boot screen (its screenshot is
  `build/web-shots/android-frame.png`). `make` exits with status 2, which is the failure count.
- `XIAOJI_WEB_BOOT=1 make test-web` (the Boot Android check, pack mode, the page's default, with the WebGL backend): all checks
  pass. The boot reaches "Starting phase 100" in 44 s of wall time.
- Browser, the Android boot in pack mode (`?gl=webgl`, 300 s, the product tree), read from the log: the backend is ready
  (`the WebGL2 backend is ready`), RenderEngine reports `renderer: xiaoji WebGL2`, and SurfaceFlinger sets the display's power
  mode. Phase 100 at 42 s, phase 200 at 61 s, `System now ready` at 89 s, phase 600 at 112 s, and `FallbackHome` starts at 120 s.
  Then the app processes abort: `com.android.se`, the phone process and the settings process each log
  `pthread_create failed: clone failed: Try again` (EAGAIN from the engine's `clone`) and abort. No window is drawn, so no frame
  is composed. The native run (null backend) logs the same `clone failed` lines (22 of them), so this limit is in the engine's
  thread creation, not in the GL path. It was not diagnosed.
- Browser, the earlier run in manifest mode (`?boot=manifest`, 200 s, the same product tree): the backend is ready and RenderEngine
  reports `renderer: xiaoji WebGL2`; no frame arrived, and no shader compile or link failed.
- Native boot (null backend, `build/rootfs-xiaoji`, 480 s): RenderEngine and the display come up, `FallbackHome` starts, and
  `SystemUIFactory` fails to build. No `frame ... posted` line appears in any run of this tree.

### Performance

- Frames per second and time to the first Android frame: not measured, because no Android frame was composed. The synthetic frame's
  cost (one readback of 540x960x4 bytes, about 2 MB, and one copy to the page) was not timed.
- The browser and native boots were not timed against each other, so no speed ratio is given.

### Known gaps

1. **No Android frame.** SurfaceFlinger composes only with a layer. In this tree no window is drawn: SystemUI fails to build, and
   FallbackHome does not draw. This is the boot's state (docs/boot.md), not the backend's. The boot frame check therefore fails in
   this tree.
2. **The app process abort** (`clone` returns EAGAIN, about 120 s into the browser boot, after `System now ready`): the same lines
   appear in the native run, so it is the engine's thread creation, not the GL path. It is not diagnosed, and it is the first thing
   to fix before apps can draw.
3. **Sync objects, mapping, instanced and range draws, and the other skipped calls** (above): the first will be needed by Skia.
4. **Per-context state**: the enables, the other texture units and the sampler bindings are shared. Stream slots are not released.
5. **Share groups**: not mapped (names are host names; the share argument is ignored).
6. **Extensions**: `GL_OES_EGL_image_external_essl3` is advertised without WebGL2 support (above).
7. **Renderbuffer aliases** are attachments only, and a renderbuffer alias cannot be written in place.
8. **Surface depth and stencil** are not attached (above).
9. **GLES 1, queries, program binaries, compute and image calls**: the null defaults.
10. **Native GLES**: not built (the plan's optional step).
