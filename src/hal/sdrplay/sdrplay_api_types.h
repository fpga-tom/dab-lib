#ifndef DAB_HAL_SDRPLAY_API_TYPES_H
#define DAB_HAL_SDRPLAY_API_TYPES_H

#include <stdint.h>

typedef struct dab_sdrplay_api_device {
    char SerNo[64];
    unsigned char hwVer;
    uint32_t tuner;
    uint32_t rspDuoMode;
    unsigned char valid;
    double rspDuoSampleFreq;
    void *dev;
} dab_sdrplay_api_device_t;

typedef enum {
    DAB_SDRPLAY_TUNER_NEITHER = 0,
    DAB_SDRPLAY_TUNER_A = 1,
    DAB_SDRPLAY_TUNER_B = 2,
    DAB_SDRPLAY_TUNER_BOTH = 3
} dab_sdrplay_tuner_select_t;

typedef enum {
    DAB_SDRPLAY_RSPDUO_MODE_UNKNOWN = 0,
    DAB_SDRPLAY_RSPDUO_MODE_SINGLE_TUNER = 1,
    DAB_SDRPLAY_RSPDUO_MODE_DUAL_TUNER = 2,
    DAB_SDRPLAY_RSPDUO_MODE_MASTER = 4,
    DAB_SDRPLAY_RSPDUO_MODE_SLAVE = 8
} dab_sdrplay_rspduo_mode_t;

typedef enum {
    DAB_SDRPLAY_BW_UNDEFINED = 0,
    DAB_SDRPLAY_BW_0_200 = 200,
    DAB_SDRPLAY_BW_0_300 = 300,
    DAB_SDRPLAY_BW_0_600 = 600,
    DAB_SDRPLAY_BW_1_536 = 1536,
    DAB_SDRPLAY_BW_5_000 = 5000,
    DAB_SDRPLAY_BW_6_000 = 6000,
    DAB_SDRPLAY_BW_7_000 = 7000,
    DAB_SDRPLAY_BW_8_000 = 8000
} dab_sdrplay_bw_mhz_t;

typedef enum {
    DAB_SDRPLAY_IF_UNDEFINED = -1,
    DAB_SDRPLAY_IF_ZERO = 0,
    DAB_SDRPLAY_IF_0_450 = 450,
    DAB_SDRPLAY_IF_1_620 = 1620,
    DAB_SDRPLAY_IF_2_048 = 2048
} dab_sdrplay_if_khz_t;

typedef enum {
    DAB_SDRPLAY_LO_UNDEFINED = 0,
    DAB_SDRPLAY_LO_AUTO = 1
} dab_sdrplay_lo_mode_t;

typedef enum {
    DAB_SDRPLAY_NORMAL_MIN_GR = 20
} dab_sdrplay_min_gain_reduction_t;

typedef enum {
    DAB_SDRPLAY_AGC_DISABLE = 0,
    DAB_SDRPLAY_AGC_100HZ = 1,
    DAB_SDRPLAY_AGC_50HZ = 2,
    DAB_SDRPLAY_AGC_5HZ = 3,
    DAB_SDRPLAY_AGC_CTRL_EN = 4
} dab_sdrplay_agc_control_t;

typedef uint32_t dab_sdrplay_reason_for_update_t;
typedef uint32_t dab_sdrplay_reason_for_update_ext1_t;

