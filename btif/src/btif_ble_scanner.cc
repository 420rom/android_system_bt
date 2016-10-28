/******************************************************************************
 *
 *  Copyright (C) 2016 The Android Open Source Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_scanner"

#include <base/bind.h>
#include <base/threading/thread.h>
#include <errno.h>
#include <hardware/bluetooth.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_set>
#include "device/include/controller.h"

#include "btcore/include/bdaddr.h"
#include "btif_common.h"
#include "btif_util.h"

#if (BLE_INCLUDED == TRUE)

#include <hardware/bt_gatt.h>

#include "bta_api.h"
#include "bta_gatt_api.h"
#include "btif_config.h"
#include "btif_dm.h"
#include "btif_gatt.h"
#include "btif_gatt_util.h"
#include "btif_storage.h"
#include "osi/include/log.h"
#include "vendor_api.h"

using base::Bind;
using base::Owned;
using std::vector;

extern bt_status_t do_in_jni_thread(const base::Closure& task);
extern const btgatt_callbacks_t* bt_gatt_callbacks;

#define  SCAN_CBACK_IN_JNI(P_CBACK, ...)                                       \
  do {                                                                         \
    if (bt_gatt_callbacks && bt_gatt_callbacks->scanner->P_CBACK) {            \
      BTIF_TRACE_API("HAL bt_gatt_callbacks->client->%s", #P_CBACK);           \
      do_in_jni_thread(Bind(bt_gatt_callbacks->scanner->P_CBACK, __VA_ARGS__)); \
    } else {                                                                   \
      ASSERTC(0, "Callback is NULL", 0);                                       \
    }                                                                          \
  } while (0)

#define CHECK_BTGATT_INIT()                                      \
  do {                                                           \
    if (bt_gatt_callbacks == NULL) {                             \
      LOG_WARN(LOG_TAG, "%s: BTGATT not initialized", __func__); \
      return BT_STATUS_NOT_READY;                                \
    } else {                                                     \
      LOG_VERBOSE(LOG_TAG, "%s", __func__);                      \
    }                                                            \
  } while (0)

namespace std {
template <>
struct hash<bt_bdaddr_t> {
  size_t operator()(const bt_bdaddr_t& f) const {
    return f.address[0] + f.address[1] + f.address[2] + f.address[3] +
           f.address[4] + f.address[5];
  }
};

template <>
struct equal_to<bt_bdaddr_t> {
  size_t operator()(const bt_bdaddr_t& x, const bt_bdaddr_t& y) const {
    return memcmp(x.address, y.address, BD_ADDR_LEN);
  }
};
}

namespace {

std::unordered_set<bt_bdaddr_t> p_dev_cb;

void btif_gattc_add_remote_bdaddr(BD_ADDR p_bda, uint8_t addr_type) {
  bt_bdaddr_t bd_addr;
  memcpy(bd_addr.address, p_bda, BD_ADDR_LEN);
  p_dev_cb.insert(bd_addr);
}

bool btif_gattc_find_bdaddr(BD_ADDR p_bda) {
  bt_bdaddr_t bd_addr;
  memcpy(bd_addr.address, p_bda, BD_ADDR_LEN);
  return (p_dev_cb.count(bd_addr) != 0);
}

void btif_gattc_init_dev_cb(void) { p_dev_cb.clear(); }

btgattc_error_t btif_gattc_translate_btm_status(tBTM_STATUS status) {
  switch (status) {
    case BTM_SUCCESS:
    case BTM_SUCCESS_NO_SECURITY:
      return BT_GATTC_COMMAND_SUCCESS;

    case BTM_CMD_STARTED:
      return BT_GATTC_COMMAND_STARTED;

    case BTM_BUSY:
      return BT_GATTC_COMMAND_BUSY;

    case BTM_CMD_STORED:
      return BT_GATTC_COMMAND_STORED;

    case BTM_NO_RESOURCES:
      return BT_GATTC_NO_RESOURCES;

    case BTM_MODE_UNSUPPORTED:
    case BTM_WRONG_MODE:
    case BTM_MODE4_LEVEL4_NOT_SUPPORTED:
      return BT_GATTC_MODE_UNSUPPORTED;

    case BTM_ILLEGAL_VALUE:
    case BTM_SCO_BAD_LENGTH:
      return BT_GATTC_ILLEGAL_VALUE;

    case BTM_UNKNOWN_ADDR:
      return BT_GATTC_UNKNOWN_ADDR;

    case BTM_DEVICE_TIMEOUT:
      return BT_GATTC_DEVICE_TIMEOUT;

    case BTM_FAILED_ON_SECURITY:
    case BTM_REPEATED_ATTEMPTS:
    case BTM_NOT_AUTHORIZED:
      return BT_GATTC_SECURITY_ERROR;

    case BTM_DEV_RESET:
    case BTM_ILLEGAL_ACTION:
      return BT_GATTC_INCORRECT_STATE;

    case BTM_BAD_VALUE_RET:
      return BT_GATTC_INVALID_CONTROLLER_OUTPUT;

    case BTM_DELAY_CHECK:
      return BT_GATTC_DELAYED_ENCRYPTION_CHECK;

    case BTM_ERR_PROCESSING:
    default:
      return BT_GATTC_ERR_PROCESSING;
  }
}

void btif_gatts_upstreams_evt(uint16_t event, char* p_param) {
  LOG_VERBOSE(LOG_TAG, "%s: Event %d", __func__, event);

  tBTA_GATTC* p_data = (tBTA_GATTC*)p_param;
  switch (event) {
    case BTA_GATTC_REG_EVT: {
      bt_uuid_t app_uuid;
      bta_to_btif_uuid(&app_uuid, &p_data->reg_oper.app_uuid);
      HAL_CBACK(bt_gatt_callbacks, scanner->register_scanner_cb,
                p_data->reg_oper.status, p_data->reg_oper.client_if, &app_uuid);
      break;
    }

    case BTA_GATTC_DEREG_EVT:
      break;

    case BTA_GATTC_SEARCH_CMPL_EVT: {
      HAL_CBACK(bt_gatt_callbacks, client->search_complete_cb,
                p_data->search_cmpl.conn_id, p_data->search_cmpl.status);
      break;
    }

    default:
      LOG_ERROR(LOG_TAG, "%s: Unhandled event (%d)!", __func__, event);
      break;
  }
}

void bta_gatts_cback(tBTA_GATTC_EVT event, tBTA_GATTC* p_data) {
  bt_status_t status =
      btif_transfer_context(btif_gatts_upstreams_evt, (uint16_t)event,
                            (char*)p_data, sizeof(tBTA_GATTC), NULL);
  ASSERTC(status == BT_STATUS_SUCCESS, "Context transfer failed!", status);
}

void bta_scan_param_setup_cb(tGATT_IF client_if, tBTM_STATUS status) {
  SCAN_CBACK_IN_JNI(scan_parameter_setup_completed_cb, client_if,
                   btif_gattc_translate_btm_status(status));
}

void bta_scan_filt_cfg_cb(tBTA_DM_BLE_PF_ACTION action,
                          tBTA_DM_BLE_SCAN_COND_OP cfg_op,
                          tBTA_DM_BLE_PF_AVBL_SPACE avbl_space,
                          tBTA_STATUS status, tBTA_DM_BLE_REF_VALUE ref_value) {
  SCAN_CBACK_IN_JNI(scan_filter_cfg_cb, action, ref_value, status, cfg_op,
                   avbl_space);
}

void bta_scan_filt_param_setup_cb(uint8_t action_type,
                                  tBTA_DM_BLE_PF_AVBL_SPACE avbl_space,
                                  tBTA_DM_BLE_REF_VALUE ref_value,
                                  tBTA_STATUS status) {
  SCAN_CBACK_IN_JNI(scan_filter_param_cb, action_type, ref_value, status,
                   avbl_space);
}

void bta_scan_filt_status_cb(uint8_t action, tBTA_STATUS status,
                             tBTA_DM_BLE_REF_VALUE ref_value) {
  SCAN_CBACK_IN_JNI(scan_filter_status_cb, action, ref_value, status);
}

void bta_batch_scan_setup_cb(tBTA_BLE_BATCH_SCAN_EVT evt,
                             tBTA_DM_BLE_REF_VALUE ref_value,
                             tBTA_STATUS status) {
  BTIF_TRACE_DEBUG("bta_batch_scan_setup_cb-Status:%x, client_if:%d, evt=%d",
                   status, ref_value, evt);

  switch (evt) {
    case BTA_BLE_BATCH_SCAN_ENB_EVT: {
      SCAN_CBACK_IN_JNI(batchscan_enb_disable_cb, 1, ref_value, status);
      return;
    }

    case BTA_BLE_BATCH_SCAN_DIS_EVT: {
      SCAN_CBACK_IN_JNI(batchscan_enb_disable_cb, 0, ref_value, status);
      return;
    }

    case BTA_BLE_BATCH_SCAN_CFG_STRG_EVT: {
      SCAN_CBACK_IN_JNI(batchscan_cfg_storage_cb, ref_value, status);
      return;
    }

    case BTA_BLE_BATCH_SCAN_DATA_EVT: {
      SCAN_CBACK_IN_JNI(batchscan_reports_cb, ref_value, status, 0, 0,
                       vector<uint8_t>());
      return;
    }

    case BTA_BLE_BATCH_SCAN_THRES_EVT: {
      SCAN_CBACK_IN_JNI(batchscan_threshold_cb, ref_value);
      return;
    }

    default:
      return;
  }
}

void bta_batch_scan_threshold_cb(tBTA_DM_BLE_REF_VALUE ref_value) {
  SCAN_CBACK_IN_JNI(batchscan_threshold_cb, ref_value);
}

void bta_batch_scan_reports_cb(tBTA_DM_BLE_REF_VALUE ref_value,
                               uint8_t report_format, uint8_t num_records,
                               uint16_t data_len, uint8_t* p_rep_data,
                               tBTA_STATUS status) {
  BTIF_TRACE_DEBUG("%s - client_if:%d, %d, %d, %d", __func__, ref_value, status,
                   num_records, data_len);

  if (data_len > 0) {
    vector<uint8_t> data(p_rep_data, p_rep_data + data_len);
    osi_free(p_rep_data);

    SCAN_CBACK_IN_JNI(batchscan_reports_cb, ref_value, status, report_format,
                     num_records, std::move(data));
  } else {
    SCAN_CBACK_IN_JNI(batchscan_reports_cb, ref_value, status, report_format,
                     num_records, vector<uint8_t>());
  }
}

void bta_scan_results_cb_impl(bt_bdaddr_t bd_addr, tBT_DEVICE_TYPE device_type,
                              int8_t rssi, uint8_t addr_type,
                              vector<uint8_t> value) {
  uint8_t remote_name_len;
  const uint8_t* p_eir_remote_name = NULL;
  bt_device_type_t dev_type;
  bt_property_t properties;

  p_eir_remote_name = BTM_CheckEirData(
      value.data(), BTM_EIR_COMPLETE_LOCAL_NAME_TYPE, &remote_name_len);

  if (p_eir_remote_name == NULL) {
    p_eir_remote_name = BTM_CheckEirData(
        value.data(), BT_EIR_SHORTENED_LOCAL_NAME_TYPE, &remote_name_len);
  }

  if ((addr_type != BLE_ADDR_RANDOM) || (p_eir_remote_name)) {
    if (!btif_gattc_find_bdaddr(bd_addr.address)) {
      btif_gattc_add_remote_bdaddr(bd_addr.address, addr_type);

      if (p_eir_remote_name) {
        bt_bdname_t bdname;
        memcpy(bdname.name, p_eir_remote_name, remote_name_len);
        bdname.name[remote_name_len] = '\0';

        LOG_VERBOSE(LOG_TAG, "%s BLE device name=%s len=%d dev_type=%d",
                    __func__, bdname.name, remote_name_len, device_type);
        btif_dm_update_ble_remote_properties(bd_addr.address, bdname.name,
                                             device_type);
      }
    }
  }

  dev_type = (bt_device_type_t)device_type;
  BTIF_STORAGE_FILL_PROPERTY(&properties, BT_PROPERTY_TYPE_OF_DEVICE,
                             sizeof(dev_type), &dev_type);
  btif_storage_set_remote_device_property(&(bd_addr), &properties);

  btif_storage_set_remote_addr_type(&bd_addr, addr_type);

  HAL_CBACK(bt_gatt_callbacks, scanner->scan_result_cb, &bd_addr, rssi,
            std::move(value));
}

void bta_scan_results_cb(tBTA_DM_SEARCH_EVT event, tBTA_DM_SEARCH* p_data) {
  uint8_t len;

  if (event == BTA_DM_INQ_CMPL_EVT) {
    BTIF_TRACE_DEBUG("%s  BLE observe complete. Num Resp %d", __func__,
                     p_data->inq_cmpl.num_resps);
    return;
  }

  if (event != BTA_DM_INQ_RES_EVT) {
    BTIF_TRACE_WARNING("%s : Unknown event 0x%x", __func__, event);
    return;
  }

  vector<uint8_t> value(BTGATT_MAX_ATTR_LEN);
  if (p_data->inq_res.p_eir) {
    value.insert(value.begin(), p_data->inq_res.p_eir,
                 p_data->inq_res.p_eir + 62);

    if (BTM_CheckEirData(p_data->inq_res.p_eir,
                         BTM_EIR_COMPLETE_LOCAL_NAME_TYPE, &len)) {
      p_data->inq_res.remt_name_not_required = true;
    }
  }

  bt_bdaddr_t bdaddr;
  bdcpy(bdaddr.address, p_data->inq_res.bd_addr);
  do_in_jni_thread(Bind(bta_scan_results_cb_impl, bdaddr,
                        p_data->inq_res.device_type, p_data->inq_res.rssi,
                        p_data->inq_res.ble_addr_type, std::move(value)));
}

void bta_track_adv_event_cb(tBTA_DM_BLE_TRACK_ADV_DATA* p_track_adv_data) {
  btgatt_track_adv_info_t* btif_scan_track_cb = new btgatt_track_adv_info_t;

  BTIF_TRACE_DEBUG("%s", __func__);
  btif_gatt_move_track_adv_data(btif_scan_track_cb,
                                (btgatt_track_adv_info_t*)p_track_adv_data);

  SCAN_CBACK_IN_JNI(track_adv_event_cb, Owned(btif_scan_track_cb));
}


void btif_gattc_register_scanner_impl(tBT_UUID uuid) {
  BTA_GATTC_AppRegister(&uuid, bta_gatts_cback);
}

bt_status_t btif_gattc_register_scanner(bt_uuid_t* uuid) {
  CHECK_BTGATT_INIT();

  tBT_UUID bt_uuid;
  btif_to_bta_uuid(&bt_uuid, uuid);
  return do_in_jni_thread(Bind(&btif_gattc_register_scanner_impl, bt_uuid));
}

void btif_gattc_unregister_scanner_impl(int client_if) {
  BTA_GATTC_AppDeregister(client_if);
}

bt_status_t btif_gattc_unregister_scanner(int scanner_id) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(&btif_gattc_unregister_scanner_impl, scanner_id));
}

bt_status_t btif_gattc_scan(bool start) {
  CHECK_BTGATT_INIT();
  if (start) {
    btif_gattc_init_dev_cb();
    return do_in_jni_thread(Bind(&BTA_DmBleObserve, true, 0,
                                 (tBTA_DM_SEARCH_CBACK*)bta_scan_results_cb));
  }

  return do_in_jni_thread(Bind(&BTA_DmBleObserve, false, 0, nullptr));
}

void btif_gattc_scan_filter_param_setup_impl(
    int client_if, uint8_t action, int filt_index,
    tBTA_DM_BLE_PF_FILT_PARAMS* adv_filt_param) {
  if (1 == adv_filt_param->dely_mode) {
    BTA_DmBleTrackAdvertiser(client_if, bta_track_adv_event_cb);
  }

  BTA_DmBleScanFilterSetup(action, filt_index, adv_filt_param, NULL,
                           bta_scan_filt_param_setup_cb, client_if);
}

bt_status_t btif_gattc_scan_filter_param_setup(
    btgatt_filt_param_setup_t filt_param) {
  CHECK_BTGATT_INIT();
  BTIF_TRACE_DEBUG("%s", __func__);

  tBTA_DM_BLE_PF_FILT_PARAMS* adv_filt_param = new tBTA_DM_BLE_PF_FILT_PARAMS;
  adv_filt_param->feat_seln = filt_param.feat_seln;
  adv_filt_param->list_logic_type = filt_param.list_logic_type;
  adv_filt_param->filt_logic_type = filt_param.filt_logic_type;
  adv_filt_param->rssi_high_thres = filt_param.rssi_high_thres;
  adv_filt_param->rssi_low_thres = filt_param.rssi_low_thres;
  adv_filt_param->dely_mode = filt_param.dely_mode;
  adv_filt_param->found_timeout = filt_param.found_timeout;
  adv_filt_param->lost_timeout = filt_param.lost_timeout;
  adv_filt_param->found_timeout_cnt = filt_param.found_timeout_cnt;
  adv_filt_param->num_of_tracking_entries = filt_param.num_of_tracking_entries;

  return do_in_jni_thread(
      Bind(base::IgnoreResult(&btif_gattc_scan_filter_param_setup_impl),
           filt_param.client_if, filt_param.action, filt_param.filt_index,
           base::Owned(adv_filt_param)));
}

void btif_gattc_scan_filter_add_srvc_uuid(tBT_UUID uuid,
                                          tBTA_DM_BLE_PF_COND_MASK* p_uuid_mask,
                                          int action, int filt_type,
                                          int filt_index, int client_if) {
  tBTA_DM_BLE_PF_COND_PARAM cond;
  memset(&cond, 0, sizeof(tBTA_DM_BLE_PF_COND_PARAM));

  cond.srvc_uuid.p_target_addr = NULL;
  cond.srvc_uuid.cond_logic = BTA_DM_BLE_PF_LOGIC_AND;
  cond.srvc_uuid.uuid = uuid;
  cond.srvc_uuid.p_uuid_mask = p_uuid_mask;

  BTA_DmBleCfgFilterCondition(action, filt_type, filt_index, &cond,
                              &bta_scan_filt_cfg_cb, client_if);
}

void btif_gattc_scan_filter_add_local_name(vector<uint8_t> data, int action,
                                           int filt_type, int filt_index,
                                           int client_if) {
  tBTA_DM_BLE_PF_COND_PARAM cond;
  memset(&cond, 0, sizeof(tBTA_DM_BLE_PF_COND_PARAM));

  cond.local_name.data_len = data.size();
  cond.local_name.p_data = const_cast<uint8_t*>(data.data());
  BTA_DmBleCfgFilterCondition(action, filt_type, filt_index, &cond,
                              &bta_scan_filt_cfg_cb, client_if);
}

void btif_gattc_scan_filter_add_manu_data(int company_id, int company_id_mask,
                                          vector<uint8_t> pattern,
                                          vector<uint8_t> pattern_mask,
                                          int action, int filt_type,
                                          int filt_index, int client_if) {
  tBTA_DM_BLE_PF_COND_PARAM cond;
  memset(&cond, 0, sizeof(tBTA_DM_BLE_PF_COND_PARAM));

  cond.manu_data.company_id = company_id;
  cond.manu_data.company_id_mask = company_id_mask ? company_id_mask : 0xFFFF;
  cond.manu_data.data_len = pattern.size();
  cond.manu_data.p_pattern = const_cast<uint8_t*>(pattern.data());
  cond.manu_data.p_pattern_mask = const_cast<uint8_t*>(pattern_mask.data());
  BTA_DmBleCfgFilterCondition(action, filt_type, filt_index, &cond,
                              &bta_scan_filt_cfg_cb, client_if);
}

void btif_gattc_scan_filter_add_data_pattern(vector<uint8_t> pattern,
                                             vector<uint8_t> pattern_mask,
                                             int action, int filt_type,
                                             int filt_index, int client_if) {
  tBTA_DM_BLE_PF_COND_PARAM cond;
  memset(&cond, 0, sizeof(tBTA_DM_BLE_PF_COND_PARAM));

  cond.srvc_data.data_len = pattern.size();
  cond.srvc_data.p_pattern = const_cast<uint8_t*>(pattern.data());
  cond.srvc_data.p_pattern_mask = const_cast<uint8_t*>(pattern_mask.data());
  BTA_DmBleCfgFilterCondition(action, filt_type, filt_index, &cond,
                              &bta_scan_filt_cfg_cb, client_if);
}

bt_status_t btif_gattc_scan_filter_add_remove(
    int client_if, int action, int filt_type, int filt_index, int company_id,
    int company_id_mask, const bt_uuid_t* p_uuid, const bt_uuid_t* p_uuid_mask,
    const bt_bdaddr_t* bd_addr, char addr_type, vector<uint8_t> data,
    vector<uint8_t> mask) {
  CHECK_BTGATT_INIT();
  BTIF_TRACE_DEBUG("%s, %d, %d", __func__, action, filt_type);

  /* If data is passed, both mask and data have to be the same length */
  if (data.size() != mask.size() && data.size() != 0 && mask.size() != 0)
    return BT_STATUS_PARM_INVALID;

  switch (filt_type) {
    case BTA_DM_BLE_PF_ADDR_FILTER:
    {
      tBTA_DM_BLE_PF_COND_PARAM* cond = new tBTA_DM_BLE_PF_COND_PARAM;
      memset(cond, 0, sizeof(tBTA_DM_BLE_PF_COND_PARAM));

      bdcpy(cond->target_addr.bda, bd_addr->address);
      cond->target_addr.type = addr_type;
      return do_in_jni_thread(Bind(&BTA_DmBleCfgFilterCondition, action,
                                   filt_type, filt_index, base::Owned(cond),
                                   &bta_scan_filt_cfg_cb, client_if));
    }

    case BTA_DM_BLE_PF_SRVC_DATA:
      return do_in_jni_thread(Bind(&BTA_DmBleCfgFilterCondition, action,
                                   filt_type, filt_index, nullptr,
                                   &bta_scan_filt_cfg_cb, client_if));

    case BTA_DM_BLE_PF_SRVC_UUID:
    {
      tBT_UUID bt_uuid;
      btif_to_bta_uuid(&bt_uuid, p_uuid);

      if (p_uuid_mask != NULL) {
        tBTA_DM_BLE_PF_COND_MASK* uuid_mask = new tBTA_DM_BLE_PF_COND_MASK;
        btif_to_bta_uuid_mask(uuid_mask, p_uuid_mask, p_uuid);
        return do_in_jni_thread(Bind(&btif_gattc_scan_filter_add_srvc_uuid,
                                     bt_uuid, base::Owned(uuid_mask), action,
                                     filt_type, filt_index, client_if));
      }

      return do_in_jni_thread(Bind(&btif_gattc_scan_filter_add_srvc_uuid,
                                   bt_uuid, nullptr, action, filt_type,
                                   filt_index, client_if));
    }

    case BTA_DM_BLE_PF_SRVC_SOL_UUID:
    {
      tBTA_DM_BLE_PF_COND_PARAM* cond = new tBTA_DM_BLE_PF_COND_PARAM;
      memset(cond, 0, sizeof(tBTA_DM_BLE_PF_COND_PARAM));

      cond->solicitate_uuid.p_target_addr = NULL;
      cond->solicitate_uuid.cond_logic = BTA_DM_BLE_PF_LOGIC_AND;
      btif_to_bta_uuid(&cond->solicitate_uuid.uuid, p_uuid);

      return do_in_jni_thread(Bind(&BTA_DmBleCfgFilterCondition, action,
                                   filt_type, filt_index, base::Owned(cond),
                                   &bta_scan_filt_cfg_cb, client_if));
    }

    case BTA_DM_BLE_PF_LOCAL_NAME:
    {
      return do_in_jni_thread(Bind(&btif_gattc_scan_filter_add_local_name,
                                   std::move(data), action, filt_type,
                                   filt_index, client_if));
    }

    case BTA_DM_BLE_PF_MANU_DATA:
    {
      return do_in_jni_thread(Bind(&btif_gattc_scan_filter_add_manu_data,
                                   company_id, company_id_mask, std::move(data),
                                   std::move(mask), action, filt_type,
                                   filt_index, client_if));
    }

    case BTA_DM_BLE_PF_SRVC_DATA_PATTERN:
    {
      return do_in_jni_thread(Bind(&btif_gattc_scan_filter_add_data_pattern,
                                   std::move(data), std::move(mask), action,
                                   filt_type, filt_index, client_if));
    }

    default:
      LOG_ERROR(LOG_TAG, "%s: Unknown filter type (%d)!", __func__, action);
      return (bt_status_t)BTA_GATT_OK;
  }
}

