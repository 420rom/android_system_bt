/******************************************************************************
 *
 *  Copyright (C) 2009-2014 Broadcom Corporation
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

/*******************************************************************************
 *
 *  Filename:      btif_gatt_client.c
 *
 *  Description:   GATT client implementation
 *
 *******************************************************************************/

#define LOG_TAG "bt_btif_gattc"

#include <base/at_exit.h>
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
#include "btif_gatt_multi_adv_util.h"
#include "btif_gatt_util.h"
#include "btif_storage.h"
#include "btif_storage.h"
#include "osi/include/log.h"
#include "vendor_api.h"

using base::Bind;
using base::Owned;
using std::vector;

extern bt_status_t do_in_jni_thread(const base::Closure &task);
extern bt_status_t btif_gattc_test_command_impl(int command,
                                                btgatt_test_params_t *params);
extern const btgatt_callbacks_t *bt_gatt_callbacks;

/*******************************************************************************
**  Constants & Macros
********************************************************************************/

#define CLI_CBACK_IN_JNI(P_CBACK, ...)                                       \
  if (bt_gatt_callbacks && bt_gatt_callbacks->client->P_CBACK) {             \
    BTIF_TRACE_API("HAL bt_gatt_callbacks->client->%s", #P_CBACK);           \
    do_in_jni_thread(Bind(bt_gatt_callbacks->client->P_CBACK, __VA_ARGS__)); \
  } else {                                                                   \
    ASSERTC(0, "Callback is NULL", 0);                                       \
  }

#define CHECK_BTGATT_INIT()                                        \
  if (bt_gatt_callbacks == NULL) {                                 \
    LOG_WARN(LOG_TAG, "%s: BTGATT not initialized", __func__); \
    return BT_STATUS_NOT_READY;                                    \
  } else {                                                         \
    LOG_VERBOSE(LOG_TAG, "%s", __func__);                      \
  }

#define BLE_RESOLVE_ADDR_MSB 0x40  /* bit7, bit6 is 01 to be resolvable random \
                                      */
#define BLE_RESOLVE_ADDR_MASK 0xc0 /* bit 6, and bit7 */
#define BTM_BLE_IS_RESOLVE_BDA(x) \
  (((x)[0] & BLE_RESOLVE_ADDR_MASK) == BLE_RESOLVE_ADDR_MSB)

namespace std {
template <>
struct hash<bt_bdaddr_t> {
  size_t operator()(const bt_bdaddr_t &f) const {
    return f.address[0] + f.address[1] + f.address[2] + f.address[3] +
           f.address[4] + f.address[5];
  }
};

template <>
struct equal_to<bt_bdaddr_t> {
  size_t operator()(const bt_bdaddr_t &x, const bt_bdaddr_t &y) const {
    return memcmp(x.address, y.address, BD_ADDR_LEN);
  }
};
}

namespace {

std::unordered_set<bt_bdaddr_t> p_dev_cb;
uint8_t rssi_request_client_if;

bt_status_t btif_gattc_multi_adv_disable(int client_if);
void btif_multi_adv_stop_cb(void *data) {
  int client_if = PTR_TO_INT(data);
  btif_gattc_multi_adv_disable(client_if);  // Does context switch
}

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

void btif_gattc_init_dev_cb(void) { p_dev_cb.clear(); }

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

void btif_gattc_upstreams_evt(uint16_t event, char *p_param) {
  LOG_VERBOSE(LOG_TAG, "%s: Event %d", __func__, event);

  tBTA_GATTC *p_data = (tBTA_GATTC *)p_param;
  switch (event) {
    case BTA_GATTC_REG_EVT: {
      bt_uuid_t app_uuid;
      bta_to_btif_uuid(&app_uuid, &p_data->reg_oper.app_uuid);
      HAL_CBACK(bt_gatt_callbacks, client->register_client_cb,
                p_data->reg_oper.status, p_data->reg_oper.client_if, &app_uuid);
      break;
    }

    case BTA_GATTC_DEREG_EVT:
      break;

    case BTA_GATTC_EXEC_EVT: {
      HAL_CBACK(bt_gatt_callbacks, client->execute_write_cb,
                p_data->exec_cmpl.conn_id, p_data->exec_cmpl.status);
      break;
    }

    case BTA_GATTC_SEARCH_CMPL_EVT: {
      HAL_CBACK(bt_gatt_callbacks, client->search_complete_cb,
                p_data->search_cmpl.conn_id, p_data->search_cmpl.status);
      break;
    }

    case BTA_GATTC_NOTIF_EVT: {
      btgatt_notify_params_t data;

      bdcpy(data.bda.address, p_data->notify.bda);
      memcpy(data.value, p_data->notify.value, p_data->notify.len);

      data.handle = p_data->notify.handle;
      data.is_notify = p_data->notify.is_notify;
      data.len = p_data->notify.len;

      HAL_CBACK(bt_gatt_callbacks, client->notify_cb, p_data->notify.conn_id,
                &data);

      if (p_data->notify.is_notify == false)
        BTA_GATTC_SendIndConfirm(p_data->notify.conn_id, p_data->notify.handle);

      break;
    }

    case BTA_GATTC_OPEN_EVT: {
      bt_bdaddr_t bda;
      bdcpy(bda.address, p_data->open.remote_bda);

      HAL_CBACK(bt_gatt_callbacks, client->open_cb, p_data->open.conn_id,
                p_data->open.status, p_data->open.client_if, &bda);

      if (GATT_DEF_BLE_MTU_SIZE != p_data->open.mtu && p_data->open.mtu) {
        HAL_CBACK(bt_gatt_callbacks, client->configure_mtu_cb,
                  p_data->open.conn_id, p_data->open.status, p_data->open.mtu);
      }

      if (p_data->open.status == BTA_GATT_OK)
        btif_gatt_check_encrypted_link(p_data->open.remote_bda,
                                       p_data->open.transport);
      break;
    }

    case BTA_GATTC_CLOSE_EVT: {
      bt_bdaddr_t bda;
      bdcpy(bda.address, p_data->close.remote_bda);
      HAL_CBACK(bt_gatt_callbacks, client->close_cb, p_data->close.conn_id,
                p_data->status, p_data->close.client_if, &bda);
      break;
    }

    case BTA_GATTC_ACL_EVT:
      LOG_DEBUG(LOG_TAG, "BTA_GATTC_ACL_EVT: status = %d", p_data->status);
      /* Ignore for now */
      break;

    case BTA_GATTC_CANCEL_OPEN_EVT:
      break;

    case BTA_GATTC_LISTEN_EVT: {
      HAL_CBACK(bt_gatt_callbacks, client->listen_cb, p_data->reg_oper.status,
                p_data->reg_oper.client_if);
      break;
    }

    case BTA_GATTC_CFG_MTU_EVT: {
      HAL_CBACK(bt_gatt_callbacks, client->configure_mtu_cb,
                p_data->cfg_mtu.conn_id, p_data->cfg_mtu.status,
                p_data->cfg_mtu.mtu);
      break;
    }

    case BTA_GATTC_CONGEST_EVT:
      HAL_CBACK(bt_gatt_callbacks, client->congestion_cb,
                p_data->congest.conn_id, p_data->congest.congested);
      break;

    default:
      LOG_ERROR(LOG_TAG, "%s: Unhandled event (%d)!", __func__, event);
      break;
  }
}

void bta_gattc_cback(tBTA_GATTC_EVT event, tBTA_GATTC *p_data) {
  bt_status_t status =
      btif_transfer_context(btif_gattc_upstreams_evt, (uint16_t)event,
                            (char *)p_data, sizeof(tBTA_GATTC), NULL);
  ASSERTC(status == BT_STATUS_SUCCESS, "Context transfer failed!", status);
}

void multi_adv_enable_cb_impl(int client_if, int status, int inst_id) {
  if (0xFF != inst_id) btif_multi_adv_add_instid_map(client_if, inst_id, false);
  HAL_CBACK(bt_gatt_callbacks, client->multi_adv_enable_cb, client_if, status);
  btif_multi_adv_timer_ctrl(
      client_if, (status == BTA_GATT_OK) ? btif_multi_adv_stop_cb : NULL);
}

void multi_adv_update_cb_impl(int client_if, int status, int inst_id) {
  HAL_CBACK(bt_gatt_callbacks, client->multi_adv_update_cb, client_if, status);
  btif_multi_adv_timer_ctrl(
      client_if, (status == BTA_GATT_OK) ? btif_multi_adv_stop_cb : NULL);
}

void multi_adv_data_cb_impl(int client_if, int status, int inst_id) {
  btif_gattc_clear_clientif(client_if, false);
  HAL_CBACK(bt_gatt_callbacks, client->multi_adv_data_cb, client_if, status);
}

void multi_adv_disable_cb_impl(int client_if, int status, int inst_id) {
  btif_gattc_clear_clientif(client_if, true);
  HAL_CBACK(bt_gatt_callbacks, client->multi_adv_disable_cb, client_if, status);
}

void bta_gattc_multi_adv_cback(tBTA_BLE_MULTI_ADV_EVT event, uint8_t inst_id,
                               void *p_ref, tBTA_STATUS status) {
  uint8_t client_if = 0;

  if (NULL == p_ref) {
    BTIF_TRACE_WARNING("%s Invalid p_ref received", __func__);
  } else {
    client_if = *(uint8_t *)p_ref;
  }

  BTIF_TRACE_DEBUG("%s -Inst ID %d, Status:%x, client_if:%d", __func__, inst_id,
                   status, client_if);

  if (event == BTA_BLE_MULTI_ADV_ENB_EVT)
    do_in_jni_thread(
        Bind(multi_adv_enable_cb_impl, client_if, status, inst_id));
  else if (event == BTA_BLE_MULTI_ADV_DISABLE_EVT)
    do_in_jni_thread(
        Bind(multi_adv_disable_cb_impl, client_if, status, inst_id));
  else if (event == BTA_BLE_MULTI_ADV_PARAM_EVT)
    do_in_jni_thread(
        Bind(multi_adv_update_cb_impl, client_if, status, inst_id));
  else if (event == BTA_BLE_MULTI_ADV_DATA_EVT)
    do_in_jni_thread(Bind(multi_adv_data_cb_impl, client_if, status, inst_id));
}

void bta_gattc_set_adv_data_cback(tBTA_STATUS call_status) {
  do_in_jni_thread(Bind(&btif_gattc_cleanup_inst_cb, STD_ADV_INSTID, false));
}

void bta_batch_scan_setup_cb(tBTA_BLE_BATCH_SCAN_EVT evt,
                             tBTA_DM_BLE_REF_VALUE ref_value,
                             tBTA_STATUS status) {
  BTIF_TRACE_DEBUG("bta_batch_scan_setup_cb-Status:%x, client_if:%d, evt=%d",
                   status, ref_value, evt);

  switch (evt) {
    case BTA_BLE_BATCH_SCAN_ENB_EVT: {
      CLI_CBACK_IN_JNI(batchscan_enb_disable_cb, 1, ref_value, status);
      return;
    }

    case BTA_BLE_BATCH_SCAN_DIS_EVT: {
      CLI_CBACK_IN_JNI(batchscan_enb_disable_cb, 0, ref_value, status);
      return;
    }

    case BTA_BLE_BATCH_SCAN_CFG_STRG_EVT: {
      CLI_CBACK_IN_JNI(batchscan_cfg_storage_cb, ref_value, status);
      return;
    }

    case BTA_BLE_BATCH_SCAN_DATA_EVT: {
      CLI_CBACK_IN_JNI(batchscan_reports_cb, ref_value, status, 0, 0, vector<uint8_t>());
      return;
    }

    case BTA_BLE_BATCH_SCAN_THRES_EVT: {
      CLI_CBACK_IN_JNI(batchscan_threshold_cb, ref_value);
      return;
    }

    default:
      return;
  }
}

void bta_batch_scan_threshold_cb(tBTA_DM_BLE_REF_VALUE ref_value) {
  CLI_CBACK_IN_JNI(batchscan_threshold_cb, ref_value);
}

void bta_batch_scan_reports_cb(tBTA_DM_BLE_REF_VALUE ref_value,
                               uint8_t report_format, uint8_t num_records,
                               uint16_t data_len, uint8_t *p_rep_data,
                               tBTA_STATUS status) {
  BTIF_TRACE_DEBUG("%s - client_if:%d, %d, %d, %d", __func__, ref_value,
                   status, num_records, data_len);

  if (data_len > 0) {

    vector<uint8_t> data(p_rep_data, p_rep_data + data_len);
    osi_free(p_rep_data);

    CLI_CBACK_IN_JNI(batchscan_reports_cb, ref_value, status, report_format,
                     num_records, std::move(data));
  } else {
    CLI_CBACK_IN_JNI(batchscan_reports_cb, ref_value, status, report_format,
                     num_records, vector<uint8_t>());
  }
}

void bta_scan_results_cb_impl(bt_bdaddr_t bd_addr, tBT_DEVICE_TYPE device_type,
                              int8_t rssi, uint8_t addr_type,
                              vector<uint8_t> value) {
  uint8_t remote_name_len;
  const uint8_t *p_eir_remote_name = NULL;
  bt_device_type_t dev_type;
  bt_property_t properties;

  p_eir_remote_name = BTM_CheckEirData(value.data(),
                                       BTM_EIR_COMPLETE_LOCAL_NAME_TYPE,
                                       &remote_name_len);

  if (p_eir_remote_name == NULL) {
    p_eir_remote_name = BTM_CheckEirData(value.data(),
                                         BT_EIR_SHORTENED_LOCAL_NAME_TYPE,
                                         &remote_name_len);
  }

  if ((addr_type != BLE_ADDR_RANDOM) || (p_eir_remote_name)) {
    if (!btif_gattc_find_bdaddr(bd_addr.address)) {
      btif_gattc_add_remote_bdaddr(bd_addr.address, addr_type);

      if (p_eir_remote_name) {
        bt_bdname_t bdname;
        memcpy(bdname.name, p_eir_remote_name, remote_name_len);
        bdname.name[remote_name_len] = '\0';

        LOG_VERBOSE(LOG_TAG, "%s BLE device name=%s len=%d dev_type=%d", __func__,
                  bdname.name, remote_name_len, device_type);
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

  HAL_CBACK(bt_gatt_callbacks, client->scan_result_cb, &bd_addr, rssi, std::move(value));
}

void bta_scan_results_cb(tBTA_DM_SEARCH_EVT event, tBTA_DM_SEARCH *p_data) {
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
    value.insert(value.begin(), p_data->inq_res.p_eir, p_data->inq_res.p_eir + 62);

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

void bta_track_adv_event_cb(tBTA_DM_BLE_TRACK_ADV_DATA *p_track_adv_data) {
  btgatt_track_adv_info_t *btif_scan_track_cb = new btgatt_track_adv_info_t;

  BTIF_TRACE_DEBUG("%s", __func__);
  btif_gatt_move_track_adv_data(btif_scan_track_cb,
                                (btgatt_track_adv_info_t *)p_track_adv_data);

  CLI_CBACK_IN_JNI(track_adv_event_cb, Owned(btif_scan_track_cb));
}

void btm_read_rssi_cb(tBTM_RSSI_RESULTS *p_result) {
  if (!p_result)
    return;

  bt_bdaddr_t *addr = new bt_bdaddr_t;
  bdcpy(addr->address, p_result->rem_bda);
  CLI_CBACK_IN_JNI(read_remote_rssi_cb, rssi_request_client_if,
                   base::Owned(addr), p_result->rssi, p_result->status);
}

void bta_scan_param_setup_cb(tGATT_IF client_if, tBTM_STATUS status) {
  CLI_CBACK_IN_JNI(scan_parameter_setup_completed_cb, client_if,
                   btif_gattc_translate_btm_status(status));
}

void bta_scan_filt_cfg_cb(tBTA_DM_BLE_PF_ACTION action,
                          tBTA_DM_BLE_SCAN_COND_OP cfg_op,
                          tBTA_DM_BLE_PF_AVBL_SPACE avbl_space,
                          tBTA_STATUS status, tBTA_DM_BLE_REF_VALUE ref_value) {
  CLI_CBACK_IN_JNI(scan_filter_cfg_cb, action, ref_value, status, cfg_op,
                   avbl_space);
}

void bta_scan_filt_param_setup_cb(uint8_t action_type,
                                  tBTA_DM_BLE_PF_AVBL_SPACE avbl_space,
                                  tBTA_DM_BLE_REF_VALUE ref_value,
                                  tBTA_STATUS status) {
  CLI_CBACK_IN_JNI(scan_filter_param_cb, action_type, ref_value, status,
                   avbl_space);
}

void bta_scan_filt_status_cb(uint8_t action, tBTA_STATUS status,
                             tBTA_DM_BLE_REF_VALUE ref_value) {
  CLI_CBACK_IN_JNI(scan_filter_status_cb, action, ref_value, status);
}

/*******************************************************************************
**  Client API Functions
********************************************************************************/

void btif_gattc_register_app_impl(tBT_UUID uuid) {
  btif_gattc_incr_app_count();
  BTA_GATTC_AppRegister(&uuid, bta_gattc_cback);
}

bt_status_t btif_gattc_register_app(bt_uuid_t *uuid) {
  CHECK_BTGATT_INIT();

  tBT_UUID bt_uuid;
  btif_to_bta_uuid(&bt_uuid, uuid);
  return do_in_jni_thread(Bind(&btif_gattc_register_app_impl, bt_uuid));
}

void btif_gattc_unregister_app_impl(int client_if) {
  btif_gattc_clear_clientif(client_if, true);
  btif_gattc_decr_app_count();
  BTA_GATTC_AppDeregister(client_if);
}

bt_status_t btif_gattc_unregister_app(int client_if) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(&btif_gattc_unregister_app_impl, client_if));
}

bt_status_t btif_gattc_scan(bool start) {
  CHECK_BTGATT_INIT();
  if (start) {
    btif_gattc_init_dev_cb();
    return do_in_jni_thread(Bind(&BTA_DmBleObserve, true, 0,
                                 (tBTA_DM_SEARCH_CBACK *)bta_scan_results_cb));
  } else {
    return do_in_jni_thread(Bind(&BTA_DmBleObserve, false, 0, nullptr));
  }
}

void btif_gattc_open_impl(int client_if, BD_ADDR address, bool is_direct,
                          int transport_p) {
  // Ensure device is in inquiry database
  int addr_type = 0;
  int device_type = 0;
  tBTA_GATT_TRANSPORT transport = (tBTA_GATT_TRANSPORT)BTA_GATT_TRANSPORT_LE;

  if (btif_get_address_type(address, &addr_type) &&
      btif_get_device_type(address, &device_type) &&
      device_type != BT_DEVICE_TYPE_BREDR) {
    BTA_DmAddBleDevice(address, addr_type, device_type);
  }

  // Check for background connections
  if (!is_direct) {
    // Check for privacy 1.0 and 1.1 controller and do not start background
    // connection if RPA offloading is not supported, since it will not
    // connect after change of random address
    if (!controller_get_interface()->supports_ble_privacy() &&
        (addr_type == BLE_ADDR_RANDOM) && BTM_BLE_IS_RESOLVE_BDA(address)) {
      tBTM_BLE_VSC_CB vnd_capabilities;
      BTM_BleGetVendorCapabilities(&vnd_capabilities);
      if (!vnd_capabilities.rpa_offloading) {
        HAL_CBACK(bt_gatt_callbacks, client->open_cb, 0, BT_STATUS_UNSUPPORTED,
                  client_if, (bt_bdaddr_t *)&address);
        return;
      }
    }
    BTA_DmBleSetBgConnType(BTM_BLE_CONN_AUTO, NULL);
  }

  // Determine transport
  if (transport_p != GATT_TRANSPORT_AUTO) {
    transport = transport_p;
  } else {
    switch (device_type) {
      case BT_DEVICE_TYPE_BREDR:
        transport = BTA_GATT_TRANSPORT_BR_EDR;
        break;

      case BT_DEVICE_TYPE_BLE:
        transport = BTA_GATT_TRANSPORT_LE;
        break;

      case BT_DEVICE_TYPE_DUMO:
        if (transport == GATT_TRANSPORT_LE)
          transport = BTA_GATT_TRANSPORT_LE;
        else
          transport = BTA_GATT_TRANSPORT_BR_EDR;
        break;
    }
  }

  // Connect!
  BTIF_TRACE_DEBUG("%s Transport=%d, device type=%d", __func__, transport,
                   device_type);
  BTA_GATTC_Open(client_if, address, is_direct, transport);
}

bt_status_t btif_gattc_open(int client_if, const bt_bdaddr_t *bd_addr,
                            bool is_direct, int transport) {
  CHECK_BTGATT_INIT();
  // Closure will own this value and free it.
  uint8_t *address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(Bind(&btif_gattc_open_impl, client_if,
                               base::Owned(address), is_direct, transport));
}

void btif_gattc_close_impl(int client_if, BD_ADDR address, int conn_id) {
  // Disconnect established connections
  if (conn_id != 0)
    BTA_GATTC_Close(conn_id);
  else
    BTA_GATTC_CancelOpen(client_if, address, true);

  // Cancel pending background connections (remove from whitelist)
  BTA_GATTC_CancelOpen(client_if, address, false);
}

bt_status_t btif_gattc_close(int client_if, const bt_bdaddr_t *bd_addr,
                             int conn_id) {
  CHECK_BTGATT_INIT();
  // Closure will own this value and free it.
  uint8_t *address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(
      Bind(&btif_gattc_close_impl, client_if, base::Owned(address), conn_id));
}

bt_status_t btif_gattc_listen(int client_if, bool start) {
  CHECK_BTGATT_INIT();
#if (defined(BLE_PERIPHERAL_MODE_SUPPORT) && \
     (BLE_PERIPHERAL_MODE_SUPPORT == true))
  return do_in_jni_thread(Bind(&BTA_GATTC_Listen, client_if, start, nullptr));
#else
  return do_in_jni_thread(Bind(&BTA_GATTC_Broadcast, client_if, start));
#endif
}

void btif_gattc_set_adv_data_impl(btif_adv_data_t *p_adv_data) {
  const int cbindex = CLNT_IF_IDX;
  if (cbindex >= 0 && btif_gattc_copy_datacb(cbindex, p_adv_data, false)) {
    btgatt_multi_adv_common_data *p_multi_adv_data_cb =
        btif_obtain_multi_adv_data_cb();
    if (!p_adv_data->set_scan_rsp) {
      BTA_DmBleSetAdvConfig(p_multi_adv_data_cb->inst_cb[cbindex].mask,
                            &p_multi_adv_data_cb->inst_cb[cbindex].data,
                            bta_gattc_set_adv_data_cback);
    } else {
      BTA_DmBleSetScanRsp(p_multi_adv_data_cb->inst_cb[cbindex].mask,
                          &p_multi_adv_data_cb->inst_cb[cbindex].data,
                          bta_gattc_set_adv_data_cback);
    }
  } else {
    BTIF_TRACE_ERROR("%s: failed to get instance data cbindex: %d", __func__,
                     cbindex);
  }
}

bt_status_t btif_gattc_set_adv_data(
    int client_if, bool set_scan_rsp, bool include_name, bool include_txpower,
    int min_interval, int max_interval, int appearance,
    vector<uint8_t> manufacturer_data, vector<uint8_t> service_data,
    vector<uint8_t> service_uuid) {
  CHECK_BTGATT_INIT();

  btif_adv_data_t *adv_data = new btif_adv_data_t;

  btif_gattc_adv_data_packager(
      client_if, set_scan_rsp, include_name, include_txpower, min_interval,
      max_interval, appearance, std::move(manufacturer_data), std::move(service_data),
      std::move(service_uuid), adv_data);

  return do_in_jni_thread(
      Bind(&btif_gattc_set_adv_data_impl, base::Owned(adv_data)));
}

bt_status_t btif_gattc_refresh(int client_if, const bt_bdaddr_t *bd_addr) {
  CHECK_BTGATT_INIT();
  // Closure will own this value and free it.
  uint8_t *address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(Bind(&BTA_GATTC_Refresh, base::Owned(address)));
}

bt_status_t btif_gattc_search_service(int conn_id, bt_uuid_t *filter_uuid) {
  CHECK_BTGATT_INIT();

  if (filter_uuid) {
    tBT_UUID *uuid = new tBT_UUID;
    btif_to_bta_uuid(uuid, filter_uuid);
    return do_in_jni_thread(
        Bind(&BTA_GATTC_ServiceSearchRequest, conn_id, base::Owned(uuid)));
  } else {
    return do_in_jni_thread(
        Bind(&BTA_GATTC_ServiceSearchRequest, conn_id, nullptr));
  }
}

void btif_gattc_get_gatt_db_impl(int conn_id) {
  btgatt_db_element_t *db = NULL;
  int count = 0;
  BTA_GATTC_GetGattDb(conn_id, 0x0000, 0xFFFF, &db, &count);

  HAL_CBACK(bt_gatt_callbacks, client->get_gatt_db_cb, conn_id, db, count);
  osi_free(db);
}

bt_status_t btif_gattc_get_gatt_db(int conn_id) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(&btif_gattc_get_gatt_db_impl, conn_id));
}

void read_char_cb(uint16_t conn_id, tGATT_STATUS status, uint16_t handle,
                  uint16_t len, uint8_t *value, void *data) {
  btgatt_read_params_t *params = new btgatt_read_params_t;
  params->value_type = 0x00 /* GATTC_READ_VALUE_TYPE_VALUE */;
  params->status = status;
  params->handle = handle;
  params->value.len = len;
  assert(len <= BTGATT_MAX_ATTR_LEN);
  if (len > 0) memcpy(params->value.value, value, len);

  CLI_CBACK_IN_JNI(read_characteristic_cb, conn_id, status,
                   base::Owned(params));
}

bt_status_t btif_gattc_read_char(int conn_id, uint16_t handle, int auth_req) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(&BTA_GATTC_ReadCharacteristic, conn_id, handle,
                               auth_req, read_char_cb, nullptr));
}

