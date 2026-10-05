#
# Copyright (C) 2024 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

DEVICE_PATH := device/huawei/mediapadm5lte

# Inherit from hi3660
include device/huawei/hi3660/BoardConfigCommon.mk

# This device uses the C00 EMUI 9.1 partition layout, not the legacy
# mmcblk0pN assignments in the shared Huawei policy.
BOARD_SEPOLICY_M4DEFS += huawei_mediapadm5lte=true
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy/vendor

# Dynamic partitions (China EMUI 9.1 layout)
# Reuse preload as backing storage; leave prets and pretvs untouched.
BOARD_SUPER_PARTITION_BLOCK_DEVICES += preload
BOARD_SUPER_PARTITION_PRELOAD_DEVICE_SIZE := 8388608
BOARD_SUPER_PARTITION_SIZE := 5058330624
# Reserve 16 MiB for metadata and alignment across all backing devices.
BOARD_HUAWEI_DYNAMIC_PARTITIONS_SIZE := $(shell expr $(BOARD_SUPER_PARTITION_SIZE) - 16777216)

# Assert
TARGET_OTA_ASSERT_DEVICE := mediapadm5lte

# kernel
TARGET_KERNEL_CONFIG += modem.config

# Properties
TARGET_VENDOR_PROP += $(DEVICE_PATH)/vendor.prop

# RIL
BOARD_PROVIDES_LIBRIL := true
ENABLE_VENDOR_RIL_SERVICE := true

# Load the C00 signal bridge before rild's dependencies, including in AT_SECURE
# mode where init's LD_PRELOAD environment would be ignored.
TARGET_LD_SHIM_LIBS += \
    /vendor/bin/hw/rild|/vendor/lib64/libhisi-ril-signal.so

# Root
BOARD_ROOT_EXTRA_FOLDERS += \
    3rdmodem \
    3rdmodemnvm \
    3rdmodemnvmbkp \
    modem_log \
    splash2 \
    hw_odm
    
# Vintf
DEVICE_MANIFEST_FILE += $(DEVICE_PATH)/prebuilts/manifest.xml