bt_status_t btif_gattc_scan_filter_clear(int client_if, int filter_index) {
  CHECK_BTGATT_INIT();
  BTIF_TRACE_DEBUG("%s: filter_index: %d", __func__, filter_index);

  return do_in_jni_thread(Bind(&BTA_DmBleCfgFilterCondition,
                               BTA_DM_BLE_SCAN_COND_CLEAR,
                               BTA_DM_BLE_PF_TYPE_ALL, filter_index, nullptr,
                               &bta_scan_filt_cfg_cb, client_if));
}

bt_status_t btif_gattc_scan_filter_enable(int client_if, bool enable) {
  CHECK_BTGATT_INIT();
  BTIF_TRACE_DEBUG("%s: enable: %d", __func__, enable);

  uint8_t action = enable ? 1 : 0;

  return do_in_jni_thread(Bind(&BTA_DmEnableScanFilter, action,
                               &bta_scan_filt_status_cb, client_if));
}

bt_status_t btif_gattc_set_scan_parameters(int client_if, int scan_interval,
                                           int scan_window) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(BTA_DmSetBleScanParams, client_if, scan_interval, scan_window,
           BTM_BLE_SCAN_MODE_ACTI,
           (tBLE_SCAN_PARAM_SETUP_CBACK)bta_scan_param_setup_cb));
}


