#
# Copyright (C) 2020-2024 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

LOCAL_PATH := $(call my-dir)

ifneq ($(filter mediapadm5lte,$(TARGET_DEVICE)),)

include $(call all-makefiles-under,$(LOCAL_PATH))

# The stock RIL falls back to /system/ons.bin. Keep that pathname as a
# symlink, with the operator name table on vendor where rild may read it.
M5_ONS_SYMLINK := $(TARGET_OUT)/ons.bin
$(M5_ONS_SYMLINK): $(TARGET_OUT_VENDOR)/etc/ons.bin $(LOCAL_PATH)/Android.mk
	@mkdir -p $(dir $@)
	$(hide) ln -sf /vendor/etc/ons.bin $@

ALL_DEFAULT_INSTALLED_MODULES += $(M5_ONS_SYMLINK)

endif