void read_desc_cb(uint16_t conn_id, tGATT_STATUS status, uint16_t handle,
                  uint16_t len, uint8_t *value, void *data) {
  btgatt_read_params_t *params = new btgatt_read_params_t;
  params->value_type = 0x00 /* GATTC_READ_VALUE_TYPE_VALUE */;
  params->status = status;
  params->handle = handle;
  params->value.len = len;
  assert(len <= BTGATT_MAX_ATTR_LEN);
  if (len > 0) memcpy(params->value.value, value, len);

  CLI_CBACK_IN_JNI(read_descriptor_cb, conn_id, status, base::Owned(params));
}

bt_status_t btif_gattc_read_char_descr(int conn_id, uint16_t handle,
                                       int auth_req) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(&BTA_GATTC_ReadCharDescr, conn_id, handle,
                               auth_req, read_desc_cb, nullptr));
}

void write_char_cb(uint16_t conn_id, tGATT_STATUS status, uint16_t handle,
                   void *data) {
  CLI_CBACK_IN_JNI(write_characteristic_cb, conn_id, status, handle);
}

bt_status_t btif_gattc_write_char(int conn_id, uint16_t handle, int write_type,
                                  int auth_req, vector<uint8_t> value) {
  CHECK_BTGATT_INIT();

  if (value.size() > BTGATT_MAX_ATTR_LEN)
    value.resize(BTGATT_MAX_ATTR_LEN);

  return do_in_jni_thread(Bind(&BTA_GATTC_WriteCharValue, conn_id, handle,
                               write_type, std::move(value), auth_req,
                               write_char_cb, nullptr));
}

