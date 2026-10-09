// I2C总线驱动

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

// 含引脚配置
#include "i2c_bus.h"

static const char *TAG = "I2C_BUS";

// 扫描的地址范围
#define I2C_SCAN_ADDR_MIN       0x08
#define I2C_SCAN_ADDR_MAX       0x77

// 探测一次等50毫秒
#define I2C_SCAN_PROBE_TIMEOUT_MS   50

// 滤掉太短的毛刺
#define I2C_GLITCH_IGNORE_CNT       7

// 总线把手先存这儿
static i2c_master_bus_handle_t s_bus = NULL;

// 把 I2C 线拉起来
esp_err_t i2c_bus_init(void) {
    // 起过就直接返回
    if (s_bus != NULL) {
        return ESP_OK;
    }

    // 填脚和频率
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA_GPIO,
        .scl_io_num = BSP_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = I2C_GLITCH_IGNORE_CNT,
        // 中断优先级默认
        .intr_priority = 0,
        // 只同步发，不用队列
        .trans_queue_depth = 0,
        // 再兜一层上拉
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        // 清了，下次能重试
        s_bus = NULL;
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C%d ready: SDA=GPIO%d SCL=GPIO%d @%dHz",
             (int)BSP_I2C_PORT, (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO,
             (int)BSP_I2C_FREQ_HZ);
    return ESP_OK;
}

// 交出总线把手
i2c_master_bus_handle_t i2c_bus_get_handle(void) {
    return s_bus;
}

// 看地址在不在线
esp_err_t i2c_bus_probe(uint8_t dev_addr) {
    if (s_bus == NULL) {
        ESP_LOGE(TAG, "probe 0x%02X before bus init", dev_addr);
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_probe(s_bus, dev_addr, BSP_I2C_TIMEOUT_MS);
}

// 扫一遍谁在线
int i2c_bus_scan(void) {
    if (s_bus == NULL) {
        ESP_LOGE(TAG, "scan before bus init");
        return 0;
    }

    int found = 0;
    ESP_LOGI(TAG, "scanning 0x%02X ~ 0x%02X ...", I2C_SCAN_ADDR_MIN, I2C_SCAN_ADDR_MAX);

    for (uint16_t addr = I2C_SCAN_ADDR_MIN; addr <= I2C_SCAN_ADDR_MAX; addr++) {
        // 应答就算在线
        if (i2c_master_probe(s_bus, addr, I2C_SCAN_PROBE_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "  found device at 0x%02X", (unsigned)addr);

            // 标出认识的芯片
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
