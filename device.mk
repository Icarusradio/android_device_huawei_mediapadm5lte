#
# Copyright (C) 2024 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from hi3660
$(call inherit-product, device/huawei/hi3660/common.mk)

# Enable the 64-bit, known-blob audio stream ABI adapter for this product only.
$(call soong_config_set,huaweiAudioVars,mediapadm5lte_stream_compat,true)

# Init
PRODUCT_PACKAGES += \
    init.mediapadm5lte.rc \
    fstab.mediapadm5lte \
    fstab.modem

# Device-specific Bluetooth HCI feature compatibility proxy
PRODUCT_PACKAGES += \
    android.hardware.bluetooth@1.0-service.huawei-compat

# Graphics
PRODUCT_PACKAGES += libGLES_huawei_compat

# Overlays
DEVICE_PACKAGE_OVERLAYS += \
    $(LOCAL_PATH)/overlay

# Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.telephony.gsm.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.gsm.xml \
    frameworks/native/data/etc/android.hardware.telephony.cdma.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.cdma.xml

# Radio
PRODUCT_PACKAGES += \
    libcmr_radio \
    android.hardware.radio.deprecated@1.0.vendor \
    android.hardware.radio.config@1.2.vendor \
    android.hardware.radio@1.4.vendor

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/prebuilts/ons.bin:$(TARGET_COPY_OUT_SYSTEM)/ons.bin

PRODUCT_PACKAGES += \
    librilutils

PRODUCT_PACKAGES += \
    android.hardware.secure_element@1.0.vendor \
    android.hardware.nfc@1.1.vendor

# RRO
PRODUCT_ENFORCE_RRO_TARGETS := *

PRODUCT_PACKAGES += \
    TetheringConfigOverlay

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(LOCAL_PATH)

# Call the proprietary setup
$(call inherit-product, vendor/huawei/mediapadm5lte/mediapadm5lte-vendor.mk)
