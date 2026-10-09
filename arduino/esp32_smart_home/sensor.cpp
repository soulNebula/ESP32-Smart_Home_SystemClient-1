#include "sensor.h"

#include <Arduino.h>
#include <Wire.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"

// ======================================================================
// 传感器：温湿度 AHT20/SHT30 + 光敏电阻 + 雨滴
//
// 对应 ESP-IDF 工程的 components/BSP/SENSOR/sensor.c，
// 采样节拍、平滑滤波、换算公式、连错掉线、拼行格式都照那份抄，数值一个没改。
// 换成 Arduino 之后有三处不一样，下面都注了：
//   1. 原版起一个 FreeRTOS 任务定时采，这边改成主循环喊 sensor_poll()，
//      对着 millis() 走节拍；温湿度要等芯片转换，所以拆成"发命令"和"收结果"两步，
//      主循环里绝不停着等；
//   2. 原版那条 I2C 上还挂着 BH1750，这块板子没有，光照只走光敏电阻那路 ADC；
//   3. 原版 ADC 读数可能失败（返回负数），Arduino 的 analogReadMilliVolts 不会，
//      所以"读失败保留上次值"那几条分支没搬过来。
// ======================================================================

// 可调参数都放这里
// 平滑系数，越小越迟钝
#define APP_SENSOR_FILTER_ALPHA         0.30f
// 雨滴单独更快的系数：下雨关窗要快
#define APP_SENSOR_RAIN_FILTER_ALPHA    0.60f
// 多采几次取平均
#define APP_SENSOR_ADC_SAMPLES          16
// 默认多久采一次
#define APP_SENSOR_DEF_PERIOD_MS        1000
// 最快也得隔这么久
#define APP_SENSOR_MIN_PERIOD_MS        100
// 隔多久再找一次配件
#define APP_SENSOR_REPROBE_PERIOD_MS    5000
// 连错几次就算掉线
#define APP_SENSOR_FAIL_OFFLINE         3

// 光敏电阻的标定值
// 全黑时的电压
#define APP_LIGHT_MV_DARK               100.0f
// 全亮时的电压
#define APP_LIGHT_MV_BRIGHT             3000.0f

// 雨滴模块的标定值
// 全干时的电压
#define APP_RAIN_MV_DRY                 3300.0f
// 全湿时的电压
#define APP_RAIN_MV_WET                 300.0f

// 芯片命令字
// 叫 SHT30 测一次
#define APP_SHT30_CMD_MEAS_HI           0x2C06
#define APP_SHT30_MEAS_DELAY_MS         20
// 让 AHT20 先自校准
#define APP_AHT20_CMD_INIT              0xBE
// 叫 AHT20 测一次
#define APP_AHT20_CMD_MEAS              0xAC
#define APP_AHT20_ARG_0                 0x00
#define APP_AHT20_ARG_INIT              0x08
#define APP_AHT20_ARG_MEAS              0x33
#define APP_AHT20_INIT_DELAY_MS         10
#define APP_AHT20_MEAS_DELAY_MS         80
// 看它忙不忙
#define APP_AHT20_STATUS_BUSY           0x80

// 现在用哪个温湿度芯片
typedef enum {
    TH_CHIP_NONE = 0,
    TH_CHIP_SHT30,
    TH_CHIP_AHT20,
} th_chip_t;

// 读一次温湿度的结果，错因照原版分几类，因为算不算掉线不一样
typedef enum {
    TH_OK = 0,
    // 芯片没应答或者读不动
    TH_ERR_IO,
    // 校验没过
    TH_ERR_CRC,
    // 还在测，下次再来
    TH_ERR_BUSY,
} th_result_t;

// 一轮采样走到哪了
typedef enum {
    STEP_IDLE = 0,
    // 命令发出去了，等芯片转换完
    STEP_WAIT_TH,
} sample_step_t;

// 记下有没有开过机
static bool          s_inited;
// 存最近一次采样结果，主循环一个人用，不用加锁
static sensor_data_t s_data;
// 这一轮正在攒的数据，整轮采完才搬到 s_data
static sensor_data_t s_work;

