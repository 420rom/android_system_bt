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

#include <gtest/gtest.h>

#include "stack/include/a2dp_api.h"
#include "stack/include/a2dp_sbc.h"
#include "stack/include/a2dp_vendor.h"

//TODO(jpawlowski): remove once weird dependency in hci_layer.cc is removed
#include <hardware/bluetooth.h>
bt_bdaddr_t btif_local_bd_addr;

namespace {
const uint8_t codec_info_sbc[AVDT_CODEC_SIZE] = {
  6,                    // Length (A2DP_SBC_INFO_LEN)
  0,                    // Media Type: AVDT_MEDIA_TYPE_AUDIO
  0,                    // Media Codec Type: A2DP_MEDIA_CT_SBC
  0x20 | 0x01,          // Sample Frequency: A2DP_SBC_IE_SAMP_FREQ_44 |
                        // Channel Mode: A2DP_SBC_IE_CH_MD_JOINT
  0x10 | 0x04 | 0x01,   // Block Length: A2DP_SBC_IE_BLOCKS_16 |
                        // Subbands: A2DP_SBC_IE_SUBBAND_8 |
                        // Allocation Method: A2DP_SBC_IE_ALLOC_MD_L
  2,                    // MinimumBitpool Value: A2DP_SBC_IE_MIN_BITPOOL
  53,                   // Maximum Bitpool Value: A2DP_SBC_MAX_BITPOOL
  7,                    // Dummy
  8,                    // Dummy
  9                     // Dummy
};

const uint8_t codec_info_sbc_sink[AVDT_CODEC_SIZE] = {
  6,                    // Length (A2DP_SBC_INFO_LEN)
  0,                    // Media Type: AVDT_MEDIA_TYPE_AUDIO
  0,                    // Media Codec Type: A2DP_MEDIA_CT_SBC
  0x20 | 0x10 |         // Sample Frequency: A2DP_SBC_IE_SAMP_FREQ_44 |
                        // A2DP_SBC_IE_SAMP_FREQ_48 |
  0x08 | 0x04 | 0x02 | 0x01, // Channel Mode: A2DP_SBC_IE_CH_MD_MONO |
                        // A2DP_SBC_IE_CH_MD_DUAL |
                        // A2DP_SBC_IE_CH_MD_STEREO |
                        // A2DP_SBC_IE_CH_MD_JOINT
  0x80 | 0x40 | 0x20 | 0x10 | // Block Length: A2DP_SBC_IE_BLOCKS_4 |
                        // A2DP_SBC_IE_BLOCKS_8 |
                        // A2DP_SBC_IE_BLOCKS_12 |
                        // A2DP_SBC_IE_BLOCKS_16 |
  0x08 | 0x04 |         // Subbands: A2DP_SBC_IE_SUBBAND_4 |
                        // A2DP_SBC_IE_SUBBAND_8 |
  0x02 | 0x01,          // Allocation Method: A2DP_SBC_IE_ALLOC_MD_S |
                        // A2DP_SBC_IE_ALLOC_MD_L
  2,                    // MinimumBitpool Value: A2DP_SBC_IE_MIN_BITPOOL
  250,                  // Maximum Bitpool Value: A2DP_SBC_IE_MAX_BITPOOL
  7,                    // Dummy
  8,                    // Dummy
  9                     // Dummy
};

const uint8_t codec_info_non_a2dp[AVDT_CODEC_SIZE] = {
  8,                    // Length
  0,                    // Media Type: AVDT_MEDIA_TYPE_AUDIO
  0xFF,                 // Media Codec Type: A2DP_MEDIA_CT_NON_A2DP
  3, 4, 0, 0,           // Vendor ID: LSB first, upper two octets should be 0
  7, 8,                 // Codec ID: LSB first
  9                     // Dummy
};

const uint8_t codec_info_non_a2dp_dummy[AVDT_CODEC_SIZE] = {
  8,                    // Length
  0,                    // Media Type: AVDT_MEDIA_TYPE_AUDIO
  0xFF,                 // Media Codec Type: A2DP_MEDIA_CT_NON_A2DP
  3, 4, 0, 0,           // Vendor ID: LSB first, upper two octets should be 0
  7, 8,                 // Codec ID: LSB first
  10                    // Dummy
};

} // namespace

