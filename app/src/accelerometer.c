#include <accelerometer.h>
#include <bmi270_port.h>
#include <bmi270.h>
#include <zephyr/logging/log.h>
#include <events/accel_event.h>
#include <zephyr/zbus/zbus.h>

LOG_MODULE_REGISTER(accel, LOG_LEVEL_DBG);

/*! Earth's gravity in m/s^2 */
#define GRAVITY_EARTH  (9.80665f)

ZBUS_CHAN_DECLARE(accel_data_chan);

typedef void(*feature_config_func)(struct bmi2_sens_config *config);

typedef struct bmi270_feature_config_set_t {
    uint8_t                 sensor_id;
    feature_config_func    cfg_func;
} bmi270_feature_config_set_t;

static void send_accel_event(accelerometer_evt_t *data);

static int8_t configure_enable_all_bmi270(struct bmi2_dev *bmi2_dev);
static void configue_accel(struct bmi2_sens_config *config);
static void configue_gyro(struct bmi2_sens_config *config);
static void configure_step_counter(struct bmi2_sens_config *config);
static void configure_anymotion(struct bmi2_sens_config *config);
static void configure_gesture_detect(struct bmi2_sens_config *config);
static void configure_wrist_wakeup(struct bmi2_sens_config *config);

static int bmi270_init_interrupt(void);
static void bmi270_int1_work_cb(struct k_work *work);
static void bmi270_int2_work_cb(struct k_work *work);

// List of the features used on the BMI270.
static bmi270_feature_config_set_t bmi270_enabled_features[] = {
    { .sensor_id = BMI2_ACCEL, .cfg_func = configue_accel},
    // Gyro not used for now, disable to keep power consumption down
    //{ .sensor_id = BMI2_GYRO, .cfg_func = configue_gyro},
    { .sensor_id = BMI2_STEP_COUNTER, .cfg_func = configure_step_counter},
    { .sensor_id = BMI2_SIG_MOTION, .cfg_func = NULL},
    { .sensor_id = BMI2_ANY_MOTION, .cfg_func = configure_anymotion},
    { .sensor_id = BMI2_STEP_ACTIVITY, .cfg_func = NULL},
    { .sensor_id = BMI2_WRIST_GESTURE, .cfg_func = configure_gesture_detect},
    { .sensor_id = BMI2_WRIST_WEAR_WAKE_UP, .cfg_func = configure_wrist_wakeup},
};

static struct bmi2_dev bmi2_dev;

// Assumes only one sensor
static const struct gpio_dt_spec int1_gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(bmi270), int1_gpios);
static const struct gpio_dt_spec int2_gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(bmi270), int2_gpios);
static K_WORK_DEFINE(int1_work, bmi270_int1_work_cb);
static K_WORK_DEFINE(int2_work, bmi270_int2_work_cb);
static struct gpio_callback gpio_int1_cb;
static struct gpio_callback gpio_int2_cb;

int accelerometer_init(void)
{
    int8_t rslt;

    rslt = bmi2_interface_init(&bmi2_dev, BMI2_I2C_INTF);
    bmi2_error_codes_print_result(rslt);

    /* Initialize bmi270. */
    rslt = bmi270_init(&bmi2_dev);
    bmi2_error_codes_print_result(rslt);

    if (rslt == BMI2_OK) {
        /* Accel and gyro configuration settings. */
        rslt = configure_enable_all_bmi270(&bmi2_dev);
        bmi2_error_codes_print_result(rslt);
    }

    if (rslt == BMI2_OK) {
        if (int1_gpio.port) {
            if (bmi270_init_interrupt() < 0) {
                LOG_DBG("Could not initialize interrupts");
                return -EIO;
            }
        }
    }

    return 0;
}

int accelerometer_fetch_xyz(int16_t *x, int16_t *y, int16_t *z)
{
    int8_t rslt;
    struct bmi2_sens_data sensor_data = { { 0 } };

    /* Get accel and gyro data for x, y and z axis. */
    rslt = bmi2_get_sensor_data(&sensor_data, &bmi2_dev);
    bmi2_error_codes_print_result(rslt);

    if (rslt == BMI2_OK) {
        *x = sensor_data.acc.x;
        *y = sensor_data.acc.y;
        *z = sensor_data.acc.z;
    }

    return rslt == BMI2_OK ? 0 : -EIO;
}

int accelerometer_fetch_num_steps(int16_t *num_steps)
{
    return -ENOENT;
}

int accelerometer_fetch_temperature(struct sensor_value *temperature)
{
    return -ENOENT;
}

int accelerometer_reset_step_count(void)
{
    return -ENOENT;
}

