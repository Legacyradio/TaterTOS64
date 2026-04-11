/*
 * hda.h — Intel High Definition Audio driver for TaterTOS64v3
 *
 * Implements HDA controller initialization, codec discovery,
 * output path setup, and PCM playback via DMA.
 *
 * Reference: Intel High Definition Audio Specification Rev 1.0a
 * Target: Intel PCH HDA on Dell Precision 7530 (Coffee Lake)
 */

#ifndef TATER_HDA_H
#define TATER_HDA_H

#include <stdint.h>

/* ================================================================== */
/* HDA Controller Registers (offsets from BAR0)                        */
/* ================================================================== */

#define HDA_GCAP       0x00   /* Global Capabilities (16-bit) */
#define HDA_VMIN       0x02   /* Minor Version */
#define HDA_VMAJ       0x03   /* Major Version */
#define HDA_OUTPAY     0x04   /* Output Payload Capability */
#define HDA_INPAY      0x06   /* Input Payload Capability */
#define HDA_GCTL       0x08   /* Global Control (32-bit) */
#define HDA_WAKEEN     0x0C   /* Wake Enable */
#define HDA_STATESTS   0x0E   /* State Change Status (16-bit) */
#define HDA_GSTS       0x10   /* Global Status */
#define HDA_INTCTL     0x20   /* Interrupt Control (32-bit) */
#define HDA_INTSTS     0x24   /* Interrupt Status */
#define HDA_WALCLK     0x30   /* Wall Clock Counter */
#define HDA_SSYNC      0x38   /* Stream Synchronization */

/* CORB registers */
#define HDA_CORBLBASE  0x40   /* CORB Lower Base Address */
#define HDA_CORBUBASE  0x44   /* CORB Upper Base Address */
#define HDA_CORBWP     0x48   /* CORB Write Pointer (16-bit) */
#define HDA_CORBRP     0x4A   /* CORB Read Pointer (16-bit) */
#define HDA_CORBCTL    0x4C   /* CORB Control (8-bit) */
#define HDA_CORBSTS    0x4D   /* CORB Status */
#define HDA_CORBSIZE   0x4E   /* CORB Size (8-bit) */

/* RIRB registers */
#define HDA_RIRBLBASE  0x50   /* RIRB Lower Base Address */
#define HDA_RIRBUBASE  0x54   /* RIRB Upper Base Address */
#define HDA_RIRBWP     0x58   /* RIRB Write Pointer (16-bit) */
#define HDA_RINTCNT    0x5A   /* Response Interrupt Count */
#define HDA_RIRBCTL    0x5C   /* RIRB Control (8-bit) */
#define HDA_RIRBSTS    0x5D   /* RIRB Status */
#define HDA_RIRBSIZE   0x5E   /* RIRB Size (8-bit) */

/* Stream Descriptor registers (base = 0x80 + n*0x20 for input,
   0x80 + ISS*0x20 + n*0x20 for output) */
#define HDA_SD_CTL     0x00   /* Stream Descriptor Control (24-bit) */
#define HDA_SD_STS     0x03   /* Stream Descriptor Status (8-bit) */
#define HDA_SD_LPIB    0x04   /* Link Position in Buffer (32-bit) */
#define HDA_SD_CBL     0x08   /* Cyclic Buffer Length (32-bit) */
#define HDA_SD_LVI     0x0C   /* Last Valid Index (16-bit) */
#define HDA_SD_FIFOW   0x0E   /* FIFO Watermark */
#define HDA_SD_FIFOSIZE 0x10  /* FIFO Size (16-bit) */
#define HDA_SD_FORMAT  0x12   /* Stream Format (16-bit) */
#define HDA_SD_BDLPL   0x18   /* BDL Pointer Lower (32-bit) */
#define HDA_SD_BDLPU   0x1C   /* BDL Pointer Upper (32-bit) */

/* GCTL bits */
#define HDA_GCTL_CRST  (1 << 0)   /* Controller Reset */
#define HDA_GCTL_FCNTRL (1 << 1)  /* Flush Control */
#define HDA_GCTL_UNSOL  (1 << 8)  /* Accept Unsolicited Response */

/* CORBCTL bits */
#define HDA_CORBCTL_RUN  (1 << 1)  /* CORB DMA Engine Run */

/* RIRBCTL bits */
#define HDA_RIRBCTL_RUN  (1 << 1)  /* RIRB DMA Engine Run */
#define HDA_RIRBCTL_INT  (1 << 0)  /* Response Interrupt Control */

/* Stream Descriptor CTL bits */
#define HDA_SD_CTL_RUN   (1 << 1)  /* Stream Run */
#define HDA_SD_CTL_IOCE  (1 << 2)  /* Interrupt on Completion Enable */
#define HDA_SD_CTL_STRIPE (1 << 4) /* Stripe Control */

/* Stream format register encoding */
#define HDA_FMT_48KHZ   (0 << 14)                /* Base = 48kHz */
#define HDA_FMT_44KHZ   (1 << 14)                /* Base = 44.1kHz */
#define HDA_FMT_MULT_1  (0 << 11)                /* Multiplier x1 */
#define HDA_FMT_DIV_1   (0 << 8)                 /* Divisor /1 */
#define HDA_FMT_16BIT   (1 << 4)                 /* 16 bits per sample */
#define HDA_FMT_STEREO  (1 << 0)                 /* 2 channels (stereo) */