TEST(StackA2dpTest, test_a2dp_is_codec_valid) {
  EXPECT_TRUE(A2DP_IsSourceCodecValid(codec_info_sbc));
  EXPECT_TRUE(A2DP_IsPeerSourceCodecValid(codec_info_sbc));

  EXPECT_TRUE(A2DP_IsSinkCodecValid(codec_info_sbc_sink));
  EXPECT_TRUE(A2DP_IsPeerSinkCodecValid(codec_info_sbc_sink));

  EXPECT_FALSE(A2DP_IsSourceCodecValid(codec_info_non_a2dp));
  EXPECT_FALSE(A2DP_IsSinkCodecValid(codec_info_non_a2dp));
  EXPECT_FALSE(A2DP_IsPeerSourceCodecValid(codec_info_non_a2dp));
  EXPECT_FALSE(A2DP_IsPeerSinkCodecValid(codec_info_non_a2dp));

  // Test with invalid SBC codecs
  uint8_t codec_info_sbc_invalid[AVDT_CODEC_SIZE];
  memset(codec_info_sbc_invalid, 0, sizeof(codec_info_sbc_invalid));
  EXPECT_FALSE(A2DP_IsSourceCodecValid(codec_info_sbc_invalid));
  EXPECT_FALSE(A2DP_IsSinkCodecValid(codec_info_sbc_invalid));
  EXPECT_FALSE(A2DP_IsPeerSourceCodecValid(codec_info_sbc_invalid));
  EXPECT_FALSE(A2DP_IsPeerSinkCodecValid(codec_info_sbc_invalid));

  memcpy(codec_info_sbc_invalid, codec_info_sbc, sizeof(codec_info_sbc));
  codec_info_sbc_invalid[0] = 0;        // Corrupt the Length field
  EXPECT_FALSE(A2DP_IsSourceCodecValid(codec_info_sbc_invalid));
  EXPECT_FALSE(A2DP_IsSinkCodecValid(codec_info_sbc_invalid));
  EXPECT_FALSE(A2DP_IsPeerSourceCodecValid(codec_info_sbc_invalid));
  EXPECT_FALSE(A2DP_IsPeerSinkCodecValid(codec_info_sbc_invalid));

  memcpy(codec_info_sbc_invalid, codec_info_sbc, sizeof(codec_info_sbc));
  codec_info_sbc_invalid[1] = 0xff;        // Corrupt the Media Type field
  EXPECT_FALSE(A2DP_IsSourceCodecValid(codec_info_sbc_invalid));
  EXPECT_FALSE(A2DP_IsSinkCodecValid(codec_info_sbc_invalid));
  EXPECT_FALSE(A2DP_IsPeerSourceCodecValid(codec_info_sbc_invalid));
  EXPECT_FALSE(A2DP_IsPeerSinkCodecValid(codec_info_sbc_invalid));
}

TEST(StackA2dpTest, test_a2dp_get_codec_type) {
  tA2DP_CODEC_TYPE codec_type = A2DP_GetCodecType(codec_info_sbc);
  EXPECT_EQ(codec_type, A2DP_MEDIA_CT_SBC);

  codec_type = A2DP_GetCodecType(codec_info_non_a2dp);
  EXPECT_EQ(codec_type, A2DP_MEDIA_CT_NON_A2DP);
}

TEST(StackA2dpTest, test_a2dp_is_source_codec_supported) {
  EXPECT_TRUE(A2DP_IsSourceCodecSupported(codec_info_sbc));
  EXPECT_TRUE(A2DP_IsSourceCodecSupported(codec_info_sbc_sink));
  EXPECT_FALSE(A2DP_IsSourceCodecSupported(codec_info_non_a2dp));
}

TEST(StackA2dpTest, test_a2dp_is_sink_codec_supported) {
  EXPECT_TRUE(A2DP_IsSinkCodecSupported(codec_info_sbc));
  EXPECT_TRUE(A2DP_IsSinkCodecSupported(codec_info_sbc_sink));
  EXPECT_FALSE(A2DP_IsSinkCodecSupported(codec_info_non_a2dp));
}

TEST(StackA2dpTest, test_a2dp_is_peer_source_codec_supported) {
  EXPECT_TRUE(A2DP_IsPeerSourceCodecSupported(codec_info_sbc));
  EXPECT_TRUE(A2DP_IsPeerSourceCodecSupported(codec_info_sbc_sink));
  EXPECT_FALSE(A2DP_IsPeerSourceCodecSupported(codec_info_non_a2dp));
}

TEST(StackA2dpTest, test_init_default_codec) {
  uint8_t codec_info_result[AVDT_CODEC_SIZE];

  memset(codec_info_result, 0, sizeof(codec_info_result));
  A2DP_InitDefaultCodec(codec_info_result);

  // Compare the result codec with the local test codec info
  for (size_t i = 0; i < codec_info_sbc[0] + 1; i++) {
    EXPECT_EQ(codec_info_result[i], codec_info_sbc[i]);
  }
}

