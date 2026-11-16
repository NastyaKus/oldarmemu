/*
 * I2C peripherals of the HTC HD2:
 *
 *  leo-touch   capacitive touch controller ("type 2A", addr 0x15).
 *              After power-up it answers a 4 byte read with 0x55; after
 *              the D0 00 01 init write every 9 byte read returns a report
 *              (0x5A, packed 12 bit X/Y for up to 2 points, count in
 *              byte 8).  The active-low interrupt line (GPIO 92) is held
 *              low while a report is pending.  See board-htcleo-ts.c.
 *
 *  htc-microp  HTC "MicroP" microcontroller (addr 0x66): LEDs, LCD
 *              backlight, light sensor, ADC.  Command byte + data;
 *              reads return per-command buffers.
 *
 *  i2c-regfile generic 8-bit register file with auto-increment, used for
 *              PMICs / sensors that only need to ACK and remember writes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/qdev-properties.h"
#include "ui/input.h"
#include "migration/vmstate.h"
#include "msm.h"

/* ======================= touch screen ======================= */

OBJECT_DECLARE_SIMPLE_TYPE(LeoTouchState, LEO_TOUCH)

#define TS_X_MIN    6
#define TS_X_MAX    576
#define TS_Y_MIN    6
#define TS_Y_MAX    952
#define TS_REPORT_INTERVAL_NS   (12 * 1000 * 1000)

struct LeoTouchState {
    I2CSlave parent_obj;
    qemu_irq irq_n;             /* active low */
    QEMUTimer *timer;
    QemuInputHandlerState *input;

    bool powered;
    bool initialized;
    bool pending;               /* report waiting to be read */
    uint8_t rx[8];
    int rx_len;
    int tx_pos;
    uint8_t report[9];

    /* host input */
    bool down;
    uint32_t x, y;              /* panel coordinates */
    bool was_down;
};

static void ts_update_irq(LeoTouchState *s)
{
    qemu_set_irq(s->irq_n, !(s->powered && s->pending));
}

static void ts_build_report(LeoTouchState *s)
{
    uint8_t *b = s->report;

    memset(b, 0, sizeof(s->report));
    b[0] = 0x5A;
    if (s->down) {
        b[1] = ((s->x >> 8) & 0xf) << 4 | ((s->y >> 8) & 0xf);
        b[2] = s->x & 0xff;
        b[3] = s->y & 0xff;
        b[8] = 1 << 1;
    }
}

static void ts_queue_report(LeoTouchState *s)
{
    if (!s->powered || !s->initialized) {
        return;
    }
    ts_build_report(s);
    s->pending = true;
    ts_update_irq(s);
}

/* while the finger is down keep reporting, like the real controller */
static void ts_timer_cb(void *opaque)
{
    LeoTouchState *s = opaque;
    if (s->down) {
        ts_queue_report(s);
        timer_mod(s->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TS_REPORT_INTERVAL_NS);
    }
}

static void ts_power(void *opaque, int n, int level)
{
    LeoTouchState *s = opaque;

    if (level && !s->powered) {
        s->powered = true;
        s->initialized = false;
        s->pending = true;      /* controller signals "ready" */
    } else if (!level) {
        s->powered = false;
        s->pending = false;
    }
    ts_update_irq(s);
}

static int ts_event(I2CSlave *i2c, enum i2c_event ev)
{
    LeoTouchState *s = LEO_TOUCH(i2c);

    switch (ev) {
    case I2C_START_SEND:
        s->rx_len = 0;
        break;
    case I2C_START_RECV:
        s->tx_pos = 0;
        if (s->initialized && !s->pending) {
            ts_build_report(s);
        }
        break;
    case I2C_FINISH:
        if (s->rx_len >= 1 && s->rx[0] == 0xD0) {
            s->initialized = true;
            s->pending = false;
            ts_update_irq(s);
        }
        break;
    default:
        break;
    }
    return 0;
}

static uint8_t ts_recv(I2CSlave *i2c)
{
    LeoTouchState *s = LEO_TOUCH(i2c);
    uint8_t v;

    if (!s->initialized) {
        v = (s->tx_pos == 0) ? 0x55 : 0x00;
        s->pending = false;
    } else {
        v = s->tx_pos < sizeof(s->report) ? s->report[s->tx_pos] : 0;
        if (s->tx_pos == sizeof(s->report) - 1) {
            s->pending = false;
        }
    }
    s->tx_pos++;
    ts_update_irq(s);
    return v;
}

static int ts_send(I2CSlave *i2c, uint8_t data)
{
    LeoTouchState *s = LEO_TOUCH(i2c);
    if (s->rx_len < sizeof(s->rx)) {
        s->rx[s->rx_len++] = data;
    }
    return 0;
}

