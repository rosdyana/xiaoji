# xiaoji: forked from build/make/target/product/generic_system.mk (android13-release).
# Removed packages: LiveWallpapersPicker, PartnerBookmarksProvider, Stk, Tag, com.android.nfc_extras.
# They run persistent processes or features a browser tab has no use for
# (NFC, SIM toolkit, secure element, printing, dreams, VR, ...).

#
# Copyright (C) 2018 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# This makefile is the basis of a generic system image for a handheld device.
$(call inherit-product, device/xiaoji/xiaoji_handheld_system.mk)
# telephony_system.mk is not inherited (B16): no telephony. It brings ONS, which runs in the com.android.phone process
# and restarted in a loop with no radio (boot9: "has crashed too many times"), CarrierDefaultApp, CallLogBackup, the
# cell broadcast APEX and CellBroadcastLegacyApp.
$(call inherit-product, $(SRC_TARGET_DIR)/product/languages_default.mk)
# Add adb keys to debuggable AOSP builds (if they exist)
$(call inherit-product-if-exists, vendor/google/security/adb/vendor_key.mk)

# Enable updating of APEXes
$(call inherit-product, $(SRC_TARGET_DIR)/product/updatable_apex.mk)

# Shared java libs
PRODUCT_PACKAGES += \

# Applications
PRODUCT_PACKAGES += \

# OTA support
PRODUCT_PACKAGES += \
    recovery-refresh \
    update_engine \
    update_verifier \

# Wrapped net utils for /vendor access.
PRODUCT_PACKAGES += netutils-wrapper-1.0

# Charger images
PRODUCT_PACKAGES += charger_res_images

# system_other support
PRODUCT_PACKAGES += \
    cppreopts.sh \
    otapreopt_script \

# For ringtones that rely on forward lock encryption
PRODUCT_PACKAGES += libfwdlockengine

# System libraries commonly depended on by things on the system_ext or product partitions.
# These lists will be pruned periodically.
PRODUCT_PACKAGES += \
    android.hardware.biometrics.fingerprint@2.1 \
    android.hardware.radio@1.0 \
    android.hardware.radio@1.1 \
    android.hardware.radio@1.2 \
    android.hardware.radio@1.3 \
    android.hardware.radio@1.4 \
    android.hardware.radio.config@1.0 \
    android.hardware.radio.deprecated@1.0 \
    android.hardware.secure_element@1.0 \
    android.hardware.wifi@1.0 \
    libaudio-resampler \
    libaudiohal \
    libdrm \
    liblogwrap \
    liblz4 \
    libminui \
    libnl \
    libprotobuf-cpp-full \

# These libraries are empty and have been combined into libhidlbase, but are still depended
# on by things off /system.
# TODO(b/135686713): remove these
PRODUCT_PACKAGES += \
    libhidltransport \
    libhwbinder \

PRODUCT_PACKAGES_DEBUG += \
    avbctl \
    bootctl \
    tinycap \
    tinyhostless \
    tinymix \
    tinypcminfo \
    tinyplay \
    update_engine_client \

PRODUCT_HOST_PACKAGES += \
    tinyplay

# Enable configurable audio policy
PRODUCT_PACKAGES += \
    libaudiopolicyengineconfigurable \
    libpolicy-subsystem

# Include all zygote init scripts. "ro.zygote" will select one of them.
PRODUCT_COPY_FILES += \
    system/core/rootdir/init.zygote32.rc:system/etc/init/hw/init.zygote32.rc \
    system/core/rootdir/init.zygote64.rc:system/etc/init/hw/init.zygote64.rc \
    system/core/rootdir/init.zygote64_32.rc:system/etc/init/hw/init.zygote64_32.rc \

# Enable dynamic partition size
PRODUCT_USE_DYNAMIC_PARTITION_SIZE := true

PRODUCT_ENFORCE_RRO_TARGETS := *

PRODUCT_NAME := generic_system
PRODUCT_BRAND := generic

# Define /system partition-specific product properties to identify that /system
# partition is generic_system.
PRODUCT_SYSTEM_NAME := mainline
PRODUCT_SYSTEM_BRAND := Android
PRODUCT_SYSTEM_MANUFACTURER := Android
PRODUCT_SYSTEM_MODEL := mainline
PRODUCT_SYSTEM_DEVICE := generic

# xiaoji_netd (docs/boot.md, B12) registers the "netd" binder service in place of the stock netd.
PRODUCT_PACKAGES += xiaoji_netd

_base_mk_allowed_list :=

_my_allowed_list := $(_base_mk_allowed_list)

# For mainline, system.img should be mounted at /, so we include ROOT here.
_my_paths := \
  $(TARGET_COPY_OUT_ROOT)/ \
  $(TARGET_COPY_OUT_SYSTEM)/ \

$(call require-artifacts-in-path, $(_my_paths), $(_my_allowed_list))
