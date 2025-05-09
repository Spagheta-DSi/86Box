/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Driver for IBM PS/2 ESDI disk controller (MCA)
 *
 *            AdapterID:       0xDDFF
 *            AdapterName:     "ESDI Fixed Disk Controller"
 *            NumBytes         2
 *            I/O base:        0x3510-0x3517
 *            IRQ:             14
 *
 *            Primary Board    pos[0]=XXxx xx0X    0x3510
 *            Secondary Board  pos[0]=XXxx xx1X    0x3518
 *
 *            DMA 5            pos[0]=XX01 01XX
 *            DMA 6            pos[0]=XX01 10XX
 *            DMA 7            pos[0]=XX01 11XX
 *            DMA 0            pos[0]=XX00 00XX
 *            DMA 1            pos[0]=XX00 01XX
 *            DMA 3            pos[0]=XX00 11XX
 *            DMA 4            pos[0]=XX01 00XX
 *
 *            MCA Fairness ON  pos[0]=X1XX XXXX
 *            MCA Fairness OFF pos[0]=X0XX XXXX
 *
 *            ROM C000         pos[1]=XXXX 0000
 *            ROM C400         pos[1]=XXXX 0001
 *            ROM C800         pos[1]=XXXX 0010
 *            ROM CC00         pos[1]=XXXX 0011
 *            ROM D000         pos[1]=XXXX 0100
 *            ROM D400         pos[1]=XXXX 0101
 *            ROM D800         pos[1]=XXXX 0110
 *            ROM DC00         pos[1]=XXXX 0111
 *            ROM Disabled     pos[1]=XXXX 1XXX
 *
 *            DMA Burst 8      pos[1]=XX01 XXXX
 *            DMA Burst 16     pos[1]=XX10 XXXX
 *            DMA Burst 24     pos[1]=XX11 XXXX
 *            DMA Disabled     pos[1]=XX00 XXXX
 *
 *          Although this is an MCA device, meaning that the system
 *          software will take care of device configuration, the ESDI
 *          controller is a somewhat weird one.. it's I/O base address
 *          and IRQ channel are locked to 0x3510 and IRQ14, possibly
 *          to enforce compatibility with the IBM MFM disk controller
 *          that was also in use on these systems. All other settings,
 *          however, are auto-configured by the system software as
 *          shown above.
 *
 *
 *
 * Authors: Sarah Walker, <https://pcem-emulator.co.uk/>
 *          Fred N. van Kempen, <decwiz@yahoo.com>
 *
 *          Copyright 2008-2018 Sarah Walker.
 *          Copyright 2017-2018 Fred N. van Kempen.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <inttypes.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/dma.h>
#include <86box/io.h>
#include <86box/mca.h>
#include <86box/mem.h>
#include <86box/pic.h>
#include <86box/rom.h>
#include <86box/timer.h>
#include <86box/ui.h>
#include <86box/hdc.h>
#include <86box/hdd.h>
#include <86box/plat_unused.h>

/* These are hardwired. */
#define ESDI_IOADDR_PRI 0x3510
#define ESDI_IOADDR_SEC 0x3518
#define ESDI_IRQCHAN    14

#define BIOS_FILE_L     "roms/hdd/esdi/90x8969.bin"
#define BIOS_FILE_H     "roms/hdd/esdi/90x8970.bin"

#define ESDI_TIME       500.0
#define CMD_ADAPTER     0

typedef struct esdi_drive_t {
    int spt;
    int hpc;
    int tracks;
    int sectors;
    int present;
    int hdd_num;
} drive_t;

typedef struct esdi_t {
    int8_t dma;

    uint32_t bios;
    rom_t    bios_rom;

    uint8_t  basic_ctrl;
    uint8_t  status;
    uint8_t  irq_status;
    int      irq_ena_disable;
    int      irq_in_progress;
    int      cmd_req_in_progress;
    int      cmd_pos;
    uint16_t cmd_data[4];
    int      cmd_dev;

    int status_pos;
    int status_len;

    uint16_t status_data[256];

    int      data_pos;
    uint16_t data[256];

    uint16_t sector_buffer[256][256];

    int sector_pos;
    int sector_count;

    int command;
    int cmd_state;

    int        in_reset;
    pc_timer_t timer;

    uint32_t rba;

    struct cmds {
        int req_in_progress;
    } cmds[3];

    drive_t drives[2];

    uint8_t pos_regs[8];
} esdi_t;

enum {
    ESDI_IS_ADAPTER,
    ESDI_IS_INTEGRATED
};

#define STATUS_DMA_ENA             (1 << 7)
#define STATUS_IRQ_PENDING         (1 << 6)
#define STATUS_CMD_IN_PROGRESS     (1 << 5)
#define STATUS_BUSY                (1 << 4)
#define STATUS_STATUS_OUT_FULL     (1 << 3)
#define STATUS_CMD_IR_FULL         (1 << 2)
#define STATUS_TRANSFER_REQ        (1 << 1)
#define STATUS_IRQ                 (1 << 0)

#define CTRL_RESET                 (1 << 7)
#define CTRL_DMA_ENA               (1 << 1)
#define CTRL_IRQ_ENA               (1 << 0)

#define IRQ_HOST_ADAPTER           (7 << 5)
#define IRQ_DEVICE_0               (0 << 5)
#define IRQ_CMD_COMPLETE_SUCCESS   0x1
#define IRQ_RESET_COMPLETE         0xa
#define IRQ_DATA_TRANSFER_READY    0xb
#define IRQ_CMD_COMPLETE_FAILURE   0xc

#define ATTN_DEVICE_SEL            (7 << 5)
#define ATTN_HOST_ADAPTER          (7 << 5)
#define ATTN_DEVICE_0              (0 << 5)
#define ATTN_DEVICE_1              (1 << 5)
#define ATTN_REQ_MASK              0x0f
#define ATTN_CMD_REQ               1
#define ATTN_EOI                   2
#define ATTN_RESET                 4

#define CMD_SIZE_4                 (1 << 14)

#define CMD_DEVICE_SEL             (7 << 5)
#define CMD_MASK                   0x1f
#define CMD_READ                   0x01
#define CMD_WRITE                  0x02
#define CMD_READ_VERIFY            0x03
#define CMD_WRITE_VERIFY           0x04
#define CMD_SEEK                   0x05
#define CMD_PARK_HEADS             0x06
#define CMD_GET_DEV_STATUS         0x08
#define CMD_GET_DEV_CONFIG         0x09
#define CMD_GET_POS_INFO           0x0a
#define CMD_FORMAT_UNIT            0x16
#define CMD_FORMAT_PREPARE         0x17