TEST(StackA2dpTest, test_set_codec) {
  uint8_t codec_info_result[AVDT_CODEC_SIZE];

  const tA2DP_FEEDING_PARAMS feeding_params = {
      .sampling_freq = 44100,
      .num_channel = 2,
      .bit_per_sample = 16
  };
  tA2DP_FEEDING_PARAMS bad_feeding_params;

  EXPECT_TRUE(A2DP_SetSourceCodec(A2DP_CODEC_SEP_INDEX_SOURCE_SBC,
                                  &feeding_params, codec_info_result));

  // Compare the result codec with the local test codec info
  for (size_t i = 0; i < codec_info_sbc[0] + 1; i++) {
    EXPECT_EQ(codec_info_result[i], codec_info_sbc[i]);
  }

  // Test for invalid values of source codec SEP index
  EXPECT_FALSE(A2DP_SetSourceCodec(A2DP_CODEC_SEP_INDEX_SINK_SBC,
                                   &feeding_params, codec_info_result));
  EXPECT_FALSE(A2DP_SetSourceCodec(A2DP_CODEC_SEP_INDEX_SINK_MAX,
                                   &feeding_params, codec_info_result));

  // Test invalid feeding - invalid num_channel
  bad_feeding_params = feeding_params;
  bad_feeding_params.num_channel = 3;
  EXPECT_FALSE(A2DP_SetSourceCodec(A2DP_CODEC_SEP_INDEX_SOURCE_SBC,
                                   &bad_feeding_params, codec_info_result));

  // Test invalid feeding - invalid bit_per_sample
  bad_feeding_params = feeding_params;
  bad_feeding_params.bit_per_sample = 7;
  EXPECT_FALSE(A2DP_SetSourceCodec(A2DP_CODEC_SEP_INDEX_SOURCE_SBC,
                                   &bad_feeding_params, codec_info_result));

  // Test invalid feeding - invalid sampling_freq
  bad_feeding_params = feeding_params;
  bad_feeding_params.sampling_freq = 7999;
  EXPECT_FALSE(A2DP_SetSourceCodec(A2DP_CODEC_SEP_INDEX_SOURCE_SBC,
                                   &bad_feeding_params, codec_info_result));
}

TEST(StackA2dpTest, test_build_src2sink_config) {
  uint8_t codec_info_result[AVDT_CODEC_SIZE];

  memset(codec_info_result, 0, sizeof(codec_info_result));
  EXPECT_EQ(A2DP_BuildSrc2SinkConfig(codec_info_sbc, codec_info_result),
            A2DP_SUCCESS);
  // Compare the result codec with the local test codec info
  for (size_t i = 0; i < codec_info_sbc[0] + 1; i++) {
    EXPECT_EQ(codec_info_result[i], codec_info_sbc[i]);
  }

  // Include extra (less preferred) capabilities and test again
  uint8_t codec_info_sbc_test1[AVDT_CODEC_SIZE];
  memcpy(codec_info_sbc_test1, codec_info_sbc, sizeof(codec_info_sbc));
  codec_info_sbc_test1[3] |= (A2DP_SBC_IE_CH_MD_STEREO | A2DP_SBC_IE_CH_MD_DUAL |
                              A2DP_SBC_IE_CH_MD_MONO);
  codec_info_sbc_test1[4] |= (A2DP_SBC_IE_BLOCKS_12 | A2DP_SBC_IE_BLOCKS_8 |
                              A2DP_SBC_IE_BLOCKS_4);
  codec_info_sbc_test1[4] |= A2DP_SBC_IE_SUBBAND_4;
  codec_info_sbc_test1[4] |= A2DP_SBC_IE_ALLOC_MD_S;
  memset(codec_info_result, 0, sizeof(codec_info_result));
  EXPECT_EQ(A2DP_BuildSrc2SinkConfig(codec_info_sbc_test1, codec_info_result),
            A2DP_SUCCESS);
  // Compare the result codec with the local test codec info
  for (size_t i = 0; i < codec_info_sbc[0] + 1; i++) {
    EXPECT_EQ(codec_info_result[i], codec_info_sbc[i]);
  }

  // Test invalid codec info
  memset(codec_info_result, 0, sizeof(codec_info_result));
  memset(codec_info_sbc_test1, 0, sizeof(codec_info_sbc_test1));
  EXPECT_NE(A2DP_BuildSrc2SinkConfig(codec_info_sbc_test1, codec_info_result),
            A2DP_SUCCESS);
}