#define DAB_SDRPLAY_UPDATE_NONE ((dab_sdrplay_reason_for_update_t)0x00000000u)
#define DAB_SDRPLAY_UPDATE_DEV_FS ((dab_sdrplay_reason_for_update_t)0x00000001u)
#define DAB_SDRPLAY_UPDATE_DEV_PPM ((dab_sdrplay_reason_for_update_t)0x00000002u)
#define DAB_SDRPLAY_UPDATE_RSP1A_BIAS_T ((dab_sdrplay_reason_for_update_t)0x00000010u)
#define DAB_SDRPLAY_UPDATE_RSP1A_RF_DAB_NOTCH ((dab_sdrplay_reason_for_update_t)0x00000040u)
#define DAB_SDRPLAY_UPDATE_RSP2_BIAS_T ((dab_sdrplay_reason_for_update_t)0x00000080u)
#define DAB_SDRPLAY_UPDATE_RSP2_ANTENNA ((dab_sdrplay_reason_for_update_t)0x00000200u)
#define DAB_SDRPLAY_UPDATE_RSPDUO_BIAS_T ((dab_sdrplay_reason_for_update_t)0x08000000u)
#define DAB_SDRPLAY_UPDATE_RSPDUO_RF_DAB_NOTCH ((dab_sdrplay_reason_for_update_t)0x80000000u)
#define DAB_SDRPLAY_UPDATE_TUNER_GR ((dab_sdrplay_reason_for_update_t)0x00008000u)
#define DAB_SDRPLAY_UPDATE_TUNER_FRF ((dab_sdrplay_reason_for_update_t)0x00020000u)
#define DAB_SDRPLAY_UPDATE_TUNER_BWTYPE ((dab_sdrplay_reason_for_update_t)0x00040000u)
#define DAB_SDRPLAY_UPDATE_TUNER_IFTYPE ((dab_sdrplay_reason_for_update_t)0x00080000u)
#define DAB_SDRPLAY_UPDATE_CTRL_AGC ((dab_sdrplay_reason_for_update_t)0x01000000u)

#define DAB_SDRPLAY_UPDATE_EXT1_NONE ((dab_sdrplay_reason_for_update_ext1_t)0x00000000u)
#define DAB_SDRPLAY_UPDATE_RSPDX_BIAS_T ((dab_sdrplay_reason_for_update_ext1_t)0x00000002u)
#define DAB_SDRPLAY_UPDATE_RSPDX_ANTENNA ((dab_sdrplay_reason_for_update_ext1_t)0x00000004u)
#define DAB_SDRPLAY_UPDATE_RSPDX_RF_DAB_NOTCH ((dab_sdrplay_reason_for_update_ext1_t)0x00000010u)

typedef enum {
    DAB_SDRPLAY_RSPDX_ANTENNA_A = 0,
    DAB_SDRPLAY_RSPDX_ANTENNA_B = 1,
    DAB_SDRPLAY_RSPDX_ANTENNA_C = 2
} dab_sdrplay_rspdx_antenna_select_t;

typedef enum {
    DAB_SDRPLAY_RSP2_ANTENNA_A = 5,
    DAB_SDRPLAY_RSP2_ANTENNA_B = 6
} dab_sdrplay_rsp2_antenna_select_t;

typedef struct {
    unsigned int firstSampleNum;
    int grChanged;
    int rfChanged;
    int fsChanged;
    unsigned int numSamples;
} dab_sdrplay_stream_cb_params_t;

typedef struct {
    float curr;
    float max;
    float min;
} dab_sdrplay_gain_values_t;

typedef struct {
    int gRdB;
    unsigned char LNAstate;
    unsigned char syncUpdate;
    dab_sdrplay_min_gain_reduction_t minGr;
    dab_sdrplay_gain_values_t gainVals;
} dab_sdrplay_gain_t;

typedef struct {
    double rfHz;
    unsigned char syncUpdate;
} dab_sdrplay_rf_freq_t;

typedef struct {
    unsigned char dcCal;
    unsigned char speedUp;
    int trackTime;
    int refreshRateTime;
} dab_sdrplay_dc_offset_tuner_t;

typedef struct {
    dab_sdrplay_bw_mhz_t bwType;
    dab_sdrplay_if_khz_t ifType;
    dab_sdrplay_lo_mode_t loMode;
    dab_sdrplay_gain_t gain;
    dab_sdrplay_rf_freq_t rfFreq;
    dab_sdrplay_dc_offset_tuner_t dcOffsetTuner;
} dab_sdrplay_tuner_params_t;

typedef struct {
    unsigned char DCenable;
    unsigned char IQenable;
} dab_sdrplay_dc_offset_t;

typedef struct {
    unsigned char enable;
    unsigned char decimationFactor;
    unsigned char wideBandSignal;
} dab_sdrplay_decimation_t;

typedef struct {
    dab_sdrplay_agc_control_t enable;
    int setPoint_dBfs;
    unsigned short attack_ms;
    unsigned short decay_ms;
    unsigned short decay_delay_ms;
    unsigned short decay_threshold_dB;
    int syncUpdate;
} dab_sdrplay_agc_t;

