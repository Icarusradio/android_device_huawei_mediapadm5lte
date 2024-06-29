#
# Copyright (C) 2024 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from hi3660
$(call inherit-product, device/huawei/hi3660/common.mk)

# Init
PRODUCT_PACKAGES += \
    init.mediapadm5lte.rc \
    fstab.mediapadm5lte \
    fstab.modem

# Overlays
DEVICE_PACKAGE_OVERLAYS += \
    $(LOCAL_PATH)/overlay

# Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.telephony.gsm.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.gsm.xml \
    frameworks/native/data/etc/android.hardware.telephony.cdma.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.cdma.xml

# Radio
PRODUCT_PACKAGES += \
    android.hardware.radio.deprecated@1.0.vendor \
    android.hardware.radio.config@1.2.vendor \
    android.hardware.radio@1.4.vendor

PRODUCT_PACKAGES += \
    librilutils \
    libril

PRODUCT_PACKAGES += \
    android.hardware.secure_element@1.0.vendor \
    android.hardware.nfc@1.1.vendor

# RRO
PRODUCT_ENFORCE_RRO_TARGETS := *

PRODUCT_PACKAGES += \
    TetheringConfigOverlay

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(LOCAL_PATH) \
    $(LOCAL_PATH)/resources

# Call the proprietary setup
$(call inherit-product, vendor/huawei/mediapadm5lte/mediapadm5lte-vendor.mk)
