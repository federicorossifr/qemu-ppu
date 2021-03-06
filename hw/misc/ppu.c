/*
 * QEMU educational PCI device
 *
 * Copyright (c) 2012-2015 Jiri Slaby
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/hw.h"
#include "hw/pci/msi.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "qemu/main-loop.h" /* iothread mutex */
#include "qemu/module.h"
#include "qapi/visitor.h"
#include "cposit.h"
#define TYPE_PCI_EDU_DEVICE "ppu"
typedef struct PPUState PPUState;

typedef union {
    uint32_t i;
    float f;
} float_t;

DECLARE_INSTANCE_CHECKER(PPUState, PPU,
                         TYPE_PCI_EDU_DEVICE)

#define FACT_IRQ        0x00000001
#define DMA_IRQ         0x00000100

#define DMA_START       0x40000
#define DMA_SIZE        4096

struct PPUState {
    PCIDevice pdev;
    MemoryRegion mmio;

    QemuThread thread;
    QemuMutex thr_mutex;
    QemuCond thr_cond;
    bool stopping;

    uint32_t mode;
    uint32_t fact;
#define EDU_STATUS_COMPUTING    0x01
#define EDU_STATUS_IRQFACT      0x80
    uint32_t status;
    uint32_t irq_status;

#define EDU_DMA_RUN             0x1
#define EDU_DMA_DIR(cmd)        (((cmd) & 0x2) >> 1)
# define EDU_DMA_FROM_PCI       0
# define EDU_DMA_TO_PCI         1
#define EDU_DMA_IRQ             0x4
    struct dma_state {
        dma_addr_t src;
        dma_addr_t dst;
        dma_addr_t cnt;
        dma_addr_t cmd;
    } dma;
    QEMUTimer dma_timer;
    char dma_buf[DMA_SIZE];
    uint64_t dma_mask;
};

static bool edu_msi_enabled(PPUState *edu)
{
    return msi_enabled(&edu->pdev);
}

static void edu_raise_irq(PPUState *edu, uint32_t val)
{
    edu->irq_status |= val;
    if (edu->irq_status) {
        if (edu_msi_enabled(edu)) {
            msi_notify(&edu->pdev, 0);
        } else {
            pci_set_irq(&edu->pdev, 1);
        }
    }
}

static void edu_lower_irq(PPUState *edu, uint32_t val)
{
    edu->irq_status &= ~val;

    if (!edu->irq_status && !edu_msi_enabled(edu)) {
        pci_set_irq(&edu->pdev, 0);
    }
}

static bool within(uint64_t addr, uint64_t start, uint64_t end)
{
    return start <= addr && addr < end;
}

static void edu_check_range(uint64_t addr, uint64_t size1, uint64_t start,
                uint64_t size2)
{
    uint64_t end1 = addr + size1;
    uint64_t end2 = start + size2;

    if (within(addr, start, end2) &&
            end1 > addr && within(end1, start, end2)) {
        return;
    }

    hw_error("PPU: DMA range 0x%016"PRIx64"-0x%016"PRIx64
             " out of bounds (0x%016"PRIx64"-0x%016"PRIx64")!",
            addr, end1 - 1, start, end2 - 1);
}

static dma_addr_t edu_clamp_addr(const PPUState *edu, dma_addr_t addr)
{
    dma_addr_t res = addr & edu->dma_mask;

    if (addr != res) {
        printf("PPU: clamping DMA %#.16"PRIx64" to %#.16"PRIx64"!\n", addr, res);
    }

    return res;
}

static void edu_dma_timer(void *opaque)
{
    PPUState *edu = opaque;
    bool raise_irq = false;

    if (!(edu->dma.cmd & EDU_DMA_RUN)) {
        return;
    }

    if (EDU_DMA_DIR(edu->dma.cmd) == EDU_DMA_FROM_PCI) {
        uint64_t dst = edu->dma.dst;
        edu_check_range(dst, edu->dma.cnt, DMA_START, DMA_SIZE);
        dst -= DMA_START;
        printf("[ppu dma] Reading %ld bytes to %#4lx\n",edu->dma.cnt,dst);
        pci_dma_read(&edu->pdev, edu_clamp_addr(edu, edu->dma.src),
                edu->dma_buf + dst, edu->dma.cnt);
        printf("[ppu dma] Read: %d\n",*(int*)(&edu->dma_buf[0]));
    } else {
        uint64_t src = edu->dma.src;
        edu_check_range(src, edu->dma.cnt, DMA_START, DMA_SIZE);
        src -= DMA_START;
        printf("[ppu dma] Writing %ld bytes to %lx\n",edu->dma.cnt,edu->dma.dst);

        pci_dma_write(&edu->pdev, edu_clamp_addr(edu, edu->dma.dst),
                edu->dma_buf + src, edu->dma.cnt);
    }

    edu->dma.cmd &= ~EDU_DMA_RUN;
    if (edu->dma.cmd & EDU_DMA_IRQ) {
        raise_irq = true;
    }

    if (raise_irq) {
        edu_raise_irq(edu, DMA_IRQ);
    }
}