typedef struct {
    dab_sdrplay_dc_offset_t dcOffset;
    dab_sdrplay_decimation_t decimation;
    dab_sdrplay_agc_t agc;
    uint32_t adsbMode;
} dab_sdrplay_control_params_t;

typedef struct {
    unsigned char biasTEnable;
} dab_sdrplay_rsp1a_tuner_params_t;

typedef struct {
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
} dab_sdrplay_rsp1a_params_t;

typedef struct {
    unsigned char biasTEnable;
    uint32_t amPortSel;
    dab_sdrplay_rsp2_antenna_select_t antennaSel;
    unsigned char rfNotchEnable;
} dab_sdrplay_rsp2_tuner_params_t;

typedef struct {
    unsigned char extRefOutputEn;
} dab_sdrplay_rsp2_params_t;

typedef struct {
    unsigned char biasTEnable;
    uint32_t tuner1AmPortSel;
    unsigned char tuner1AmNotchEnable;
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
    struct {
        unsigned char resetGainUpdate;
        unsigned char resetRfUpdate;
    } resetSlaveFlags;
} dab_sdrplay_rspduo_tuner_params_t;

typedef struct {
    int extRefOutputEn;
} dab_sdrplay_rspduo_params_t;

typedef struct {
    unsigned char hdrEnable;
    unsigned char biasTEnable;
    dab_sdrplay_rspdx_antenna_select_t antennaSel;
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
} dab_sdrplay_rspdx_params_t;

typedef struct {
    uint32_t hdrBw;
} dab_sdrplay_rspdx_tuner_params_t;

typedef struct {
    dab_sdrplay_tuner_params_t tunerParams;
    dab_sdrplay_control_params_t ctrlParams;
    dab_sdrplay_rsp1a_tuner_params_t rsp1aTunerParams;
    dab_sdrplay_rsp2_tuner_params_t rsp2TunerParams;
    dab_sdrplay_rspduo_tuner_params_t rspDuoTunerParams;
    dab_sdrplay_rspdx_tuner_params_t rspDxTunerParams;
} dab_sdrplay_rx_channel_params_t;

typedef enum {
    DAB_SDRPLAY_ISOCH = 0,
    DAB_SDRPLAY_BULK = 1
} dab_sdrplay_transfer_mode_t;

typedef struct {
    double fsHz;
    unsigned char syncUpdate;
    unsigned char reCal;
} dab_sdrplay_fs_freq_t;

typedef struct {
    unsigned int sampleNum;
    unsigned int period;
} dab_sdrplay_sync_update_t;

typedef struct {
    unsigned char resetGainUpdate;
    unsigned char resetRfUpdate;
    unsigned char resetFsUpdate;
} dab_sdrplay_reset_flags_t;

typedef struct {
    double ppm;
    dab_sdrplay_fs_freq_t fsFreq;
    dab_sdrplay_sync_update_t syncUpdate;
    dab_sdrplay_reset_flags_t resetFlags;
    dab_sdrplay_transfer_mode_t mode;
    unsigned int samplesPerPkt;
    dab_sdrplay_rsp1a_params_t rsp1aParams;
    dab_sdrplay_rsp2_params_t rsp2Params;
    dab_sdrplay_rspduo_params_t rspDuoParams;
    dab_sdrplay_rspdx_params_t rspDxParams;
} dab_sdrplay_dev_params_t;

typedef struct {
    dab_sdrplay_dev_params_t *devParams;
    dab_sdrplay_rx_channel_params_t *rxChannelA;
    dab_sdrplay_rx_channel_params_t *rxChannelB;
} dab_sdrplay_device_params_t;

typedef void (*dab_sdrplay_stream_callback_fn)(short *xi,
                                               short *xq,
                                               dab_sdrplay_stream_cb_params_t *params,
                                               unsigned int num_samples,
                                               unsigned int reset,
                                               void *cb_context);

typedef void (*dab_sdrplay_event_callback_fn)(uint32_t event_id,
                                              dab_sdrplay_tuner_select_t tuner,
                                              void *params,
                                              void *cb_context);

typedef struct dab_sdrplay_api_callbacks {
    dab_sdrplay_stream_callback_fn stream_a_cb;
    dab_sdrplay_stream_callback_fn stream_b_cb;
    dab_sdrplay_event_callback_fn event_cb;
} dab_sdrplay_api_callbacks_t;

#endif
