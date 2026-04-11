/*
 * hda.c — Intel High Definition Audio driver for TaterTOS64v3
 *
 * Minimal HDA controller driver for PCM audio playback.
 * Targets Intel PCH HDA on Coffee Lake (Dell Precision 7530).
 *
 * Flow: PCI detect → BAR0 map → controller reset → CORB/RIRB setup →
 *       codec discover → widget enumeration → find output DAC + pin →
 *       configure output path → stream setup → DMA playback.
 *
 * Reference: Intel HD Audio Specification Rev 1.0a
 */

#include "hda.h"
#include <stdint.h>
#include <stddef.h>
#include "../pci/pci.h"

/* Kernel headers */
void kprint(const char *fmt, ...);
void *kmalloc(size_t size);
void kfree(void *ptr);
uint64_t vmm_map_mmio(uint64_t phys, size_t size);

/* From PCI */
struct pci_device_info;
const struct pci_device_info *pci_get_devices(uint32_t *count);

/* From PMM */
uint64_t pmm_alloc_pages(uint32_t count);
void pmm_free_pages(uint64_t phys, uint32_t count);

/* From HPET */
void hpet_sleep_ms(uint64_t ms);

/* Physmap access */
static volatile void *g_physmap_base = (void *)0xFFFF800000000000ULL;
#define PHYSMAP(phys) ((volatile void *)((uint64_t)g_physmap_base + (uint64_t)(phys)))

/* ================================================================== */
/* Driver state                                                        */
/* ================================================================== */

static volatile uint8_t *g_hda_mmio = NULL;   /* BAR0 virtual address */
static int g_hda_ready = 0;

/* CORB (Command Output Ring Buffer) */
#define CORB_ENTRIES 256
static volatile uint32_t *g_corb = NULL;       /* CORB virtual address */
static uint64_t g_corb_phys = 0;

/* RIRB (Response Input Ring Buffer) */
#define RIRB_ENTRIES 256
struct rirb_entry {
    uint32_t response;
    uint32_t resp_ex;   /* codec address in bits 3:0 + unsolicited flag */
};
static volatile struct rirb_entry *g_rirb = NULL;
static uint64_t g_rirb_phys = 0;
static uint16_t g_rirb_rp = 0;  /* our read pointer */

/* Codec info */
#define MAX_CODECS 4
static uint8_t g_codec_addr = 0;  /* first discovered codec address */
static int g_codec_found = 0;

/* Output path */
static uint8_t g_dac_nid = 0;     /* DAC widget NID */
static uint8_t g_pin_nid = 0;     /* output pin widget NID */

/* Stream */
static int g_stream_active = 0;
static uint8_t g_stream_tag = 1;  /* stream tag (1-15) */

/* DMA buffer */
#define HDA_DMA_PAGES    4         /* 16KB DMA buffer (4 pages) */
#define HDA_DMA_SIZE     (HDA_DMA_PAGES * 4096)
#define HDA_BDL_ENTRIES  2         /* double-buffer: 2 BDL entries */
static uint64_t g_dma_phys = 0;
static volatile uint8_t *g_dma_virt = NULL;
static uint64_t g_bdl_phys = 0;
static volatile struct hda_bdl_entry *g_bdl = NULL;
static uint32_t g_dma_write_pos = 0;  /* write position in DMA buffer */

/* Output stream descriptor base offset */
static uint32_t g_osd_base = 0;

/* ================================================================== */
/* MMIO access helpers                                                 */
/* ================================================================== */

static uint8_t hda_read8(uint32_t offset) {
    return *(volatile uint8_t *)(g_hda_mmio + offset);
}

static uint16_t hda_read16(uint32_t offset) {
    return *(volatile uint16_t *)(g_hda_mmio + offset);
}

static uint32_t hda_read32(uint32_t offset) {
    return *(volatile uint32_t *)(g_hda_mmio + offset);
}

static void hda_write8(uint32_t offset, uint8_t val) {
    *(volatile uint8_t *)(g_hda_mmio + offset) = val;
}