static void dma_rw(PPUState *edu, bool write, dma_addr_t *val, dma_addr_t *dma,
                bool timer)
{
    if (write && (edu->dma.cmd & EDU_DMA_RUN)) {
        return;
    }

    if (write) {
        *dma = *val;
    } else {
        *val = *dma;
    }

    if (timer) {
        timer_mod(&edu->dma_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
    }
}

static uint64_t edu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    PPUState *edu = opaque;
    uint64_t val = ~0ULL;

    if (addr < 0x80 && size != 4) {
        return val;
    }

    if (addr >= 0x80 && size != 4 && size != 8) {
        return val;
    }

    switch (addr) {
    case 0x00:
        val = 0xFEDEFEDE;
        break;
    case 0x04:
        val = edu->mode;
        break;
    case 0x08:
        qemu_mutex_lock(&edu->thr_mutex);
        val = edu->fact;
        qemu_mutex_unlock(&edu->thr_mutex);
        break;
    case 0x20:
        val = qatomic_read(&edu->status);
        qemu_printf("[PPU] Status register: %#8x\n",(uint32_t)val);
        break;
    case 0x24:
        val = edu->irq_status;
        break;
    case 0x80:
        dma_rw(edu, false, &val, &edu->dma.src, false);
        break;
    case 0x88:
        dma_rw(edu, false, &val, &edu->dma.dst, false);
        break;
    case 0x90:
        dma_rw(edu, false, &val, &edu->dma.cnt, false);
        break;
    case 0x98:
        dma_rw(edu, false, &val, &edu->dma.cmd, false);
        break;
    }

    return val;
}

static void edu_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                unsigned size)
{
    PPUState *edu = opaque;
    qemu_printf("[PPU] Req address %ld\n",addr);
    if (addr < 0x80 && size != 4) {
        return;
    }

    if (addr >= 0x80 && size != 4 && size != 8) {
        return;
    }

    switch (addr) {
    case 0x04:
        qemu_printf("[PPU] Changing PPU MODE to: %#8x\n",(uint32_t)val);
        edu->mode = val;
        break;
    case 0x08:
        if (qatomic_read(&edu->status) & EDU_STATUS_COMPUTING) {
            break;
        }
        /* EDU_STATUS_COMPUTING cannot go 0->1 concurrently, because it is only
         * set in this function and it is under the iothread mutex.
         */
        qemu_mutex_lock(&edu->thr_mutex);
        edu->fact = val;
        qatomic_or(&edu->status, EDU_STATUS_COMPUTING);
        qemu_cond_signal(&edu->thr_cond);
        qemu_mutex_unlock(&edu->thr_mutex);
        break;
    case 0x20:
        if (val & EDU_STATUS_IRQFACT) {
            qatomic_or(&edu->status, EDU_STATUS_IRQFACT);
        } else {
            qatomic_and(&edu->status, ~EDU_STATUS_IRQFACT);
        }
        break;
    case 0x60:
        edu_raise_irq(edu, val);
        break;
    case 0x64:
        edu_lower_irq(edu, val);
        break;
    case 0x80:
        qemu_printf("[PPU] DMA SRC: %#8x\n",(uint32_t)val);
        dma_rw(edu, true, &val, &edu->dma.src, false);
        break;
    case 0x88:
        qemu_printf("[PPU] DMA DST: %#8x\n",(uint32_t)val);
        dma_rw(edu, true, &val, &edu->dma.dst, false);
        break;
    case 0x90:
        qemu_printf("[PPU] DMA CNT: %d\n",(uint32_t)val);
        dma_rw(edu, true, &val, &edu->dma.cnt, false);
        break;
    case 0x98:

        if (!(val & EDU_DMA_RUN)) {
            qemu_printf("[PPU] BUSY: %d\n",(uint32_t)val);
            break;
        }
        qemu_printf("[PPU] DMA CMD: %d\n",(uint32_t)val);

        dma_rw(edu, true, &val, &edu->dma.cmd, true);
        break;
    }
}

static const MemoryRegionOps edu_mmio_ops = {
    .read = edu_mmio_read,
    .write = edu_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },

};

