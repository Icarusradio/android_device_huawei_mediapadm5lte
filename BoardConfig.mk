#
# Copyright (C) 2024 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

DEVICE_PATH := device/huawei/mediapadm5lte

# Inherit from hi3660
include device/huawei/hi3660/BoardConfigCommon.mk

# Assert
TARGET_OTA_ASSERT_DEVICE := mediapadm5lte

# Display
TARGET_SCREEN_DENSITY := 420

# kernel
TARGET_KERNEL_CONFIG += modem.config

# Properties
TARGET_VENDOR_PROP += $(DEVICE_PATH)/vendor.prop

# RIL
BOARD_PROVIDES_LIBRIL := true
ENABLE_VENDOR_RIL_SERVICE := true
BOARD_USES_LIBRIL_WRAPPER := true

# Root
BOARD_ROOT_EXTRA_FOLDERS += \
    3rdmodem \
    3rdmodemnvm \
    3rdmodemnvmbkp \
    modem_log \
    splash2 \
    hw_odm
    
# SEPolicy
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy/vendor

# Vintf
DEVICE_MANIFEST_FILE += $(DEVICE_PATH)/prebuilts/manifest.xml