static void ts_input_event(DeviceState *dev, QemuConsole *src,
                           InputEvent *evt)
{
    LeoTouchState *s = LEO_TOUCH(dev);
    InputMoveEvent *move;
    InputBtnEvent *btn;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        move = evt->u.abs.data;
        if (move->axis == INPUT_AXIS_X) {
            s->x = TS_X_MIN + (uint64_t)move->value * (TS_X_MAX - TS_X_MIN) /
                   INPUT_EVENT_ABS_MAX;
        } else if (move->axis == INPUT_AXIS_Y) {
            s->y = TS_Y_MIN + (uint64_t)move->value * (TS_Y_MAX - TS_Y_MIN) /
                   INPUT_EVENT_ABS_MAX;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        btn = evt->u.btn.data;
        if (btn->button == INPUT_BUTTON_LEFT) {
            s->down = btn->down;
        }
        break;
    default:
        break;
    }
}

static void ts_input_sync(DeviceState *dev)
{
    LeoTouchState *s = LEO_TOUCH(dev);

    if (s->down || s->was_down) {
        ts_queue_report(s);
    }
    if (s->down && !s->was_down) {
        timer_mod(s->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TS_REPORT_INTERVAL_NS);
    }
    s->was_down = s->down;
}

static const QemuInputHandler ts_input_handler = {
    .name  = "HTC HD2 touchscreen",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = ts_input_event,
    .sync  = ts_input_sync,
};

static void ts_realize(DeviceState *dev, Error **errp)
{
    LeoTouchState *s = LEO_TOUCH(dev);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ts_timer_cb, s);
    s->input = qemu_input_handler_register(dev, &ts_input_handler);
    qemu_input_handler_activate(s->input);
}

static void ts_reset(DeviceState *dev)
{
    LeoTouchState *s = LEO_TOUCH(dev);
    s->powered = s->initialized = s->pending = false;
    s->down = s->was_down = false;
    s->rx_len = s->tx_pos = 0;
    ts_update_irq(s);
}

static void ts_init(Object *obj)
{
    LeoTouchState *s = LEO_TOUCH(obj);
    qdev_init_gpio_out_named(DEVICE(obj), &s->irq_n, "irq-n", 1);
    qdev_init_gpio_in_named(DEVICE(obj), ts_power, "power", 1);
}

static const VMStateDescription vmstate_leo_touch = {
    .name = "leo-touch",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, LeoTouchState),
        VMSTATE_BOOL(powered, LeoTouchState),
        VMSTATE_BOOL(initialized, LeoTouchState),
        VMSTATE_BOOL(pending, LeoTouchState),
        VMSTATE_END_OF_LIST()
    }
};

static void ts_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    dc->realize = ts_realize;
    device_class_set_legacy_reset(dc, ts_reset);
    dc->vmsd = &vmstate_leo_touch;
    k->event = ts_event;
    k->recv = ts_recv;
    k->send = ts_send;
}

/* ======================= HTC MicroP ======================= */

OBJECT_DECLARE_SIMPLE_TYPE(HtcMicropState, HTC_MICROP)

#define MICROP_RCMD_VERSION         0x30
#define MICROP_RCMD_LSENSOR         0x33
#define MICROP_WCMD_LCM_BL_MANU_CTL 0x22
#define MICROP_RCMD_ADC_VALUE       0x62
#define MICROP_RCMD_GPI_INT_STATUS  0x82
#define MICROP_RCMD_GPIO_STATUS     0x83

struct HtcMicropState {
    I2CSlave parent_obj;
    uint8_t cmd;
    bool have_cmd;
    int pos;
    uint8_t data[256][8];
};

static int microp_event(I2CSlave *i2c, enum i2c_event ev)
{
    HtcMicropState *s = HTC_MICROP(i2c);

    if (ev == I2C_START_SEND) {
        s->have_cmd = false;
    }
    s->pos = 0;
    return 0;
}

static uint8_t microp_recv(I2CSlave *i2c)
{
    HtcMicropState *s = HTC_MICROP(i2c);
    uint8_t v = s->pos < 8 ? s->data[s->cmd][s->pos] : 0;
    s->pos++;
    return v;
}

static int microp_send(I2CSlave *i2c, uint8_t d)
{
    HtcMicropState *s = HTC_MICROP(i2c);

    if (!s->have_cmd) {
        s->cmd = d;
        s->have_cmd = true;
        s->pos = 0;
        return 0;
    }
    /* reads of these commands return device state, not echoed writes */
    switch (s->cmd) {
    case MICROP_RCMD_VERSION:
    case MICROP_RCMD_LSENSOR:
    case MICROP_RCMD_ADC_VALUE:
        break;
    default:
        if (s->pos < 8) {
            s->data[s->cmd][s->pos] = d;
        }
        break;
    }
    if (s->cmd == MICROP_WCMD_LCM_BL_MANU_CTL && s->pos == 1) {
        qemu_log_mask(LOG_UNIMP, "htc-microp: backlight level %u\n", d);
    }
    s->pos++;
    return 0;
}

