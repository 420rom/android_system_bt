/******************************************************************************
 *
 *  Copyright (C) 2004-2012 Broadcom Corporation
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

/******************************************************************************
 *
 *  This is the advanced audio/video call-out function implementation for
 *  BTIF.
 *
 ******************************************************************************/

#include "bta_av_co.h"
#include <assert.h>
#include <string.h>
#include "a2dp_api.h"
#include "bt_target.h"
#include "bta_av_api.h"
#include "bta_av_ci.h"
#include "bta_sys.h"

#include "btif_av_co.h"
#include "btif_util.h"
#include "osi/include/mutex.h"
#include "osi/include/osi.h"

/*****************************************************************************
 **  Constants
 *****************************************************************************/

/* Macro to retrieve the number of elements in a statically allocated array */
#define BTA_AV_CO_NUM_ELEMENTS(__a) (sizeof(__a) / sizeof((__a)[0]))

/* MIN and MAX macros */
#define BTA_AV_CO_MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#define BTA_AV_CO_MAX(X, Y) ((X) > (Y) ? (X) : (Y))

/* Macro to convert audio handle to index and vice versa */
#define BTA_AV_CO_AUDIO_HNDL_TO_INDX(hndl) (((hndl) & (~BTA_AV_CHNL_MSK)) - 1)
#define BTA_AV_CO_AUDIO_INDX_TO_HNDL(indx) (((indx) + 1) | BTA_AV_CHNL_AUDIO)

/* SCMS-T protect info */
const uint8_t bta_av_co_cp_scmst[AVDT_CP_INFO_LEN] = {0x02, 0x02, 0x00};

/*****************************************************************************
 *  Local data
 ****************************************************************************/
typedef struct {
  uint8_t sep_info_idx;                   /* local SEP index (in BTA tables) */
  uint8_t seid;                           /* peer SEP index (in peer tables) */
  uint8_t codec_caps[AVDT_CODEC_SIZE];    /* peer SEP codec capabilities */
  uint8_t num_protect;                    /* peer SEP number of CP elements */
  uint8_t protect_info[AVDT_CP_INFO_LEN]; /* peer SEP content protection info */
} tBTA_AV_CO_SINK;

typedef struct {
  BD_ADDR addr; /* address of audio/video peer */
  tBTA_AV_CO_SINK
      sinks[A2DP_CODEC_SEP_INDEX_MAX];            /* array of supported sinks */
  tBTA_AV_CO_SINK srcs[A2DP_CODEC_SEP_INDEX_MAX]; /* array of supported srcs */
  uint8_t num_sinks;     /* total number of sinks at peer */
  uint8_t num_srcs;      /* total number of srcs at peer */
  uint8_t num_seps;      /* total number of seids at peer */
  uint8_t num_rx_sinks;  /* number of received sinks */
  uint8_t num_rx_srcs;   /* number of received srcs */
  uint8_t num_sup_sinks; /* number of supported sinks in the sinks array */
  uint8_t num_sup_srcs;  /* number of supported srcs in the srcs array */
  const tBTA_AV_CO_SINK* p_sink;         /* currently selected sink */
  const tBTA_AV_CO_SINK* p_src;          /* currently selected src */
  uint8_t codec_config[AVDT_CODEC_SIZE]; /* current codec configuration */
  bool cp_active;                        /* current CP configuration */
  bool acp;                              /* acceptor */
  bool reconfig_needed;                  /* reconfiguration is needed */
  bool opened;                           /* opened */
  uint16_t mtu;                          /* maximum transmit unit size */
  uint16_t uuid_to_connect;              /* uuid of peer device */
} tBTA_AV_CO_PEER;

typedef struct {
  bool active;
  uint8_t flag;
} tBTA_AV_CO_CP;

typedef struct {
  /* Connected peer information */
  tBTA_AV_CO_PEER peers[BTA_AV_NUM_STRS];
  /* Current codec configuration - access to this variable must be protected */
  uint8_t codec_config[AVDT_CODEC_SIZE];
  uint8_t codec_config_setconfig[AVDT_CODEC_SIZE]; /* remote peer setconfig
                                                    * preference */

  tBTA_AV_CO_CP cp;
} tBTA_AV_CO_CB;

/* Control block instance */
static tBTA_AV_CO_CB bta_av_co_cb;

static bool bta_av_co_cp_is_scmst(const uint8_t* p_protectinfo);
static bool bta_av_co_audio_sink_has_scmst(const tBTA_AV_CO_SINK* p_sink);
static const tBTA_AV_CO_SINK* bta_av_co_find_peer_sink_supports_codec(
    const uint8_t* codec_config, const tBTA_AV_CO_PEER* p_peer);
static const tBTA_AV_CO_SINK* bta_av_co_find_peer_src_supports_codec(
    const tBTA_AV_CO_PEER* p_peer);
static bool bta_av_co_audio_codec_selected(const uint8_t* codec_config);

/*******************************************************************************
 **
 ** Function         bta_av_co_cp_get_flag
 **
 ** Description      Get content protection flag
 **                  AVDT_CP_SCMS_COPY_NEVER
 **                  AVDT_CP_SCMS_COPY_ONCE
 **                  AVDT_CP_SCMS_COPY_FREE
 **
 ** Returns          The current flag value
 **
 ******************************************************************************/
static uint8_t bta_av_co_cp_get_flag(void) { return bta_av_co_cb.cp.flag; }

/*******************************************************************************
 **
 ** Function         bta_av_co_cp_set_flag
 **
 ** Description      Set content protection flag
 **                  AVDT_CP_SCMS_COPY_NEVER
 **                  AVDT_CP_SCMS_COPY_ONCE
 **                  AVDT_CP_SCMS_COPY_FREE
 **
 ** Returns          true if setting the SCMS flag is supported else false
 **
 ******************************************************************************/