// 认到的温湿度芯片
static th_chip_t     s_chip = TH_CHIP_NONE;
// 温湿度连错几次了
static int           s_temp_fail;
// 上次找配件的时间
static uint32_t      s_last_probe_ms;

// 头一次直接取真值
static bool          s_filt_temp_ready;
static bool          s_filt_hum_ready;
static bool          s_filt_pct_ready;
static bool          s_filt_rain_ready;

static sensor_cb_t   s_cb;
static void         *s_cb_user;

// 自动采样的开关和节拍
static bool          s_auto_on;
static uint32_t      s_period_ms = APP_SENSOR_DEF_PERIOD_MS;
static uint32_t      s_last_sample_ms;

// 采样状态机
static sample_step_t s_step = STEP_IDLE;
// 这一轮要等芯片多久，到点了才去收结果
static uint32_t      s_wait_ms;
static uint32_t      s_wait_until_ms;

// 几个小工具函数
// 把数值夹在上下限内
static float clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

// 慢速平滑，去掉跳动
static float lowpass(float old, float raw, bool *ready, float alpha) {
    if (!*ready) {
        *ready = true;
        return raw;
    }
    return old + alpha * (raw - old);
}

// 读一路 ADC，连采几次取平均
static int adc_read_mv_avg(int gpio, int samples) {
    int32_t sum = 0;
    for (int i = 0; i < samples; i++) {
        // 这个口自带校准，衰减用默认那档（约 3.1V 量程），跟原版的 ADC_ATTEN_DB_12 是一回事
        sum += (int32_t)analogReadMilliVolts(gpio);
    }
    // 四舍五入
    return (int)((sum + samples / 2) / samples);
}

// 算校验值查错
static uint8_t crc8_sensirion(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x31U) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// 读写 I2C 芯片
// 问一声这个地址上有没有人应
static bool i2c_probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    // 只发地址，有人应就是 0
    return Wire.endTransmission() == 0;
}

// 给芯片写命令
static bool i2c_write(uint8_t addr, const uint8_t *buf, size_t len) {
    Wire.beginTransmission(addr);
    for (size_t i = 0; i < len; i++) {
        Wire.write(buf[i]);
    }
    return Wire.endTransmission() == 0;
}

// 从芯片读数据
static bool i2c_read(uint8_t addr, uint8_t *buf, size_t len) {
    if (Wire.requestFrom((uint8_t)addr, (uint8_t)len) != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (Wire.available() <= 0) {
            return false;
        }
        buf[i] = (uint8_t)Wire.read();
    }
    return true;
}

// 让 AHT20 先校准
static bool aht20_wake(void) {
    const uint8_t cmd[3] = {APP_AHT20_CMD_INIT, APP_AHT20_ARG_INIT, APP_AHT20_ARG_0};
    if (!i2c_write(BSP_I2C_ADDR_AHT20, cmd, sizeof(cmd))) {
        Serial.printf("SENSOR: AHT20 初始化命令失败\n");
        return false;
    }
    // 这十毫秒必须等：芯片正在自校准，这时候去读只会拿到脏数据
    delay(APP_AHT20_INIT_DELAY_MS);
    return true;
}

// 叫温湿度芯片测一次，只发命令不等结果
static th_result_t th_trigger(void) {
    if (s_chip == TH_CHIP_SHT30) {
        const uint8_t cmd[2] = {
            (uint8_t)(APP_SHT30_CMD_MEAS_HI >> 8),
            (uint8_t)(APP_SHT30_CMD_MEAS_HI & 0xFF),
        };
        if (!i2c_write(BSP_I2C_ADDR_SHT30, cmd, sizeof(cmd))) {
            return TH_ERR_IO;
        }
        s_wait_ms = APP_SHT30_MEAS_DELAY_MS;
        return TH_OK;
    }

    if (s_chip == TH_CHIP_AHT20) {
        const uint8_t cmd[3] = {APP_AHT20_CMD_MEAS, APP_AHT20_ARG_MEAS, APP_AHT20_ARG_0};
        if (!i2c_write(BSP_I2C_ADDR_AHT20, cmd, sizeof(cmd))) {
            return TH_ERR_IO;
        }
        s_wait_ms = APP_AHT20_MEAS_DELAY_MS;
        return TH_OK;
    }

    return TH_ERR_IO;
}