TEST(StackA2dpTest, test_build_sink_config) {
  uint8_t codec_info_result[AVDT_CODEC_SIZE];
  uint8_t codec_info_expected[AVDT_CODEC_SIZE];

  memcpy(codec_info_expected, codec_info_sbc, sizeof(codec_info_sbc));
  codec_info_expected[5] = codec_info_sbc_sink[5];
  codec_info_expected[6] = codec_info_sbc_sink[6];

  memset(codec_info_result, 0, sizeof(codec_info_result));
  EXPECT_EQ(A2DP_BuildSinkConfig(codec_info_sbc, codec_info_sbc_sink,
                                codec_info_result),
            A2DP_SUCCESS);
  // Compare the result codec with the local test codec info
  for (size_t i = 0; i < codec_info_expected[0] + 1; i++) {
    EXPECT_EQ(codec_info_result[i], codec_info_expected[i]);
  }

  // Change the min/max bitpool and test again
  uint8_t codec_info_sbc_sink_test1[AVDT_CODEC_SIZE];
  memcpy(codec_info_sbc_sink_test1, codec_info_sbc_sink,
         sizeof(codec_info_sbc_sink));
  codec_info_sbc_sink_test1[5] = 3;
  codec_info_sbc_sink_test1[6] = 200;
  codec_info_expected[5] = codec_info_sbc_sink_test1[5];
  codec_info_expected[6] = codec_info_sbc_sink_test1[6];
  memset(codec_info_result, 0, sizeof(codec_info_result));
  EXPECT_EQ(A2DP_BuildSinkConfig(codec_info_sbc, codec_info_sbc_sink_test1,
                                codec_info_result),
            A2DP_SUCCESS);
  // Compare the result codec with the local test codec info
  for (size_t i = 0; i < codec_info_expected[0] + 1; i++) {
    EXPECT_EQ(codec_info_result[i], codec_info_expected[i]);
  }

  // Test invalid codec info
  uint8_t codec_info_sbc_test1[AVDT_CODEC_SIZE];
  memset(codec_info_sbc_test1, 0, sizeof(codec_info_sbc_test1));
  EXPECT_NE(A2DP_BuildSinkConfig(codec_info_sbc_test1, codec_info_sbc_sink,
                                codec_info_result),
            A2DP_SUCCESS);
}

TEST(StackA2dpTest, test_a2dp_uses_rtp_header) {
  EXPECT_TRUE(A2DP_UsesRtpHeader(true, codec_info_sbc));
  EXPECT_TRUE(A2DP_UsesRtpHeader(false, codec_info_sbc));
  EXPECT_TRUE(A2DP_UsesRtpHeader(true, codec_info_non_a2dp));
  EXPECT_TRUE(A2DP_UsesRtpHeader(false, codec_info_non_a2dp));
}

TEST(StackA2dpTest, test_a2dp_codec_sep_index_str) {
  // Explicit tests for known codecs
  EXPECT_STREQ(A2DP_CodecSepIndexStr(A2DP_CODEC_SEP_INDEX_SOURCE_SBC), "SBC");
  EXPECT_STREQ(A2DP_CodecSepIndexStr(A2DP_CODEC_SEP_INDEX_SINK_SBC), "SBC SINK");

  // Test that the unknown codec string has not changed
  EXPECT_STREQ(A2DP_CodecSepIndexStr(A2DP_CODEC_SEP_INDEX_MAX),
               "UNKNOWN CODEC SEP INDEX");

  // Test that each codec has a known string
  for (int i = 0; i < A2DP_CODEC_SEP_INDEX_MAX; i++) {
    tA2DP_CODEC_SEP_INDEX codec_sep_index =
      static_cast<tA2DP_CODEC_SEP_INDEX>(i);
    EXPECT_STRNE(A2DP_CodecSepIndexStr(codec_sep_index),
                 "UNKNOWN CODEC SEP INDEX");
  }
}

