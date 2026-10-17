#define DT_DRV_COMPAT avago_a320

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdlib.h>

#include "a320.h"

LOG_MODULE_REGISTER(A320, CONFIG_SENSOR_LOG_LEVEL);

static int a320_read_reg(const struct device *dev, uint8_t reg_addr) {
    const struct a320_config *cfg = dev->config;
    uint8_t value = 0;

    if (!i2c_reg_read_byte_dt(&cfg->bus, reg_addr, &value)) {
        return value;
    }

    LOG_ERR("failed to read 0x%x register", reg_addr);
    return -1;
}

// The sensor's own finger-navigation features (register 0x60, OFN_Engine)
// are all off after power-up, and this driver leaves them off by default.
//
// One of them, assert/de-assert, stops the sensor reporting motion once its
// shutter value shows the finger leaving the surface, which stops the
// pointer jumping as a thumb is lifted. It was switched on for a while
// (0xA0) and then taken out again: it works from fixed shutter thresholds,
// and on some trackpads those reject a bare finger altogether, so the
// trackpad only responded through something more reflective. Where it did
// work it also made tracking a little uneven.
//
// CONFIG_INPUT_A320_OFN_ENGINE is what the register should hold, 0x00 unless
// someone is experimenting. It is written even when it is 0x00: the sensor
// is not reset when the keyboard firmware restarts or is reflashed, so it
// can still hold what an earlier firmware wrote. The write is read back to
// confirm and retried soon if that fails. A non-zero value is also checked
// every so often, in case the sensor has reset itself.
#define A320_ENGINE_RETRY_POLLS 16
#define A320_ENGINE_RECHECK_POLLS 1024

static int a320_write_reg(const struct device *dev, uint8_t reg_addr, uint8_t value) {
    const struct a320_config *cfg = dev->config;

    return i2c_reg_write_byte_dt(&cfg->bus, reg_addr, value);
}

static bool a320_apply_engine(const struct device *dev, uint8_t value) {
    if (a320_read_reg(dev, OFN_Engine) == value) {
        return true;
    }
    if (a320_write_reg(dev, OFN_Engine, value) != 0) {
        return false;
    }
    return a320_read_reg(dev, OFN_Engine) == value;
}

static void a320_sync_engine(const struct device *dev) {
    static uint32_t polls;
    static bool applied;

    if (applied) {
        if (CONFIG_INPUT_A320_OFN_ENGINE == 0 || (++polls % A320_ENGINE_RECHECK_POLLS) != 0) {
            return;
        }
    } else if ((polls++ % A320_ENGINE_RETRY_POLLS) != 0) {
        return;
    }

    applied = a320_apply_engine(dev, CONFIG_INPUT_A320_OFN_ENGINE);
    if (!applied) {
        LOG_ERR("failed to set OFN_Engine");
    }
}

#if IS_ENABLED(CONFIG_INPUT_A320_DIAG)
// Diagnostics for tuning lift detection: what the sensor sees (surface
// quality, shutter, pixel levels) alongside the motion it reports. Lines are
// logged when there is motion, when the readings shift, and twice a second
// otherwise. Read them over USB logging (CONFIG_ZMK_USB_LOGGING).
static void a320_diag_dump_registers(const struct device *dev) {
    static const uint8_t regs[] = {
        Product_ID,      Revision_ID,     Inverse_Product_ID, Inverse_Revision_ID,
        Configuration_Bits, LED_Control,  IO_Mode,            Motion_Control,
        Shutter_Max_Hi,  Shutter_Max_Lo,  OFN_Engine,         0x61,
        OFN_Resolution,  OFN_Speed_Control, OFN_AD_CTRL,      OFN_AD_ATH_HIGH,
        OFN_AD_DTH_HIGH, OFN_AD_ATH_LOW,  OFN_AD_DTH_LOW,     OFN_Quantize_CTRL,
        OFN_XYQ_THRESH,  OFN_FPD_CTRL,    OFN_Orientation_CTRL,
    };

    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        LOG_INF("tpreg 0x%02x = 0x%02x", regs[i], a320_read_reg(dev, regs[i]) & 0xff);
    }
}

