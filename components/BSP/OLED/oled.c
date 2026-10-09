// OLED驱动
// SSD1306 128x64

#include "oled.h"
#include "i2c_bus.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "board_config.h"

static const char *TAG = "OLED";

// 六十四行分八页
#define OLED_PAGES          (OLED_HEIGHT / 8)
// 一共一千零二十四字节
#define OLED_BUF_SIZE       (OLED_WIDTH * OLED_HEIGHT / 8)

// 命令和数据分开
#define OLED_CTRL_CMD       0x00
#define OLED_CTRL_DATA      0x40

// 一次最多发一页
#define OLED_CHUNK_MAX      OLED_WIDTH

// 出错日志最多三条
#define OLED_IO_ERR_LOG_MAX 3

// 显存一千零二十四字节
static uint8_t s_framebuf[OLED_BUF_SIZE];

// 屏幕的句柄
static i2c_master_dev_handle_t s_dev = NULL;
// 探到的地址
static uint8_t s_addr = BSP_I2C_ADDR_SSD1306;
// 屏幕能不能用
static bool s_ready = false;
// 出错次数只用来限日志
static uint32_t s_io_err_cnt = 0;

// 屏幕的上电设置
static const uint8_t s_init_cmds[] = {
    // 先关显示
    0xAE,
    // 设成顺序寻址
    0x20, 0x00,
    // 从第零页开始
    0xB0,
    // 上下方向不倒
    0xC8,
    // 列地址低四位
    0x00,
    // 列地址高四位
    0x10,
    // 从第零行开始
    0x40,
    // 对比度调到中间
    0x81, 0xCF,
    // 左右方向不倒
    0xA1,
    // 正常显示不反白
    0xA6,
    // 六十四行全用上
    0xA8, 0x3F,
    // 跟着显存内容走
    0xA4,
    // 显示不偏移
    0xD3, 0x00,
    // 内部时钟分频
    0xD5, 0x80,
    // 预充电时间
    0xD9, 0xF1,
    // 引脚接法
    0xDA, 0x12,
    // 电压档位
    0xDB, 0x40,
    // 开内部升压必须开
    0x8D, 0x14,
    // 最后开显示
    0xAF,
};

// 往屏上写一段数据
static esp_err_t oled_i2c_write(const uint8_t *buf, size_t len) {
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // 等着的时间取自板级表
    return i2c_master_transmit(s_dev, buf, len, BSP_I2C_TIMEOUT_MS);
}

// 数错误少打日志
static void oled_note_io_err(const char *what, esp_err_t err) {
    if (s_io_err_cnt < OLED_IO_ERR_LOG_MAX) {
        ESP_LOGW(TAG, "%s failed: %s (%u)", what, esp_err_to_name(err),
                 (unsigned)(s_io_err_cnt + 1));
    }
    s_io_err_cnt++;
}

