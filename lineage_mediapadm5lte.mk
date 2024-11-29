#
# Copyright (C) 2024 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from those products. Most specific first.
$(call inherit-product, $(LOCAL_PATH)/pre-base.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/aosp_base_telephony.mk)

# Inherit from stanford device
$(call inherit-product, device/huawei/mediapadm5lte/device.mk)

# Inherit some common LineageOS stuff.
$(call inherit-product, vendor/lineage/config/common_full_tablet.mk)

LINEAGE_BUILDTYPE := RELEASE

# Device identifier. This must come after all inclusions
PRODUCT_DEVICE := mediapadm5lte
PRODUCT_NAME := lineage_mediapadm5lte
PRODUCT_BRAND := HUAWEI
PRODUCT_MODEL := MediaPad-M5
PRODUCT_MANUFACTURER := HUAWEI

PRODUCT_BUILD_PROP_OVERRIDES += \
    PRIVATE_BUILD_DESC="CMR-AL09-user 9.1.0 HUAWEICMR-AL09 333-OVS-LGRP2 release-keys"

# Set BUILD_FINGERPRINT variable to be picked up by both system and vendor build.prop
BUILD_FINGERPRINT := "HUAWEI/CMR-AL09/HWCMR:9/HUAWEICMR-AL09/9.1.0.333C432:user/release-keys"