void write_descr_cb(uint16_t conn_id, tGATT_STATUS status, uint16_t handle,
                    void *data) {
  CLI_CBACK_IN_JNI(write_descriptor_cb, conn_id, status, handle);
}

bt_status_t btif_gattc_write_char_descr(int conn_id, uint16_t handle,
                                        int auth_req, vector<uint8_t> value) {
  CHECK_BTGATT_INIT();

  if (value.size() > BTGATT_MAX_ATTR_LEN)
    value.resize(BTGATT_MAX_ATTR_LEN);

  return do_in_jni_thread(Bind(&BTA_GATTC_WriteCharDescr, conn_id, handle,
                               std::move(value), auth_req,
                               write_descr_cb, nullptr));
}

bt_status_t btif_gattc_execute_write(int conn_id, int execute) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(&BTA_GATTC_ExecuteWrite, conn_id, (uint8_t)execute));
}

void btif_gattc_reg_for_notification_impl(tBTA_GATTC_IF client_if,
                                          const BD_ADDR bda, uint16_t handle) {
  tBTA_GATT_STATUS status = BTA_GATTC_RegisterForNotifications(
      client_if, const_cast<uint8_t *>(bda), handle);

  // TODO(jpawlowski): conn_id is currently unused
  HAL_CBACK(bt_gatt_callbacks, client->register_for_notification_cb,
            /* conn_id */ 0, 1, status, handle);
}