static void a320_diag_sample(const struct device *dev, uint8_t motion, int8_t dx, int8_t dy) {
    static uint32_t polls;
    static uint32_t last_log_ms;
    static int last_squal = -1;
    static int last_shutter = -1;

    // The register dump is repeated now and then so that it is not missed if
    // the log is opened late.
    if ((polls++ % 3000) == 0) {
        a320_diag_dump_registers(dev);
    }

    const int squal = a320_read_reg(dev, SQUAL) & 0xff;
    const int shutter =
        ((a320_read_reg(dev, Shutter_Upper) & 0xff) << 8) | (a320_read_reg(dev, Shutter_Lower) & 0xff);
    const int pix_max = a320_read_reg(dev, Maximum_Pixel) & 0xff;
    const int pix_avg = a320_read_reg(dev, Pixel_Sun) & 0xff;
    const int pix_min = a320_read_reg(dev, Minimum_Pixel) & 0xff;

    const uint32_t now = k_uptime_get_32();
    const bool moved = (motion & BIT_MOTION_MOT) != 0;
    const bool shifted = abs(squal - last_squal) > 3 || abs(shutter - last_shutter) > 16;

    if (moved || shifted || (now - last_log_ms) >= 500) {
        LOG_INF("tp t=%u mot=%02x dx=%d dy=%d sq=%d sh=%d px=%d/%d/%d", now, motion, dx, dy,
                squal, shutter, pix_max, pix_avg, pix_min);
        last_log_ms = now;
        last_squal = squal;
        last_shutter = shutter;
    }
}
#endif

static int a320_sample_fetch(const struct device *dev, enum sensor_channel chan) { return 0; }

static int a320_channel_get(const struct device *dev, enum sensor_channel chan,
                            struct sensor_value *val) {
    a320_sync_engine(dev);

    const int motion = a320_read_reg(dev, Motion);
    if (motion < 0) {
        return -EIO;
    }
    const uint8_t ifmotion = motion;
    const int overflow = a320_read_reg(dev, Motion);
    if (overflow < 0) {
        return -EIO;
    }
    const uint8_t ovflow = overflow;
    if ((ifmotion & BIT_MOTION_MOT) && !(ovflow & BIT_MOTION_OVF)) {
        const int dx = a320_read_reg(dev, Delta_X);
        const int dy = a320_read_reg(dev, Delta_Y);
        if (dx < 0 || dy < 0) {
            return -EIO;
        }
        val->val1 = dx;
        val->val2 = dy;
        LOG_DBG("you get the x value : %d", val->val1);
        LOG_DBG("you get the y value : %d", val->val2);
    } else {
        val->val1 = 0;
        val->val2 = 0;
    }
#if IS_ENABLED(CONFIG_INPUT_A320_DIAG)
    a320_diag_sample(dev, ifmotion, (int8_t)val->val1, (int8_t)val->val2);
#endif
    return 0;
}

static const struct sensor_driver_api a320_driver_api = {
    .sample_fetch = a320_sample_fetch,
    .channel_get = a320_channel_get,
};

static int a320_init(const struct device *dev) {

    const struct a320_config *cfg = dev->config;
    if (!device_is_ready(cfg->bus.bus)) {
        LOG_ERR("I2C bus %s is not ready!", cfg->bus.bus->name);
        return -EINVAL;
    }
    a320_read_reg(dev, Motion);
    a320_read_reg(dev, Motion);
    a320_read_reg(dev, Delta_X);
    a320_read_reg(dev, Delta_Y);
    LOG_DBG("A320 Init done, Ready to read data.");

    return 0;
}

#define A320_DEFINE(inst)                                                                          \
    struct a320_data a3200_data_##inst;                                                            \
    static const struct a320_config a3200_cfg_##inst = {.bus = I2C_DT_SPEC_INST_GET(inst)};        \
    DEVICE_DT_INST_DEFINE(inst, a320_init, NULL, &a3200_data_##inst, &a3200_cfg_##inst,            \
                          POST_KERNEL, 60, &a320_driver_api);

DT_INST_FOREACH_STATUS_OKAY(A320_DEFINE)
