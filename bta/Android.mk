LOCAL_PATH:= $(call my-dir)

# BTA static library for target
# ========================================================
include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := .cc

LOCAL_SRC_FILES:= \
    ./dm/bta_dm_ci.cc \
    ./dm/bta_dm_act.cc \
    ./dm/bta_dm_pm.cc \
    ./dm/bta_dm_main.cc \
    ./dm/bta_dm_cfg.cc \
    ./dm/bta_dm_api.cc \
    ./dm/bta_dm_sco.cc \
    ./gatt/bta_gattc_api.cc \
    ./gatt/bta_gatts_act.cc \
    ./gatt/bta_gatts_main.cc \
    ./gatt/bta_gattc_utils.cc \
    ./gatt/bta_gatts_api.cc \
    ./gatt/bta_gattc_main.cc \
    ./gatt/bta_gattc_act.cc \
    ./gatt/bta_gattc_cache.cc \
    ./gatt/bta_gatts_utils.cc \
    ./ag/bta_ag_sdp.c \
    ./ag/bta_ag_sco.c \
    ./ag/bta_ag_cfg.c \
    ./ag/bta_ag_main.c \
    ./ag/bta_ag_api.c \
    ./ag/bta_ag_rfc.c \
    ./ag/bta_ag_act.c \
    ./ag/bta_ag_cmd.c \
    ./ag/bta_ag_ci.c \
    ./ag/bta_ag_at.c \
    ./hf_client/bta_hf_client_act.c \
    ./hf_client/bta_hf_client_api.c \
    ./hf_client/bta_hf_client_main.c \
    ./hf_client/bta_hf_client_rfc.c \
    ./hf_client/bta_hf_client_at.c \
    ./hf_client/bta_hf_client_sdp.c \
    ./hf_client/bta_hf_client_sco.c \
    ./hf_client/bta_hf_client_cmd.c \
    ./hh/bta_hh_cfg.cc \
    ./hh/bta_hh_act.cc \
    ./hh/bta_hh_api.cc \
    ./hh/bta_hh_le.cc \
    ./hh/bta_hh_utils.cc \
    ./hh/bta_hh_main.cc \
    ./pan/bta_pan_main.c \
    ./pan/bta_pan_ci.c \
    ./pan/bta_pan_act.c \
    ./pan/bta_pan_api.c \
    ./av/bta_av_act.c \
    ./av/bta_av_ci.c \
    ./av/bta_av_api.c \
    ./av/bta_av_aact.c \
    ./av/bta_av_main.c \
    ./av/bta_av_cfg.c \
    ./av/bta_av_ssm.c \
    ./av/bta_av_sbc.c \
    ./ar/bta_ar.c \
    ./hl/bta_hl_act.c \
    ./hl/bta_hl_api.c \
    ./hl/bta_hl_main.c \
    ./hl/bta_hl_utils.c \
    ./hl/bta_hl_sdp.c \
    ./hl/bta_hl_ci.c \
    ./sdp/bta_sdp_api.c \
    ./sdp/bta_sdp_act.c \
    ./sdp/bta_sdp.c \
    ./sdp/bta_sdp_cfg.c \
    ./sys/bta_sys_main.c \
    ./sys/bta_sys_conn.c \
    ./sys/utl.c \
    ./jv/bta_jv_act.c \
    ./jv/bta_jv_cfg.c \
    ./jv/bta_jv_main.c \
    ./jv/bta_jv_api.c

LOCAL_MODULE := libbt-bta
LOCAL_MODULE_CLASS := STATIC_LIBRARIES
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := libcutils libc

LOCAL_C_INCLUDES := . \
                   $(LOCAL_PATH)/include \
                   $(LOCAL_PATH)/sys \
                   $(LOCAL_PATH)/dm \
                   $(LOCAL_PATH)/hh \
                   $(LOCAL_PATH)/../ \
                   $(LOCAL_PATH)/../btcore/include \
                   $(LOCAL_PATH)/../hci/include \
                   $(LOCAL_PATH)/../include \
                   $(LOCAL_PATH)/../stack/include \
                   $(LOCAL_PATH)/../stack/btm \
                   $(LOCAL_PATH)/../udrv/include \
                   $(LOCAL_PATH)/../vnd/include \
                   $(LOCAL_PATH)/../utils/include \
                   $(bluetooth_C_INCLUDES)

LOCAL_CFLAGS += $(bluetooth_CFLAGS) -DBUILDCFG
LOCAL_CONLYFLAGS += $(bluetooth_CONLYFLAGS)
LOCAL_CPPFLAGS += $(bluetooth_CPPFLAGS)

include $(BUILD_STATIC_LIBRARY)