bt_status_t btif_gattc_reg_for_notification(int client_if,
                                            const bt_bdaddr_t *bd_addr,
                                            uint16_t handle) {
  CHECK_BTGATT_INIT();

  uint8_t *address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(
      Bind(base::IgnoreResult(&btif_gattc_reg_for_notification_impl), client_if,
           base::Owned(address), handle));
}

void btif_gattc_dereg_for_notification_impl(tBTA_GATTC_IF client_if,
                                            const BD_ADDR bda, uint16_t handle) {
  tBTA_GATT_STATUS status = BTA_GATTC_DeregisterForNotifications(
      client_if, const_cast<uint8_t *>(bda), handle);

  // TODO(jpawlowski): conn_id is currently unused
  HAL_CBACK(bt_gatt_callbacks, client->register_for_notification_cb,
            /* conn_id */ 0, 0, status, handle);
}

bt_status_t btif_gattc_dereg_for_notification(int client_if,
                                              const bt_bdaddr_t *bd_addr,
                                              uint16_t handle) {
  CHECK_BTGATT_INIT();

  uint8_t *address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(
      Bind(base::IgnoreResult(&btif_gattc_dereg_for_notification_impl),
           client_if, base::Owned(address), handle));
}

bt_status_t btif_gattc_read_remote_rssi(int client_if,
                                        const bt_bdaddr_t *bd_addr) {
  CHECK_BTGATT_INIT();
  rssi_request_client_if = client_if;
  // Closure will own this value and free it.
  uint8_t *address = new BD_ADDR;
  bdcpy(address, bd_addr->address);
  return do_in_jni_thread(Bind(base::IgnoreResult(&BTM_ReadRSSI),
                               base::Owned(address),
                               (tBTM_CMPL_CB *)btm_read_rssi_cb));
}