static void microp_reset(DeviceState *dev)
{
    HtcMicropState *s = HTC_MICROP(dev);

    memset(s->data, 0, sizeof(s->data));
    s->data[MICROP_RCMD_VERSION][0] = 0x0B;     /* firmware 0x0B05 */
    s->data[MICROP_RCMD_VERSION][1] = 0x05;
    s->data[MICROP_RCMD_LSENSOR][0] = 0x00;     /* moderate ambient light */
    s->data[MICROP_RCMD_LSENSOR][1] = 0x80;
    s->data[MICROP_RCMD_ADC_VALUE][0] = 0x01;
    s->data[MICROP_RCMD_ADC_VALUE][1] = 0x00;
    s->have_cmd = false;
    s->pos = 0;
}

static void microp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    device_class_set_legacy_reset(dc, microp_reset);
    k->event = microp_event;
    k->recv = microp_recv;
    k->send = microp_send;
}

/* ======================= generic register file ======================= */

OBJECT_DECLARE_SIMPLE_TYPE(I2cRegfileState, I2C_REGFILE)

struct I2cRegfileState {
    I2CSlave parent_obj;
    uint8_t regs[256];
    uint8_t ptr;
    bool have_ptr;
};

static int regfile_event(I2CSlave *i2c, enum i2c_event ev)
{
    I2cRegfileState *s = I2C_REGFILE(i2c);
    if (ev == I2C_START_SEND) {
        s->have_ptr = false;
    }
    return 0;
}

static uint8_t regfile_recv(I2CSlave *i2c)
{
    I2cRegfileState *s = I2C_REGFILE(i2c);
    return s->regs[s->ptr++];
}

static int regfile_send(I2CSlave *i2c, uint8_t d)
{
    I2cRegfileState *s = I2C_REGFILE(i2c);
    if (!s->have_ptr) {
        s->ptr = d;
        s->have_ptr = true;
    } else {
        s->regs[s->ptr++] = d;
    }
    return 0;
}

static void regfile_reset(DeviceState *dev)
{
    I2cRegfileState *s = I2C_REGFILE(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->ptr = 0;
    s->have_ptr = false;
}

static void regfile_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    device_class_set_legacy_reset(dc, regfile_reset);
    k->event = regfile_event;
    k->recv = regfile_recv;
    k->send = regfile_send;
}

/* ======================= DS2746 fuel gauge ======================= */

/*
 * Maxim DS2746 battery monitor (I2C 0x26), read by the HD2 kernel's
 * ds2746_battery driver every few seconds.  Present a healthy, full
 * battery: without it the gauge reads as missing/empty and Android
 * powers the phone off.  Register values follow the driver's scaling
 * (drivers/power/ds2746_param.c):
 *   VOLT  (0x0c/0x0d) >> 4  * 2.44 mV
 *   AUX0  (0x08/0x09) >> 4  doubles as battery ID and NTC temperature
 *   ACR   (0x10/0x11)       * 625/1580 - 500 mAh
 */
#define TYPE_LEO_DS2746 "leo-ds2746"

static void ds2746_reset(DeviceState *dev)
{
    I2cRegfileState *s = I2C_REGFILE(dev);
    uint16_t volt = (4150 * 100 / 244) << 4;         /* 4.15 V */
    /*
     * 30 C (600k NTC table), also read as a 1230 mAh pack.  Not 25.0 C:
     * the driver starts from "25.0 C, 99%" and only reports the level to
     * htc_battery when one of them changes, so Android would see 0% (and
     * power off) until the next poll 50 s later.
     */
    uint16_t aux0 = 255 << 4;
    uint16_t acr = (1230 + 500) * 1580 / 625 + 1;     /* full */

    regfile_reset(dev);
    s->regs[0x01] = 0x03;                   /* STATUS: AIN0 | AIN1 ready */
    s->regs[0x08] = aux0 >> 8;
    s->regs[0x09] = aux0;
    s->regs[0x0a] = aux0 >> 8;              /* AUX1 */
    s->regs[0x0b] = aux0;
    s->regs[0x0c] = volt >> 8;
    s->regs[0x0d] = volt;
    s->regs[0x0e] = 0;                      /* CURRENT: 0 mA */
    s->regs[0x0f] = 0;
    s->regs[0x10] = acr >> 8;
    s->regs[0x11] = acr;
}

static void ds2746_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), ds2746_reset);
}

static const TypeInfo leo_i2c_types[] = {
    {
        .name = TYPE_LEO_TOUCH,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(LeoTouchState),
        .instance_init = ts_init,
        .class_init = ts_class_init,
    }, {
        .name = TYPE_HTC_MICROP,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(HtcMicropState),
        .class_init = microp_class_init,
    }, {
        .name = TYPE_I2C_REGFILE,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(I2cRegfileState),
        .class_init = regfile_class_init,
    }, {
        .name = TYPE_LEO_DS2746,
        .parent = TYPE_I2C_REGFILE,
        .class_init = ds2746_class_init,
    },
};

DEFINE_TYPES(leo_i2c_types)