TEST(StackA2dpTest, test_a2dp_init_codec_config) {
  tAVDT_CFG avdt_cfg;

  //
  // Test for SBC Source
  //
  memset(&avdt_cfg, 0, sizeof(avdt_cfg));
  EXPECT_TRUE(A2DP_InitCodecConfig(A2DP_CODEC_SEP_INDEX_SOURCE_SBC, &avdt_cfg));
  // Compare the result codec with the local test codec info
  for (size_t i = 0; i < codec_info_sbc[0] + 1; i++) {
    EXPECT_EQ(avdt_cfg.codec_info[i], codec_info_sbc[i]);
  }
  // Test for content protection
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
  EXPECT_EQ(avdt_cfg.protect_info[0], AVDT_CP_LOSC);
  EXPECT_EQ(avdt_cfg.protect_info[1], (AVDT_CP_SCMS_T_ID & 0xFF));
  EXPECT_EQ(avdt_cfg.protect_info[2], ((AVDT_CP_SCMS_T_ID >> 8) & 0xFF));
  EXPECT_EQ(avdt_cfg.num_protect, 1);
#endif

  //
  // Test for SBC Sink
  //
  memset(&avdt_cfg, 0, sizeof(avdt_cfg));
  EXPECT_TRUE(A2DP_InitCodecConfig(A2DP_CODEC_SEP_INDEX_SINK_SBC, &avdt_cfg));
  // Compare the result codec with the local test codec info
  for (size_t i = 0; i < codec_info_sbc_sink[0] + 1; i++) {
    EXPECT_EQ(avdt_cfg.codec_info[i], codec_info_sbc_sink[i]);
  }
}

TEST(StackA2dpTest, test_a2dp_get_media_type) {
  uint8_t codec_info_test[AVDT_CODEC_SIZE];

  EXPECT_EQ(A2DP_GetMediaType(codec_info_sbc), AVDT_MEDIA_TYPE_AUDIO);
  EXPECT_EQ(A2DP_GetMediaType(codec_info_non_a2dp), AVDT_MEDIA_TYPE_AUDIO);

  // Prepare dummy codec info for video and for multimedia
  memset(codec_info_test, 0, sizeof(codec_info_test));
  codec_info_test[0] = sizeof(codec_info_test);
  codec_info_test[1] = 0x01 << 4;
  EXPECT_EQ(A2DP_GetMediaType(codec_info_test), AVDT_MEDIA_TYPE_VIDEO);
  codec_info_test[1] = 0x02 << 4;
  EXPECT_EQ(A2DP_GetMediaType(codec_info_test), AVDT_MEDIA_TYPE_MULTI);
}

TEST(StackA2dpTest, test_a2dp_codec_name) {
  uint8_t codec_info_test[AVDT_CODEC_SIZE];

  // Explicit tests for known codecs
  EXPECT_STREQ(A2DP_CodecName(codec_info_sbc), "SBC");
  EXPECT_STREQ(A2DP_CodecName(codec_info_sbc_sink), "SBC");
  EXPECT_STREQ(A2DP_CodecName(codec_info_non_a2dp), "UNKNOWN VENDOR CODEC");

  // Test all unknown codecs
  memcpy(codec_info_test, codec_info_sbc, sizeof(codec_info_sbc));
  for (uint8_t codec_type = A2DP_MEDIA_CT_SBC + 1;
       codec_type < A2DP_MEDIA_CT_NON_A2DP;
       codec_type++) {
    codec_info_test[2] = codec_type;        // Unknown codec type
    EXPECT_STREQ(A2DP_CodecName(codec_info_test), "UNKNOWN CODEC");
  }
}

TEST(StackA2dpTest, test_a2dp_vendor) {
  EXPECT_FALSE(A2DP_IsVendorSourceCodecSupported(codec_info_non_a2dp));
  EXPECT_EQ(A2DP_VendorCodecGetVendorId(codec_info_non_a2dp),
              (uint32_t)0x00000403);
  EXPECT_EQ(A2DP_VendorCodecGetCodecId(codec_info_non_a2dp), (uint16_t)0x0807);
  EXPECT_TRUE(A2DP_VendorUsesRtpHeader(true, codec_info_non_a2dp));
  EXPECT_TRUE(A2DP_VendorUsesRtpHeader(false, codec_info_non_a2dp));
}

TEST(StackA2dpTest, test_a2dp_codec_type_equals) {
  EXPECT_TRUE(A2DP_CodecTypeEquals(codec_info_sbc, codec_info_sbc_sink));
  EXPECT_TRUE(A2DP_CodecTypeEquals(codec_info_non_a2dp,
                                  codec_info_non_a2dp_dummy));
  EXPECT_FALSE(A2DP_CodecTypeEquals(codec_info_sbc, codec_info_non_a2dp));
}