bt_status_t btif_gattc_configure_mtu(int conn_id, int mtu) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(base::IgnoreResult(&BTA_GATTC_ConfigureMTU), conn_id, mtu));
}

void btif_gattc_conn_parameter_update_impl(bt_bdaddr_t addr, int min_interval,
                                           int max_interval, int latency,
                                           int timeout) {
  if (BTA_DmGetConnectionState(addr.address))
    BTA_DmBleUpdateConnectionParams(addr.address, min_interval,
                                    max_interval, latency, timeout);
  else
    BTA_DmSetBlePrefConnParams(addr.address, min_interval,
                               max_interval, latency, timeout);
}

bt_status_t btif_gattc_conn_parameter_update(const bt_bdaddr_t *bd_addr,
                                             int min_interval, int max_interval,
                                             int latency, int timeout) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(base::IgnoreResult(&btif_gattc_conn_parameter_update_impl),
           *bd_addr, min_interval, max_interval, latency, timeout));
}

void btif_gattc_scan_filter_param_setup_impl(
    int client_if, uint8_t action, int filt_index,
    tBTA_DM_BLE_PF_FILT_PARAMS *adv_filt_param) {
  if (1 == adv_filt_param->dely_mode)
    BTA_DmBleTrackAdvertiser(client_if, bta_track_adv_event_cb);
  BTA_DmBleScanFilterSetup(action, filt_index, adv_filt_param, NULL,
                           bta_scan_filt_param_setup_cb, client_if);
}