// 把转换好的结果收回来
static th_result_t th_fetch(float *out_temp, float *out_rh) {
    if (s_chip == TH_CHIP_SHT30) {
        // 读回六个字节
        uint8_t buf[6] = {0};
        if (!i2c_read(BSP_I2C_ADDR_SHT30, buf, sizeof(buf))) {
            return TH_ERR_IO;
        }

        if (crc8_sensirion(&buf[0], 2) != buf[2] || crc8_sensirion(&buf[3], 2) != buf[5]) {
            Serial.printf("SENSOR: SHT30 CRC 校验失败，丢弃本次数据（保留上次值）\n");
            return TH_ERR_CRC;
        }

        const uint16_t raw_t  = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
        const uint16_t raw_rh = (uint16_t)(((uint16_t)buf[3] << 8) | buf[4]);

        // 按公式算出温度
        *out_temp = -45.0f + 175.0f * (float)raw_t / 65535.0f;
        *out_rh   = clampf(100.0f * (float)raw_rh / 65535.0f, 0.0f, 100.0f);
        return TH_OK;
    }

    if (s_chip == TH_CHIP_AHT20) {
        // 读回状态和数据
        uint8_t buf[7] = {0};
        if (!i2c_read(BSP_I2C_ADDR_AHT20, buf, sizeof(buf))) {
            return TH_ERR_IO;
        }

        if ((buf[0] & APP_AHT20_STATUS_BUSY) != 0) {
            // 还在测就下次再来
            Serial.printf("SENSOR: AHT20 忙标志未清（本次未测完），跳过本次采样\n");
            return TH_ERR_BUSY;
        }

        // 查数据对不对
        if (crc8_sensirion(&buf[0], 6) != buf[6]) {
            Serial.printf("SENSOR: AHT20 CRC 校验失败，丢弃本次数据（保留上次值）\n");
            return TH_ERR_CRC;
        }

        // 拼出湿度原始值
        const uint32_t raw_h = (((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | ((uint32_t)buf[3] >> 4));
        // 拼出温度原始值
        const uint32_t raw_t = ((((uint32_t)buf[3] & 0x0FU) << 16) | ((uint32_t)buf[4] << 8) | (uint32_t)buf[5]);

        *out_rh   = clampf((float)raw_h * 100.0f / 1048576.0f, 0.0f, 100.0f);
        *out_temp = (float)raw_t * 200.0f / 1048576.0f - 50.0f;
        return TH_OK;
    }

    return TH_ERR_IO;
}

// 电压换成亮度百分比
static float light_mv_to_pct(int mv) {
    float pct = ((float)mv - APP_LIGHT_MV_DARK) * 100.0f / (APP_LIGHT_MV_BRIGHT - APP_LIGHT_MV_DARK);
    pct = clampf(pct, 0.0f, 100.0f);
    // 接反了就翻过来
#if BSP_LIGHT_ADC_INVERT
    pct = 100.0f - pct;
#endif
    return pct;
}

// 电压换成下雨百分比
static float rain_mv_to_pct(int mv) {
    const float pct = (APP_RAIN_MV_DRY - (float)mv) * 100.0f / (APP_RAIN_MV_DRY - APP_RAIN_MV_WET);
    return clampf(pct, 0.0f, 100.0f);
}

// 自己找一遍配件
static void sensor_redetect(void) {
    // 缺件警告只打一次
    static bool s_th_missing_warned = false;

    // 总线没起来的话下面探测全会失败，跟原版一样当没插芯片处理
    // 没认到过才重新找
    if (s_chip == TH_CHIP_NONE) {
        if (i2c_probe(BSP_I2C_ADDR_SHT30)) {
            s_chip = TH_CHIP_SHT30;
            s_temp_fail         = 0;
            s_filt_temp_ready   = false;
            s_filt_hum_ready    = false;
            s_th_missing_warned = false;
            Serial.printf("SENSOR: 检测到温湿度传感器：SHT30/SHT31 (0x%02X)\n", BSP_I2C_ADDR_SHT30);
        } else if (i2c_probe(BSP_I2C_ADDR_AHT20)) {
            s_chip = TH_CHIP_AHT20;
            s_temp_fail         = 0;
            s_filt_temp_ready   = false;
            s_filt_hum_ready    = false;
            s_th_missing_warned = false;
            Serial.printf("SENSOR: 检测到温湿度传感器：AHT20 (0x%02X)\n", BSP_I2C_ADDR_AHT20);
            // 失败就下次再试
            (void)aht20_wake();
        } else if (!s_th_missing_warned) {
            Serial.printf("SENSOR: 未检测到温湿度传感器（0x%02X / 0x%02X），温度联动将停用\n",
                          BSP_I2C_ADDR_SHT30, BSP_I2C_ADDR_AHT20);
            s_th_missing_warned = true;
        }
    }
}

// 这一轮采成了，先算滤波
static void th_apply(float t, float rh) {
    if (s_temp_fail >= APP_SENSOR_FAIL_OFFLINE) {
        Serial.printf("SENSOR: 温湿度传感器已恢复\n");
    }
    s_temp_fail = 0;
    s_work.valid_temp  = true;
    s_work.temperature = lowpass(s_work.temperature, t, &s_filt_temp_ready, APP_SENSOR_FILTER_ALPHA);
    s_work.humidity    = lowpass(s_work.humidity, rh, &s_filt_hum_ready, APP_SENSOR_FILTER_ALPHA);
}

// 这一轮没读成，看错因算不算掉线
static void th_count_fail(th_result_t r) {
    if (r == TH_ERR_CRC || r == TH_ERR_BUSY) {
        // 这两种错不算掉线
        return;
    }

    s_temp_fail++;
    if (s_temp_fail == APP_SENSOR_FAIL_OFFLINE) {
        s_work.valid_temp = false;
        // 好了就跳回真值
        s_filt_temp_ready = false;
        s_filt_hum_ready  = false;
        Serial.printf("SENSOR: 温湿度传感器连续 %d 次读取失败，valid_temp=false\n", s_temp_fail);
    }
}

// 这一轮跑完了：存好，采完就叫一下上层
static void sample_finish(void) {
    const uint32_t now_ms = millis();

    s_work.timestamp_ms = now_ms;
    // 下一轮从这会儿起算，跟原版"采完再歇一个周期"是一个意思
    s_last_sample_ms = now_ms;

    s_data = s_work;

    sensor_cb_t cb = s_cb;
    if (cb != NULL) {
        const sensor_data_t *last = sensor_get_last();
        // 原版一轮分三类各叫一次，这边也分三次
        cb(SENSOR_KIND_TEMP, last, s_cb_user);
        cb(SENSOR_KIND_LIGHT, last, s_cb_user);
        cb(SENSOR_KIND_RAIN, last, s_cb_user);
    }
}

// 开一轮采样：ADC 这两路是快活，先读完；温湿度得等芯片，发完命令就撒手
static void sample_begin(void) {
    // 拿上次结果打底，谁读不成谁就留着老值
    s_work = s_data;

    // 光照这路
    const int light_mv = adc_read_mv_avg(BSP_GPIO_LIGHT_ADC, APP_SENSOR_ADC_SAMPLES);
    s_work.light_mv  = light_mv;
    s_work.light_pct = lowpass(s_work.light_pct, light_mv_to_pct(light_mv), &s_filt_pct_ready,
                               APP_SENSOR_FILTER_ALPHA);

    // 雨滴这路。模块上那个开关量脚（BSP_GPIO_RAIN_DO）原版就没用，
    // 下没下雨全靠这路电压算，这边照原样不接它
    const int rain_mv = adc_read_mv_avg(BSP_GPIO_RAIN_AO, APP_SENSOR_ADC_SAMPLES);
    s_work.rain_mv  = rain_mv;
    s_work.rain_pct = lowpass(s_work.rain_pct, rain_mv_to_pct(rain_mv), &s_filt_rain_ready,
                              APP_SENSOR_RAIN_FILTER_ALPHA);
    // 先按默认阈值初判，上层要改阈值自己再判一遍
    s_work.rain_detected = (s_work.rain_pct > (float)BSP_DEF_RAIN_PCT);

    // 温湿度把测量命令发出去，到点了下一回 sensor_poll 再收
    s_step = STEP_IDLE;
    if (s_chip != TH_CHIP_NONE) {
        const th_result_t r = th_trigger();
        if (r == TH_OK) {
            s_wait_until_ms = millis() + s_wait_ms;
            s_step = STEP_WAIT_TH;
            return;
        }
        // 命令都发不出去就按失败算
        th_count_fail(r);
    }

    // 没温湿度芯片的话这一轮就到底了
    sample_finish();
}

// 把这一轮接着往下走，返回 true 就是采完了
static bool sample_step(void) {
    if (s_step != STEP_WAIT_TH) {
        return false;
    }

    // 没到点就先回去，主循环不在这等
    if ((int32_t)(millis() - s_wait_until_ms) < 0) {
        return false;
    }

    float t  = 0.0f;
    float rh = 0.0f;
    const th_result_t r = th_fetch(&t, &rh);
    if (r == TH_OK) {
        th_apply(t, rh);
    } else {
        th_count_fail(r);
    }

    s_step = STEP_IDLE;
    sample_finish();
    return true;
}

// 开机把传感器备好
bool sensor_init(void) {
    if (s_inited) {
        // 开过就直接返回
        return true;
    }
    // 先占位防重入
    s_inited = true;

    memset(&s_data, 0, sizeof(s_data));
    memset(&s_work, 0, sizeof(s_work));
    // 没认到芯片就无效
    s_data.valid_temp = false;
    // 往前挪一个周期，认完硬件就马上采一次
    s_last_sample_ms = (uint32_t)(millis() - APP_SENSOR_DEF_PERIOD_MS);
    s_last_probe_ms  = millis();

    // 屏幕和温湿度共用这条 I2C，屏幕那边可能已经起过一次了，
    // 重复初始化无害，返回 false 也不算错，提一句就行
    if (!Wire.begin(BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO, BSP_I2C_FREQ_HZ)) {
        Serial.printf("SENSOR: Wire.begin 返回 false（屏幕那边可能先起过总线，忽略）\n");
    }

    // 认一遍配件
    // 原版这儿还要 adc_bus_init() 起一次 ADC，Arduino 的 analogRead 拿起来就能用，不用单独起
    sensor_redetect();

    // 原版这儿还会扫一遍总线把地址打出来，Arduino 的 Wire 没现成的扫描口，就不占这份代码了
    const char *th_name = (s_chip == TH_CHIP_SHT30) ? "SHT30" :
                          (s_chip == TH_CHIP_AHT20) ? "AHT20" : "未接(valid_temp=false)";
    Serial.printf("SENSOR: 初始化完成：温湿度=%s，光照=%s，雨滴=ADC(GPIO%d)\n",
                  th_name, "光敏电阻(ADC 百分比)", BSP_GPIO_RAIN_AO);

    // 缺件也不报错
    return true;
}

// 马上采一次
bool sensor_read(sensor_data_t *out) {
    if (!s_inited) {
        // 没开过机就先开机
        sensor_init();
    }

    // 过一阵再找一次配件
    if ((uint32_t)(millis() - s_last_probe_ms) >= APP_SENSOR_REPROBE_PERIOD_MS) {
        s_last_probe_ms = millis();
        sensor_redetect();
    }

    // 上一轮要是没走完就先接着走，没有就新开一轮
    if (s_step == STEP_IDLE) {
        sample_begin();
    }

    // 这是同步接口，只有这条路上会等芯片转换完那几十毫秒（AHT20 要 80ms），
    // 顶多等到点就出来，芯片不在也不会卡死；主循环那条自动采样的路走 sensor_poll，不停在这
    while (s_step != STEP_IDLE) {
        sample_step();
        // 让一下别的活，别把看门狗饿着
        yield();
    }

    if (out != NULL) {
        *out = s_data;
    }

    // 缺一个不算整体失败
    return true;
}

// 取上次采样结果
const sensor_data_t *sensor_get_last(void) {
    // 这里的数据永远在
    return &s_data;
}

// 登记采完的回调，只留最后一个
void sensor_register_cb(sensor_cb_t cb, void *user_data) {
    // 传空就是取消登记
    s_cb      = cb;
    s_cb_user = user_data;
}

// 开自动采样，给周期毫秒
void sensor_start_auto(uint32_t period_ms) {
    if (period_ms == 0) {
        period_ms = APP_SENSOR_DEF_PERIOD_MS;
    }
    if (period_ms < APP_SENSOR_MIN_PERIOD_MS) {
        Serial.printf("SENSOR: 采样周期 %u ms 太小，钳到 %d ms\n",
                      (unsigned)period_ms, APP_SENSOR_MIN_PERIOD_MS);
        period_ms = APP_SENSOR_MIN_PERIOD_MS;
    }
    s_period_ms = period_ms;

    if (s_auto_on) {
        Serial.printf("SENSOR: 自动采样已在跑，周期改成 %u ms\n", (unsigned)period_ms);
        return;
    }

    s_auto_on = true;
    // 同样往前挪一个周期，开起来就马上采一次
    s_last_sample_ms = (uint32_t)(millis() - s_period_ms);
    Serial.printf("SENSOR: 自动采样已开，周期 %u ms\n", (unsigned)period_ms);
}

// 停掉自动采样
void sensor_stop_auto(void) {
    if (!s_auto_on) {
        // 没在跑就返回
        return;
    }
    s_auto_on = false;
    Serial.printf("SENSOR: 自动采样已停\n");
}

// 主循环喊这个，到点才真采
void sensor_poll(void) {
    // 上一轮开了头就得走完，不然芯片那次转换白搭
    if (s_step != STEP_IDLE) {
        sample_step();
        return;
    }

    if (!s_auto_on) {
        return;
    }

    // 头一回先把硬件认一遍（里面就 AHT20 自校准那 10ms 是等的），认过就不再进
    if (!s_inited) {
        sensor_init();
        return;
    }

    // 没到点就先回去，主循环该干嘛干嘛
    if ((uint32_t)(millis() - s_last_sample_ms) < s_period_ms) {
        return;
    }

    // 到点了开一轮新的：这一步不阻塞，温湿度发完命令就交给下一回 sensor_poll
    sample_begin();
}

// 拼一行给人看的字
void sensor_format_line(const sensor_data_t *d, char *buf, size_t len) {
    if (buf == NULL || len == 0) {
        return;
    }
    if (d == NULL) {
        d = sensor_get_last();
    }

    char t_str[20];
    char h_str[20];
    char l_str[24];
    char r_str[24];

    if (d->valid_temp) {
        snprintf(t_str, sizeof(t_str), "T:%.1fC", d->temperature);
        snprintf(h_str, sizeof(h_str), "H:%.1f%%", d->humidity);
    } else {
        // 没芯片就打横杠
        snprintf(t_str, sizeof(t_str), "T:--");
        snprintf(h_str, sizeof(h_str), "H:--");
    }

    // 这块板子上没有 BH1750，光照永远是光敏电阻那套，带上电压好查极性
    snprintf(l_str, sizeof(l_str), "L:%.1f%%(%dmV)", d->light_pct, d->light_mv);

    snprintf(r_str, sizeof(r_str), "Rain:%.1f%%", d->rain_pct);

    snprintf(buf, len, "%s %s %s %s", t_str, h_str, l_str, r_str);
}