TEST(StackA2dpTest, test_a2dp_codec_equals) {
  uint8_t codec_info_sbc_test[AVDT_CODEC_SIZE];
  uint8_t codec_info_non_a2dp_test[AVDT_CODEC_SIZE];

  // Test two identical SBC codecs
  memset(codec_info_sbc_test, 0xAB, sizeof(codec_info_sbc_test));
  memcpy(codec_info_sbc_test, codec_info_sbc, sizeof(codec_info_sbc));
  EXPECT_TRUE(A2DP_CodecEquals(codec_info_sbc, codec_info_sbc_test));

  // Test two identical non-A2DP codecs that are not recognized
  memset(codec_info_non_a2dp_test, 0xAB, sizeof(codec_info_non_a2dp_test));
  memcpy(codec_info_non_a2dp_test, codec_info_non_a2dp,
         sizeof(codec_info_non_a2dp));
  EXPECT_FALSE(A2DP_CodecEquals(codec_info_non_a2dp, codec_info_non_a2dp_test));

  // Test two codecs that have different types
  EXPECT_FALSE(A2DP_CodecEquals(codec_info_sbc, codec_info_non_a2dp));

  // Test two SBC codecs that are slightly different
  memset(codec_info_sbc_test, 0xAB, sizeof(codec_info_sbc_test));
  memcpy(codec_info_sbc_test, codec_info_sbc, sizeof(codec_info_sbc));
  codec_info_sbc_test[5] = codec_info_sbc[5] + 1;
  EXPECT_FALSE(A2DP_CodecEquals(codec_info_sbc, codec_info_sbc_test));
  codec_info_sbc_test[5] = codec_info_sbc[5];
  codec_info_sbc_test[6] = codec_info_sbc[6] + 1;
  EXPECT_FALSE(A2DP_CodecEquals(codec_info_sbc, codec_info_sbc_test));

  // Test two SBC codecs that are identical, but with different dummy
  // trailer data.
  memset(codec_info_sbc_test, 0xAB, sizeof(codec_info_sbc_test));
  memcpy(codec_info_sbc_test, codec_info_sbc, sizeof(codec_info_sbc));
  codec_info_sbc_test[7] = codec_info_sbc[7] + 1;
  EXPECT_TRUE(A2DP_CodecEquals(codec_info_sbc, codec_info_sbc_test));
}

TEST(StackA2dpTest, test_a2dp_codec_requires_reconfig) {
  uint8_t codec_info_sbc_test[AVDT_CODEC_SIZE];

  // Test two identical SBC codecs
  memset(codec_info_sbc_test, 0xAB, sizeof(codec_info_sbc_test));
  memcpy(codec_info_sbc_test, codec_info_sbc, sizeof(codec_info_sbc));
  EXPECT_FALSE(A2DP_CodecRequiresReconfig(codec_info_sbc, codec_info_sbc_test));

  // Test two codecs that have different types
  EXPECT_TRUE(A2DP_CodecRequiresReconfig(codec_info_sbc, codec_info_non_a2dp));

  // Test two SBC codecs that are slightly different, and don't require
  // reconfig.
  memset(codec_info_sbc_test, 0xAB, sizeof(codec_info_sbc_test));
  memcpy(codec_info_sbc_test, codec_info_sbc, sizeof(codec_info_sbc));
  codec_info_sbc_test[5] = codec_info_sbc[5] + 1;
  EXPECT_FALSE(A2DP_CodecRequiresReconfig(codec_info_sbc, codec_info_sbc_test));
  codec_info_sbc_test[5] = codec_info_sbc[5];
  codec_info_sbc_test[6] = codec_info_sbc[6] + 1;
  EXPECT_FALSE(A2DP_CodecRequiresReconfig(codec_info_sbc, codec_info_sbc_test));

  // Test two SBC codecs that are slightly different, and require reconfig.
  memset(codec_info_sbc_test, 0xAB, sizeof(codec_info_sbc_test));
  memcpy(codec_info_sbc_test, codec_info_sbc, sizeof(codec_info_sbc));
  codec_info_sbc_test[3] = 0x10 | 0x01; // A2DP_SBC_IE_SAMP_FREQ_48 |
                                        // A2DP_SBC_IE_CH_MD_JOINT
  EXPECT_TRUE(A2DP_CodecRequiresReconfig(codec_info_sbc, codec_info_sbc_test));

  // Test two SBC codecs that are identical, but with different dummy
  // trailer data.
  memset(codec_info_sbc_test, 0xAB, sizeof(codec_info_sbc_test));
  memcpy(codec_info_sbc_test, codec_info_sbc, sizeof(codec_info_sbc));
  codec_info_sbc_test[7] = codec_info_sbc[7] + 1;
  EXPECT_FALSE(A2DP_CodecRequiresReconfig(codec_info_sbc, codec_info_sbc_test));
}