#define STATUS_LEN(x)              ((x) << 8)
#define STATUS_DEVICE(x)           ((x) << 5)
#define STATUS_DEVICE_HOST_ADAPTER (7 << 5)

#ifdef ENABLE_ESDI_MCA_LOG
int esdi_mca_do_log = ENABLE_ESDI_MCA_LOG;

static void
esdi_mca_log(const char *fmt, ...)
{
    va_list ap;

    if (esdi_mca_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define esdi_mca_log(fmt, ...)
#endif

static __inline void
set_irq(esdi_t *dev)
{
    dev->irq_ena_disable = 1;
    esdi_mca_log("Set IRQ 14: bit=%x, cmd=%02x.\n", dev->basic_ctrl & CTRL_IRQ_ENA, dev->command);
    if (dev->basic_ctrl & CTRL_IRQ_ENA)
        picint_common(1 << ESDI_IRQCHAN, PIC_IRQ_EDGE, 1, NULL);
}

static __inline void
clear_irq(esdi_t *dev)
{
    dev->irq_ena_disable = 0;
    esdi_mca_log("Clear IRQ 14: bit=%x, cmd=%02x.\n", dev->basic_ctrl & CTRL_IRQ_ENA, dev->command);
    if (dev->basic_ctrl & CTRL_IRQ_ENA)
        picint_common(1 << ESDI_IRQCHAN, PIC_IRQ_EDGE, 0, NULL);
}

static __inline void
update_irq(esdi_t *dev)
{
    uint8_t set = (dev->basic_ctrl & CTRL_IRQ_ENA) && dev->irq_ena_disable;
    picint_common(1 << ESDI_IRQCHAN, PIC_IRQ_EDGE, set, NULL);
}

static void
esdi_mca_set_callback(esdi_t *dev, double callback)
{
    if (!dev) {
        return;
    }

    if (callback == 0.0) {
        esdi_mca_log("Callback Stopped.\n");
        timer_stop(&dev->timer);
    } else {
        timer_on_auto(&dev->timer, callback);
    }
}

static double
esdi_mca_get_xfer_time(UNUSED(esdi_t *esdi), int size)
{
    /* 390.625 us per sector at 10 Mbit/s = 1280 kB/s. */
    return (3125.0 / 8.0) * (double) size;
}

static void
cmd_unsupported(esdi_t *dev)
{
    dev->status_len     = 9;
    dev->status_data[0] = dev->command | STATUS_LEN(9) | dev->cmd_dev;
    dev->status_data[1] = 0x0f03; /*Attention error, command not supported*/
    dev->status_data[2] = 0x0002; /*Interface fault*/
    dev->status_data[3] = 0;
    dev->status_data[4] = 0;
    dev->status_data[5] = 0;
    dev->status_data[6] = 0;
    dev->status_data[7] = 0;
    dev->status_data[8] = 0;

    dev->status          = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
    dev->irq_status      = dev->cmd_dev | IRQ_CMD_COMPLETE_FAILURE;
    dev->irq_in_progress = 1;
    set_irq(dev);
    ui_sb_update_icon(SB_HDD | HDD_BUS_ESDI, 0);
    ui_sb_update_icon_write(SB_HDD | HDD_BUS_ESDI, 0);
}

static void
device_not_present(esdi_t *dev)
{
    dev->status_len     = 9;
    dev->status_data[0] = dev->command | STATUS_LEN(9) | dev->cmd_dev;
    dev->status_data[1] = 0x0c11; /*Command failed, internal hardware error*/
    dev->status_data[2] = 0x000b; /*Selection error*/
    dev->status_data[3] = 0;
    dev->status_data[4] = 0;
    dev->status_data[5] = 0;
    dev->status_data[6] = 0;
    dev->status_data[7] = 0;
    dev->status_data[8] = 0;

    dev->status          = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
    dev->irq_status      = dev->cmd_dev | IRQ_CMD_COMPLETE_FAILURE;
    dev->irq_in_progress = 1;
    set_irq(dev);
    ui_sb_update_icon(SB_HDD | HDD_BUS_ESDI, 0);
    ui_sb_update_icon_write(SB_HDD | HDD_BUS_ESDI, 0);
}

static void
rba_out_of_range(esdi_t *dev)
{
    dev->status_len     = 9;
    dev->status_data[0] = dev->command | STATUS_LEN(9) | dev->cmd_dev;
    dev->status_data[1] = 0x0e01; /*Command block error, invalid parameter*/
    dev->status_data[2] = 0x0007; /*RBA out of range*/
    dev->status_data[3] = 0;
    dev->status_data[4] = 0;
    dev->status_data[5] = 0;
    dev->status_data[6] = 0;
    dev->status_data[7] = 0;
    dev->status_data[8] = 0;

    dev->status          = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
    dev->irq_status      = dev->cmd_dev | IRQ_CMD_COMPLETE_FAILURE;
    dev->irq_in_progress = 1;
    set_irq(dev);
    ui_sb_update_icon(SB_HDD | HDD_BUS_ESDI, 0);
    ui_sb_update_icon_write(SB_HDD | HDD_BUS_ESDI, 0);
}

static void
defective_block(esdi_t *dev)
{
    dev->status_len     = 9;
    dev->status_data[0] = dev->command | STATUS_LEN(9) | dev->cmd_dev;
    dev->status_data[1] = 0x0e01; /*Command block error, invalid parameter*/
    dev->status_data[2] = 0x0009; /*Defective block*/
    dev->status_data[3] = 0;
    dev->status_data[4] = 0;
    dev->status_data[5] = 0;
    dev->status_data[6] = 0;
    dev->status_data[7] = 0;
    dev->status_data[8] = 0;

    dev->status          = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
    dev->irq_status      = dev->cmd_dev | IRQ_CMD_COMPLETE_FAILURE;
    dev->irq_in_progress = 1;
    set_irq(dev);
    ui_sb_update_icon(SB_HDD | HDD_BUS_ESDI, 0);
    ui_sb_update_icon_write(SB_HDD | HDD_BUS_ESDI, 0);
}

static void
complete_command_status(esdi_t *dev)
{
    dev->status_len = 7;
    if (dev->cmd_dev == ATTN_DEVICE_0)
        dev->status_data[0] = dev->command | STATUS_LEN(7) | STATUS_DEVICE(0);
    else
        dev->status_data[0] = dev->command | STATUS_LEN(7) | STATUS_DEVICE(1);
    dev->status_data[1] = 0x0000;                  /*Error bits*/
    dev->status_data[2] = 0x1900;                  /*Device status*/
    dev->status_data[3] = 0;                       /*Number of blocks left to do*/
    dev->status_data[4] = (dev->rba - 1) & 0xffff; /*Last RBA processed*/
    dev->status_data[5] = (dev->rba - 1) >> 8;
    dev->status_data[6] = 0; /*Number of blocks requiring error recovery*/
    ui_sb_update_icon(SB_HDD | HDD_BUS_ESDI, 0);
    ui_sb_update_icon_write(SB_HDD | HDD_BUS_ESDI, 0);
}

#define ESDI_ADAPTER_ONLY()                      \
        if (dev->cmd_dev != ATTN_HOST_ADAPTER) { \
            cmd_unsupported(dev);                \
            return;                              \
        }

#define ESDI_DRIVE_ONLY()                                                     \
        if (dev->cmd_dev != ATTN_DEVICE_0 && dev->cmd_dev != ATTN_DEVICE_1) { \
            cmd_unsupported(dev);                                             \
            return;                                                           \
        }                                                                     \
        if (dev->cmd_dev == ATTN_DEVICE_0)                                    \
            drive = &dev->drives[0];                                          \
        else                                                                  \
            drive = &dev->drives[1];

static void
esdi_callback(void *priv)
{
    esdi_t        *dev = (esdi_t *) priv;
    const drive_t *drive;
    int            val;
    double         cmd_time = 0.0;

    /* If we are returning from a RESET, handle this first. */
    if (dev->in_reset) {
        esdi_mca_log("ESDI reset.\n");
        dev->in_reset   = 0;
        dev->status     = STATUS_IRQ | STATUS_TRANSFER_REQ | STATUS_STATUS_OUT_FULL;
        dev->status_len = 1; /*ToDo: better implementation for Xenix?*/
        dev->status_data[0] = STATUS_LEN(1) | ATTN_HOST_ADAPTER;
        dev->irq_status = IRQ_HOST_ADAPTER | IRQ_RESET_COMPLETE;
        return;
    }

    esdi_mca_log("Command=%02x.\n", dev->command);
    switch (dev->command) {
        case CMD_READ:
        case 0x15:
            ESDI_DRIVE_ONLY();

            if (!drive->present) {
                device_not_present(dev);
                return;
            }

            switch (dev->cmd_state) {
                case 0:
                    if (dev->command == CMD_READ)
                        dev->rba = (dev->cmd_data[2] | (dev->cmd_data[3] << 16)) & 0x0fffffff;

                    dev->sector_pos   = 0;
                    dev->sector_count = dev->cmd_data[1];

                    if ((dev->rba + dev->sector_count) > hdd_image_get_last_sector(drive->hdd_num)) {
                        rba_out_of_range(dev);
                        return;
                    }

                    dev->status          = STATUS_IRQ | STATUS_CMD_IN_PROGRESS | STATUS_TRANSFER_REQ;
                    dev->irq_status      = dev->cmd_dev | IRQ_DATA_TRANSFER_READY;
                    dev->irq_in_progress = 1;
                    set_irq(dev);

                    dev->cmd_state = 1;
                    esdi_mca_set_callback(dev, ESDI_TIME);
                    dev->data_pos = 0;
                    break;

                case 1:
                    if (!(dev->basic_ctrl & CTRL_DMA_ENA)) {
                        esdi_mca_set_callback(dev, ESDI_TIME);
                        return;
                    }

                    while (dev->sector_pos < dev->sector_count) {
                        if (!dev->data_pos) {
                            if (dev->rba >= drive->sectors)
                                fatal("Read past end of drive\n");
                            if (hdd_image_read(drive->hdd_num, dev->rba, 1, (uint8_t *) dev->data) < 0) {
                                defective_block(dev);
                                return;
                            }
                            cmd_time += hdd_timing_read(&hdd[drive->hdd_num], dev->rba, 1);
                            cmd_time += esdi_mca_get_xfer_time(dev, 1);
                        }

                        while (dev->data_pos < 256) {
                            val = dma_channel_write(dev->dma, dev->data[dev->data_pos]);

                            if (val == DMA_NODATA) {
                                esdi_mca_set_callback(dev, ESDI_TIME + cmd_time);
                                return;
                            }

                            dev->data_pos++;
                        }

                        dev->data_pos = 0;
                        dev->sector_pos++;
                        dev->rba++;
                    }

                    dev->status    = STATUS_CMD_IN_PROGRESS;
                    dev->cmd_state = 2;
                    esdi_mca_set_callback(dev, cmd_time);
                    break;

                case 2:
                    complete_command_status(dev);
                    dev->status          = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
                    dev->irq_status      = dev->cmd_dev | IRQ_CMD_COMPLETE_SUCCESS;
                    dev->irq_in_progress = 1;
                    set_irq(dev);
                    break;

                default:
                    break;
            }
            break;

        case CMD_WRITE:
        case CMD_WRITE_VERIFY:
            ESDI_DRIVE_ONLY();
            if (!drive->present) {
                device_not_present(dev);
                return;
            }

            switch (dev->cmd_state) {
                case 0:
                    dev->rba = (dev->cmd_data[2] | (dev->cmd_data[3] << 16)) & 0x0fffffff;

                    dev->sector_pos   = 0;
                    dev->sector_count = dev->cmd_data[1];

                    if ((dev->rba + dev->sector_count) > hdd_image_get_last_sector(drive->hdd_num)) {
                        rba_out_of_range(dev);
                        return;
                    }

                    dev->status          = STATUS_IRQ | STATUS_CMD_IN_PROGRESS | STATUS_TRANSFER_REQ;
                    dev->irq_status      = dev->cmd_dev | IRQ_DATA_TRANSFER_READY;
                    dev->irq_in_progress = 1;
                    set_irq(dev);

                    dev->cmd_state = 1;
                    esdi_mca_set_callback(dev, ESDI_TIME);
                    dev->data_pos = 0;
                    break;

                case 1:
                    if (!(dev->basic_ctrl & CTRL_DMA_ENA)) {
                        esdi_mca_set_callback(dev, ESDI_TIME);
                        return;
                    }

                    while (dev->sector_pos < dev->sector_count) {
                        while (dev->data_pos < 256) {
                            val = dma_channel_read(dev->dma);

                            if (val == DMA_NODATA) {
                                esdi_mca_set_callback(dev, ESDI_TIME + cmd_time);
                                return;
                            }

                            dev->data[dev->data_pos++] = val & 0xffff;
                        }

                        if (dev->rba >= drive->sectors)
                            fatal("Write past end of drive\n");
                        if (hdd_image_write(drive->hdd_num, dev->rba, 1, (uint8_t *) dev->data) < 0) {
                            defective_block(dev);
                            return;
                        }
                        cmd_time += hdd_timing_write(&hdd[drive->hdd_num], dev->rba, 1);
                        cmd_time += esdi_mca_get_xfer_time(dev, 1);
                        dev->rba++;
                        dev->sector_pos++;
                        dev->data_pos = 0;
                    }

                    dev->status    = STATUS_CMD_IN_PROGRESS;
                    dev->cmd_state = 2;
                    esdi_mca_set_callback(dev, cmd_time);
                    break;

                case 2:
                    complete_command_status(dev);
                    dev->status          = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
                    dev->irq_status      = dev->cmd_dev | IRQ_CMD_COMPLETE_SUCCESS;
                    dev->irq_in_progress = 1;
                    set_irq(dev);
                    break;

                default:
                    break;
            }
            break;

        case CMD_READ_VERIFY:
            ESDI_DRIVE_ONLY();

            if (!drive->present) {
                device_not_present(dev);
                return;
            }

            switch (dev->cmd_state) {
                case 0:
                    dev->rba          = (dev->cmd_data[2] | (dev->cmd_data[3] << 16)) & 0x0fffffff;
                    dev->sector_count = dev->cmd_data[1];

                    if ((dev->rba + dev->sector_count) > hdd_image_get_last_sector(drive->hdd_num)) {
                        rba_out_of_range(dev);
                        return;
                    }

                    cmd_time = hdd_timing_read(&hdd[drive->hdd_num], dev->rba, dev->sector_count);
                    esdi_mca_set_callback(dev, ESDI_TIME + cmd_time);
                    dev->cmd_state = 1;
                    break;

                case 1:
                    complete_command_status(dev);
                    dev->status          = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
                    dev->irq_status      = dev->cmd_dev | IRQ_CMD_COMPLETE_SUCCESS;
                    dev->irq_in_progress = 1;
                    set_irq(dev);
                    break;

                default:
                    break;
            }
            break;

        case CMD_SEEK:
            ESDI_DRIVE_ONLY();

            if (!drive->present) {
                device_not_present(dev);
                return;
            }

            if ((dev->rba + dev->sector_count) > hdd_image_get_last_sector(drive->hdd_num)) {
                rba_out_of_range(dev);
                return;
            }

            switch (dev->cmd_state) {
                case 0:
                    dev->rba = (dev->cmd_data[2] | (dev->cmd_data[3] << 16)) & 0x0fffffff;
                    cmd_time = hdd_seek_get_time(&hdd[drive->hdd_num], dev->rba, HDD_OP_SEEK, 0, 0.0);
                    esdi_mca_set_callback(dev, ESDI_TIME + cmd_time);
                    dev->cmd_state = 1;
                    break;

                case 1:
                    complete_command_status(dev);
                    dev->status          = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
                    dev->irq_status      = dev->cmd_dev | IRQ_CMD_COMPLETE_SUCCESS;
                    dev->irq_in_progress = 1;
                    set_irq(dev);
                    break;

                default:
                    break;
            }
            break;

        case CMD_PARK_HEADS:
            ESDI_DRIVE_ONLY();

            if (!drive->present) {
                device_not_present(dev);
                return;
            }

            switch (dev->cmd_state) {
                case 0:
                    dev->rba = 0x00000000;
                    cmd_time = hdd_seek_get_time(&hdd[drive->hdd_num], dev->rba, HDD_OP_SEEK, 0, 0.0);
                    esdi_mca_set_callback(dev, ESDI_TIME + cmd_time);
                    dev->cmd_state = 1;
                    break;

                case 1:
                    complete_command_status(dev);
                    dev->status          = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
                    dev->irq_status      = dev->cmd_dev | IRQ_CMD_COMPLETE_SUCCESS;
                    dev->irq_in_progress = 1;
                    set_irq(dev);
                    break;

                default:
                    break;
            }
            break;

        case CMD_GET_DEV_STATUS:
            ESDI_DRIVE_ONLY();

            if (!drive->present) {
                device_not_present(dev);
                return;
            }

            if ((dev->status & STATUS_IRQ) || dev->irq_in_progress)
                fatal("IRQ in progress %02x %i\n", dev->status, dev->irq_in_progress);

            dev->status_len     = 9;
            dev->status_data[0] = CMD_GET_DEV_STATUS | STATUS_LEN(9) | STATUS_DEVICE_HOST_ADAPTER;
            dev->status_data[1] = 0x0000; /*Error bits*/
            dev->status_data[2] = 0x1900; /*Device status*/
            dev->status_data[3] = 0;      /*ESDI Standard Status*/
            dev->status_data[4] = 0;      /*ESDI Vendor Unique Status*/
            dev->status_data[5] = 0;
            dev->status_data[6] = 0;
            dev->status_data[7] = 0;
            dev->status_data[8] = 0;

            dev->status          = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
            dev->irq_status      = dev->cmd_dev | IRQ_CMD_COMPLETE_SUCCESS;
            dev->irq_in_progress = 1;
            set_irq(dev);
            ui_sb_update_icon(SB_HDD | HDD_BUS_ESDI, 0);
            ui_sb_update_icon_write(SB_HDD | HDD_BUS_ESDI, 0);
            break;

        case CMD_GET_DEV_CONFIG:
            if (dev->cmd_dev == ATTN_HOST_ADAPTER)
            {
                if ((dev->status & STATUS_IRQ) || dev->irq_in_progress)
                    fatal("IRQ in progress %02x %i\n", dev->status, dev->irq_in_progress);
                /* INT 13, AX=1C0B - ESDI FIXED DISK - GET ADAPTER CONFIGURATION */
                /* The PS/55 will test sector buffer after this request is done. */
                dev->status_len = 6;
                dev->status_data[0] = CMD_GET_DEV_CONFIG | STATUS_LEN(6) | STATUS_DEVICE_HOST_ADAPTER;
                dev->status_data[1] = 0;
                dev->status_data[2] = 0;
                /* bit 15-12: chip revision = 0011b, bit 11-8: sector buffer size = n * 256 bytes (n must be < 6) */
                dev->status_data[3] = 0x3200;
                dev->status_data[4] = 0;
                dev->status_data[5] = 0;
            }
            else
            {
                ESDI_DRIVE_ONLY();
                if (!drive->present) {
                    device_not_present(dev);
                    return;
                }

                if ((dev->status & STATUS_IRQ) || dev->irq_in_progress)
                    fatal("IRQ in progress %02x %i\n", dev->status, dev->irq_in_progress);

                dev->status_len = 6;
                dev->status_data[0] = CMD_GET_DEV_CONFIG | STATUS_LEN(6) | STATUS_DEVICE_HOST_ADAPTER;
                dev->status_data[1] = 0x10; /*Zero defect*/
                dev->status_data[2] = drive->sectors & 0xffff;
                dev->status_data[3] = drive->sectors >> 16;
                dev->status_data[4] = drive->tracks;
                dev->status_data[5] = drive->hpc | (drive->spt << 16);
            }
            esdi_mca_log("CMD_GET_DEV_CONFIG %i  %04x %04x %04x %04x %04x %04x\n",
                drive->sectors,
                dev->status_data[0], dev->status_data[1],
                dev->status_data[2], dev->status_data[3],
                dev->status_data[4], dev->status_data[5]);

            dev->status = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
            dev->irq_status = dev->cmd_dev | IRQ_CMD_COMPLETE_SUCCESS;
            dev->irq_in_progress = 1;
            set_irq(dev);
            ui_sb_update_icon(SB_HDD | HDD_BUS_ESDI, 0);
            ui_sb_update_icon_write(SB_HDD | HDD_BUS_ESDI, 0);
            break;

        case CMD_GET_POS_INFO:
            ESDI_ADAPTER_ONLY();

            if ((dev->status & STATUS_IRQ) || dev->irq_in_progress)
                fatal("IRQ in progress %02x %i\n", dev->status, dev->irq_in_progress);

            dev->status_len     = 5;
            dev->status_data[0] = CMD_GET_POS_INFO | STATUS_LEN(5) | STATUS_DEVICE_HOST_ADAPTER;
            dev->status_data[1] = dev->pos_regs[1] | (dev->pos_regs[0] << 8); /*MCA ID*/
            dev->status_data[2] = dev->pos_regs[3] | (dev->pos_regs[2] << 8);
            dev->status_data[3] = 0xff;
            dev->status_data[4] = 0xff;

            dev->status          = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
            dev->irq_status      = IRQ_HOST_ADAPTER | IRQ_CMD_COMPLETE_SUCCESS;
            dev->irq_in_progress = 1;
            set_irq(dev);
            ui_sb_update_icon(SB_HDD | HDD_BUS_ESDI, 0);
            ui_sb_update_icon_write(SB_HDD | HDD_BUS_ESDI, 0);
            break;

        case 0x10:
            ESDI_ADAPTER_ONLY();
            switch (dev->cmd_state) {
                case 0:
                    dev->sector_pos   = 0;
                    dev->sector_count = dev->cmd_data[1];
                    if (dev->sector_count > 256)
                        fatal("Write sector buffer count %04x\n", dev->cmd_data[1]);

                    dev->status          = STATUS_IRQ | STATUS_CMD_IN_PROGRESS | STATUS_TRANSFER_REQ;
                    dev->irq_status      = IRQ_HOST_ADAPTER | IRQ_DATA_TRANSFER_READY;
                    dev->irq_in_progress = 1;
                    set_irq(dev);

                    dev->cmd_state = 1;
                    esdi_mca_set_callback(dev, ESDI_TIME);
                    dev->data_pos = 0;
                    break;

                case 1:
                    if (!(dev->basic_ctrl & CTRL_DMA_ENA)) {
                        esdi_mca_set_callback(dev, ESDI_TIME);
                        return;
                    }
                    while (dev->sector_pos < dev->sector_count) {
                        while (dev->data_pos < 256) {
                            val = dma_channel_read(dev->dma);

                            if (val == DMA_NODATA) {
                                esdi_mca_set_callback(dev, ESDI_TIME);
                                return;
                            }

                            dev->data[dev->data_pos++] = val & 0xffff;
                        }

                        memcpy(dev->sector_buffer[dev->sector_pos++], dev->data, 512);
                        dev->data_pos = 0;
                    }

                    dev->status    = STATUS_CMD_IN_PROGRESS;
                    dev->cmd_state = 2;
                    esdi_mca_set_callback(dev, ESDI_TIME);
                    break;

                case 2:
                    dev->status          = STATUS_IRQ;
                    dev->irq_status      = IRQ_HOST_ADAPTER | IRQ_CMD_COMPLETE_SUCCESS;
                    dev->irq_in_progress = 1;
                    set_irq(dev);
                    ui_sb_update_icon(SB_HDD | HDD_BUS_ESDI, 0);
                    ui_sb_update_icon_write(SB_HDD | HDD_BUS_ESDI, 0);
                    break;

                default:
                    break;
            }
            break;

        case 0x11:
            ESDI_ADAPTER_ONLY();
            switch (dev->cmd_state) {
                case 0:
                    dev->sector_pos   = 0;
                    dev->sector_count = dev->cmd_data[1];
                    if (dev->sector_count > 256)
                        fatal("Read sector buffer count %04x\n", dev->cmd_data[1]);

                    dev->status          = STATUS_IRQ | STATUS_CMD_IN_PROGRESS | STATUS_TRANSFER_REQ;
                    dev->irq_status      = IRQ_HOST_ADAPTER | IRQ_DATA_TRANSFER_READY;
                    dev->irq_in_progress = 1;
                    set_irq(dev);

                    dev->cmd_state = 1;
                    esdi_mca_set_callback(dev, ESDI_TIME);
                    dev->data_pos = 0;
                    break;

                case 1:
                    if (!(dev->basic_ctrl & CTRL_DMA_ENA)) {
                        esdi_mca_set_callback(dev, ESDI_TIME);
                        return;
                    }

                    while (dev->sector_pos < dev->sector_count) {
                        if (!dev->data_pos)
                            memcpy(dev->data, dev->sector_buffer[dev->sector_pos++], 512);
                        while (dev->data_pos < 256) {
                            val = dma_channel_write(dev->dma, dev->data[dev->data_pos]);

                            if (val == DMA_NODATA) {
                                esdi_mca_set_callback(dev, ESDI_TIME);
                                return;
                            }

                            dev->data_pos++;
                        }

                        dev->data_pos = 0;
                    }

                    dev->status    = STATUS_CMD_IN_PROGRESS;
                    dev->cmd_state = 2;
                    esdi_mca_set_callback(dev, ESDI_TIME);
                    break;

                case 2:
                    dev->status          = STATUS_IRQ;
                    dev->irq_status      = IRQ_HOST_ADAPTER | IRQ_CMD_COMPLETE_SUCCESS;
                    dev->irq_in_progress = 1;
                    set_irq(dev);
                    ui_sb_update_icon(SB_HDD | HDD_BUS_ESDI, 0);
                    break;

                default:
                    break;
            }
            break;

        case 0x12:
            ESDI_ADAPTER_ONLY();
            if ((dev->status & STATUS_IRQ) || dev->irq_in_progress)
                fatal("IRQ in progress %02x %i\n", dev->status, dev->irq_in_progress);

            dev->status_len     = 2;
            dev->status_data[0] = 0x12 | STATUS_LEN(5) | STATUS_DEVICE_HOST_ADAPTER;
            dev->status_data[1] = 0;

            dev->status          = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
            dev->irq_status      = IRQ_HOST_ADAPTER | IRQ_CMD_COMPLETE_SUCCESS;
            dev->irq_in_progress = 1;
            set_irq(dev);
            ui_sb_update_icon(SB_HDD | HDD_BUS_ESDI, 0);
            break;

        case CMD_FORMAT_UNIT:
        case CMD_FORMAT_PREPARE:
            ESDI_DRIVE_ONLY();

            if (!drive->present) {
                device_not_present(dev);
                return;
            }

            switch (dev->cmd_state) {
                case 0:
                    dev->rba = hdd_image_get_last_sector(drive->hdd_num);

                    if (dev->command == CMD_FORMAT_UNIT)
                        dev->sector_count = dev->cmd_data[1];
                    else
                        dev->sector_count = 0;

                    dev->status          = STATUS_IRQ | STATUS_CMD_IN_PROGRESS | STATUS_TRANSFER_REQ;
                    dev->irq_status      = dev->cmd_dev | IRQ_DATA_TRANSFER_READY;
                    dev->irq_in_progress = 1;
                    set_irq(dev);

                    dev->cmd_state = 1;
                    esdi_mca_set_callback(dev, ESDI_TIME);
                    break;

                case 1:
                    if (!(dev->basic_ctrl & CTRL_DMA_ENA)) {
                        esdi_mca_set_callback(dev, ESDI_TIME);
                        return;
                    }

                    if (dev->command == CMD_FORMAT_UNIT)
                        hdd_image_zero(drive->hdd_num, 0, hdd_image_get_last_sector(drive->hdd_num) + 1);

                    dev->status    = STATUS_CMD_IN_PROGRESS;
                    dev->cmd_state = 2;
                    esdi_mca_set_callback(dev, ESDI_TIME);
                    break;

                case 2:
                    complete_command_status(dev);
                    dev->status          = STATUS_IRQ | STATUS_STATUS_OUT_FULL;
                    dev->irq_status      = dev->cmd_dev | IRQ_CMD_COMPLETE_SUCCESS;
                    dev->irq_in_progress = 1;
                    set_irq(dev);
                    break;

                default:
                    break;
            }
            break;

        default:
            fatal("BAD COMMAND %02x %i\n", dev->command, dev->cmd_dev);
    }
}

static uint8_t
esdi_read(uint16_t port, void *priv)
{
    esdi_t *dev = (esdi_t *) priv;
    uint8_t ret = 0x00;

    switch (port & 7) {
        case 2: /*Basic status register*/
            ret = dev->status;
            break;

        case 3: /*IRQ status*/
            dev->status &= ~STATUS_IRQ;
            ret = dev->irq_status;
            break;

        default:
            esdi_mca_log("esdi_read port=%04x\n", port);
            break;
    }

    esdi_mca_log("ESDI: rr(%04x, %02x)\n", port & 7, ret);
    return ret;
}

static void
esdi_write(uint16_t port, uint8_t val, void *priv)
{
    esdi_t *dev = (esdi_t *) priv;
    uint8_t old;

    esdi_mca_log("ESDI: wr(%04x, %02x)\n", port & 7, val);

    switch (port & 7) {
        case 2: /*Basic control register*/
            if ((dev->basic_ctrl & CTRL_RESET) && !(val & CTRL_RESET)) {
                dev->in_reset = 1;
                esdi_mca_set_callback(dev, ESDI_TIME * 50);
                dev->status = STATUS_BUSY;
            } else if (!(dev->basic_ctrl & CTRL_RESET) && (val & CTRL_RESET)) {
                esdi_mca_set_callback(dev, 0.0);
                dev->status = STATUS_BUSY;
            }
            old = dev->basic_ctrl;
            dev->basic_ctrl = val;
            if ((val & CTRL_IRQ_ENA) && !(old & CTRL_IRQ_ENA))
                update_irq(dev);
            break;

        case 3: /*Attention register*/
            switch (val & ATTN_DEVICE_SEL) {
                case ATTN_HOST_ADAPTER:
                    switch (val & ATTN_REQ_MASK) {
                        case ATTN_CMD_REQ:
                            if (dev->cmd_req_in_progress)
                                fatal("Try to start command on in_progress adapter\n");
                            dev->cmd_req_in_progress = 1;
                            dev->cmd_dev             = ATTN_HOST_ADAPTER;
                            dev->status |= STATUS_BUSY;
                            dev->cmd_pos    = 0;
                            dev->status_pos = 0;
                            break;

                        case ATTN_EOI:
                            dev->irq_in_progress = 0;
                            dev->status &= ~STATUS_IRQ;
                            clear_irq(dev);
                            break;

                        case ATTN_RESET:
                            dev->in_reset = 1;
                            esdi_mca_set_callback(dev, ESDI_TIME * 50);
                            dev->status = STATUS_BUSY;
                            break;

                        default:
                            fatal("Bad attention request %02x\n", val);
                    }
                    break;

                case ATTN_DEVICE_0:
                    esdi_mca_log("ATTN Device 0.\n");
                    switch (val & ATTN_REQ_MASK) {
                        case ATTN_CMD_REQ:
                            if (dev->cmd_req_in_progress)
                                fatal("Try to start command on in_progress device0\n");
                            dev->cmd_req_in_progress = 1;
                            dev->cmd_dev             = ATTN_DEVICE_0;
                            dev->status |= STATUS_BUSY;
                            dev->cmd_pos    = 0;
                            dev->status_pos = 0;
                            break;

                        case ATTN_EOI:
                            esdi_mca_log("EOI.\n");
                            dev->irq_in_progress = 0;
                            dev->status &= ~STATUS_IRQ;
                            clear_irq(dev);
                            break;

                        default:
                            fatal("Bad attention request %02x\n", val);
                    }
                    break;

                case ATTN_DEVICE_1:
                    switch (val & ATTN_REQ_MASK) {
                        case ATTN_CMD_REQ:
                            if (dev->cmd_req_in_progress)
                                fatal("Try to start command on in_progress device0\n");
                            dev->cmd_req_in_progress = 1;
                            dev->cmd_dev             = ATTN_DEVICE_1;
                            dev->status |= STATUS_BUSY;
                            dev->cmd_pos    = 0;
                            dev->status_pos = 0;
                            break;

                        case ATTN_EOI:
                            dev->irq_in_progress = 0;
                            dev->status &= ~STATUS_IRQ;
                            clear_irq(dev);
                            break;

                        default:
                            fatal("Bad attention request %02x\n", val);
                    }
                    break;

                default:
                    fatal("Attention to unknown device %02x\n", val);
            }
            break;

        default:
            fatal("esdi_write port=%04x val=%02x\n", port, val);
    }
}

static uint16_t
esdi_readw(uint16_t port, void *priv)
{
    esdi_t  *dev = (esdi_t *) priv;
    uint16_t ret = 0xffff;

    switch (port & 7) {
        case 0: /*Status Interface Register*/
            if (dev->status_pos >= dev->status_len) {
                esdi_mca_log("esdi_readw port=%04x, ret=0000 (pos=%d, len=%d).\n", port, dev->status_pos, dev->status_len);
                return 0;
            }
            ret = dev->status_data[dev->status_pos++];
            if (dev->status_pos >= dev->status_len) {
                dev->status &= ~STATUS_STATUS_OUT_FULL;
                dev->status_pos = dev->status_len = 0;
            }
            break;

        default:
            fatal("esdi_readw port=%04x\n", port);
    }

    esdi_mca_log("esdi_readw port=%04x, ret=%04x.\n", port, ret);
    return ret;
}

static void
esdi_writew(uint16_t port, uint16_t val, void *priv)
{
    esdi_t *dev = (esdi_t *) priv;

    esdi_mca_log("ESDI: wrw(%04x, %04x)\n", port & 7, val);

    switch (port & 7) {
        case 0: /*Command Interface Register*/
            if (dev->cmd_pos >= 4)
                fatal("CIR pos 4\n");
            dev->cmd_data[dev->cmd_pos++] = val;
            if (((dev->cmd_data[0] & CMD_SIZE_4) && dev->cmd_pos == 4) || (!(dev->cmd_data[0] & CMD_SIZE_4) && dev->cmd_pos == 2)) {
                dev->cmd_pos             = 0;
                dev->cmd_req_in_progress = 0;
                dev->cmd_state           = 0;

                if ((dev->cmd_data[0] & CMD_DEVICE_SEL) != dev->cmd_dev)
                    fatal("Command device mismatch with attn\n");
                dev->command = dev->cmd_data[0] & CMD_MASK;
                esdi_mca_set_callback(dev, ESDI_TIME);
                dev->status   = STATUS_BUSY;
                dev->data_pos = 0;
                ui_sb_update_icon(SB_HDD | HDD_BUS_ESDI, 1);
            }
            break;

        default:
            fatal("esdi_writew port=%04x val=%04x\n", port, val);
    }
}

static uint8_t
esdi_mca_read(int port, void *priv)
{
    const esdi_t *dev = (esdi_t *) priv;

    esdi_mca_log("ESDI: mcard(%04x)\n", port);

    return (dev->pos_regs[port & 7]);
}

static void
esdi_mca_write(int port, uint8_t val, void *priv)
{
    esdi_t *dev = (esdi_t *) priv;

    esdi_mca_log("ESDI: mcawr(%04x, %02x)  pos[2]=%02x pos[3]=%02x\n",
                 port, val, dev->pos_regs[2], dev->pos_regs[3]);

    if (port < 0x102)
        return;

    /* Save the new value. */
    dev->pos_regs[port & 7] = val;

    io_removehandler(ESDI_IOADDR_PRI, 8,
                     esdi_read, esdi_readw, NULL,
                     esdi_write, esdi_writew, NULL, dev);
    mem_mapping_disable(&dev->bios_rom.mapping);

    switch (dev->pos_regs[2] & 0x3c) {
        case 0x14:
            dev->dma = 5;
            break;
        case 0x18:
            dev->dma = 6;
            break;
        case 0x1c:
            dev->dma = 7;
            break;
        case 0x00:
            dev->dma = 0;
            break;
        case 0x04:
            dev->dma = 1;
            break;
        case 0x0c:
            dev->dma = 3;
            break;
        case 0x10:
            dev->dma = 4;
            break;

        default:
            break;
    }

    if (!(dev->pos_regs[3] & 8)) {
        switch (dev->pos_regs[3] & 7) {
            case 2:
                dev->bios = 0xc8000;
                break;
            case 3:
                dev->bios = 0xcc000;
                break;
            case 4:
                dev->bios = 0xd0000;
                break;
            case 5:
                dev->bios = 0xd4000;
                break;
            case 6:
                dev->bios = 0xd8000;
                break;
            case 7:
                dev->bios = 0xdc000;
                break;
            default:
                break;
        }
    } else
        dev->bios = 0;

    if (dev->pos_regs[2] & 1) {
        io_sethandler(ESDI_IOADDR_PRI, 8,
                      esdi_read, esdi_readw, NULL,
                      esdi_write, esdi_writew, NULL, dev);

        if (dev->bios) {
            mem_mapping_enable(&dev->bios_rom.mapping);
            mem_mapping_set_addr(&dev->bios_rom.mapping, dev->bios, 0x4000);
        }

        /* Say hello. */
        esdi_mca_log("ESDI: I/O=3510, IRQ=14, DMA=%d, BIOS @%05X\n",
                     dev->dma, dev->bios);
    }
}

static void
esdi_integrated_mca_write(int port, uint8_t val, void* priv)
{
    esdi_t* dev = (esdi_t*)priv;

    esdi_mca_log("ESDI: mcawr(%04x, %02x)  pos[2]=%02x pos[3]=%02x\n",
        port, val, dev->pos_regs[2], dev->pos_regs[3]);

    if (port < 0x102)
        return;

    /* Save the new value. */
    dev->pos_regs[port & 7] = val;

    io_removehandler(ESDI_IOADDR_PRI, 8,
        esdi_read, esdi_readw, NULL,
        esdi_write, esdi_writew, NULL, dev);

    switch (dev->pos_regs[2] & 0x3c) {
    case 0x14:
        dev->dma = 5;
        break;
    case 0x18:
        dev->dma = 6;
        break;
    case 0x1c:
        dev->dma = 7;
        break;
    case 0x00:
        dev->dma = 0;
        break;
    case 0x04:
        dev->dma = 1;
        break;
    case 0x0c:
        dev->dma = 3;
        break;
    case 0x10:
        dev->dma = 4;
        break;

    default:
        break;
    }

    if (dev->pos_regs[2] & 1) {
        io_sethandler(ESDI_IOADDR_PRI, 8,
            esdi_read, esdi_readw, NULL,
            esdi_write, esdi_writew, NULL, dev);

        /* Say hello. */
        esdi_mca_log("ESDI: I/O=3510, IRQ=14, DMA=%d\n",
            dev->dma);
    }
}

static uint8_t
esdi_mca_feedb(void *priv)
{
    const esdi_t *dev = (esdi_t *) priv;

    return (dev->pos_regs[2] & 1);
}

static void esdi_reset(void* priv)
{
    esdi_t* dev = (esdi_t*)priv;
    if (!dev->in_reset) {
        dev->in_reset = 1;
        esdi_mca_set_callback(dev, ESDI_TIME * 50);
        dev->status = STATUS_BUSY;
    }
}

static void *
esdi_init(UNUSED(const device_t *info))
{
    drive_t *drive;
    esdi_t  *dev;
    uint8_t  c;
    uint8_t  i;

    dev = calloc(1, sizeof(esdi_t));
    if (dev == NULL)
        return (NULL);

    /* Mark as unconfigured. */
    dev->irq_status = 0xff;

    if (info->local == ESDI_IS_ADAPTER) {
        rom_init_interleaved(&dev->bios_rom,
            BIOS_FILE_H, BIOS_FILE_L,
            0xc8000, 0x4000, 0x3fff, 0, MEM_MAPPING_EXTERNAL);
        mem_mapping_disable(&dev->bios_rom.mapping);
    }

    dev->drives[0].present = dev->drives[1].present = 0;

    for (c = 0, i = 0; i < HDD_NUM; i++) {
        if ((hdd[i].bus_type == HDD_BUS_ESDI) && (hdd[i].esdi_channel < ESDI_NUM)) {
            /* This is an ESDI drive. */
            drive = &dev->drives[hdd[i].esdi_channel];

            /* Try to load an image for the drive. */
            if (!hdd_image_load(i)) {
                /* Nope. */
                drive->present = 0;
                continue;
            }

            hdd_preset_apply(i);

            /* OK, so fill in geometry info. */
            drive->spt     = hdd[i].spt;
            drive->hpc     = hdd[i].hpc;
            drive->tracks  = hdd[i].tracks;
            drive->sectors = hdd_image_get_last_sector(i);
            drive->hdd_num = i;

            /* Mark drive as present. */
            drive->present = 1;
        }

        if (++c >= ESDI_NUM)
            break;
    }

    /* Set the MCA ID for this controller. */
    if (info->local == ESDI_IS_ADAPTER) {
        dev->pos_regs[0] = 0xff;
        dev->pos_regs[1] = 0xdd;
    } else if (info->local == ESDI_IS_INTEGRATED) {
        dev->pos_regs[0] = 0x9f;
        dev->pos_regs[1] = 0xdf;
    }

    /* Enable the device. */
    if (info->local == ESDI_IS_INTEGRATED) {
        /* The slot number of this controller is fixed by the planar. IBM PS/55 5551-T assigns it #5. */
        int slotno = device_get_config_int("in_esdi_slot");
        if (slotno)
            mca_add_to_slot(esdi_mca_read, esdi_integrated_mca_write, esdi_mca_feedb, esdi_reset, dev, slotno - 1);
        else
            mca_add(esdi_mca_read, esdi_integrated_mca_write, esdi_mca_feedb, esdi_reset, dev);
    } else
        mca_add(esdi_mca_read, esdi_mca_write, esdi_mca_feedb, NULL, dev);

    /* Mark for a reset. */
    dev->in_reset = 1;
    esdi_mca_set_callback(dev, ESDI_TIME * 50);
    dev->status = STATUS_BUSY;

    /* Set the reply timer. */
    timer_add(&dev->timer, esdi_callback, dev, 0);

    return dev;
}

static void
esdi_close(void *priv)
{
    esdi_t        *dev = (esdi_t *) priv;
    const drive_t *drive;

    dev->drives[0].present = dev->drives[1].present = 0;

    for (uint8_t d = 0; d < 2; d++) {
        drive = &dev->drives[d];

        hdd_image_close(drive->hdd_num);
    }

    free(dev);
}

static int
esdi_available(void)
{
    return (rom_present(BIOS_FILE_L) && rom_present(BIOS_FILE_H));
}

const device_t esdi_ps2_device = {
    .name          = "IBM PS/2 ESDI Fixed Disk Adapter (MCA)",
    .internal_name = "esdi_mca",
    .flags         = DEVICE_MCA,
    .local         = ESDI_IS_ADAPTER,
    .init          = esdi_init,
    .close         = esdi_close,
    .reset         = NULL,
    .available     = esdi_available,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

static device_config_t
    esdi_integrated_config[] = {
          {
           .name        = "in_esdi_slot",
           .description = "Slot #",
           .type        = CONFIG_SELECTION,
           .selection   = {
                { .description = "Auto", .value = 0 },
                { .description = "1", .value = 1 },
                { .description = "2", .value = 2 },
                { .description = "3", .value = 3 },
                { .description = "4", .value = 4 },
                { .description = "5", .value = 5 },
                { .description = "6", .value = 6 },
                { .description = "7", .value = 7 },
                { .description = "8", .value = 8 }
            },
           .default_int = 0
        },
          { .type = -1 }
};

/*
Device for an IBM DBA (Direct Bus Attachment) hard disk.
The Disk BIOS is included in the System ROM.
Some models have an exclusive channel slot for the DBA hard disk.
Following IBM machines are supported:
  * PS/2 model 55SX
  * PS/2 model 65SX
  * PS/2 model 70 type 3 (Slot #4)
  * PS/2 model 70 type 4 (Slot #4)
  * PS/55 model 5550-T (Slot #5)
  * PS/55 model 5550-V (Slot #5)
*/
const device_t
esdi_integrated_device = {
    .name = "IBM Integrated Fixed Disk and Controller (MCA)",
    .internal_name = "esdi_integrated_mca",
    .flags = DEVICE_MCA,
    .local = ESDI_IS_INTEGRATED,
    .init = esdi_init,
    .close = esdi_close,
    .reset = esdi_reset,
    .available = NULL,
    .speed_changed = NULL,
    .force_redraw = NULL,
    .config = esdi_integrated_config
};