static void send_accel_event(accelerometer_evt_t *data)
{
    zbus_chan_pub(&accel_data_chan, data, K_MSEC(250));
}

static inline void setup_int1(bool enable)
{
    gpio_pin_interrupt_configure_dt(&int1_gpio,
                                    (enable ? GPIO_INT_EDGE_RISING : GPIO_INT_DISABLE));
}

static inline void setup_int2(bool enable)
{
    gpio_pin_interrupt_configure_dt(&int2_gpio,
                                    (enable ? GPIO_INT_EDGE_RISING : GPIO_INT_DISABLE));
}

static void bmi270_gpio_int1_callback(const struct device *dev,
                                      struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(pins);

    setup_int1(false);
    k_work_submit(&int1_work);
}

static void bmi270_gpio_int2_callback(const struct device *dev,
                                      struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(pins);

    setup_int2(false);
    k_work_submit(&int2_work);
}

static void bmi270_int1_work_cb(struct k_work *work)
{
    int8_t rslt;
    uint16_t int_status;
    struct bmi2_sens_data sensor_data = { 0 };

    rslt = bmi2_get_int_status(&int_status, &bmi2_dev);
    bmi2_error_codes_print_result(rslt);

    if (int_status & BMI2_GYR_DRDY_INT_MASK || int_status & BMI2_ACC_DRDY_INT_MASK) {
        LOG_DBG("INT1: BMI2_GYR_DRDY_INT_MASK | BMI2_ACC_DRDY_INT_MASK\n");
        rslt = bmi2_get_sensor_data(&sensor_data, &bmi2_dev);
        bmi2_error_codes_print_result(rslt);
        // TODO Do something with this
    }

    setup_int1(true);
}

static void bmi270_int2_work_cb(struct k_work *work)
{
    int8_t rslt;
    uint16_t int_status;
    accelerometer_evt_t evt;
    struct bmi2_feat_sensor_data sensor_data = { 0 };

    rslt = bmi2_get_int_status(&int_status, &bmi2_dev);
    bmi2_error_codes_print_result(rslt);

    if (int_status & BMI270_SIG_MOT_STATUS_MASK) {
        LOG_DBG("INT: BMI270_SIG_MOT_STATUS_MASK\n");
        evt.type = ACCELEROMETER_EVT_TYPE_SIGNIFICANT_MOTION;
        send_accel_event(&evt);
    } else if (int_status & BMI270_STEP_CNT_STATUS_MASK) {
        LOG_DBG("INT: BMI270_STEP_CNT_STATUS_MASK\n");
        sensor_data.type = BMI2_STEP_COUNTER;
        rslt = bmi270_get_feature_data(&sensor_data, 1, &bmi2_dev);
        bmi2_error_codes_print_result(rslt);
        evt.type = ACCELEROMETER_EVT_TYPE_STEP;
        evt.data.step.count = (long unsigned int)sensor_data.sens_data.step_counter_output;
        send_accel_event(&evt);
        LOG_DBG("No of steps counted  = %lu", (long unsigned int)evt.data.step.count );
    } else if (int_status & BMI270_STEP_ACT_STATUS_MASK) {
        LOG_DBG("INT: BMI270_STEP_ACT_STATUS_MASK\n");
        sensor_data.type = BMI2_STEP_ACTIVITY;
        rslt = bmi270_get_feature_data(&sensor_data, 1, &bmi2_dev);
        bmi2_error_codes_print_result(rslt);
        evt.type = ACCELEROMETER_EVT_TYPE_STEP_ACTIVITY;
        evt.data.step_activity = (accelerometer_data_step_activity_t)sensor_data.sens_data.activity_output;
        const char *activity_output[4] = { "BMI2_STILL", "BMI2_WALK", "BMI2_RUN", "BMI2_UNKNOWN" };
        LOG_DBG("Step activity: %s", activity_output[sensor_data.sens_data.activity_output]);
        send_accel_event(&evt);
    } else if (int_status & BMI270_WRIST_WAKE_UP_STATUS_MASK) {
        LOG_DBG("INT: BMI270_WRIST_WAKE_UP_STATUS_MASK\n");
        evt.type = ACCELEROMETER_EVT_TYPE_WRIST_WAKEUP;
        send_accel_event(&evt);
    } else if (int_status & BMI270_WRIST_GEST_STATUS_MASK) {
        LOG_DBG("INT: BMI270_WRIST_GEST_STATUS_MASK\n");
        sensor_data.type = BMI2_WRIST_GESTURE;
        rslt = bmi270_get_feature_data(&sensor_data, 1, &bmi2_dev);
        bmi2_error_codes_print_result(rslt);
        evt.type = ACCELEROMETER_EVT_TYPE_GESTURE;
        evt.data.gesture = (accelerometer_data_step_gesture_t)sensor_data.sens_data.wrist_gesture_output;
        const char *gesture_output[6] = { "unknown_gesture", "push_arm_down", "pivot_up", "wrist_shake_jiggle", "flick_in", "flick_out" };
        LOG_DBG("Gesture detected: %s", gesture_output[sensor_data.sens_data.wrist_gesture_output]);
        send_accel_event(&evt);
    } else if (int_status & BMI270_NO_MOT_STATUS_MASK) {
    } else if (int_status & BMI270_ANY_MOT_STATUS_MASK) {
    }

    setup_int2(true);
}