bt_status_t btif_gattc_scan_filter_param_setup(
    btgatt_filt_param_setup_t filt_param) {
  CHECK_BTGATT_INIT();
  BTIF_TRACE_DEBUG("%s", __func__);

  tBTA_DM_BLE_PF_FILT_PARAMS *adv_filt_param = new tBTA_DM_BLE_PF_FILT_PARAMS;
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
                                          tBTA_DM_BLE_PF_COND_MASK *p_uuid_mask,
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

void btif_gattc_scan_filter_add_local_name(vector<uint8_t> data,
                                           int action, int filt_type,
                                           int filt_index, int client_if) {
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
    int company_id_mask, const bt_uuid_t *p_uuid, const bt_uuid_t *p_uuid_mask,
    const bt_bdaddr_t *bd_addr, char addr_type, vector<uint8_t> data,
    vector<uint8_t> mask) {
  CHECK_BTGATT_INIT();
  BTIF_TRACE_DEBUG("%s, %d, %d", __func__, action, filt_type);

  /* If data is passed, both mask and data have to be the same length */
  if (data.size() != mask.size() && data.size() != 0 && mask.size() != 0)
    return BT_STATUS_PARM_INVALID;

  switch (filt_type) {
    case BTA_DM_BLE_PF_ADDR_FILTER:  // 0
    {
      tBTA_DM_BLE_PF_COND_PARAM *cond = new tBTA_DM_BLE_PF_COND_PARAM;
      memset(cond, 0, sizeof(tBTA_DM_BLE_PF_COND_PARAM));

      bdcpy(cond->target_addr.bda, bd_addr->address);
      cond->target_addr.type = addr_type;
      return do_in_jni_thread(Bind(&BTA_DmBleCfgFilterCondition, action,
                                   filt_type, filt_index, base::Owned(cond),
                                   &bta_scan_filt_cfg_cb, client_if));
    }

    case BTA_DM_BLE_PF_SRVC_DATA:  // 1
      return do_in_jni_thread(Bind(&BTA_DmBleCfgFilterCondition, action,
                                   filt_type, filt_index, nullptr,
                                   &bta_scan_filt_cfg_cb, client_if));

    case BTA_DM_BLE_PF_SRVC_UUID:  // 2
    {
      tBT_UUID bt_uuid;
      btif_to_bta_uuid(&bt_uuid, p_uuid);

      if (p_uuid_mask != NULL) {
        tBTA_DM_BLE_PF_COND_MASK *uuid_mask = new tBTA_DM_BLE_PF_COND_MASK;
        btif_to_bta_uuid_mask(uuid_mask, p_uuid_mask);
        return do_in_jni_thread(Bind(&btif_gattc_scan_filter_add_srvc_uuid,
                                     bt_uuid, base::Owned(uuid_mask), action,
                                     filt_type, filt_index, client_if));
      }

      return do_in_jni_thread(Bind(&btif_gattc_scan_filter_add_srvc_uuid,
                                   bt_uuid, nullptr, action, filt_type,
                                   filt_index, client_if));
    }

    case BTA_DM_BLE_PF_SRVC_SOL_UUID:  // 3
    {
      tBTA_DM_BLE_PF_COND_PARAM *cond = new tBTA_DM_BLE_PF_COND_PARAM;
      memset(cond, 0, sizeof(tBTA_DM_BLE_PF_COND_PARAM));

      cond->solicitate_uuid.p_target_addr = NULL;
      cond->solicitate_uuid.cond_logic = BTA_DM_BLE_PF_LOGIC_AND;
      btif_to_bta_uuid(&cond->solicitate_uuid.uuid, p_uuid);

      return do_in_jni_thread(Bind(&BTA_DmBleCfgFilterCondition, action,
                                   filt_type, filt_index, base::Owned(cond),
                                   &bta_scan_filt_cfg_cb, client_if));
    }

    case BTA_DM_BLE_PF_LOCAL_NAME:  // 4
    {
      return do_in_jni_thread(Bind(&btif_gattc_scan_filter_add_local_name,
                                   std::move(data), action, filt_type,
                                   filt_index, client_if));
    }

    case BTA_DM_BLE_PF_MANU_DATA:  // 5
    {
      return do_in_jni_thread(
          Bind(&btif_gattc_scan_filter_add_manu_data, company_id,
               company_id_mask, std::move(data), std::move(mask), action,
               filt_type, filt_index, client_if));
    }

    case BTA_DM_BLE_PF_SRVC_DATA_PATTERN:  // 6
    {
      return do_in_jni_thread(Bind(
          &btif_gattc_scan_filter_add_data_pattern, std::move(data),
          std::move(mask), action, filt_type, filt_index, client_if));
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

  uint8_t action = enable ? 1: 0;

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

int btif_gattc_get_device_type(const bt_bdaddr_t *bd_addr) {
  int device_type = 0;
  char bd_addr_str[18] = {0};

  bdaddr_to_string(bd_addr, bd_addr_str, sizeof(bd_addr_str));
  if (btif_config_get_int(bd_addr_str, "DevType", &device_type))
    return device_type;
  return 0;
}

void btif_gattc_multi_adv_enable_impl(int client_if, int min_interval,
                                      int max_interval, int adv_type,
                                      int chnl_map, int tx_power,
                                      int timeout_s) {
  tBTA_BLE_ADV_PARAMS param;
  param.adv_int_min = min_interval;
  param.adv_int_max = max_interval;
  param.adv_type = adv_type;
  param.channel_map = chnl_map;
  param.adv_filter_policy = 0;
  param.tx_power = tx_power;

  int cbindex = -1;
  int arrindex = btif_multi_adv_add_instid_map(client_if, INVALID_ADV_INST, true);
  if (arrindex >= 0)
    cbindex = btif_gattc_obtain_idx_for_datacb(client_if, CLNT_IF_IDX);

  if (cbindex >= 0 && arrindex >= 0) {
    btgatt_multi_adv_common_data *p_multi_adv_data_cb =
        btif_obtain_multi_adv_data_cb();
    memcpy(&p_multi_adv_data_cb->inst_cb[cbindex].param, &param,
           sizeof(tBTA_BLE_ADV_PARAMS));
    p_multi_adv_data_cb->inst_cb[cbindex].timeout_s = timeout_s;
    BTIF_TRACE_DEBUG("%s, client_if value: %d", __func__,
                     p_multi_adv_data_cb->clntif_map[arrindex + arrindex]);
    BTA_BleEnableAdvInstance(
        &(p_multi_adv_data_cb->inst_cb[cbindex].param),
        bta_gattc_multi_adv_cback,
        &(p_multi_adv_data_cb->clntif_map[arrindex + arrindex]));
  } else {
    // let the error propagate up from BTA layer
    BTIF_TRACE_ERROR("%s invalid index arrindex: %d, cbindex: %d",
                     __func__, arrindex, cbindex);
    BTA_BleEnableAdvInstance(&param, bta_gattc_multi_adv_cback, NULL);
  }
}

bt_status_t btif_gattc_multi_adv_enable(int client_if, int min_interval,
                                        int max_interval, int adv_type,
                                        int chnl_map, int tx_power,
                                        int timeout_s) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(btif_gattc_multi_adv_enable_impl, client_if,
                               min_interval, max_interval, adv_type, chnl_map,
                               tx_power, timeout_s));
}

void btif_gattc_multi_adv_update_impl(int client_if, int min_interval,
                                      int max_interval, int adv_type,
                                      int chnl_map, int tx_power) {
  tBTA_BLE_ADV_PARAMS param;
  param.adv_int_min = min_interval;
  param.adv_int_max = max_interval;
  param.adv_type = adv_type;
  param.channel_map = chnl_map;
  param.adv_filter_policy = 0;
  param.tx_power = tx_power;

  int inst_id = btif_multi_adv_instid_for_clientif(client_if);
  int cbindex = btif_gattc_obtain_idx_for_datacb(client_if, CLNT_IF_IDX);
  if (inst_id >= 0 && cbindex >= 0) {
    btgatt_multi_adv_common_data *p_multi_adv_data_cb =
        btif_obtain_multi_adv_data_cb();
    memcpy(&p_multi_adv_data_cb->inst_cb[cbindex].param, &param,
           sizeof(tBTA_BLE_ADV_PARAMS));
    BTA_BleUpdateAdvInstParam((uint8_t)inst_id,
                              &(p_multi_adv_data_cb->inst_cb[cbindex].param));
  } else {
    BTIF_TRACE_ERROR("%s invalid index in BTIF_GATTC_UPDATE_ADV", __func__);
  }
}

bt_status_t btif_gattc_multi_adv_update(int client_if, int min_interval,
                                        int max_interval, int adv_type,
                                        int chnl_map, int tx_power,
                                        int timeout_s) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(btif_gattc_multi_adv_update_impl, client_if,
                               min_interval, max_interval, adv_type, chnl_map,
                               tx_power));
}

