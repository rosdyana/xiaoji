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
$(call inherit-product, $(SRC_TARGET_DIR)/product/generic_system.mk)

# system_ext: SystemUI, Launcher3QuickStep, Settings (no telephony_system_ext).
$(call inherit-product, $(SRC_TARGET_DIR)/product/handheld_system_ext.mk)

# product: media only (no handheld_product/aosp_product app bundle).
$(call inherit-product, $(SRC_TARGET_DIR)/product/media_product.mk)

# vendor: the arm64 emulator's vendor side, unchanged for now.
$(call inherit-product, device/generic/goldfish/arm64-vendor.mk)
$(call inherit-product, device/generic/goldfish/64bitonly/product/emulator64_vendor.mk)
$(call inherit-product, device/generic/goldfish/emulator64_arm64/device.mk)

PRODUCT_PROPERTY_OVERRIDES += \
    ro.config.low_ram=true \
    debug.sf.nobootanimation=1 \
    dalvik.vm.heapgrowthlimit=128m \
    dalvik.vm.heapsize=256m

PRODUCT_BRAND := Android
PRODUCT_NAME := xiaoji_arm64
PRODUCT_DEVICE := xiaoji_arm64
PRODUCT_MODEL := xiaoji