static int bmi270_init_interrupt(void)
{
    /* setup data ready and feature gpio interrupt */
    if (!device_is_ready(int1_gpio.port)) {
        LOG_ERR("GPIO device not ready");
        return -ENODEV;
    }
    if (!device_is_ready(int2_gpio.port)) {
        LOG_ERR("GPIO device not ready");
        return -ENODEV;
    }

    gpio_pin_configure_dt(&int1_gpio, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure_dt(&int2_gpio, GPIO_INPUT | GPIO_PULL_DOWN);

    gpio_init_callback(&gpio_int1_cb,
                       bmi270_gpio_int1_callback,
                       BIT(int1_gpio.pin));
    gpio_init_callback(&gpio_int2_cb,
                       bmi270_gpio_int2_callback,
                       BIT(int2_gpio.pin));

    if (gpio_add_callback(int1_gpio.port, &gpio_int1_cb) < 0) {
        LOG_DBG("Could not set gpio1 callback");
        return -EIO;
    }
    if (gpio_add_callback(int2_gpio.port, &gpio_int2_cb) < 0) {
        LOG_DBG("Could not set gpio2 callback");
        return -EIO;
    }

    setup_int1(true);
    setup_int2(true);

    return 0;
}

static void configue_accel(struct bmi2_sens_config *config)
{
    /* NOTE: The user can change the following configuration parameters according to their requirement. */
    /* Set Output Data Rate */
    config->cfg.acc.odr = BMI2_ACC_ODR_100HZ;

    /* Gravity range of the sensor (+/- 2G, 4G, 8G, 16G). */
    config->cfg.acc.range = BMI2_ACC_RANGE_2G;

    /* The bandwidth parameter is used to configure the number of sensor samples that are averaged
        * if it is set to 2, then 2^(bandwidth parameter) samples
        * are averaged, resulting in 4 averaged samples.
        * Note1 : For more information, refer the datasheet.
        * Note2 : A higher number of averaged samples will result in a lower noise level of the signal, but
        * this has an adverse effect on the power consumed.
        */
    config->cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;

    /* Enable the filter performance mode where averaging of samples
        * will be done based on above set bandwidth and ODR.
        * There are two modes
        *  0 -> Ultra low power mode
        *  1 -> High performance mode(Default)
        * For more info refer datasheet.
        */
    config->cfg.acc.filter_perf = BMI2_POWER_OPT_MODE;
}

static void configue_gyro(struct bmi2_sens_config *config)
{
    /* The user can change the following configuration parameters according to their requirement. */
    /* Set Output Data Rate */
    config->cfg.gyr.odr = BMI2_GYR_ODR_200HZ;

    /* Gyroscope Angular Rate Measurement Range.By default the range is 2000dps. */
    config->cfg.gyr.range = BMI2_GYR_RANGE_2000;

    /* Gyroscope bandwidth parameters. By default the gyro bandwidth is in normal mode. */
    config->cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;

    /* Enable/Disable the noise performance mode for precision yaw rate sensing
        * There are two modes
        *  0 -> Ultra low power mode(Default)
        *  1 -> High performance mode
        */
    config->cfg.gyr.noise_perf = BMI2_POWER_OPT_MODE;

    /* Enable/Disable the filter performance mode where averaging of samples
        * will be done based on above set bandwidth and ODR.
        * There are two modes
        *  0 -> Ultra low power mode
        *  1 -> High performance mode(Default)
        */
    config->cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
}

static void configure_step_counter(struct bmi2_sens_config *config)
{
    config->cfg.step_counter.watermark_level = 1;
}

static void configure_anymotion(struct bmi2_sens_config *config)
{
    /* 1LSB equals 20ms. Default is 100ms, setting to 80ms. */
    config->cfg.any_motion.duration = 0x04;

    /* 1LSB equals to 0.48mg. Default is 83mg, setting to 50mg. */
    config->cfg.any_motion.threshold = 0x68;
}

static void configure_gesture_detect(struct bmi2_sens_config *config)
{
    config->cfg.wrist_gest.wearable_arm = BMI2_ARM_LEFT;
}

static void configure_wrist_wakeup(struct bmi2_sens_config *config)
{
    config->cfg.wrist_gest_w.device_position = BMI2_ARM_LEFT;
    // TODO many things to configure here
}

static bool is_sensor_feature(uint8_t sensor_id)
{
    switch (sensor_id) {
        case BMI2_SIG_MOTION:
        case BMI2_WRIST_GESTURE:
        case BMI2_ANY_MOTION:
        case BMI2_NO_MOTION:
        case BMI2_STEP_COUNTER:
        case BMI2_STEP_DETECTOR:
        case BMI2_STEP_ACTIVITY:
        case BMI2_WRIST_WEAR_WAKE_UP:
            return true;
        default:
            return false;
    }
}

static int8_t configure_enable_all_bmi270(struct bmi2_dev *bmi2_dev)
{
    int8_t rslt;

    // INT setup
    struct bmi2_int_pin_config int_cfg;

    // Structure to define all sensors and their configs
    struct bmi2_sens_config config[ARRAY_SIZE(bmi270_enabled_features)];

    // To enable the sensors the Bosch API expects a list of all features.
    uint8_t all_sensors[ARRAY_SIZE(bmi270_enabled_features)];

    // There is a difference between a "sensor" and a "feature".
    // Accel, Gyro are sensors, but step counter is a feature.
    // We map sensor INT to INT1 pin and feature ISR to INT2 pin.
    // The API needs a list of all those features to do that map.
    struct bmi2_sens_int_config all_features[ARRAY_SIZE(bmi270_enabled_features)];
    uint8_t num_features = 0;

    for (int i = 0; i < ARRAY_SIZE(bmi270_enabled_features); i++) {
        config[i].type = bmi270_enabled_features[i].sensor_id;
        all_sensors[i] = bmi270_enabled_features[i].sensor_id;
        if (is_sensor_feature(bmi270_enabled_features[i].sensor_id)) {
            all_features[num_features].type = bmi270_enabled_features[i].sensor_id;
            all_features[num_features].hw_int_pin = BMI2_INT2;
            num_features++;
        }
    }

    // Get default configurations for the type of feature selected.
    rslt = bmi270_get_sensor_config(config, ARRAY_SIZE(bmi270_enabled_features), bmi2_dev);
    bmi2_error_codes_print_result(rslt);

    // Map data ready interrupt to interrupt pin.
    // Uncomment to generate DRDY INT on INT1
    // rslt = bmi2_map_data_int(BMI2_DRDY_INT, BMI2_INT1, bmi2_dev);
    // bmi2_error_codes_print_result(rslt);

    for (int i = 0; i < ARRAY_SIZE(bmi270_enabled_features); i++) {
        if (bmi270_enabled_features[i].cfg_func) {
            bmi270_enabled_features[i].cfg_func(&config[i]);
        }
    }

    if (rslt == BMI2_OK) {
        /* NOTE:
        * Accel and Gyro enable must be done after setting configurations.
        */
        rslt = bmi270_sensor_enable(all_sensors, ARRAY_SIZE(all_sensors), bmi2_dev);
        bmi2_error_codes_print_result(rslt);
    }

    // Setup int
    bmi2_get_int_pin_config(&int_cfg, bmi2_dev);

    int_cfg.pin_type = BMI2_INT_BOTH;
    int_cfg.pin_cfg[0].lvl = BMI2_INT_ACTIVE_HIGH;
    int_cfg.pin_cfg[0].od = BMI2_INT_PUSH_PULL;
    int_cfg.pin_cfg[0].output_en = BMI2_INT_OUTPUT_ENABLE;
    int_cfg.pin_cfg[1].lvl = BMI2_INT_ACTIVE_HIGH;
    int_cfg.pin_cfg[1].od = BMI2_INT_PUSH_PULL;
    int_cfg.pin_cfg[1].output_en = BMI2_INT_OUTPUT_ENABLE;

    rslt = bmi2_set_int_pin_config(&int_cfg, bmi2_dev);
    bmi2_error_codes_print_result(rslt);

    if (rslt == BMI2_OK) {
        rslt = bmi270_set_sensor_config(config, ARRAY_SIZE(bmi270_enabled_features), bmi2_dev);
        bmi2_error_codes_print_result(rslt);
    }

    if (rslt == BMI2_OK) {
        rslt = bmi270_map_feat_int(all_features, num_features, bmi2_dev);
        bmi2_error_codes_print_result(rslt);
    }

    return 0;
}
