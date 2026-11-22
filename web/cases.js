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