static void hda_write16(uint32_t offset, uint16_t val) {
    *(volatile uint16_t *)(g_hda_mmio + offset) = val;
}

static void hda_write32(uint32_t offset, uint32_t val) {
    *(volatile uint32_t *)(g_hda_mmio + offset) = val;
}

/* Stream descriptor register access (relative to stream base) */
static uint32_t hda_sd_read32(uint32_t reg) {
    return hda_read32(g_osd_base + reg);
}

static void hda_sd_write32(uint32_t reg, uint32_t val) {
    hda_write32(g_osd_base + reg, val);
}

static uint16_t hda_sd_read16(uint32_t reg) {
    return hda_read16(g_osd_base + reg);
}

static void hda_sd_write16(uint32_t reg, uint16_t val) {
    hda_write16(g_osd_base + reg, val);
}

static uint8_t hda_sd_read8(uint32_t reg) {
    return hda_read8(g_osd_base + reg);
}

static void hda_sd_write8(uint32_t reg, uint8_t val) {
    hda_write8(g_osd_base + reg, val);
}

/* ================================================================== */
/* CORB/RIRB command submission                                        */
/* ================================================================== */

static void hda_corb_send(uint32_t verb) {
    uint16_t wp = hda_read16(HDA_CORBWP) & 0xFF;
    wp = (wp + 1) % CORB_ENTRIES;
    g_corb[wp] = verb;
    hda_write16(HDA_CORBWP, wp);
}

static int hda_rirb_recv(uint32_t *response, uint32_t timeout_ms) {
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        uint16_t wp = hda_read16(HDA_RIRBWP) & 0xFF;
        if (g_rirb_rp != wp) {
            g_rirb_rp = (g_rirb_rp + 1) % RIRB_ENTRIES;
            *response = g_rirb[g_rirb_rp].response;
            return 0;
        }
        hpet_sleep_ms(1);
        elapsed++;
    }
    return -1; /* timeout */
}

/* Send verb and wait for response */
static int hda_send_verb(uint32_t verb, uint32_t *response) {
    hda_corb_send(verb);
    return hda_rirb_recv(response, 1000);
}

/* ================================================================== */
/* Controller initialization                                           */
/* ================================================================== */

static int hda_controller_reset(void) {
    /* Assert reset */
    hda_write32(HDA_GCTL, hda_read32(HDA_GCTL) & ~HDA_GCTL_CRST);
    hpet_sleep_ms(1);

    /* Wait for reset to take effect */
    {
        int timeout = 100;
        while ((hda_read32(HDA_GCTL) & HDA_GCTL_CRST) && timeout > 0) {
            hpet_sleep_ms(1);
            timeout--;
        }
    }

    /* Deassert reset */
    hda_write32(HDA_GCTL, hda_read32(HDA_GCTL) | HDA_GCTL_CRST);
    hpet_sleep_ms(1);

    /* Wait for controller to come out of reset */
    {
        int timeout = 100;
        while (!(hda_read32(HDA_GCTL) & HDA_GCTL_CRST) && timeout > 0) {
            hpet_sleep_ms(1);
            timeout--;
        }
        if (timeout <= 0) {
            kprint("HDA: controller reset timeout\n");
            return -1;
        }
    }

    /* Wait for codecs to enumerate */
    hpet_sleep_ms(10);

    return 0;
}