TEST(StackA2dpTest, test_a2dp_codec_config_matches_capabilities) {
  EXPECT_TRUE(A2DP_CodecConfigMatchesCapabilities(codec_info_sbc,
                                                 codec_info_sbc_sink));
  EXPECT_FALSE(A2DP_CodecConfigMatchesCapabilities(codec_info_non_a2dp,
                                                  codec_info_non_a2dp_dummy));
  EXPECT_FALSE(A2DP_CodecConfigMatchesCapabilities(codec_info_sbc,
                                                  codec_info_non_a2dp));
}

TEST(StackA2dpTest, test_a2dp_get_track_frequency) {
  EXPECT_EQ(A2DP_GetTrackFrequency(codec_info_sbc), 44100);
  EXPECT_EQ(A2DP_GetTrackFrequency(codec_info_non_a2dp), -1);
}

TEST(StackA2dpTest, test_a2dp_get_track_channel_count) {
  EXPECT_EQ(A2DP_GetTrackChannelCount(codec_info_sbc), 2);
  EXPECT_EQ(A2DP_GetTrackChannelCount(codec_info_non_a2dp), -1);
}

TEST(StackA2dpTest, test_a2dp_get_number_of_subbands) {
  EXPECT_EQ(A2DP_GetNumberOfSubbands(codec_info_sbc), 8);
  EXPECT_EQ(A2DP_GetNumberOfSubbands(codec_info_non_a2dp), -1);
}

TEST(StackA2dpTest, test_a2dp_get_number_of_blocks) {
  EXPECT_EQ(A2DP_GetNumberOfBlocks(codec_info_sbc), 16);
  EXPECT_EQ(A2DP_GetNumberOfBlocks(codec_info_non_a2dp), -1);
}

TEST(StackA2dpTest, test_a2dp_get_allocation_method_code) {
  EXPECT_EQ(A2DP_GetAllocationMethodCode(codec_info_sbc), 0);
  EXPECT_EQ(A2DP_GetAllocationMethodCode(codec_info_non_a2dp), -1);
}

TEST(StackA2dpTest, test_a2dp_get_channel_mode_code) {
  EXPECT_EQ(A2DP_GetChannelModeCode(codec_info_sbc), 3);
  EXPECT_EQ(A2DP_GetChannelModeCode(codec_info_non_a2dp), -1);
}

TEST(StackA2dpTest, test_a2dp_get_sampling_frequency_code) {
  EXPECT_EQ(A2DP_GetSamplingFrequencyCode(codec_info_sbc), 2);
  EXPECT_EQ(A2DP_GetSamplingFrequencyCode(codec_info_non_a2dp), -1);
}

TEST(StackA2dpTest, test_a2dp_get_min_bitpool) {
  EXPECT_EQ(A2DP_GetMinBitpool(codec_info_sbc), 2);
  EXPECT_EQ(A2DP_GetMinBitpool(codec_info_sbc_sink), 2);
  EXPECT_EQ(A2DP_GetMinBitpool(codec_info_non_a2dp), -1);
}

TEST(StackA2dpTest, test_a2dp_get_max_bitpool) {
  EXPECT_EQ(A2DP_GetMaxBitpool(codec_info_sbc), 53);
  EXPECT_EQ(A2DP_GetMaxBitpool(codec_info_sbc_sink), 250);
  EXPECT_EQ(A2DP_GetMaxBitpool(codec_info_non_a2dp), -1);
}

TEST(StackA2dpTest, test_a2dp_get_sink_track_channel_type) {
  EXPECT_EQ(A2DP_GetSinkTrackChannelType(codec_info_sbc), 3);
  EXPECT_EQ(A2DP_GetSinkTrackChannelType(codec_info_non_a2dp), -1);
}

TEST(StackA2dpTest, test_a2dp_get_sink_frames_count_to_process) {
  EXPECT_EQ(A2DP_GetSinkFramesCountToProcess(20, codec_info_sbc), 7);
  EXPECT_EQ(A2DP_GetSinkFramesCountToProcess(20, codec_info_non_a2dp), -1);
}

