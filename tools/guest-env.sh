# Environment for running ART in the guest, sourced by the guest's /system/bin/sh.
# Normally init exports these (init.environ.rc) and derive_classpath computes
# BOOTCLASSPATH at boot. The classpath order must match the boot image in
# /system/framework/arm64 (ART jars, then framework jars, then ICU).
export ANDROID_ROOT=/system
export ANDROID_DATA=/data
export ANDROID_ART_ROOT=/apex/com.android.art
export ANDROID_I18N_ROOT=/apex/com.android.i18n
export ANDROID_TZDATA_ROOT=/apex/com.android.tzdata
A=/apex/com.android.art/javalib
F=/system/framework
export BOOTCLASSPATH=$A/core-oj.jar:$A/core-libart.jar:$A/okhttp.jar:$A/bouncycastle.jar:$A/apache-xml.jar:$F/framework.jar:$F/framework-graphics.jar:$F/ext.jar:$F/telephony-common.jar:$F/voip-common.jar:$F/ims-common.jar:/apex/com.android.i18n/javalib/core-icu4j.jar