void btif_gattc_multi_adv_setdata_impl(btif_adv_data_t *p_adv_data) {
  int cbindex =
      btif_gattc_obtain_idx_for_datacb(p_adv_data->client_if, CLNT_IF_IDX);
  int inst_id = btif_multi_adv_instid_for_clientif(p_adv_data->client_if);
  if (inst_id >= 0 && cbindex >= 0 &&
      btif_gattc_copy_datacb(cbindex, p_adv_data, true)) {
    btgatt_multi_adv_common_data *p_multi_adv_data_cb =
        btif_obtain_multi_adv_data_cb();
    BTA_BleCfgAdvInstData((uint8_t)inst_id, p_adv_data->set_scan_rsp,
                          p_multi_adv_data_cb->inst_cb[cbindex].mask,
                          &p_multi_adv_data_cb->inst_cb[cbindex].data);
  } else {
    BTIF_TRACE_ERROR(
        "%s: failed to get invalid instance data: inst_id:%d cbindex:%d",
        __func__, inst_id, cbindex);
  }
}

bt_status_t btif_gattc_multi_adv_setdata(
    int client_if, bool set_scan_rsp, bool include_name, bool incl_txpower,
    int appearance, vector<uint8_t> manufacturer_data,
    vector<uint8_t> service_data, vector<uint8_t> service_uuid) {
  CHECK_BTGATT_INIT();

  btif_adv_data_t *multi_adv_data_inst = new btif_adv_data_t;

  const int min_interval = 0;
  const int max_interval = 0;

  btif_gattc_adv_data_packager(client_if, set_scan_rsp, include_name,
                               incl_txpower, min_interval, max_interval,
                               appearance, std::move(manufacturer_data),
                               std::move(service_data), std::move(service_uuid),
                               multi_adv_data_inst);

  return do_in_jni_thread(Bind(&btif_gattc_multi_adv_setdata_impl,
                               base::Owned(multi_adv_data_inst)));
}