static int hda_setup_corb_rirb(void) {
    /* Allocate CORB: 256 entries × 4 bytes = 1KB, must be 128-byte aligned */
    g_corb_phys = pmm_alloc_pages(1);
    if (!g_corb_phys) { kprint("HDA: CORB alloc failed\n"); return -1; }
    g_corb = (volatile uint32_t *)PHYSMAP(g_corb_phys);

    /* Allocate RIRB: 256 entries × 8 bytes = 2KB */
    g_rirb_phys = pmm_alloc_pages(1);
    if (!g_rirb_phys) { kprint("HDA: RIRB alloc failed\n"); return -1; }
    g_rirb = (volatile struct rirb_entry *)PHYSMAP(g_rirb_phys);

    /* Zero buffers */
    {
        uint32_t i;
        for (i = 0; i < CORB_ENTRIES; i++) g_corb[i] = 0;
        for (i = 0; i < RIRB_ENTRIES; i++) {
            g_rirb[i].response = 0;
            g_rirb[i].resp_ex = 0;
        }
    }

    /* Stop CORB/RIRB DMA */
    hda_write8(HDA_CORBCTL, 0);
    hda_write8(HDA_RIRBCTL, 0);
    hpet_sleep_ms(1);

    /* Set CORB base address and size */
    hda_write32(HDA_CORBLBASE, (uint32_t)(g_corb_phys & 0xFFFFFFFF));
    hda_write32(HDA_CORBUBASE, (uint32_t)(g_corb_phys >> 32));
    hda_write8(HDA_CORBSIZE, 0x02); /* 256 entries */

    /* Reset CORB read pointer */
    hda_write16(HDA_CORBRP, 0x8000); /* set reset bit */
    hpet_sleep_ms(1);
    hda_write16(HDA_CORBRP, 0x0000); /* clear reset bit */
    hpet_sleep_ms(1);

    /* Set CORB write pointer to 0 */
    hda_write16(HDA_CORBWP, 0);

    /* Set RIRB base address and size */
    hda_write32(HDA_RIRBLBASE, (uint32_t)(g_rirb_phys & 0xFFFFFFFF));
    hda_write32(HDA_RIRBUBASE, (uint32_t)(g_rirb_phys >> 32));
    hda_write8(HDA_RIRBSIZE, 0x02); /* 256 entries */

    /* Reset RIRB write pointer */
    hda_write16(HDA_RIRBWP, 0x8000); /* set reset bit */
    hpet_sleep_ms(1);

    g_rirb_rp = 0;

    /* Start CORB and RIRB DMA */
    hda_write8(HDA_CORBCTL, HDA_CORBCTL_RUN);
    hda_write8(HDA_RIRBCTL, HDA_RIRBCTL_RUN);
    hpet_sleep_ms(1);

    return 0;
}

/* ================================================================== */
/* Codec discovery and output path setup                               */
/* ================================================================== */

static int hda_discover_codecs(void) {
    uint16_t statests = hda_read16(HDA_STATESTS);
    int addr;

    for (addr = 0; addr < MAX_CODECS; addr++) {
        if (statests & (1 << addr)) {
            uint32_t vendor;
            if (hda_send_verb(HDA_VERB(addr, 0, HDA_VERB_GET_PARAM | HDA_PARAM_VENDOR_ID),
                               &vendor) == 0) {
                kprint("HDA: codec %d vendor=%04x device=%04x\n",
                       addr, vendor >> 16, vendor & 0xFFFF);
                g_codec_addr = (uint8_t)addr;
                g_codec_found = 1;
                return 0;
            }
        }
    }

    kprint("HDA: no codecs found (STATESTS=0x%04x)\n", statests);
    return -1;
}