/* Standard format: 48kHz, 16-bit, stereo */
#define HDA_FMT_PCM_48K_16_STEREO \
    (HDA_FMT_48KHZ | HDA_FMT_MULT_1 | HDA_FMT_DIV_1 | HDA_FMT_16BIT | HDA_FMT_STEREO)

/* HDA verb encoding: codec_addr(4) | node_id(8) | verb(20) */
#define HDA_VERB(codec, nid, verb) \
    (((uint32_t)(codec) << 28) | ((uint32_t)(nid) << 20) | (uint32_t)(verb))

/* Common verb IDs */
#define HDA_VERB_GET_PARAM       0xF0000
#define HDA_VERB_SET_STREAM_FMT  0x20000
#define HDA_VERB_GET_STREAM_FMT  0xA0000
#define HDA_VERB_SET_CHAN_STREAM  0x70600
#define HDA_VERB_GET_CHAN_STREAM  0xF0600
#define HDA_VERB_SET_PIN_CTL     0x70700
#define HDA_VERB_GET_PIN_CTL     0xF0700
#define HDA_VERB_SET_EAPD        0x70C00
#define HDA_VERB_SET_AMP_GAIN    0x30000
#define HDA_VERB_GET_AMP_GAIN    0xB0000
#define HDA_VERB_SET_POWER       0x70500
#define HDA_VERB_GET_POWER       0xF0500
#define HDA_VERB_SET_CONNECT     0x70100
#define HDA_VERB_GET_CONNECT     0xF0200
#define HDA_VERB_GET_CONN_LIST   0xF0200
#define HDA_VERB_GET_SUBSYS_ID   0xF2000

/* Parameter IDs (for GET_PARAM) */
#define HDA_PARAM_VENDOR_ID      0x00
#define HDA_PARAM_REVISION_ID    0x02
#define HDA_PARAM_SUBNODE_COUNT  0x04
#define HDA_PARAM_FN_GROUP_TYPE  0x05
#define HDA_PARAM_AUDIO_CAPS     0x09
#define HDA_PARAM_PIN_CAPS       0x0C
#define HDA_PARAM_AMP_IN_CAPS    0x0D
#define HDA_PARAM_AMP_OUT_CAPS   0x12
#define HDA_PARAM_CONN_LIST_LEN  0x0E
#define HDA_PARAM_POWER_STATES   0x0F
#define HDA_PARAM_GPIO_COUNT     0x11
#define HDA_PARAM_VOL_KNOB       0x13
#define HDA_PARAM_AUDIO_WIDGET   0x09
#define HDA_PARAM_SUPPORTED_FMTS 0x0A
#define HDA_PARAM_PIN_WIDGET_CAP 0x0C

/* Widget types (from Audio Widget Capabilities parameter) */
#define HDA_WIDGET_AO    0x0  /* Audio Output */
#define HDA_WIDGET_AI    0x1  /* Audio Input */
#define HDA_WIDGET_MIXER 0x2  /* Audio Mixer */
#define HDA_WIDGET_SEL   0x3  /* Audio Selector */
#define HDA_WIDGET_PIN   0x4  /* Pin Complex */
#define HDA_WIDGET_POWER 0x5  /* Power Widget */
#define HDA_WIDGET_VOL   0x6  /* Volume Knob */
#define HDA_WIDGET_BEEP  0x7  /* Beep Generator */
#define HDA_WIDGET_VENDOR 0xF /* Vendor Defined */

/* Pin default configuration: device type (bits 23:20) */
#define HDA_PIN_DEV_LINE_OUT   0x0
#define HDA_PIN_DEV_SPEAKER    0x1
#define HDA_PIN_DEV_HP_OUT     0x2
#define HDA_PIN_DEV_CD         0x3
#define HDA_PIN_DEV_SPDIF_OUT  0x4
#define HDA_PIN_DEV_DIG_OUT    0x5
#define HDA_PIN_DEV_MODEM_LINE 0x6
#define HDA_PIN_DEV_MODEM_HAND 0x7
#define HDA_PIN_DEV_LINE_IN    0x8
#define HDA_PIN_DEV_AUX        0x9
#define HDA_PIN_DEV_MIC_IN     0xA
#define HDA_PIN_DEV_TELEPHONY  0xB
#define HDA_PIN_DEV_SPDIF_IN   0xC
#define HDA_PIN_DEV_DIG_IN     0xD
#define HDA_PIN_DEV_OTHER      0xF

/* BDL (Buffer Descriptor List) entry */
struct hda_bdl_entry {
    uint64_t addr;     /* physical address of buffer */
    uint32_t length;   /* buffer length in bytes */
    uint32_t ioc;      /* interrupt on completion flag */
} __attribute__((packed));

/* Audio stream info (for userspace) */
struct hda_stream_info {
    uint32_t sample_rate;   /* Hz (e.g. 48000) */
    uint8_t  channels;      /* 1=mono, 2=stereo */
    uint8_t  bits;          /* bits per sample (16) */
    uint8_t  active;        /* 1 if stream is running */
    uint8_t  _pad;
};

/* ================================================================== */
/* Driver API                                                          */
/* ================================================================== */

void hda_init(void);
int hda_is_ready(void);

/* Stream management */
int hda_open_output(uint32_t sample_rate, uint8_t channels, uint8_t bits);
int hda_write_pcm(const void *pcm_data, uint32_t byte_count);
void hda_close_output(void);
int hda_get_stream_info(struct hda_stream_info *info);

/* DMA position (for A/V sync) */
uint32_t hda_get_position(void);

#endif /* TATER_HDA_H */