TEST(StackA2dpTest, test_a2dp_get_packet_timestamp) {
  uint8_t a2dp_data[1000];
  uint32_t timestamp;
  uint32_t *p_ts = reinterpret_cast<uint32_t *>(a2dp_data);

  memset(a2dp_data, 0xAB, sizeof(a2dp_data));
  *p_ts = 0x12345678;
  timestamp = 0xFFFFFFFF;
  EXPECT_TRUE(A2DP_GetPacketTimestamp(codec_info_sbc, a2dp_data, &timestamp));
  EXPECT_EQ(timestamp, static_cast<uint32_t>(0x12345678));

  memset(a2dp_data, 0xAB, sizeof(a2dp_data));
  *p_ts = 0x12345678;
  timestamp = 0xFFFFFFFF;
  EXPECT_FALSE(A2DP_GetPacketTimestamp(codec_info_non_a2dp, a2dp_data,
                                      &timestamp));
}

TEST(StackA2dpTest, test_a2dp_build_codec_header) {
  uint8_t a2dp_data[1000];
  BT_HDR *p_buf = reinterpret_cast<BT_HDR *>(a2dp_data);
  const uint16_t BT_HDR_LEN = 500;
  const uint16_t BT_HDR_OFFSET = 50;
  const uint8_t FRAMES_PER_PACKET = 0xCD;

  memset(a2dp_data, 0xAB, sizeof(a2dp_data));
  p_buf->len = BT_HDR_LEN;
  p_buf->offset = BT_HDR_OFFSET;
  EXPECT_TRUE(A2DP_BuildCodecHeader(codec_info_sbc, p_buf, FRAMES_PER_PACKET));
  EXPECT_EQ(p_buf->offset + 1, BT_HDR_OFFSET);  // Modified by A2DP_SBC_MPL_HDR_LEN
  EXPECT_EQ(p_buf->len - 1, BT_HDR_LEN);        // Modified by A2DP_SBC_MPL_HDR_LEN
  const uint8_t *p =
      reinterpret_cast<const uint8_t *>(p_buf + 1) + p_buf->offset;
  EXPECT_EQ(*p, static_cast<uint8_t>(0x0D));    // 0xCD masked with A2DP_SBC_HDR_NUM_MSK

  memset(a2dp_data, 0xAB, sizeof(a2dp_data));
  p_buf->len = BT_HDR_LEN;
  p_buf->offset = BT_HDR_OFFSET;
  EXPECT_FALSE(A2DP_BuildCodecHeader(codec_info_non_a2dp, p_buf,
                                    FRAMES_PER_PACKET));
}

TEST(StackA2dpTest, test_a2dp_adjust_codec) {
  uint8_t codec_info_sbc_test[AVDT_CODEC_SIZE];
  uint8_t codec_info_non_a2dp_test[AVDT_CODEC_SIZE];

  // Test updating a valid SBC codec that doesn't need adjustment
  memset(codec_info_sbc_test, 0xAB, sizeof(codec_info_sbc_test));
  memcpy(codec_info_sbc_test, codec_info_sbc, sizeof(codec_info_sbc));
  EXPECT_TRUE(A2DP_AdjustCodec(codec_info_sbc_test));
  EXPECT_TRUE(memcmp(codec_info_sbc_test, codec_info_sbc,
                     sizeof(codec_info_sbc)) == 0);

  // Test updating a valid SBC codec that needs adjustment
  memset(codec_info_sbc_test, 0xAB, sizeof(codec_info_sbc_test));
  memcpy(codec_info_sbc_test, codec_info_sbc, sizeof(codec_info_sbc));
  codec_info_sbc_test[6] = 54;  // A2DP_SBC_MAX_BITPOOL + 1
  EXPECT_TRUE(A2DP_AdjustCodec(codec_info_sbc_test));
  EXPECT_TRUE(memcmp(codec_info_sbc_test, codec_info_sbc,
                     sizeof(codec_info_sbc)) == 0);

  // Test updating an invalid SBC codec
  memset(codec_info_sbc_test, 0xAB, sizeof(codec_info_sbc_test));
  memcpy(codec_info_sbc_test, codec_info_sbc, sizeof(codec_info_sbc));
  codec_info_sbc_test[6] = 255; // Invalid MAX_BITPOOL
  EXPECT_FALSE(A2DP_AdjustCodec(codec_info_sbc_test));

  // Test updating a non-A2DP codec that is not recognized
  memset(codec_info_non_a2dp_test, 0xAB, sizeof(codec_info_non_a2dp_test));
  memcpy(codec_info_non_a2dp_test, codec_info_non_a2dp,
         sizeof(codec_info_non_a2dp));
  EXPECT_FALSE(A2DP_AdjustCodec(codec_info_non_a2dp_test));
}