static int hda_find_output_path(void) {
    uint32_t resp;
    uint8_t codec = g_codec_addr;
    uint8_t start_nid, num_nodes;

    /* Get root node subordinate count */
    if (hda_send_verb(HDA_VERB(codec, 0, HDA_VERB_GET_PARAM | HDA_PARAM_SUBNODE_COUNT),
                       &resp) < 0) return -1;
    start_nid = (uint8_t)((resp >> 16) & 0xFF);
    num_nodes = (uint8_t)(resp & 0xFF);
    kprint("HDA: root subnodes: start=%d count=%d\n", start_nid, num_nodes);

    /* Find Audio Function Group */
    uint8_t afg_nid = 0;
    {
        int i;
        for (i = 0; i < num_nodes; i++) {
            uint8_t nid = start_nid + (uint8_t)i;
            if (hda_send_verb(HDA_VERB(codec, nid, HDA_VERB_GET_PARAM | HDA_PARAM_FN_GROUP_TYPE),
                               &resp) < 0) continue;
            if ((resp & 0xFF) == 0x01) { /* 0x01 = Audio Function Group */
                afg_nid = nid;
                kprint("HDA: Audio Function Group at NID %d\n", nid);
                break;
            }
        }
    }
    if (!afg_nid) { kprint("HDA: no AFG found\n"); return -1; }

    /* Power on AFG */
    hda_send_verb(HDA_VERB(codec, afg_nid, HDA_VERB_SET_POWER | 0x00), &resp);
    hpet_sleep_ms(10);

    /* Get AFG subordinate widgets */
    if (hda_send_verb(HDA_VERB(codec, afg_nid, HDA_VERB_GET_PARAM | HDA_PARAM_SUBNODE_COUNT),
                       &resp) < 0) return -1;
    start_nid = (uint8_t)((resp >> 16) & 0xFF);
    num_nodes = (uint8_t)(resp & 0xFF);
    kprint("HDA: AFG widgets: start=%d count=%d\n", start_nid, num_nodes);

    /* Walk widgets: find first Audio Output (DAC) and first output Pin */
    {
        int i;
        for (i = 0; i < num_nodes && i < 64; i++) {
            uint8_t nid = start_nid + (uint8_t)i;
            if (hda_send_verb(HDA_VERB(codec, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AUDIO_WIDGET),
                               &resp) < 0) continue;
            uint8_t wtype = (uint8_t)((resp >> 20) & 0xF);

            if (wtype == HDA_WIDGET_AO && g_dac_nid == 0) {
                g_dac_nid = nid;
                kprint("HDA: DAC found at NID %d\n", nid);
            }
            if (wtype == HDA_WIDGET_PIN && g_pin_nid == 0) {
                /* Check pin default config for output device */
                uint32_t pin_cfg;
                if (hda_send_verb(HDA_VERB(codec, nid, 0xF1C00), &pin_cfg) == 0) {
                    uint8_t dev_type = (uint8_t)((pin_cfg >> 20) & 0xF);
                    if (dev_type == HDA_PIN_DEV_SPEAKER ||
                        dev_type == HDA_PIN_DEV_HP_OUT ||
                        dev_type == HDA_PIN_DEV_LINE_OUT) {
                        g_pin_nid = nid;
                        kprint("HDA: output pin NID %d (type=%d)\n", nid, dev_type);
                    }
                }
            }
        }
    }

    if (!g_dac_nid) { kprint("HDA: no DAC found\n"); return -1; }
    if (!g_pin_nid) { kprint("HDA: no output pin found\n"); return -1; }

    /* Configure output path: DAC → Pin */

    /* Set DAC stream format: 48kHz, 16-bit, stereo */
    hda_send_verb(HDA_VERB(codec, g_dac_nid,
                            HDA_VERB_SET_STREAM_FMT | HDA_FMT_PCM_48K_16_STEREO), &resp);

    /* Set DAC stream/channel: stream tag 1, channel 0 */
    hda_send_verb(HDA_VERB(codec, g_dac_nid,
                            HDA_VERB_SET_CHAN_STREAM | ((uint32_t)g_stream_tag << 4) | 0), &resp);

    /* Unmute DAC output amp: set output amp, left+right, gain=max */
    hda_send_verb(HDA_VERB(codec, g_dac_nid,
                            HDA_VERB_SET_AMP_GAIN | 0xB000 | 0x7F), &resp);

    /* Power on DAC */
    hda_send_verb(HDA_VERB(codec, g_dac_nid, HDA_VERB_SET_POWER | 0x00), &resp);

    /* Enable pin output */
    hda_send_verb(HDA_VERB(codec, g_pin_nid,
                            HDA_VERB_SET_PIN_CTL | 0x40), &resp); /* OUT enable */

    /* Unmute pin */
    hda_send_verb(HDA_VERB(codec, g_pin_nid,
                            HDA_VERB_SET_AMP_GAIN | 0xB000 | 0x7F), &resp);

    /* Enable EAPD if supported */
    hda_send_verb(HDA_VERB(codec, g_pin_nid,
                            HDA_VERB_SET_EAPD | 0x02), &resp);

    /* Power on pin */
    hda_send_verb(HDA_VERB(codec, g_pin_nid, HDA_VERB_SET_POWER | 0x00), &resp);

    kprint("HDA: output path configured: DAC(NID%d) -> Pin(NID%d)\n",
           g_dac_nid, g_pin_nid);
    return 0;
}