void btif_gattc_multi_adv_disable_impl(int client_if) {
  int inst_id = btif_multi_adv_instid_for_clientif(client_if);
  if (inst_id >= 0)
    BTA_BleDisableAdvInstance((uint8_t)inst_id);
  else
    BTIF_TRACE_ERROR("%s invalid instance ID", __func__);
}

bt_status_t btif_gattc_multi_adv_disable(int client_if) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(btif_gattc_multi_adv_disable_impl, client_if));
}

bt_status_t btif_gattc_cfg_storage(int client_if, int batch_scan_full_max,
                                   int batch_scan_trunc_max,
                                   int batch_scan_notify_threshold) {
  CHECK_BTGATT_INIT();
  return do_in_jni_thread(
      Bind(BTA_DmBleSetStorageParams, batch_scan_full_max, batch_scan_trunc_max,
           batch_scan_notify_threshold,
           (tBTA_BLE_SCAN_SETUP_CBACK *)bta_batch_scan_setup_cb,
           (tBTA_BLE_SCAN_THRESHOLD_CBACK *)bta_batch_scan_threshold_cb,
           (tBTA_BLE_SCAN_REP_CBACK *)bta_batch_scan_reports_cb,
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

bt_status_t btif_gattc_test_command(int command, btgatt_test_params_t *params) {
  return btif_gattc_test_command_impl(command, params);
}

}  // namespace

const btgatt_client_interface_t btgattClientInterface = {
    btif_gattc_register_app,
    btif_gattc_unregister_app,
    btif_gattc_scan,
    btif_gattc_open,
    btif_gattc_close,
    btif_gattc_listen,
    btif_gattc_refresh,
    btif_gattc_search_service,
    btif_gattc_read_char,
    btif_gattc_write_char,
    btif_gattc_read_char_descr,
    btif_gattc_write_char_descr,
    btif_gattc_execute_write,
    btif_gattc_reg_for_notification,
    btif_gattc_dereg_for_notification,
    btif_gattc_read_remote_rssi,
    btif_gattc_scan_filter_param_setup,
    btif_gattc_scan_filter_add_remove,
    btif_gattc_scan_filter_clear,
    btif_gattc_scan_filter_enable,
    btif_gattc_get_device_type,
    btif_gattc_set_adv_data,
    btif_gattc_configure_mtu,
    btif_gattc_conn_parameter_update,
    btif_gattc_set_scan_parameters,
    btif_gattc_multi_adv_enable,
    btif_gattc_multi_adv_update,
    btif_gattc_multi_adv_setdata,
    btif_gattc_multi_adv_disable,
    btif_gattc_cfg_storage,
    btif_gattc_enb_batch_scan,
    btif_gattc_dis_batch_scan,
    btif_gattc_read_batch_scan_reports,
    btif_gattc_test_command,
    btif_gattc_get_gatt_db};

#endif
