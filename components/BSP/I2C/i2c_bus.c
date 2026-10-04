/**
 * @file  i2c_bus.c
 * @brief I2C0 总线管理实现 —— OLED / SHT30 / AHT20 / BH1750 共用
 *
 * ============================ 设计要点 ============================
 * 1) 只用 ESP-IDF v5.4 的新版 I2C Master 驱动（driver/i2c_master.h）：
 *      i2c_new_master_bus() / i2c_master_probe()
 *    绝不碰已废弃的 i2c_cmd_link_create() 那一套老 API。
 * 2) 单例：总线句柄缓存在静态变量 s_bus 里，i2c_bus_init() 重复调用直接
 *    返回 ESP_OK，不会重复创建总线（重复创建会返回 ESP_ERR_NOT_FOUND，
 *    进而把已经建好的总线弄成"半初始化"状态）。
 * 3) 引脚 / 频率全部来自 board_config.h，本文件不出现任何硬编码 GPIO 号。
 * 4) 扫描（i2c_bus_scan）只做"探测"，不给任何从机建句柄 —— 从机句柄由
 *    oled.c / sensor.c 自己按需 i2c_master_bus_add_device()。
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#include "i2c_bus.h"    /* 含 board_config.h */

static const char *TAG = "I2C_BUS";

/* I2C 扫描地址范围（7 位地址，去掉保留地址） */
#define I2C_SCAN_ADDR_MIN       0x08
#define I2C_SCAN_ADDR_MAX       0x77

/* 扫描时单次探测的超时：50ms 已足够（400kHz 下探测一次不到 100us），
 * 但没接上拉/线被拉死时能尽快返回，扫完 112 个地址不会卡太久。
 * 注意：i2c_master_probe() 超时用的是"毫秒"整数参数。 */
#define I2C_SCAN_PROBE_TIMEOUT_MS   50

/* 毛刺过滤：小于该值的干扰脉冲会被滤掉，典型值 7（单位：I2C 模块时钟周期） */
#define I2C_GLITCH_IGNORE_CNT       7

/* ------------------------------------------------------------------ */
/*  单例状态                                                           */
/* ------------------------------------------------------------------ */
static i2c_master_bus_handle_t s_bus = NULL;

/* ------------------------------------------------------------------ */
/*  对外接口                                                           */
/* ------------------------------------------------------------------ */

esp_err_t i2c_bus_init(void)
{
    /* 幂等：已经建好就直接返回 */
    if (s_bus != NULL) {
        return ESP_OK;
    }

    /* 注意字段名：i2c_master_bus_config_t 里是 clk_source / i2c_port /
     * sda_io_num / scl_io_num / glitch_ignore_cnt / flags.enable_internal_pullup */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA_GPIO,
        .scl_io_num = BSP_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = I2C_GLITCH_IGNORE_CNT,
        .intr_priority = 0,         /* 0 = 由驱动自己挑默认优先级 */
        .trans_queue_depth = 0,     /* 只做同步传输，不需要异步队列 */
        .flags.enable_internal_pullup = true,   /* 模块板一般自带 4.7k 上拉，这里再兜一层 */
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        s_bus = NULL;               /* 防止留下野句柄，让下次调用还能重试 */
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C%d ready: SDA=GPIO%d SCL=GPIO%d @%dHz",
             (int)BSP_I2C_PORT, (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO,
             (int)BSP_I2C_FREQ_HZ);
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    return s_bus;
}

esp_err_t i2c_bus_probe(uint8_t dev_addr)
{
    if (s_bus == NULL) {
        ESP_LOGE(TAG, "probe 0x%02X before bus init", dev_addr);
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_probe(s_bus, dev_addr, BSP_I2C_TIMEOUT_MS);
}

int i2c_bus_scan(void)
{
    if (s_bus == NULL) {
        ESP_LOGE(TAG, "scan before bus init");
        return 0;
    }

    int found = 0;
    ESP_LOGI(TAG, "scanning 0x%02X ~ 0x%02X ...", I2C_SCAN_ADDR_MIN, I2C_SCAN_ADDR_MAX);

    for (uint16_t addr = I2C_SCAN_ADDR_MIN; addr <= I2C_SCAN_ADDR_MAX; addr++) {
        /* ESP_OK = 有 ACK（在线）；ESP_ERR_NOT_FOUND = 无应答（正常，忽略）；
         * 其它错误 = 总线本身有问题（没上拉/被拉死），这里也只在没扫到东西时提示 */
        if (i2c_master_probe(s_bus, addr, I2C_SCAN_PROBE_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "  found device at 0x%02X", (unsigned)addr);

            /* 顺手把已知器件名字标出来，调试时一眼能看出地址对不对 */
            switch (addr) {
            case BSP_I2C_ADDR_SSD1306:
                ESP_LOGI(TAG, "     ^ SSD1306 OLED");
                break;
            case BSP_I2C_ADDR_SHT30:
                ESP_LOGI(TAG, "     ^ SHT30/SHT31 温湿度");
                break;
            case BSP_I2C_ADDR_AHT20:
                ESP_LOGI(TAG, "     ^ AHT20 温湿度");
                break;
            case BSP_I2C_ADDR_BH1750:
                ESP_LOGI(TAG, "     ^ BH1750 光照");
                break;
            case BSP_I2C_ADDR_PCF8574:
                ESP_LOGI(TAG, "     ^ PCF8574 IO 扩展");
                break;
            default:
                break;
            }
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGW(TAG, "no I2C device found (check SDA=GPIO%d / SCL=GPIO%d and pull-ups)",
                 (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO);
    } else {
        ESP_LOGI(TAG, "scan done, %d device(s) found", found);
    }
    return found;
}
