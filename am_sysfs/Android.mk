LOCAL_PATH := $(call my-dir)
#use systen control service rw sysfs file

include $(CLEAR_VARS)

LOCAL_MODULE    := libam_sysfs
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := am_syswrite.cpp

LOCAL_ARM_MODE := arm
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../include/am_sysfs\
 					$(LOCAL_PATH)/../include/am_adp\
                    vendor/amlogic/frameworks/services/systemcontrol

LOCAL_SHARED_LIBRARIES+=libcutils liblog libc
#for bind

LOCAL_SHARED_LIBRARIES+=libutils  libbinder libsystemcontrolservice


LOCAL_PRELINK_MODULE := false

LOCAL_32_BIT_ONLY := true

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_MODULE    := libam_sysfs
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := am_syswrite.cpp

LOCAL_ARM_MODE := arm
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../include/am_sysfs\
 					$(LOCAL_PATH)/../include/am_adp\
                    vendor/amlogic/frameworks/services/systemcontrol

LOCAL_SHARED_LIBRARIES+=libcutils liblog libc
#for bind

LOCAL_SHARED_LIBRARIES+=libutils  libbinder libsystemcontrolservice


LOCAL_PRELINK_MODULE := false

LOCAL_32_BIT_ONLY := true

include $(BUILD_STATIC_LIBRARY)