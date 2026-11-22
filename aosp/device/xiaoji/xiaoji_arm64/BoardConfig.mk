# The arm64 emulator board, plus two changes for the xiaoji engine:
# - flattened APEXes: plain directories under /system/apex, so the image needs
#   no APEX unpacking step;
# - MALLOC_SVELTE: bionic's low-memory allocator configuration, which reserves
#   far less address space than the default (~21 GiB of PROT_NONE today).
include device/generic/goldfish/emulator64_arm64/BoardConfig.mk

TARGET_FLATTEN_APEX := true
MALLOC_SVELTE := true