/* ================================================================== */
/* Stream setup                                                        */
/* ================================================================== */

static int hda_setup_stream(void) {
    uint16_t gcap = hda_read16(HDA_GCAP);
    uint8_t iss = (uint8_t)((gcap >> 8) & 0xF);  /* input stream count */
    /* Output stream descriptors start after input streams */
    g_osd_base = 0x80 + (uint32_t)iss * 0x20;

    kprint("HDA: output stream descriptor at offset 0x%x\n", g_osd_base);

    /* Stop stream if running */
    hda_sd_write8(HDA_SD_CTL, 0);
    hpet_sleep_ms(1);

    /* Clear status bits */
    hda_sd_write8(HDA_SD_STS, 0x1C); /* write 1 to clear */

    /* Allocate DMA buffer */
    g_dma_phys = pmm_alloc_pages(HDA_DMA_PAGES);
    if (!g_dma_phys) { kprint("HDA: DMA buffer alloc failed\n"); return -1; }
    g_dma_virt = (volatile uint8_t *)PHYSMAP(g_dma_phys);

    /* Zero DMA buffer (silence) */
    {
        uint32_t i;
        for (i = 0; i < HDA_DMA_SIZE; i++) g_dma_virt[i] = 0;
    }

    /* Allocate BDL (Buffer Descriptor List) — needs 128-byte alignment */
    g_bdl_phys = pmm_alloc_pages(1);
    if (!g_bdl_phys) { kprint("HDA: BDL alloc failed\n"); return -1; }
    g_bdl = (volatile struct hda_bdl_entry *)PHYSMAP(g_bdl_phys);

    /* Set up 2 BDL entries (double buffer) */
    g_bdl[0].addr = g_dma_phys;
    g_bdl[0].length = HDA_DMA_SIZE / 2;
    g_bdl[0].ioc = 1;  /* interrupt on completion */

    g_bdl[1].addr = g_dma_phys + HDA_DMA_SIZE / 2;
    g_bdl[1].length = HDA_DMA_SIZE / 2;
    g_bdl[1].ioc = 1;

    /* Set BDL address */
    hda_sd_write32(HDA_SD_BDLPL, (uint32_t)(g_bdl_phys & 0xFFFFFFFF));
    hda_sd_write32(HDA_SD_BDLPU, (uint32_t)(g_bdl_phys >> 32));

    /* Set cyclic buffer length */
    hda_sd_write32(HDA_SD_CBL, HDA_DMA_SIZE);

    /* Set last valid BDL index */
    hda_sd_write16(HDA_SD_LVI, HDA_BDL_ENTRIES - 1);

    /* Set stream format */
    hda_sd_write16(HDA_SD_FORMAT, HDA_FMT_PCM_48K_16_STEREO);

    /* Set stream tag in CTL (bits 23:20) */
    {
        uint32_t ctl = hda_sd_read32(HDA_SD_CTL);
        ctl = (ctl & 0xFF0FFFFF) | ((uint32_t)g_stream_tag << 20);
        hda_sd_write32(HDA_SD_CTL, ctl);
    }

    g_dma_write_pos = 0;
    return 0;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

void hda_init(void) {
    uint32_t pci_count;
    const struct pci_device_info *devs = pci_get_devices(&pci_count);
    uint32_t bar0 = 0;
    uint64_t bar0_64 = 0;
    uint32_t i;

    g_hda_ready = 0;

    /* Find HDA controller: class 0x04, subclass 0x03 (Audio) */
    for (i = 0; i < pci_count; i++) {
        if (devs[i].class_code == 0x04 && devs[i].subclass == 0x03) {
            bar0 = devs[i].bar0;
            bar0_64 = (uint64_t)bar0 & ~0xFULL;
            /* If BAR0 is 64-bit, combine with BAR1 */
            if ((bar0 & 0x4) != 0) {
                bar0_64 |= ((uint64_t)devs[i].bar1 << 32);
            }
            kprint("HDA: found controller at PCI %d:%d.%d, BAR0=0x%llx\n",
                   devs[i].bus, devs[i].slot, devs[i].func, bar0_64);
            break;
        }
    }

    if (!bar0_64) {
        kprint("HDA: no audio controller found\n");
        return;
    }

    /* Map MMIO */
    g_hda_mmio = (volatile uint8_t *)PHYSMAP(bar0_64);
    kprint("HDA: MMIO mapped, version %d.%d\n",
           hda_read8(HDA_VMAJ), hda_read8(HDA_VMIN));

    /* Reset controller */
    if (hda_controller_reset() < 0) return;

    /* Setup CORB/RIRB */
    if (hda_setup_corb_rirb() < 0) return;

    /* Discover codecs */
    if (hda_discover_codecs() < 0) return;

    /* Find and configure output path */
    if (hda_find_output_path() < 0) return;

    /* Setup output stream */
    if (hda_setup_stream() < 0) return;

    g_hda_ready = 1;
    kprint("HDA: audio driver ready (48kHz 16-bit stereo)\n");
}

int hda_is_ready(void) {
    return g_hda_ready;
}

int hda_open_output(uint32_t sample_rate, uint8_t channels, uint8_t bits) {
    if (!g_hda_ready) return -1;
    if (g_stream_active) return -1;

    /* Currently only support 48kHz/16-bit/stereo */
    (void)sample_rate; (void)channels; (void)bits;

    /* Start stream */
    {
        uint32_t ctl = hda_sd_read32(HDA_SD_CTL);
        ctl |= HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE;
        hda_sd_write32(HDA_SD_CTL, ctl);
    }

    g_stream_active = 1;
    g_dma_write_pos = 0;
    return 0;
}

int hda_write_pcm(const void *pcm_data, uint32_t byte_count) {
    if (!g_hda_ready || !g_stream_active) return -1;

    const uint8_t *src = (const uint8_t *)pcm_data;
    uint32_t written = 0;

    while (written < byte_count) {
        /* Get current DMA read position (hardware is reading from here) */
        uint32_t hw_pos = hda_sd_read32(HDA_SD_LPIB);

        /* Calculate available space */
        uint32_t avail;
        if (g_dma_write_pos >= hw_pos)
            avail = HDA_DMA_SIZE - (g_dma_write_pos - hw_pos) - 1;
        else
            avail = hw_pos - g_dma_write_pos - 1;

        if (avail == 0) {
            /* Buffer full — wait a bit */
            hpet_sleep_ms(1);
            continue;
        }

        uint32_t chunk = byte_count - written;
        if (chunk > avail) chunk = avail;

        /* Write to DMA buffer (handle wrap-around) */
        uint32_t to_end = HDA_DMA_SIZE - g_dma_write_pos;
        if (chunk <= to_end) {
            uint32_t j;
            for (j = 0; j < chunk; j++)
                g_dma_virt[g_dma_write_pos + j] = src[written + j];
        } else {
            uint32_t j;
            for (j = 0; j < to_end; j++)
                g_dma_virt[g_dma_write_pos + j] = src[written + j];
            for (j = 0; j < chunk - to_end; j++)
                g_dma_virt[j] = src[written + to_end + j];
        }

        g_dma_write_pos = (g_dma_write_pos + chunk) % HDA_DMA_SIZE;
        written += chunk;
    }

    return (int)written;
}

void hda_close_output(void) {
    if (!g_hda_ready) return;

    /* Stop stream */
    {
        uint32_t ctl = hda_sd_read32(HDA_SD_CTL);
        ctl &= ~(HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE);
        hda_sd_write32(HDA_SD_CTL, ctl);
    }

    g_stream_active = 0;
}

int hda_get_stream_info(struct hda_stream_info *info) {
    if (!info) return -1;
    info->sample_rate = 48000;
    info->channels = 2;
    info->bits = 16;
    info->active = (uint8_t)g_stream_active;
    return 0;
}

uint32_t hda_get_position(void) {
    if (!g_hda_ready) return 0;
    return hda_sd_read32(HDA_SD_LPIB);
}