static bool bta_av_co_cp_set_flag(uint8_t cp_flag) {
  APPL_TRACE_DEBUG("%s: cp_flag = %d", __func__, cp_flag);

#if (BTA_AV_CO_CP_SCMS_T == TRUE)
#else
  if (cp_flag != AVDT_CP_SCMS_COPY_FREE) {
    return false;
  }
#endif
  bta_av_co_cb.cp.flag = cp_flag;
  return true;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_get_peer
 **
 ** Description      find the peer entry for a given handle
 **
 ** Returns          the control block
 **
 ******************************************************************************/
static tBTA_AV_CO_PEER* bta_av_co_get_peer(tBTA_AV_HNDL hndl) {
  uint8_t index;

  index = BTA_AV_CO_AUDIO_HNDL_TO_INDX(hndl);

  APPL_TRACE_DEBUG("%s: handle = %d index = %d", __func__, hndl, index);

  /* Sanity check */
  if (index >= BTA_AV_CO_NUM_ELEMENTS(bta_av_co_cb.peers)) {
    APPL_TRACE_ERROR("%s: peer index out of bounds: %d", __func__, index);
    return NULL;
  }

  return &bta_av_co_cb.peers[index];
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_init
 **
 ** Description      This callout function is executed by AV when it is
 **                  started by calling BTA_AvRegister().  This function can be
 **                  used by the phone to initialize audio paths or for other
 **                  initialization purposes.
 **
 **
 ** Returns          Stream codec and content protection capabilities info.
 **
 ******************************************************************************/
bool bta_av_co_audio_init(tA2DP_CODEC_SEP_INDEX codec_sep_index,
                          tAVDT_CFG* p_cfg) {
  /* reset remote preference through setconfig */
  memset(bta_av_co_cb.codec_config_setconfig, 0,
         sizeof(bta_av_co_cb.codec_config_setconfig));

  return A2DP_InitCodecConfig(codec_sep_index, p_cfg);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_disc_res
 **
 ** Description      This callout function is executed by AV to report the
 **                  number of stream end points (SEP) were found during the
 **                  AVDT stream discovery process.
 **
 **
 ** Returns          void.
 **
 ******************************************************************************/
void bta_av_co_audio_disc_res(tBTA_AV_HNDL hndl, uint8_t num_seps,
                              uint8_t num_sink, uint8_t num_src, BD_ADDR addr,
                              uint16_t uuid_local) {
  tBTA_AV_CO_PEER* p_peer;

  APPL_TRACE_DEBUG("%s: h:x%x num_seps:%d num_sink:%d num_src:%d", __func__,
                   hndl, num_seps, num_sink, num_src);

  /* Find the peer info */
  p_peer = bta_av_co_get_peer(hndl);
  if (p_peer == NULL) {
    APPL_TRACE_ERROR("%s: could not find peer entry", __func__);
    return;
  }

  /* Sanity check : this should never happen */
  if (p_peer->opened) {
    APPL_TRACE_ERROR("%s: peer already opened", __func__);
  }

  /* Copy the discovery results */
  bdcpy(p_peer->addr, addr);
  p_peer->num_sinks = num_sink;
  p_peer->num_srcs = num_src;
  p_peer->num_seps = num_seps;
  p_peer->num_rx_sinks = 0;
  p_peer->num_rx_srcs = 0;
  p_peer->num_sup_sinks = 0;
  if (uuid_local == UUID_SERVCLASS_AUDIO_SINK)
    p_peer->uuid_to_connect = UUID_SERVCLASS_AUDIO_SOURCE;
  else if (uuid_local == UUID_SERVCLASS_AUDIO_SOURCE)
    p_peer->uuid_to_connect = UUID_SERVCLASS_AUDIO_SINK;
}

/*******************************************************************************
 **
 ** Function         bta_av_audio_sink_getconfig
 **
 ** Description      This callout function is executed by AV to retrieve the
 **                  desired codec and content protection configuration for the
 **                  A2DP Sink audio stream in Initiator.
 **
 **
 ** Returns          Pass or Fail for current getconfig.
 **
 ******************************************************************************/
static tA2DP_STATUS bta_av_audio_sink_getconfig(
    tBTA_AV_HNDL hndl, uint8_t* p_codec_info, uint8_t* p_sep_info_idx,
    uint8_t seid, uint8_t* p_num_protect, uint8_t* p_protect_info) {
  tA2DP_STATUS result = A2DP_FAIL;
  tBTA_AV_CO_PEER* p_peer;
  uint8_t pref_config[AVDT_CODEC_SIZE];

  APPL_TRACE_DEBUG("%s: handle:0x%x codec:%s seid:%d", __func__, hndl,
                   A2DP_CodecName(p_codec_info), seid);
  APPL_TRACE_DEBUG("%s: num_protect:0x%02x protect_info:0x%02x%02x%02x",
                   __func__, *p_num_protect, p_protect_info[0],
                   p_protect_info[1], p_protect_info[2]);

  /* Retrieve the peer info */
  p_peer = bta_av_co_get_peer(hndl);
  if (p_peer == NULL) {
    APPL_TRACE_ERROR("%s: could not find peer entry", __func__);
    return A2DP_FAIL;
  }

  APPL_TRACE_DEBUG("%s: peer(o=%d,n_sinks=%d,n_rx_sinks=%d,n_sup_sinks=%d)",
                   __func__, p_peer->opened, p_peer->num_srcs,
                   p_peer->num_rx_srcs, p_peer->num_sup_srcs);

  p_peer->num_rx_srcs++;

  /* Check the peer's SOURCE codec */
  if (A2DP_IsPeerSourceCodecValid(p_codec_info)) {
    /* If there is room for a new one */
    if (p_peer->num_sup_srcs < BTA_AV_CO_NUM_ELEMENTS(p_peer->srcs)) {
      tBTA_AV_CO_SINK* p_src = &p_peer->srcs[p_peer->num_sup_srcs++];

      APPL_TRACE_DEBUG("%s: saved caps[%x:%x:%x:%x:%x:%x]", __func__,
                       p_codec_info[1], p_codec_info[2], p_codec_info[3],
                       p_codec_info[4], p_codec_info[5], p_codec_info[6]);

      memcpy(p_src->codec_caps, p_codec_info, AVDT_CODEC_SIZE);
      p_src->sep_info_idx = *p_sep_info_idx;
      p_src->seid = seid;
      p_src->num_protect = *p_num_protect;
      memcpy(p_src->protect_info, p_protect_info, AVDT_CP_INFO_LEN);
    } else {
      APPL_TRACE_ERROR("%s: no more room for SRC info", __func__);
    }
  }

  /* If last SINK get capabilities or all supported codec caps retrieved */
  if ((p_peer->num_rx_srcs == p_peer->num_srcs) ||
      (p_peer->num_sup_srcs == BTA_AV_CO_NUM_ELEMENTS(p_peer->srcs))) {
    APPL_TRACE_DEBUG("%s: last SRC reached", __func__);

    /* Protect access to bta_av_co_cb.codec_config */
    mutex_global_lock();

    /* Find a src that matches the codec config */
    const tBTA_AV_CO_SINK* p_src =
        bta_av_co_find_peer_src_supports_codec(p_peer);
    if (p_src != NULL) {
      APPL_TRACE_DEBUG("%s: codec supported", __func__);

      /* Build the codec configuration for this sink */
      {
        /* Save the new configuration */
        p_peer->p_src = p_src;
        /* get preferred config from src_caps */
        if (A2DP_BuildSrc2SinkConfig(p_src->codec_caps, pref_config) !=
            A2DP_SUCCESS) {
          mutex_global_unlock();
          return A2DP_FAIL;
        }
        memcpy(p_peer->codec_config, pref_config, AVDT_CODEC_SIZE);

        APPL_TRACE_DEBUG("%s: p_codec_info[%x:%x:%x:%x:%x:%x]", __func__,
                         p_peer->codec_config[1], p_peer->codec_config[2],
                         p_peer->codec_config[3], p_peer->codec_config[4],
                         p_peer->codec_config[5], p_peer->codec_config[6]);
        /* By default, no content protection */
        *p_num_protect = 0;

#if (BTA_AV_CO_CP_SCMS_T == TRUE)
        p_peer->cp_active = false;
        bta_av_co_cb.cp.active = false;
#endif

        *p_sep_info_idx = p_src->sep_info_idx;
        memcpy(p_codec_info, p_peer->codec_config, AVDT_CODEC_SIZE);
        result = A2DP_SUCCESS;
      }
    }
    /* Protect access to bta_av_co_cb.codec_config */
    mutex_global_unlock();
  }
  return result;
}
/*******************************************************************************
 **
 ** Function         bta_av_co_audio_getconfig
 **
 ** Description      This callout function is executed by AV to retrieve the
 **                  desired codec and content protection configuration for the
 **                  audio stream.
 **
 **
 ** Returns          Stream codec and content protection configuration info.
 **
 ******************************************************************************/
tA2DP_STATUS bta_av_co_audio_getconfig(tBTA_AV_HNDL hndl, uint8_t* p_codec_info,
                                       uint8_t* p_sep_info_idx, uint8_t seid,
                                       uint8_t* p_num_protect,
                                       uint8_t* p_protect_info) {
  tA2DP_STATUS result = A2DP_FAIL;
  tBTA_AV_CO_PEER* p_peer;
  uint8_t codec_config[AVDT_CODEC_SIZE];

  APPL_TRACE_DEBUG("%s", __func__);

  /* Retrieve the peer info */
  p_peer = bta_av_co_get_peer(hndl);
  if (p_peer == NULL) {
    APPL_TRACE_ERROR("%s: could not find peer entry", __func__);
    return A2DP_FAIL;
  }

  if (p_peer->uuid_to_connect == UUID_SERVCLASS_AUDIO_SOURCE) {
    result = bta_av_audio_sink_getconfig(hndl, p_codec_info, p_sep_info_idx,
                                         seid, p_num_protect, p_protect_info);
    return result;
  }
  APPL_TRACE_DEBUG("%s: handle:0x%x codec:%s seid:%d", __func__, hndl,
                   A2DP_CodecName(p_codec_info), seid);
  APPL_TRACE_DEBUG("%s: num_protect:0x%02x protect_info:0x%02x%02x%02x",
                   __func__, *p_num_protect, p_protect_info[0],
                   p_protect_info[1], p_protect_info[2]);
  APPL_TRACE_DEBUG("%s: peer(o=%d, n_sinks=%d, n_rx_sinks=%d, n_sup_sinks=%d)",
                   __func__, p_peer->opened, p_peer->num_sinks,
                   p_peer->num_rx_sinks, p_peer->num_sup_sinks);

  p_peer->num_rx_sinks++;

  /* Check the peer's SINK codec */
  if (A2DP_IsPeerSinkCodecValid(p_codec_info)) {
    /* If there is room for a new one */
    if (p_peer->num_sup_sinks < BTA_AV_CO_NUM_ELEMENTS(p_peer->sinks)) {
      tBTA_AV_CO_SINK* p_sink = &p_peer->sinks[p_peer->num_sup_sinks++];

      APPL_TRACE_DEBUG("%s: saved caps[%x:%x:%x:%x:%x:%x]", __func__,
                       p_codec_info[1], p_codec_info[2], p_codec_info[3],
                       p_codec_info[4], p_codec_info[5], p_codec_info[6]);

      memcpy(p_sink->codec_caps, p_codec_info, AVDT_CODEC_SIZE);
      p_sink->sep_info_idx = *p_sep_info_idx;
      p_sink->seid = seid;
      p_sink->num_protect = *p_num_protect;
      memcpy(p_sink->protect_info, p_protect_info, AVDT_CP_INFO_LEN);
    } else {
      APPL_TRACE_ERROR("%s: no more room for SINK info", __func__);
    }
  }

  /* If last SINK get capabilities or all supported codec capa retrieved */
  if ((p_peer->num_rx_sinks == p_peer->num_sinks) ||
      (p_peer->num_sup_sinks == BTA_AV_CO_NUM_ELEMENTS(p_peer->sinks))) {
    APPL_TRACE_DEBUG("%s: last sink reached", __func__);

    /* Protect access to bta_av_co_cb.codec_config */
    mutex_global_lock();

    /* Find a sink that matches the codec config */
    const tBTA_AV_CO_SINK* p_sink = NULL;

    // Initial strawman codec selection mechanism: largest codec SEP index
    // first.
    // TODO: Replace this mechanism with a better one, and abstract it
    // in a separate function.
    for (int i = A2DP_CODEC_SEP_INDEX_SOURCE_MAX - 1;
         i >= A2DP_CODEC_SEP_INDEX_SOURCE_MIN; i--) {
      tA2DP_CODEC_SEP_INDEX source_codec_sep_index =
          static_cast<tA2DP_CODEC_SEP_INDEX>(i);
      APPL_TRACE_DEBUG("%s: trying codec %s with sep_index %d", __func__,
                       A2DP_CodecSepIndexStr(source_codec_sep_index), i);
      tAVDT_CFG avdt_cfg;
      if (!A2DP_InitCodecConfig(source_codec_sep_index, &avdt_cfg)) {
        APPL_TRACE_DEBUG("%s: cannot setup source codec %s", __func__,
                         A2DP_CodecSepIndexStr(source_codec_sep_index));
        continue;
      }
      p_sink =
          bta_av_co_find_peer_sink_supports_codec(avdt_cfg.codec_info, p_peer);
      if (p_sink == NULL) continue;
      // Found a preferred codec
      APPL_TRACE_DEBUG("%s: selected codec %s", __func__,
                       A2DP_CodecName(avdt_cfg.codec_info));
      memcpy(bta_av_co_cb.codec_config, avdt_cfg.codec_info, AVDT_CODEC_SIZE);
      break;
    }

    if (p_sink == NULL) {
      APPL_TRACE_ERROR("%s: cannot find peer SINK for this codec config",
                       __func__);
    } else {
      /* stop fetching caps once we retrieved a supported codec */
      if (p_peer->acp) {
        APPL_TRACE_EVENT("%s: no need to fetch more SEPs", __func__);
        *p_sep_info_idx = p_peer->num_seps;
      }

      /* Build the codec configuration for this sink */
      memset(codec_config, 0, AVDT_CODEC_SIZE);
      if (A2DP_BuildSinkConfig(bta_av_co_cb.codec_config, p_sink->codec_caps,
                               codec_config) == A2DP_SUCCESS) {
        APPL_TRACE_DEBUG("%s: reconfig codec_config[%x:%x:%x:%x:%x:%x]",
                         __func__, codec_config[1], codec_config[2],
                         codec_config[3], codec_config[4], codec_config[5],
                         codec_config[6]);
        for (int i = 0; i < AVDT_CODEC_SIZE; i++) {
          APPL_TRACE_DEBUG("%s: p_codec_info[%d]: %x", __func__, i,
                           p_codec_info[i]);
        }

        /* Save the new configuration */
        p_peer->p_sink = p_sink;
        memcpy(p_peer->codec_config, codec_config, AVDT_CODEC_SIZE);

        /* By default, no content protection */
        *p_num_protect = 0;

#if (BTA_AV_CO_CP_SCMS_T == TRUE)
        /* Check if this sink supports SCMS */
        p_peer->cp_active = bta_av_co_audio_sink_has_scmst(p_sink);
        bta_av_co_cb.cp.active = p_peer->cp_active;
        if (p_peer->cp_active) {
          *p_num_protect = AVDT_CP_INFO_LEN;
          memcpy(p_protect_info, bta_av_co_cp_scmst, AVDT_CP_INFO_LEN);
        }
#endif

        /* If acceptor -> reconfig otherwise reply for configuration */
        if (p_peer->acp) {
          if (p_peer->reconfig_needed) {
            APPL_TRACE_DEBUG("%s: call BTA_AvReconfig(x%x)", __func__, hndl);
            BTA_AvReconfig(hndl, true, p_sink->sep_info_idx,
                           p_peer->codec_config, *p_num_protect,
                           bta_av_co_cp_scmst);
          }
        } else {
          *p_sep_info_idx = p_sink->sep_info_idx;
          memcpy(p_codec_info, p_peer->codec_config, AVDT_CODEC_SIZE);
        }
        result = A2DP_SUCCESS;
      }
    }
    /* Protect access to bta_av_co_cb.codec_config */
    mutex_global_unlock();
  }
  return result;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_setconfig
 **
 ** Description      This callout function is executed by AV to set the codec
 **                  and content protection configuration of the audio stream.
 **
 **
 ** Returns          void
 **
 ******************************************************************************/
void bta_av_co_audio_setconfig(tBTA_AV_HNDL hndl, const uint8_t* p_codec_info,
                               UNUSED_ATTR uint8_t seid,
                               UNUSED_ATTR BD_ADDR addr, uint8_t num_protect,
                               uint8_t* p_protect_info, uint8_t t_local_sep,
                               uint8_t avdt_handle) {
  tBTA_AV_CO_PEER* p_peer;
  tA2DP_STATUS status = A2DP_SUCCESS;
  uint8_t category = A2DP_SUCCESS;
  bool reconfig_needed = false;

  APPL_TRACE_DEBUG("%s: p_codec_info[%x:%x:%x:%x:%x:%x]", __func__,
                   p_codec_info[1], p_codec_info[2], p_codec_info[3],
                   p_codec_info[4], p_codec_info[5], p_codec_info[6]);
  APPL_TRACE_DEBUG("num_protect:0x%02x protect_info:0x%02x%02x%02x",
                   num_protect, p_protect_info[0], p_protect_info[1],
                   p_protect_info[2]);

  /* Retrieve the peer info */
  p_peer = bta_av_co_get_peer(hndl);
  if (p_peer == NULL) {
    APPL_TRACE_ERROR("%s: could not find peer entry", __func__);
    /* Call call-in rejecting the configuration */
    bta_av_ci_setconfig(hndl, A2DP_BUSY, AVDT_ASC_CODEC, 0, NULL, false,
                        avdt_handle);
    return;
  }

  APPL_TRACE_DEBUG("%s: peer(o=%d, n_sinks=%d, n_rx_sinks=%d, n_sup_sinks=%d)",
                   __func__, p_peer->opened, p_peer->num_sinks,
                   p_peer->num_rx_sinks, p_peer->num_sup_sinks);

  /* Sanity check: should not be opened at this point */
  if (p_peer->opened) {
    APPL_TRACE_ERROR("%s: peer already in use", __func__);
  }

  if (num_protect != 0) {
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
    /* If CP is supported */
    if ((num_protect != 1) ||
        (bta_av_co_cp_is_scmst(p_protect_info) == false)) {
      APPL_TRACE_ERROR("%s: wrong CP configuration", __func__);
      status = A2DP_BAD_CP_TYPE;
      category = AVDT_ASC_PROTECT;
    }
#else
    /* Do not support content protection for the time being */
    APPL_TRACE_ERROR("%s: wrong CP configuration", __func__);
    status = A2DP_BAD_CP_TYPE;
    category = AVDT_ASC_PROTECT;
#endif
  }

  if (status == A2DP_SUCCESS) {
    bool codec_config_supported = false;
    if (t_local_sep == AVDT_TSEP_SNK) {
      APPL_TRACE_DEBUG("%s: peer is A2DP SRC", __func__);
      codec_config_supported = A2DP_IsSinkCodecSupported(p_codec_info);
    }
    if (t_local_sep == AVDT_TSEP_SRC) {
      APPL_TRACE_DEBUG("%s: peer is A2DP SINK", __func__);
      codec_config_supported = A2DP_IsSourceCodecSupported(p_codec_info);
    }

    /* Check if codec configuration is supported */
    if (codec_config_supported) {
      /* Protect access to bta_av_co_cb.codec_config */
      mutex_global_lock();

      /* Check if the configuration matches the current codec config */
      if (A2DP_CodecRequiresReconfig(p_codec_info, bta_av_co_cb.codec_config)) {
        reconfig_needed = true;
      } else if ((num_protect == 1) && (!bta_av_co_cb.cp.active)) {
        reconfig_needed = true;
      }
      memcpy(bta_av_co_cb.codec_config_setconfig, p_codec_info,
             AVDT_CODEC_SIZE);
      if (t_local_sep == AVDT_TSEP_SNK) {
        /*
         * If Peer is SRC, and our config subset matches with what is
         * requested by peer, then just accept what peer wants.
         */
        memcpy(bta_av_co_cb.codec_config, p_codec_info, AVDT_CODEC_SIZE);
        reconfig_needed = false;
      }
      /* Protect access to bta_av_co_cb.codec_config */
      mutex_global_unlock();
    } else {
      category = AVDT_ASC_CODEC;
      status = A2DP_WRONG_CODEC;
    }
  }

  if (status != A2DP_SUCCESS) {
    APPL_TRACE_DEBUG("%s: reject s=%d c=%d", __func__, status, category);
    /* Call call-in rejecting the configuration */
    bta_av_ci_setconfig(hndl, status, category, 0, NULL, false, avdt_handle);
    return;
  }

  /* Mark that this is an acceptor peer */
  p_peer->acp = true;
  p_peer->reconfig_needed = reconfig_needed;
  APPL_TRACE_DEBUG("%s: accept reconf=%d", __func__, reconfig_needed);
  /* Call call-in accepting the configuration */
  bta_av_ci_setconfig(hndl, A2DP_SUCCESS, A2DP_SUCCESS, 0, NULL,
                      reconfig_needed, avdt_handle);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_open
 **
 ** Description      This function is called by AV when the audio stream
 **                  connection is opened.
 **
 **
 ** Returns          void
 **
 ******************************************************************************/
void bta_av_co_audio_open(tBTA_AV_HNDL hndl, uint8_t* p_codec_info,
                          uint16_t mtu) {
  tBTA_AV_CO_PEER* p_peer;

  APPL_TRACE_DEBUG("%s: mtu:%d codec:%s", __func__, mtu,
                   A2DP_CodecName(p_codec_info));

  /* Retrieve the peer info */
  p_peer = bta_av_co_get_peer(hndl);
  if (p_peer == NULL) {
    APPL_TRACE_ERROR("%s: could not find peer entry", __func__);
  } else {
    p_peer->opened = true;
    p_peer->mtu = mtu;
  }
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_close
 **
 ** Description      This function is called by AV when the audio stream
 **                  connection is closed.
 **
 **
 ** Returns          void
 **
 ******************************************************************************/
void bta_av_co_audio_close(tBTA_AV_HNDL hndl, UNUSED_ATTR uint16_t mtu)

{
  tBTA_AV_CO_PEER* p_peer;

  APPL_TRACE_DEBUG("%s", __func__);

  /* Retrieve the peer info */
  p_peer = bta_av_co_get_peer(hndl);
  if (p_peer) {
    /* Mark the peer closed and clean the peer info */
    memset(p_peer, 0, sizeof(*p_peer));
  } else {
    APPL_TRACE_ERROR("%s: could not find peer entry", __func__);
  }

  /* reset remote preference through setconfig */
  memset(bta_av_co_cb.codec_config_setconfig, 0,
         sizeof(bta_av_co_cb.codec_config_setconfig));
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_start
 **
 ** Description      This function is called by AV when the audio streaming data
 **                  transfer is started.
 **
 **
 ** Returns          void
 **
 ******************************************************************************/
void bta_av_co_audio_start(UNUSED_ATTR tBTA_AV_HNDL hndl,
                           UNUSED_ATTR uint8_t* p_codec_info,
                           UNUSED_ATTR bool* p_no_rtp_hdr) {
  APPL_TRACE_DEBUG("%s", __func__);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_stop
 **
 ** Description      This function is called by AV when the audio streaming data
 **                  transfer is stopped.
 **
 **
 ** Returns          void
 **
 ******************************************************************************/
void bta_av_co_audio_stop(UNUSED_ATTR tBTA_AV_HNDL hndl) {
  APPL_TRACE_DEBUG("%s", __func__);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_src_data_path
 **
 ** Description      This function is called to manage data transfer from
 **                  the audio codec to AVDTP.
 **
 ** Returns          Pointer to the GKI buffer to send, NULL if no buffer to
 **                  send
 **
 ******************************************************************************/
void* bta_av_co_audio_src_data_path(const uint8_t* p_codec_info,
                                    uint32_t* p_timestamp) {
  BT_HDR* p_buf;

  APPL_TRACE_DEBUG("%s: codec: %s", __func__, A2DP_CodecName(p_codec_info));

  p_buf = btif_a2dp_source_audio_readbuf();
  if (p_buf == NULL) return NULL;

  /*
   * Retrieve the timestamp information from the media packet,
   * and set up the packet header.
   *
   * In media packet, the following information is available:
   * p_buf->layer_specific : number of audio frames in the packet
   * p_buf->word[0] : timestamp
   */
  if (!A2DP_GetPacketTimestamp(p_codec_info, (const uint8_t*)(p_buf + 1),
                               p_timestamp) ||
      !A2DP_BuildCodecHeader(p_codec_info, p_buf, p_buf->layer_specific)) {
    APPL_TRACE_ERROR("%s: unsupported codec type (%d)", __func__,
                     A2DP_GetCodecType(p_codec_info));
  }

#if (BTA_AV_CO_CP_SCMS_T == TRUE)
  if (bta_av_co_cb.cp.active) {
    p_buf->len++;
    p_buf->offset--;
    uint8_t* p = (uint8_t*)(p_buf + 1) + p_buf->offset;
    *p = bta_av_co_cp_get_flag();
  }
#endif

  return p_buf;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_drop
 **
 ** Description      An Audio packet is dropped. .
 **                  It's very likely that the connected headset with this
 **                  handle is moved far away. The implementation may want to
 **                  reduce the encoder bit rate setting to reduce the packet
 **                  size.
 **
 ** Returns          void
 **
 ******************************************************************************/
void bta_av_co_audio_drop(tBTA_AV_HNDL hndl) {
  APPL_TRACE_ERROR("%s: dropped audio packet on handle 0x%x", __func__, hndl);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_delay
 **
 ** Description      This function is called by AV when the audio stream
 **                  connection needs to send the initial delay report to the
 **                  connected SRC.
 **
 **
 ** Returns          void
 **
 ******************************************************************************/
void bta_av_co_audio_delay(tBTA_AV_HNDL hndl, uint16_t delay) {
  APPL_TRACE_ERROR("%s: handle: x%x, delay:0x%x", __func__, hndl, delay);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_cp_is_scmst
 **
 ** Description      Check if a content protection service is SCMS-T
 **
 ** Returns          true if this CP is SCMS-T, false otherwise
 **
 ******************************************************************************/
static bool bta_av_co_cp_is_scmst(const uint8_t* p_protectinfo) {
  APPL_TRACE_DEBUG("%s", __func__);

  if (*p_protectinfo >= AVDT_CP_LOSC) {
    uint16_t cp_id;

    p_protectinfo++;
    STREAM_TO_UINT16(cp_id, p_protectinfo);
    if (cp_id == AVDT_CP_SCMS_T_ID) {
      APPL_TRACE_DEBUG("%s: SCMS-T found", __func__);
      return true;
    }
  }

  return false;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_sink_has_scmst
 **
 ** Description      Check if a sink supports SCMS-T
 **
 ** Returns          true if the sink supports this CP, false otherwise
 **
 ******************************************************************************/
static bool bta_av_co_audio_sink_has_scmst(const tBTA_AV_CO_SINK* p_sink) {
  uint8_t index;
  const uint8_t* p;

  APPL_TRACE_DEBUG("%s", __func__);

  /* Check if sink supports SCMS-T */
  index = p_sink->num_protect;
  p = &p_sink->protect_info[0];

  while (index) {
    if (bta_av_co_cp_is_scmst(p)) return true;
    /* Move to the next SC */
    p += *p + 1;
    /* Decrement the SC counter */
    index--;
  }
  APPL_TRACE_DEBUG("%s: SCMS-T not found", __func__);
  return false;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_sink_supports_cp
 **
 ** Description      Check if a sink supports the current content protection
 **
 ** Returns          true if the sink supports this CP, false otherwise
 **
 ******************************************************************************/
static bool bta_av_co_audio_sink_supports_cp(const tBTA_AV_CO_SINK* p_sink) {
  APPL_TRACE_DEBUG("%s", __func__);

  /* Check if content protection is enabled for this stream */
  if (bta_av_co_cp_get_flag() != AVDT_CP_SCMS_COPY_FREE)
    return bta_av_co_audio_sink_has_scmst(p_sink);

  APPL_TRACE_DEBUG("%s: not required", __func__);
  return true;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_find_peer_sink_supports_codec
 **
 ** Description      Find a peer acting as a sink that suppors codec config
 **
 ** Returns          The peer sink that supports the codec, otherwise NULL.
 **
 ******************************************************************************/
static const tBTA_AV_CO_SINK* bta_av_co_find_peer_sink_supports_codec(
    const uint8_t* codec_config, const tBTA_AV_CO_PEER* p_peer) {
  APPL_TRACE_DEBUG("%s: peer num_sup_sinks = %d", __func__,
                   p_peer->num_sup_sinks);

  for (size_t index = 0; index < p_peer->num_sup_sinks; index++) {
    if (A2DP_CodecConfigMatchesCapabilities(codec_config,
                                            p_peer->sinks[index].codec_caps)) {
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
      if (!bta_av_co_audio_sink_has_scmst(&p_peer->sinks[index])) continue;
#endif
      return &p_peer->sinks[index];
    }
  }
  return NULL;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_find_peer_src_supports_codec
 **
 ** Description      Find a peer acting as src that supports codec config
 **
 ** Returns          The peer source that supports the codec, otherwise NULL.
 **
 ******************************************************************************/
static const tBTA_AV_CO_SINK* bta_av_co_find_peer_src_supports_codec(
    const tBTA_AV_CO_PEER* p_peer) {
  APPL_TRACE_DEBUG("%s: peer num_sup_srcs = %d", __func__,
                   p_peer->num_sup_srcs);

  for (size_t index = 0; index < p_peer->num_sup_srcs; index++) {
    const uint8_t* p_codec_caps = p_peer->srcs[index].codec_caps;
    if (A2DP_CodecTypeEquals(bta_av_co_cb.codec_config, p_codec_caps) &&
        A2DP_IsPeerSourceCodecSupported(p_codec_caps)) {
      return &p_peer->srcs[index];
    }
  }
  return NULL;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_set_codec
 **
 ** Description      Set the current codec configuration from the feeding type.
 **                  This function is starting to modify the configuration, it
 **                  should be protected.
 **
 ** Returns          true if successful, false otherwise
 **
 ******************************************************************************/
bool bta_av_co_audio_set_codec(const tA2DP_FEEDING_PARAMS* p_feeding_params) {
  uint8_t new_config[AVDT_CODEC_SIZE];

  /* Protect access to bta_av_co_cb.codec_config */
  mutex_global_lock();

  // Initial strawman codec selection mechanism: largest codec SEP index first.
  // TODO: Replace this mechanism with a better one.
  for (int i = A2DP_CODEC_SEP_INDEX_SOURCE_MAX - 1;
       i >= A2DP_CODEC_SEP_INDEX_SOURCE_MIN; i--) {
    tA2DP_CODEC_SEP_INDEX source_codec_sep_index =
        static_cast<tA2DP_CODEC_SEP_INDEX>(i);
    APPL_TRACE_DEBUG("%s: trying codec %s with sep_index %d", __func__,
                     A2DP_CodecSepIndexStr(source_codec_sep_index), i);
    if (!A2DP_SetSourceCodec(source_codec_sep_index, p_feeding_params,
                             new_config)) {
      APPL_TRACE_DEBUG("%s: cannot setup source codec %s", __func__,
                       A2DP_CodecSepIndexStr(source_codec_sep_index));
      continue;
    }

    /* Try to select an open device for the codec */
    if (bta_av_co_audio_codec_selected(new_config)) {
      APPL_TRACE_DEBUG("%s: selected codec %s with sep_index %d", __func__,
                       A2DP_CodecSepIndexStr(source_codec_sep_index), i);
      mutex_global_unlock();
      return true;
    }
    APPL_TRACE_DEBUG("%s: cannot select source codec %s", __func__,
                     A2DP_CodecSepIndexStr(source_codec_sep_index));
  }
  mutex_global_unlock();
  return false;
}

// Select an open device for the given codec info.
// Return true if an open device was selected, otherwise false.
static bool bta_av_co_audio_codec_selected(const uint8_t* codec_config) {
  APPL_TRACE_DEBUG("%s", __func__);

  /* Check AV feeding is supported */
  for (uint8_t index = 0; index < BTA_AV_CO_NUM_ELEMENTS(bta_av_co_cb.peers);
       index++) {
    uint8_t peer_codec_config[AVDT_CODEC_SIZE];

    tBTA_AV_CO_PEER* p_peer = &bta_av_co_cb.peers[index];
    if (!p_peer->opened) continue;

    const tBTA_AV_CO_SINK* p_sink =
        bta_av_co_find_peer_sink_supports_codec(codec_config, p_peer);
    if (p_sink == NULL) {
      APPL_TRACE_DEBUG("%s: index %d doesn't support codec", __func__, index);
      continue;
    }

    /* Check that this sink is compatible with the CP */
    if (!bta_av_co_audio_sink_supports_cp(p_sink)) {
      APPL_TRACE_DEBUG("%s: sink of peer %d doesn't support cp", __func__,
                       index);
      continue;
    }

    /* Build the codec configuration for this sink */
    memset(peer_codec_config, 0, AVDT_CODEC_SIZE);
    if (A2DP_BuildSinkConfig(codec_config, p_sink->codec_caps,
                             peer_codec_config) != A2DP_SUCCESS) {
      continue;
    }

    /* The new config was correctly built and selected */
    memcpy(bta_av_co_cb.codec_config, codec_config,
           sizeof(bta_av_co_cb.codec_config));

    /* Save the new configuration */
    uint8_t num_protect = 0;
    p_peer->p_sink = p_sink;
    memcpy(p_peer->codec_config, peer_codec_config, AVDT_CODEC_SIZE);
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
    /* Check if this sink supports SCMS */
    bool cp_active = bta_av_co_audio_sink_has_scmst(p_sink);
    bta_av_co_cb.cp.active = cp_active;
    p_peer->cp_active = cp_active;
    if (p_peer->cp_active) num_protect = AVDT_CP_INFO_LEN;
#endif
    APPL_TRACE_DEBUG("%s: call BTA_AvReconfig(0x%x)", __func__,
                     BTA_AV_CO_AUDIO_INDX_TO_HNDL(index));
    BTA_AvReconfig(BTA_AV_CO_AUDIO_INDX_TO_HNDL(index), true,
                   p_sink->sep_info_idx, p_peer->codec_config, num_protect,
                   bta_av_co_cp_scmst);
    return true;
  }

  return false;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_codec_reset
 **
 ** Description      Reset the current codec configuration
 **
 ** Returns          void
 **
 ******************************************************************************/
static void bta_av_co_audio_codec_reset(void) {
  APPL_TRACE_DEBUG("%s", __func__);

  mutex_global_lock();

  /* Reset the current configuration to the default codec */
  A2DP_InitDefaultCodec(bta_av_co_cb.codec_config);

  mutex_global_unlock();
}

void bta_av_co_audio_encoder_init(tA2DP_ENCODER_INIT_PARAMS* p_init_params) {
  uint16_t min_mtu = 0xFFFF;

  APPL_TRACE_DEBUG("%s", __func__);
  assert(p_init_params != nullptr);

  /* Protect access to bta_av_co_cb.codec_config */
  mutex_global_lock();

  /* Compute the MTU */
  for (size_t i = 0; i < BTA_AV_CO_NUM_ELEMENTS(bta_av_co_cb.peers); i++) {
    const tBTA_AV_CO_PEER* p_peer = &bta_av_co_cb.peers[i];
    if (!p_peer->opened) continue;
    if (p_peer->mtu < min_mtu) min_mtu = p_peer->mtu;
  }

  const uint8_t* p_codec_info = bta_av_co_cb.codec_config;
  p_init_params->NumOfSubBands = A2DP_GetNumberOfSubbands(p_codec_info);
  p_init_params->NumOfBlocks = A2DP_GetNumberOfBlocks(p_codec_info);
  p_init_params->AllocationMethod = A2DP_GetAllocationMethodCode(p_codec_info);
  p_init_params->ChannelMode = A2DP_GetChannelModeCode(p_codec_info);
  p_init_params->SamplingFreq = A2DP_GetSamplingFrequencyCode(p_codec_info);
  p_init_params->MtuSize = min_mtu;

  /* Protect access to bta_av_co_cb.codec_config */
  mutex_global_unlock();
}

void bta_av_co_audio_encoder_update(
    tA2DP_ENCODER_UPDATE_PARAMS* p_update_params) {
  uint16_t min_mtu = 0xFFFF;

  APPL_TRACE_DEBUG("%s", __func__);
  assert(p_update_params != nullptr);

  /* Protect access to bta_av_co_cb.codec_config */
  mutex_global_lock();

  const uint8_t* p_codec_info = bta_av_co_cb.codec_config;
  int min_bitpool = A2DP_GetMinBitpool(p_codec_info);
  int max_bitpool = A2DP_GetMaxBitpool(p_codec_info);

  if ((min_bitpool < 0) || (max_bitpool < 0)) {
    APPL_TRACE_ERROR("%s: Invalid min/max bitpool: [%d, %d]", __func__,
                     min_bitpool, max_bitpool);
    mutex_global_unlock();
    return;
  }

  for (size_t i = 0; i < BTA_AV_CO_NUM_ELEMENTS(bta_av_co_cb.peers); i++) {
    const tBTA_AV_CO_PEER* p_peer = &bta_av_co_cb.peers[i];
    if (!p_peer->opened) continue;

    if (p_peer->mtu < min_mtu) min_mtu = p_peer->mtu;

    for (int j = 0; j < p_peer->num_sup_sinks; j++) {
      const tBTA_AV_CO_SINK* p_sink = &p_peer->sinks[j];
      if (!A2DP_CodecTypeEquals(p_codec_info, p_sink->codec_caps)) continue;
      /* Update the bitpool boundaries of the current config */
      int peer_min_bitpool = A2DP_GetMinBitpool(p_sink->codec_caps);
      int peer_max_bitpool = A2DP_GetMaxBitpool(p_sink->codec_caps);
      if (peer_min_bitpool >= 0)
        min_bitpool = BTA_AV_CO_MAX(min_bitpool, peer_min_bitpool);
      if (peer_max_bitpool >= 0)
        max_bitpool = BTA_AV_CO_MIN(max_bitpool, peer_max_bitpool);
      APPL_TRACE_EVENT("%s: sink bitpool min %d, max %d", __func__, min_bitpool,
                       max_bitpool);
      break;
    }
  }

  /*
   * Check if the remote Sink has a preferred bitpool range.
   * Adjust our preferred bitpool with the remote preference if within
   * our capable range.
   */
  if (A2DP_IsSourceCodecValid(bta_av_co_cb.codec_config_setconfig) &&
      A2DP_CodecTypeEquals(p_codec_info, bta_av_co_cb.codec_config_setconfig)) {
    int setconfig_min_bitpool =
        A2DP_GetMinBitpool(bta_av_co_cb.codec_config_setconfig);
    int setconfig_max_bitpool =
        A2DP_GetMaxBitpool(bta_av_co_cb.codec_config_setconfig);
    if (setconfig_min_bitpool >= 0)
      min_bitpool = BTA_AV_CO_MAX(min_bitpool, setconfig_min_bitpool);
    if (setconfig_max_bitpool >= 0)
      max_bitpool = BTA_AV_CO_MIN(max_bitpool, setconfig_max_bitpool);
    APPL_TRACE_EVENT("%s: sink adjusted bitpool min %d, max %d", __func__,
                     min_bitpool, max_bitpool);
  }

  /* Protect access to bta_av_co_cb.codec_config */
  mutex_global_unlock();

  if (min_bitpool > max_bitpool) {
    APPL_TRACE_ERROR("%s: Irrational min/max bitpool: [%d, %d]", __func__,
                     min_bitpool, max_bitpool);
    return;
  }

  p_update_params->MinMtuSize = min_mtu;
  p_update_params->MinBitPool = min_bitpool;
  p_update_params->MaxBitPool = max_bitpool;
}

const tA2DP_ENCODER_INTERFACE* bta_av_co_get_encoder_interface(void) {
  /* Protect access to bta_av_co_cb.codec_config */
  mutex_global_lock();

  const tA2DP_ENCODER_INTERFACE* encoder_interface =
      A2DP_GetEncoderInterface(bta_av_co_cb.codec_config);

  /* Protect access to bta_av_co_cb.codec_config */
  mutex_global_unlock();

  return encoder_interface;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_init
 **
 ** Description      Initialization
 **
 ** Returns          Nothing
 **
 ******************************************************************************/
void bta_av_co_init(void) {
  APPL_TRACE_DEBUG("%s", __func__);

  /* Reset the control block */
  memset(&bta_av_co_cb, 0, sizeof(bta_av_co_cb));

#if (BTA_AV_CO_CP_SCMS_T == TRUE)
  bta_av_co_cp_set_flag(AVDT_CP_SCMS_COPY_NEVER);
#else
  bta_av_co_cp_set_flag(AVDT_CP_SCMS_COPY_FREE);
#endif

  /* Reset the current config */
  bta_av_co_audio_codec_reset();
}