// 发一串设置命令
static esp_err_t oled_send_cmds(const uint8_t *cmds, size_t n) {
    uint8_t buf[1 + 32];

    if (cmds == NULL || n == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (n > sizeof(buf) - 1) {
        ESP_LOGE(TAG, "command block too long: %u", (unsigned)n);
        return ESP_ERR_INVALID_SIZE;
    }

    buf[0] = OLED_CTRL_CMD;
    memcpy(&buf[1], cmds, n);
    return oled_i2c_write(buf, n + 1);
}

// 只清内存不上屏
static void framebuf_clear(void) {
    memset(s_framebuf, 0x00, sizeof(s_framebuf));
}

// 把屏幕准备好
esp_err_t oled_init(void) {
    // 已经好了就直接返回
    if (s_ready) {
        ESP_LOGD(TAG, "oled already initialized");
        return ESP_OK;
    }

    // 先把总线拉起来
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_bus_init failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_master_bus_handle_t bus = i2c_bus_get_handle();
    if (bus == NULL) {
        ESP_LOGE(TAG, "i2c bus handle is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    // 两种地址都试一下
    {
        static const uint8_t candidates[] = { BSP_I2C_ADDR_SSD1306, 0x3D };
        uint8_t found = 0;

        for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
            if (i2c_master_probe(bus, candidates[i], BSP_I2C_TIMEOUT_MS) == ESP_OK) {
                found = candidates[i];
                break;
            }
        }

        if (found == 0) {
            ESP_LOGW(TAG, "SSD1306 not found at 0x%02X or 0x3D, OLED disabled",
                     BSP_I2C_ADDR_SSD1306);
            s_ready = false;
            return ESP_ERR_NOT_FOUND;
        }
        if (found != BSP_I2C_ADDR_SSD1306) {
            ESP_LOGW(TAG, "SSD1306 found at 0x%02X (default is 0x%02X) - using 0x%02X",
                     found, BSP_I2C_ADDR_SSD1306, found);
        }
        s_addr = found;
    }

    // 把屏幕挂到总线上
    if (s_dev == NULL) {
        i2c_device_config_t dev_cfg = {
            // 地址是七位
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            // 刚才探到的地址
            .device_address  = s_addr,
            // 速率取自板级表
            .scl_speed_hz    = BSP_I2C_FREQ_HZ,
        };
        err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add device 0x%02X failed: %s", s_addr, esp_err_to_name(err));
            s_dev = NULL;
            s_ready = false;
            return ESP_ERR_NOT_FOUND;
        }
    }

    // 发设置命令
    err = oled_send_cmds(s_init_cmds, sizeof(s_init_cmds));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send init sequence failed: %s, OLED disabled", esp_err_to_name(err));
        s_ready = false;
        return ESP_ERR_NOT_FOUND;
    }

    // 先清干净免得出雪花
    framebuf_clear();
    // 先置位再刷新
    s_ready = true;
    s_io_err_cnt = 0;
    oled_refresh();

    ESP_LOGI(TAG, "SSD1306 128x64 ready @0x%02X, %dkHz SDA=%d SCL=%d",
             s_addr, (int)(BSP_I2C_FREQ_HZ / 1000),
             (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO);
    return ESP_OK;
}

// 屏幕能用吗
bool oled_is_ready(void) {
    return s_ready;
}

// 清空显示
void oled_clear(void) {
    if (!s_ready) {
        return;
    }
    framebuf_clear();
}

// 写一页并上屏
void oled_write_page(uint8_t page, const uint8_t *data) {
    if (!s_ready || s_dev == NULL || data == NULL) {
        return;
    }
    if (page >= OLED_PAGES) {
        return;
    }

    // 定位到本页开头
    const uint8_t cmds[3] = {
        (uint8_t)(0xB0 | page),
        // 列地址低四位是零
        0x00,
        // 列地址高四位是零
        0x10,
    };
    esp_err_t err = oled_send_cmds(cmds, sizeof(cmds));
    if (err != ESP_OK) {
        oled_note_io_err("write_page: set cursor", err);
        return;
    }

    uint8_t buf[1 + OLED_CHUNK_MAX];
    buf[0] = OLED_CTRL_DATA;
    memcpy(&buf[1], data, OLED_WIDTH);
    err = oled_i2c_write(buf, 1 + OLED_WIDTH);
    if (err != ESP_OK) {
        oled_note_io_err("write_page: data", err);
    }
}

// 整屏推上去
void oled_refresh(void) {
    if (!s_ready || s_dev == NULL) {
        return;
    }

    // 控制字节加一页
    uint8_t buf[1 + OLED_CHUNK_MAX];
    buf[0] = OLED_CTRL_DATA;

    for (int page = 0; page < OLED_PAGES; page++) {
        // 先定位到这一页
        const uint8_t cmds[3] = {
            (uint8_t)(0xB0 | page),
            // 列地址低四位是零
            0x00,
            // 列地址高四位是零
            0x10,
        };
        esp_err_t err = oled_send_cmds(cmds, sizeof(cmds));
        if (err != ESP_OK) {
            oled_note_io_err("set page cursor", err);
            return;
        }

        memcpy(&buf[1], &s_framebuf[(size_t)page * OLED_WIDTH], OLED_WIDTH);
        err = oled_i2c_write(buf, 1 + OLED_WIDTH);
        if (err != ESP_OK) {
            oled_note_io_err("write page data", err);
            return;
        }
    }
}
