# xiaoji_lean (docs/demo.md): removes packages that makefiles we inherit unchanged add to PRODUCT_PACKAGES.
#
# A PRODUCT_PACKAGES filter cannot remove an inherited package (docs/boot.md, B16). An override can: the build drops
# every module that an installed module overrides (build/make/core/main.mk, module-overrides), the mechanism that lets
# Launcher3QuickStep replace Launcher3. This module installs one small text file (system_ext/etc/xiaoji_lean.txt, the list,
# for anyone looking at the device; system_ext, because xiaoji_system.mk's artifact path rule keeps product-level
# modules out of /system) and overrides the packages below. Each one starts a process on this product that the demo (launcher,
# SystemUI, Settings, Files, Calculator) does not use; the processes are from `ps` after a boot (docs/demo.md).
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := xiaoji_lean
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_TAGS := optional
LOCAL_LICENSE_KINDS := SPDX-license-identifier-Apache-2.0
LOCAL_LICENSE_CONDITIONS := notice
LOCAL_SRC_FILES := xiaoji_lean.txt
LOCAL_MODULE_STEM := xiaoji_lean.txt
LOCAL_SYSTEM_EXT_MODULE := true
# SecureElement (base_system.mk): process com.android.se; NFC and the secure element are off.
# LocalTransport (base_system.mk): process com.android.localtransport, the backup transport; backup is "never".
# DynamicSystemInstallationService (base_system.mk): com.android.dynsystem and :dynsystem, started by BOOT_COMPLETED.
# Not RemoteProvisioner (com.android.remoteprovisioner, started by BOOT_COMPLETED to fetch attestation keys over a
# network the product does not have): the keymint HAL lists it as a required module, and an override did not remove it
# (tried; docs/demo.md).
# MultiDisplayProvider (also in build/make/target/product/emulator_system.mk): the emulator's multi-display app.
LOCAL_OVERRIDES_MODULES := \
    SecureElement \
    LocalTransport \
    DynamicSystemInstallationService \
    MultiDisplayProvider
include $(BUILD_PREBUILT)