bt_status_t btif_gattc_cfg_storage(int client_if, int batch_scan_full_max,
                                   int batch_scan_trunc_max,
                                   int batch_scan_notify_threshold) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(BTA_DmBleSetStorageParams, batch_scan_full_max, batch_scan_trunc_max,
           batch_scan_notify_threshold,
           (tBTA_BLE_SCAN_SETUP_CBACK*)bta_batch_scan_setup_cb,
           (tBTA_BLE_SCAN_THRESHOLD_CBACK*)bta_batch_scan_threshold_cb,
           (tBTA_BLE_SCAN_REP_CBACK*)bta_batch_scan_reports_cb,
           (tBTA_DM_BLE_REF_VALUE)client_if));
}

bt_status_t btif_gattc_enb_batch_scan(int client_if, int scan_mode,
                                      int scan_interval, int scan_window,
                                      int addr_type, int discard_rule) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(BTA_DmBleEnableBatchScan, scan_mode,
                               scan_interval, scan_window, discard_rule,
                               addr_type, client_if));
}

bt_status_t btif_gattc_dis_batch_scan(int client_if) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(BTA_DmBleDisableBatchScan, client_if));
}

bt_status_t btif_gattc_read_batch_scan_reports(int client_if, int scan_mode) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(BTA_DmBleReadScanReports, scan_mode, client_if));
}

} //namespace

const btgatt_scanner_interface_t btgattScannerInterface = {
    btif_gattc_register_scanner,
    btif_gattc_unregister_scanner,
    btif_gattc_scan,
    btif_gattc_scan_filter_param_setup,
    btif_gattc_scan_filter_add_remove,
    btif_gattc_scan_filter_clear,
    btif_gattc_scan_filter_enable,
    btif_gattc_set_scan_parameters,
    btif_gattc_cfg_storage,
    btif_gattc_enb_batch_scan,
    btif_gattc_dis_batch_scan,
    btif_gattc_read_batch_scan_reports,
};

#endif
