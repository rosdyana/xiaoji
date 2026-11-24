# xiaoji_arm64: Android 13 for the xiaoji engine (browser tab, android.xiaonux.dev).
#
# Starts from the emulator's sdk_phone64_arm64 and keeps a phone UI
# (SystemUI, Launcher3, Settings, PackageInstaller, DocumentsUI), but drops
# what a tab can't use or doesn't need: telephony, the product app bundle
# (Calendar, Contacts, Camera, Gallery, Music, ...), and the boot animation.
# 64-bit only: the engine runs ARM64, not ARM32.

QEMU_USE_SYSTEM_EXT_PARTITIONS := true
PRODUCT_USE_DYNAMIC_PARTITIONS := true
PRODUCT_ENFORCE_ARTIFACT_PATH_REQUIREMENTS := relaxed
PRODUCT_BROKEN_VERIFY_USES_LIBRARIES := true

# system: the GSI system image, 64-bit only.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit_only.mk)
$(call inherit-product, device/xiaoji/xiaoji_system.mk)

# system_ext: SystemUI, Launcher3QuickStep, Settings (no telephony_system_ext). Provision stays: its HOME activity
# writes DEVICE_PROVISIONED and USER_SETUP_COMPLETE, then disables itself (B16).
$(call inherit-product, $(SRC_TARGET_DIR)/product/handheld_system_ext.mk)

# product: media only (no handheld_product/aosp_product app bundle).
$(call inherit-product, $(SRC_TARGET_DIR)/product/media_product.mk)

# vendor: the arm64 emulator's vendor side, unchanged for now.
$(call inherit-product, device/generic/goldfish/arm64-vendor.mk)
$(call inherit-product, device/xiaoji/xiaoji_emulator64_vendor.mk)
$(call inherit-product, device/generic/goldfish/emulator64_arm64/device.mk)

# The engine runs guest code at about 100 million instructions a second on the host (B16), so a service that is quick on a
# device runs past the 20 s service timeout here: SystemUIService's onCreate did, and the ANR killed SystemUI. The timeout
# is ActiveServices.SERVICE_TIMEOUT, scaled by Build.HW_TIMEOUT_MULTIPLIER (ro.hw_timeout_multiplier), the framework's
# knob for slow hardware. This widens the timeouts; it does not make the boot faster.
PRODUCT_PROPERTY_OVERRIDES += \
    ro.hw_timeout_multiplier=4 \
    ro.config.low_ram=true \
    debug.sf.nobootanimation=1 \
    dalvik.vm.heapgrowthlimit=128m \
    dalvik.vm.heapsize=256m

# vndservicemanager: the vendor binder's context manager (/dev/vndbinder). The composer and the other vendor
# HALs look services up there. base_vendor.mk (which lists it) is not inherited by this product.
PRODUCT_PACKAGES += vndservicemanager

# Networks off (B16, docs/boot.md): the features are not declared (permissions/handheld_core_hardware.xml, and
# xiaoji_vendor.mk drops the wifi and bluetooth feature XMLs), so the framework starts no Wi-Fi, Bluetooth, UWB, NFC or
# telephony service. A PRODUCT_PACKAGES filter here does not work: base_system.mk and the goldfish makefiles are
# inherited, and their words are collected from each file, so the packages they add stay (see docs/boot.md B16).
# No framework reader of ro.radio.noril was found in frameworks/base, frameworks/opt/telephony or
# packages/services/Telephony, so the property is set for the request, and has no effect on the platform.
PRODUCT_PROPERTY_OVERRIDES += ro.radio.noril=yes

PRODUCT_BRAND := Android
PRODUCT_NAME := xiaoji_arm64
PRODUCT_DEVICE := xiaoji_arm64
PRODUCT_MODEL := xiaoji
