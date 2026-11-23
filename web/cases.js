// The ART cases of the web page. The argv and the guest environment are the ones
// tests/run.sh (rootfs_argv) and tools/guest-env.sh use for the native and Node runs.
// tests/web.mjs checks ENGINE_ENV against tools/guest-env.sh, so the two cannot drift.

export const GUEST_ENV = {
  ANDROID_ROOT: '/system',
  ANDROID_DATA: '/data',
  ANDROID_ART_ROOT: '/apex/com.android.art',
  ANDROID_I18N_ROOT: '/apex/com.android.i18n',
  ANDROID_TZDATA_ROOT: '/apex/com.android.tzdata',
  BOOTCLASSPATH: [
    '/apex/com.android.art/javalib/core-oj.jar',
    '/apex/com.android.art/javalib/core-libart.jar',
    '/apex/com.android.art/javalib/okhttp.jar',
    '/apex/com.android.art/javalib/bouncycastle.jar',
    '/apex/com.android.art/javalib/apache-xml.jar',
    '/system/framework/framework.jar',
    '/system/framework/framework-graphics.jar',
    '/system/framework/ext.jar',
    '/system/framework/telephony-common.jar',
    '/system/framework/voip-common.jar',
    '/system/framework/ims-common.jar',
    '/apex/com.android.i18n/javalib/core-icu4j.jar',
  ].join(':'),
};

// The guest environment as engine arguments (--env K=V).
export const ENGINE_ENV_ARGS = Object.entries(GUEST_ENV).flatMap(([k, v]) => ['--env', `${k}=${v}`]);

const DALVIKVM = '/apex/com.android.art/bin/dalvikvm64';
const IMAGE = ['-Ximage:/system/framework/boot.art:/system/framework/boot-framework.art'];

// The Android boot of the "Boot Android" mode (W1): the native product boot (docs/boot.md, B7),
// `xiaoji --logcat --init image/xiaoji.rc --root build/rootfs-xiaoji --init-timeout 90`, on the
// product tree. The page's files: /xiaoji.rc is image/xiaoji.rc, and the tree is served under
// /rootfs-xiaoji/ with its manifest. timeoutS is the engine's wall-clock --init-timeout; the page
// allows longer, because the browser is slower than native (the page's Stop button ends a run).
// Each milestone is matched against every logd line (the text after the engine's stamp), and reported
// once, when its line first appears. The table is in the order of the native boot, not of the log
// (the sensors and health HALs come before ART). The strings are the ones the native run prints.
export const BOOT = {
  rc: 'xiaoji.rc',
  manifest: 'manifest-xiaoji.json',
  tree: 'rootfs-xiaoji',
  timeoutS: 900,
  milestones: [
    { id: 'art', label: 'ART up: zygote runs ZygoteInit',
      re: /AndroidRuntime\(\d+\): >>>>>> START com\.android\.internal\.os\.ZygoteInit/ },
    { id: 'preload', label: 'class preload done',
      re: /Zygote\(\d+\): \.\.\.preloaded \d+ classes/ },
    { id: 'sensors', label: 'sensors HAL: qemud mask 6f',
      re: /host sensors mask=6f/ },
    { id: 'health', label: 'health HAL registered',
      re: /Registered android\.hardware\.health@2\.1::IHealth\/default/ },
    { id: 'sf', label: 'SurfaceFlinger main thread ready',
      re: /SurfaceFlinger's main thread ready to run/ },
    { id: 'forked', label: 'system_server forked',
      re: /Zygote\(\d+\): System server process \d+ has been created/ },
    { id: 'entry', label: 'system_server entered',
      re: /SystemServer\(\d+\): Entered the Android system server!/ },
    { id: 'phase100', label: 'Starting phase 100',
      re: /SystemServiceManager\(\d+\): Starting phase 100/ },
    { id: 'battery', label: 'BatteryService: health update received',
      re: /BatteryService\(\d+\): health: Waited \d+ms and received the update/ },
    { id: 'fatal', label: 'system_server FATAL EXCEPTION (the next blocker, B7)',
      re: /FATAL EXCEPTION IN SYSTEM PROCESS/ },
  ],
};

export const CASES = {
  'hello-jit': {
    label: 'Hello, translator on',
    expect: 'build/oracle/hello-jit.out',
    argv: [DALVIKVM, ...IMAGE, '-cp', '/data/hello.dex', 'Hello'],
  },
  'hello-int': {
    label: 'Hello, interpreter only (-Xint)',
    expect: 'build/oracle/hello-int.out',
    argv: [DALVIKVM, '-Xint', ...IMAGE, '-cp', '/data/hello.dex', 'Hello'],
  },
};
