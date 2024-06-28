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