/*
 * We purposely use a thread, so that users are forced to wait for the status
 * register.
 */
static void *edu_fact_thread(void *opaque)
{
    PPUState *edu = opaque;

    while (1) {
        uint32_t val, ret = 1;

        qemu_mutex_lock(&edu->thr_mutex);
        while ((qatomic_read(&edu->status) & EDU_STATUS_COMPUTING) == 0 &&
                        !edu->stopping) {
            qemu_cond_wait(&edu->thr_cond, &edu->thr_mutex);
        }

        if (edu->stopping) {
            qemu_mutex_unlock(&edu->thr_mutex);
            break;
        }

        val = edu->fact;
        qemu_mutex_unlock(&edu->thr_mutex);
        qemu_printf("[PPU] Conversion begin, MODE:%#8x\n",edu->mode);
        qemu_printf("[PPU] Input: %#8x\n", val);

        uint32_t conversion_direction = (edu->mode & 0xF0);
        uint32_t conversion_ptype     = edu->mode & 0x0F;
        float_t f;
        if(conversion_direction) { // 
            f.i = val;
            switch(conversion_ptype) { // fp32>posit
                case 0x01: val = posit8_toRaw(posit8_fromFloat(f.f));break;
                case 0x02: val = posit16_toRaw(posit16_fromFloat(f.f)); break;
            }
        } else {
            switch(conversion_ptype) { // posit>fp32
                case 0x01: f.f = posit8_toFloat(posit8_fromSRaw(val));break;
                case 0x02: f.f = posit16_toFloat(posit16_fromSRaw(val)); break;
                default: f.i = 0xffc00000; break;
            }
            val = f.i;
        }

        qemu_printf("[PPU] Output: %#8x\n", val);
        ret = val;
        /*
         * We should sleep for a random period here, so that students are
         * forced to check the status properly.
         */

        qemu_mutex_lock(&edu->thr_mutex);
        edu->fact = ret;
        qemu_mutex_unlock(&edu->thr_mutex);
        qatomic_and(&edu->status, ~EDU_STATUS_COMPUTING);

        if (qatomic_read(&edu->status) & EDU_STATUS_IRQFACT) {
            qemu_mutex_lock_iothread();
            edu_raise_irq(edu, FACT_IRQ);
            qemu_mutex_unlock_iothread();
        }
    }

    return NULL;
}

static void pci_edu_realize(PCIDevice *pdev, Error **errp)
{
    PPUState *edu = PPU(pdev);
    uint8_t *pci_conf = pdev->config;

    pci_config_set_interrupt_pin(pci_conf, 1);

    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    timer_init_ms(&edu->dma_timer, QEMU_CLOCK_VIRTUAL, edu_dma_timer, edu);

    qemu_mutex_init(&edu->thr_mutex);
    qemu_cond_init(&edu->thr_cond);
    qemu_thread_create(&edu->thread, "edu", edu_fact_thread,
                       edu, QEMU_THREAD_JOINABLE);

    memory_region_init_io(&edu->mmio, OBJECT(edu), &edu_mmio_ops, edu,
                    "edu-mmio", 1 * MiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &edu->mmio);
}

static void pci_edu_uninit(PCIDevice *pdev)
{
    PPUState *edu = PPU(pdev);

    qemu_mutex_lock(&edu->thr_mutex);
    edu->stopping = true;
    qemu_mutex_unlock(&edu->thr_mutex);
    qemu_cond_signal(&edu->thr_cond);
    qemu_thread_join(&edu->thread);

    qemu_cond_destroy(&edu->thr_cond);
    qemu_mutex_destroy(&edu->thr_mutex);

    timer_del(&edu->dma_timer);
    msi_uninit(pdev);
}

static void edu_instance_init(Object *obj)
{
    PPUState *edu = PPU(obj);

    edu->dma_mask = (1UL << 32) - 1;
    object_property_add_uint64_ptr(obj, "dma_mask",
                                   &edu->dma_mask, OBJ_PROP_FLAG_READWRITE);
}

static void edu_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize = pci_edu_realize;
    k->exit = pci_edu_uninit;
    k->vendor_id = 0xFEDE;
    k->device_id = 0xFEFE;
    k->revision = 0x10;
    k->class_id = PCI_CLASS_OTHERS;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static void pci_edu_register_types(void)
{
    static InterfaceInfo interfaces[] = {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    };
    static const TypeInfo edu_info = {
        .name          = TYPE_PCI_EDU_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(PPUState),
        .instance_init = edu_instance_init,
        .class_init    = edu_class_init,
        .interfaces = interfaces,
    };

    type_register_static(&edu_info);
}
type_init(pci_edu_register_types)
