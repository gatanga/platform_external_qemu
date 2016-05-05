/* Copyright (C) 2011 The Android Open Source Project
** Copyright (C) 2014 Linaro Limited
** Copyright (C) 2015 Intel Corporation
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** Description
**
** This device provides a virtual pipe device (originally called
** goldfish_pipe and latterly qemu_pipe). This allows the android
** running under the emulator to open a fast connection to the host
** for various purposes including the adb debug bridge and
** (eventually) the opengles pass-through. This file contains only the
** basic pipe infrastructure and a couple of test pipes. Additional
** pipes are registered with the android_pipe_add_type() call.
**
** Open Questions
**
** Since this was originally written there have been a number of other
** virtual devices added to QEMU using the virtio infrastructure. We
** should give some thought to if this needs re-writing to take
** advantage of that infrastructure to create the pipes.
*/
#include "hw/misc/android_pipe.h"

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "qemu-common.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"

#include <assert.h>
#include <glib.h>

/* Set to > 0 for debug output */
#define PIPE_DEBUG 0

/* Set to 1 to debug i/o register reads/writes */
#define PIPE_DEBUG_REGS 0

#if PIPE_DEBUG >= 1
#define D(fmt, ...) \
    do { fprintf(stdout, "android_pipe: " fmt "\n", ## __VA_ARGS__); } while (0)
#else
#define D(fmt, ...)  do { /* nothing */ } while (0)
#endif

#if PIPE_DEBUG >= 2
#define DD(fmt, ...) \
    do { fprintf(stdout, "android_pipe: " fmt "\n", ## __VA_ARGS__); } while (0)
#else
#define DD(fmt, ...)  do { /* nothing */ } while (0)
#endif

#if PIPE_DEBUG_REGS >= 1
#  define DR(...)   D(__VA_ARGS__)
#else
#  define DR(...)   do { /* nothing */ } while (0)
#endif

#define E(fmt, ...)  \
    do { fprintf(stdout, "ERROR:" fmt "\n", ## __VA_ARGS__); } while (0)

#define APANIC(...)                     \
    do {                                \
        error_report(__VA_ARGS__);      \
        exit(1);                        \
    } while (0);

/* Maximum length of pipe service name, in characters (excluding final 0) */
#define MAX_PIPE_SERVICE_NAME_SIZE  255

/* from AOSP version include/hw/android/goldfish/device.h
 * FIXME?: needs to use proper qemu abstractions
 */
static inline void uint64_set_low(uint64_t *addr, uint32 value)
{
    *addr = (*addr & ~(0xFFFFFFFFULL)) | value;
}

static inline void uint64_set_high(uint64_t *addr, uint32 value)
{
    *addr = (*addr & 0xFFFFFFFFULL) | ((uint64_t)value << 32);
}

#define TYPE_ANDROID_PIPE "android_pipe"
#define ANDROID_PIPE(obj) \
    OBJECT_CHECK(AndroidPipeState, (obj), TYPE_ANDROID_PIPE)

typedef struct PipeDevice  PipeDevice;

typedef struct {
    SysBusDevice parent;
    MemoryRegion iomem;
    qemu_irq irq;

    /* TODO: roll into shared state */
    PipeDevice *dev;
} AndroidPipeState;


/***********************************************************************
 ***********************************************************************
 *****
 *****    P I P E   C O N N E C T I O N S
 *****
 *****/

typedef struct HwPipe {
    struct HwPipe               *next;
    PipeDevice                  *device;
    uint64_t                    channel; /* opaque kernel handle */
    unsigned char               wanted;
    char                        closed;
    void                        *pipe;
    QemuMutex                   lock;
} HwPipe;

static unsigned char get_and_clear_pipe_wanted(HwPipe* pipe) {
    qemu_mutex_lock(&pipe->lock);
    unsigned char val = pipe->wanted;
    pipe->wanted = 0;
    qemu_mutex_unlock(&pipe->lock);
    return val;
}

static void set_pipe_wanted_bits(HwPipe* pipe, unsigned char val) {
    qemu_mutex_lock(&pipe->lock);
    pipe->wanted |= val;
    qemu_mutex_unlock(&pipe->lock);
}

static HwPipe*
pipe_new0(PipeDevice* dev)
{
    HwPipe*  pipe;
    pipe = g_malloc0(sizeof(HwPipe));
    pipe->device = dev;
    return pipe;
}

static HwPipe*
pipe_new(uint64_t channel, PipeDevice* dev)
{
    HwPipe*  pipe = pipe_new0(dev);
    pipe->channel = channel;
    pipe->pipe  = android_pipe_new(pipe);
    qemu_mutex_init(&pipe->lock);
    return pipe;
}

static void pipe_free(HwPipe* pipe)
{
    android_pipe_free(pipe->pipe);
    /* Free stuff */
    /* note: should call destroy mutex after android_pipe_free
       because it will call qemu2_android_pipe_wake, and
       mutex could also be called in "android_pipe_free"
       */
    qemu_mutex_destroy(&pipe->lock);
    g_free(pipe);
}

/***********************************************************************
 ***********************************************************************
 *****
 *****    G O L D F I S H   P I P E   D E V I C E
 *****
 *****/

struct PipeDevice {
    AndroidPipeState *ps;       // FIXME: backlink to instance state

    // the list of all pipes
    HwPipe*  pipes;
    HwPipe*  save_pipes;
    HwPipe*  cache_pipe;
    HwPipe*  cache_pipe_64bit;

    // cache of the pipes by channel for a faster lookup
    GHashTable* pipes_by_channel;

    QemuMutex lock;

    // i/o registers
    uint64_t  address;
    uint32_t  size;
    uint32_t  status;
    uint64_t  channel;
    uint32_t  wakes;
    uint64_t  params_addr;
};

// hashtable-related functions
// 64-bit emulator just casts uint64_t to gpointer to get a key for the
// GLib's hash table. 32-bit one has to dynamically allocate an 8-byte chunk
// of memory and copy the channel value there, storing pointer as a key.
static uint64_t hash_channel_from_key(gconstpointer key) {
#ifdef __x86_64__
    // keys are just channels
    return (uint64_t)key;
#else
    // keys are pointers to channels
    return *(uint64_t*)key;
#endif
}

static gpointer hash_create_key_from_channel(uint64_t channel) {
#ifdef __x86_64__
    return (gpointer)channel;
#else
    uint64_t* on_heap = (uint64_t*)malloc(sizeof(channel));
    if (!on_heap) {
        APANIC("%s: failed to allocate RAM for a channel value\n", __func__);
    }
    *on_heap = channel;
    return on_heap;
#endif
}

static gconstpointer hash_cast_key_from_channel(const uint64_t* channel) {
#ifdef __x86_64__
    return (gconstpointer)*channel;
#else
    return (gconstpointer)channel;
#endif
}

static guint hash_channel(gconstpointer key) {
    uint64_t channel = hash_channel_from_key(key);
    return (guint)(channel ^ (channel >> 6));
}

static gboolean hash_channel_equal(gconstpointer a, gconstpointer b) {
    return hash_channel_from_key(a) == hash_channel_from_key(b);
}

#ifdef __x86_64__
// we don't need to free channels in 64-bit emulator as we store them in-place
static void (*hash_channel_destroy)(gpointer a) = NULL;
#else
static void hash_channel_destroy(gpointer a) {
    free((uint64_t*)a);
}
#endif

// Cache pipe operations
static HwPipe* get_and_clear_cache_pipe(PipeDevice* dev) {
    if (dev->cache_pipe_64bit) {
        HwPipe* val = dev->cache_pipe_64bit;
        dev->cache_pipe_64bit = NULL;
        return val;
    }
    qemu_mutex_lock(&dev->lock);
    HwPipe* val = dev->cache_pipe;
    dev->cache_pipe = NULL;
    qemu_mutex_unlock(&dev->lock);
    return val;
}

static void set_cache_pipe(PipeDevice* dev, HwPipe* cache_pipe) {
    qemu_mutex_lock(&dev->lock);
    dev->cache_pipe = cache_pipe;
    qemu_mutex_unlock(&dev->lock);
}

static void clear_cache_pipe_if_equal(PipeDevice* dev, HwPipe* pipe) {
    qemu_mutex_lock(&dev->lock);
    if (dev->cache_pipe == pipe) {
        dev->cache_pipe = NULL;
    }
    if (dev->cache_pipe_64bit == pipe) {
        dev->cache_pipe_64bit = NULL;
    }
    qemu_mutex_unlock(&dev->lock);
}

/* Update this version number if the device's interface changes. */
#define PIPE_DEVICE_VERSION  1

/* Map the guest buffer specified by the guest paddr 'phys'.
 * Returns a host pointer which should be unmapped later via
 * cpu_physical_memory_unmap(), or NULL if mapping failed (likely
 * because the paddr doesn't actually point at RAM).
 * Note that for RAM the "mapping" process doesn't actually involve a
 * data copy.
 */
static void *map_guest_buffer(hwaddr phys, size_t size, int is_write)
{
    hwaddr l = size;
    void *ptr;

    ptr = cpu_physical_memory_map(phys, &l, is_write);
    if (!ptr) {
        /* Can't happen for RAM */
        return NULL;
    }
    if (l != size) {
        /* This will only happen if the address pointed at non-RAM,
         * or if the size means the buffer end is beyond the end of
         * the RAM block.
         */
        cpu_physical_memory_unmap(ptr, l, 0, 0);
        return NULL;
    }

    return ptr;
}

static void pipeDevice_doCommand(PipeDevice* dev, uint32_t command) {
    HwPipe* pipe = (HwPipe*)g_hash_table_lookup(
            dev->pipes_by_channel, hash_cast_key_from_channel(&dev->channel));

    /* Check that we're referring a known pipe channel */
    if (command != PIPE_CMD_OPEN && pipe == NULL) {
        dev->status = PIPE_ERROR_INVAL;
        return;
    }

    /* If the pipe is closed by the host, return an error */
    if (pipe != NULL && pipe->closed && command != PIPE_CMD_CLOSE) {
        dev->status = PIPE_ERROR_IO;
        return;
    }

    switch (command) {
    case PIPE_CMD_OPEN:
        DD("%s: CMD_OPEN channel=0x%llx", __FUNCTION__, (unsigned long long)dev->channel);
        if (pipe != NULL) {
            dev->status = PIPE_ERROR_INVAL;
            break;
        }
        pipe = pipe_new(dev->channel, dev);
        pipe->next = dev->pipes;
        dev->pipes = pipe;
        dev->save_pipes = dev->pipes;
        dev->status = 0;
        g_hash_table_insert(dev->pipes_by_channel,
                            hash_create_key_from_channel(dev->channel), pipe);
        break;

    case PIPE_CMD_CLOSE: {
        DD("%s: CMD_CLOSE channel=0x%llx", __FUNCTION__, (unsigned long long)dev->channel);
        // Remove from device's lists.
        // This linear lookup is potentially slow, but we don't delete pipes
        // often enough for it to become noticable.
        HwPipe** pnode = &dev->pipes;
        while (*pnode && *pnode != pipe) {
            pnode = &(*pnode)->next;
        }
        if (!*pnode) {
            dev->status = PIPE_ERROR_INVAL;
            break;
        }
        *pnode = pipe->next;
        pipe->next = NULL;
        dev->save_pipes = dev->pipes;
        g_hash_table_remove(dev->pipes_by_channel,
                            hash_cast_key_from_channel(&pipe->channel));

        // clear the device's cache_pipe if we're closing it now
        clear_cache_pipe_if_equal(dev, pipe);

        pipe_free(pipe);
        break;
    }

    case PIPE_CMD_POLL:
        dev->status = android_pipe_poll(pipe->pipe);
        DD("%s: CMD_POLL > status=%d", __FUNCTION__, dev->status);
        break;

    case PIPE_CMD_READ_BUFFER: {
        /* Translate guest physical address into emulator memory. */
        AndroidPipeBuffer  buffer;
        buffer.data = map_guest_buffer(dev->address, dev->size, 1);
        if (!buffer.data) {
            dev->status = PIPE_ERROR_INVAL;
            break;
        }
        buffer.size = dev->size;
        dev->status = android_pipe_recv(pipe->pipe, &buffer, 1);
        DD("%s: CMD_READ_BUFFER channel=0x%llx address=0x%16llx size=%d > status=%d",
           __FUNCTION__, (unsigned long long)dev->channel, (unsigned long long)dev->address,
           dev->size, dev->status);
        cpu_physical_memory_unmap(buffer.data, dev->size, 1, dev->size);
        break;
    }

    case PIPE_CMD_WRITE_BUFFER: {
        /* Translate guest physical address into emulator memory. */
        AndroidPipeBuffer  buffer;
        buffer.data = map_guest_buffer(dev->address, dev->size, 0);
        if (!buffer.data) {
            dev->status = PIPE_ERROR_INVAL;
            break;
        }
        buffer.size = dev->size;
        dev->status = android_pipe_send(pipe->pipe, &buffer, 1);
        DD("%s: CMD_WRITE_BUFFER channel=0x%llx address=0x%16llx size=%d > status=%d",
           __FUNCTION__, (unsigned long long)dev->channel, (unsigned long long)dev->address,
           dev->size, dev->status);
        cpu_physical_memory_unmap(buffer.data, dev->size, 0, dev->size);
        break;
    }

    case PIPE_CMD_WAKE_ON_READ:
        DD("%s: CMD_WAKE_ON_READ channel=0x%llx", __FUNCTION__, (unsigned long long)dev->channel);
        if ((pipe->wanted & PIPE_WAKE_READ) == 0) {
            pipe->wanted |= PIPE_WAKE_READ;
            android_pipe_wake_on(pipe->pipe, pipe->wanted);
        }
        dev->status = 0;
        break;

    case PIPE_CMD_WAKE_ON_WRITE:
        DD("%s: CMD_WAKE_ON_WRITE channel=0x%llx", __FUNCTION__, (unsigned long long)dev->channel);
        if ((pipe->wanted & PIPE_WAKE_WRITE) == 0) {
            pipe->wanted |= PIPE_WAKE_WRITE;
            android_pipe_wake_on(pipe->pipe, pipe->wanted);
        }
        dev->status = 0;
        break;

    default:
        D("%s: command=%d (0x%x)\n", __FUNCTION__, command, command);
    }
}

static void pipe_dev_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    AndroidPipeState *state = (AndroidPipeState *) opaque;
    PipeDevice *s = state->dev;

    DR("%s: offset = 0x%" HWADDR_PRIx " value=%" PRIu64 "/0x%" PRIx64,
       __func__, offset, value, value);
    switch (offset) {
    case PIPE_REG_COMMAND:
        pipeDevice_doCommand(s, value);
        break;

    case PIPE_REG_SIZE:
        s->size = value;
        break;

    case PIPE_REG_ADDRESS:
        uint64_set_low(&s->address, value);
        break;

    case PIPE_REG_ADDRESS_HIGH:
        uint64_set_high(&s->address, value);
        break;

    case PIPE_REG_CHANNEL:
        uint64_set_low(&s->channel, value);
        break;

    case PIPE_REG_CHANNEL_HIGH:
        uint64_set_high(&s->channel, value);
        break;

    case PIPE_REG_PARAMS_ADDR_HIGH:
        uint64_set_high(&s->params_addr, value);
        break;

    case PIPE_REG_PARAMS_ADDR_LOW:
        uint64_set_low(&s->params_addr, value);
        break;

    case PIPE_REG_ACCESS_PARAMS:
    {
        union access_params aps;
        uint32_t cmd;
        bool is_64bit = true;

        /* Don't touch aps.result if anything wrong */
        if (s->params_addr == 0)
            break;

        cpu_physical_memory_read(s->params_addr, (void*)&aps, sizeof(aps.aps32));

        /* This auto-detection of 32bit/64bit ness relies on the
         * currently unused flags parameter. As the 32 bit flags
         * overlaps with the 64 bit cmd parameter. As cmd != 0 if we
         * find it as 0 it's 32bit
         */
        if (aps.aps32.flags == 0) {
            is_64bit = false;
        } else {
            cpu_physical_memory_read(s->params_addr, (void*)&aps, sizeof(aps.aps64));
        }

        if (is_64bit) {
            s->channel = aps.aps64.channel;
            s->size = aps.aps64.size;
            s->address = aps.aps64.address;
            cmd = aps.aps64.cmd;
        } else {
            s->channel = aps.aps32.channel;
            s->size = aps.aps32.size;
            s->address = aps.aps32.address;
            cmd = aps.aps32.cmd;
        }

        if ((cmd != PIPE_CMD_READ_BUFFER) && (cmd != PIPE_CMD_WRITE_BUFFER))
            break;

        pipeDevice_doCommand(s, cmd);

        if (is_64bit) {
            aps.aps64.result = s->status;
            cpu_physical_memory_write(s->params_addr, (void*)&aps, sizeof(aps.aps64));
        } else {
            aps.aps32.result = s->status;
            cpu_physical_memory_write(s->params_addr, (void*)&aps, sizeof(aps.aps32));
        }
    }
    break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: unknown register offset = 0x%"
                      HWADDR_PRIx " value=%" PRIu64 "/0x%" PRIx64 "\n",
                      __func__, offset, value, value);
        break;
    }
}

/* I/O read */
static uint64_t pipe_dev_read(void *opaque, hwaddr offset, unsigned size)
{
    AndroidPipeState *s = (AndroidPipeState *)opaque;
    PipeDevice *dev = s->dev;
    HwPipe* cache_pipe = NULL;

    switch (offset) {
    case PIPE_REG_STATUS:
        DR("%s: REG_STATUS status=%d (0x%x)", __FUNCTION__, dev->status, dev->status);
        return dev->status;

    case PIPE_REG_CHANNEL: {
        cache_pipe = get_and_clear_cache_pipe(dev);
        if (cache_pipe != NULL) {
            dev->wakes = get_and_clear_pipe_wanted(cache_pipe);
            return (uint32_t)(cache_pipe->channel & 0xFFFFFFFFUL);
        }

        HwPipe* pipe = dev->pipes;
        const bool hadPipesInList = (pipe != NULL);

        // find the next pipe to wake
        while (pipe && pipe->wanted == 0) {
            pipe = pipe->next;
        }
        if (!pipe) {
            // no pending pipes - restore the pipes list
            dev->pipes = dev->save_pipes;
            if (hadPipesInList) {
                // This means we had some pipes on the previous call and didn't
                // reset the IRQ yet; let's do it now.
                qemu_set_irq(s->irq, 0);
            }
            DD("%s: no signaled channels%s", __FUNCTION__,
               hasPipesInList ? ", lowering IRQ" : "");
            return 0;
        }

        DR("%s: channel=0x%llx wanted=%d", __FUNCTION__,
           (unsigned long long)pipe->channel, pipe->wanted);
        dev->wakes = get_and_clear_pipe_wanted(pipe);
        dev->pipes = pipe->next;
        if (dev->pipes == NULL) {
            // no next pipe, lower the IRQ and wait for a next call - that's
            // where we'll restore the pipes list
            qemu_set_irq(s->irq, 0);
            DD("%s: lowering IRQ", __FUNCTION__);
        }
        return (uint32_t)(pipe->channel & 0xFFFFFFFFUL);
    }

    case PIPE_REG_CHANNEL_HIGH: {
        // TODO(zyy): this call is really dangerous; currently the device would
        //  stop the calls as soon as we return 0 here; but it means that if the
        //  channel's upper 32 bits are zeroes (that happens), we won't be able
        //  to wake either that channel or any following ones.
        //  I think we should create a new pipe protocol to deal with this
        //  issue and also reduce chattiness of the pipe communication at the
        //  same time.

        cache_pipe = get_and_clear_cache_pipe(dev);
        if (cache_pipe != NULL) {
            dev->cache_pipe_64bit = cache_pipe;
            assert((uint32_t)(cache_pipe->channel >> 32) != 0);
            return (uint32_t)(cache_pipe->channel >> 32);
        }

        HwPipe* pipe = dev->pipes;
        const bool hadPipesInList = (pipe != NULL);

        // skip all non-waked pipes here
        while (pipe && pipe->wanted == 0) {
            pipe = pipe->next;
        }
        if (!pipe) {
            // no pending pipes - restore the pipes list
            dev->pipes = dev->save_pipes;
            if (hadPipesInList) {
                // This means we had some pipes on the previous call and didn't
                // reset the IRQ yet; let's do it now.
                qemu_set_irq(s->irq, 0);
            }
            DD("%s: no signaled channels%s", __FUNCTION__,
               hasPipesInList ? ", lowering IRQ" : "");
            return 0;
        }

        DR("%s: channel_high=0x%llx wanted=%d", __FUNCTION__,
           (unsigned long long)pipe->channel, pipe->wanted);
        dev->pipes = pipe;
        assert((uint32_t)(pipe->channel >> 32) != 0);
        return (uint32_t)(pipe->channel >> 32);
    }

    case PIPE_REG_WAKES:
        DR("%s: wakes %d", __FUNCTION__, dev->wakes);
        return dev->wakes;

    case PIPE_REG_PARAMS_ADDR_HIGH:
        return (uint32_t)(dev->params_addr >> 32);

    case PIPE_REG_PARAMS_ADDR_LOW:
        return (uint32_t)(dev->params_addr & 0xFFFFFFFFUL);

    case PIPE_REG_VERSION:
        return PIPE_DEVICE_VERSION;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: unknown register %" HWADDR_PRId
                      " (0x%" HWADDR_PRIx ")\n", __FUNCTION__, offset, offset);
    }
    return 0;
}

static const MemoryRegionOps android_pipe_iomem_ops = {
    .read = pipe_dev_read,
    .write = pipe_dev_write,
    .endianness = DEVICE_NATIVE_ENDIAN
};

static void qemu2_android_pipe_wake(void* hwpipe, unsigned flags);
static void qemu2_android_pipe_close(void* hwpipe);

static const AndroidPipeHwFuncs qemu2_android_pipe_hw_funcs = {
    .closeFromHost = qemu2_android_pipe_close,
    .signalWake = qemu2_android_pipe_wake,
};

static void android_pipe_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbdev = SYS_BUS_DEVICE(dev);
    AndroidPipeState *s = ANDROID_PIPE(dev);

    s->dev = (PipeDevice *) g_malloc0(sizeof(PipeDevice));
    s->dev->ps = s; /* HACK: backlink */
    s->dev->cache_pipe = NULL;
    s->dev->cache_pipe_64bit = NULL;
    s->dev->pipes_by_channel = g_hash_table_new_full(
                                   hash_channel, hash_channel_equal,
                                   hash_channel_destroy, NULL);
    if (!s->dev->pipes_by_channel) {
        APANIC("%s: failed to initialize pipes hash\n", __func__);
    }
    qemu_mutex_init(&s->dev->lock);

    memory_region_init_io(&s->iomem, OBJECT(s), &android_pipe_iomem_ops, s,
                          "android_pipe", 0x2000 /*TODO: ?how big?*/);
    sysbus_init_mmio(sbdev, &s->iomem);
    sysbus_init_irq(sbdev, &s->irq);

    android_zero_pipe_init();
    android_pingpong_init();
    android_throttle_init();
    android_init_opengles_pipe(NULL);

    android_pipe_set_hw_funcs(&qemu2_android_pipe_hw_funcs);

    /* TODO: This may be a complete hack and there may be beautiful QOM ways
     * to accomplish this.
     *
     * Initialize adb pipe backends
     */
    android_adb_dbg_backend_init();
}

static void qemu2_android_pipe_wake( void* hwpipe, unsigned flags )
{
    HwPipe*  pipe = hwpipe;
    PipeDevice*  dev = pipe->device;

    DD("%s: channel=0x%llx flags=%d", __FUNCTION__, (unsigned long long)pipe->channel, flags);

    set_pipe_wanted_bits(pipe, (unsigned char)flags);
    if (!pipe->closed) {
        set_cache_pipe(dev, pipe);
    }
    /* Raise IRQ to indicate there are items on our list ! */
    /* android_device_set_irq(&dev->dev, 0, 1);*/
    qemu_set_irq(dev->ps->irq, 1);
    DD("%s: raising IRQ", __FUNCTION__);
}

static void qemu2_android_pipe_close( void* hwpipe )
{
    HwPipe* pipe = hwpipe;

    D("%s: channel=0x%llx (closed=%d)", __FUNCTION__, (unsigned long long)pipe->channel, pipe->closed);

    if (!pipe->closed) {
        pipe->closed = 1;
        qemu2_android_pipe_wake( hwpipe, PIPE_WAKE_CLOSED );
    }
}

static void android_pipe_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = android_pipe_realize;
    dc->desc = "android pipe";
}

static const TypeInfo android_pipe_info = {
    .name          = TYPE_ANDROID_PIPE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AndroidPipeState),
    .class_init    = android_pipe_class_init
};

static void android_pipe_register(void)
{
    type_register_static(&android_pipe_info);
}

type_init(android_pipe_register);

