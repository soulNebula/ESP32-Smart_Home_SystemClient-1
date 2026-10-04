/*
 * 模块：
 *   屏幕。把画面画到小屏幕上，被 main.c 和 astra UI 调用，
 *   自己向下调 i2c_bus 读写屏幕芯片。没插屏也不影响别的功能。
 *
 * 功能：
 *   把硬件准备好
 *   没屏就当没这回事
 *   整屏推上去显示
 *   按页把画面推上去
 */
#include "oled.h"
#include "i2c_bus.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "board_config.h"

static const char *TAG = "OLED";

#define OLED_PAGES          (OLED_HEIGHT / 8)                 /* 功能：六十四行分八页 */
#define OLED_BUF_SIZE       (OLED_WIDTH * OLED_HEIGHT / 8)    /* 功能：一共一千零二十四字节 */

/* 功能：命令和数据分开 */
#define OLED_CTRL_CMD       0x00
#define OLED_CTRL_DATA      0x40

/* 功能：一次最多发一页 */
#define OLED_CHUNK_MAX      OLED_WIDTH

/* 功能：出错日志最多三条 */
#define OLED_IO_ERR_LOG_MAX 3

/* 功能：显存一千零二十四字节 */
static uint8_t s_framebuf[OLED_BUF_SIZE];

static i2c_master_dev_handle_t s_dev = NULL;    /* 功能：屏幕的句柄 */
static uint8_t s_addr = BSP_I2C_ADDR_SSD1306;   /* 功能：探到的地址 */
static bool s_ready = false;                    /* 功能：屏幕能不能用 */
static uint32_t s_io_err_cnt = 0;               /* 功能：出错次数只用来限日志 */

/* 功能：屏幕的上电设置 */
static const uint8_t s_init_cmds[] = {
    0xAE,               /* 功能：先关显示 */
    0x20, 0x00,         /* 功能：设成顺序寻址 */
    0xB0,               /* 功能：从第零页开始 */
    0xC8,               /* 功能：上下方向不倒 */
    0x00,               /* 功能：列地址低四位 */
    0x10,               /* 功能：列地址高四位 */
    0x40,               /* 功能：从第零行开始 */
    0x81, 0xCF,         /* 功能：对比度调到中间 */
    0xA1,               /* 功能：左右方向不倒 */
    0xA6,               /* 功能：正常显示不反白 */
    0xA8, 0x3F,         /* 功能：六十四行全用上 */
    0xA4,               /* 功能：跟着显存内容走 */
    0xD3, 0x00,         /* 功能：显示不偏移 */
    0xD5, 0x80,         /* 功能：内部时钟分频 */
    0xD9, 0xF1,         /* 功能：预充电时间 */
    0xDA, 0x12,         /* 功能：引脚接法 */
    0xDB, 0x40,         /* 功能：电压档位 */
    0x8D, 0x14,         /* 功能：开内部升压必须开 */
    0xAF,               /* 功能：最后开显示 */
};

/* 功能：往屏上写一段数据 */
static esp_err_t oled_i2c_write(const uint8_t *buf, size_t len)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 功能：等着的时间取自板级表 */
    return i2c_master_transmit(s_dev, buf, len, BSP_I2C_TIMEOUT_MS);
}

/* 功能：数错误少打日志 */
static void oled_note_io_err(const char *what, esp_err_t err)
{
    if (s_io_err_cnt < OLED_IO_ERR_LOG_MAX) {
        ESP_LOGW(TAG, "%s failed: %s (%u)", what, esp_err_to_name(err),
                 (unsigned)(s_io_err_cnt + 1));
    }
    s_io_err_cnt++;
}

/* 功能：发一串设置命令 */
static esp_err_t oled_send_cmds(const uint8_t *cmds, size_t n)
{
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

/* 功能：只清内存不上屏 */
static void framebuf_clear(void)
{
    memset(s_framebuf, 0x00, sizeof(s_framebuf));
}

/* 功能：把屏幕准备好 */
esp_err_t oled_init(void)
{
    /* 功能：已经好了就直接返回 */
    if (s_ready) {
        ESP_LOGD(TAG, "oled already initialized");
        return ESP_OK;
    }

    /* 功能：先把总线拉起来 */
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

    /* 功能：两种地址都试一下 */
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

    /* 功能：把屏幕挂到总线上 */
    if (s_dev == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,      /* 功能：地址是七位 */
            .device_address  = s_addr,                  /* 功能：刚才探到的地址 */
            .scl_speed_hz    = BSP_I2C_FREQ_HZ,         /* 功能：速率取自板级表 */
        };
        err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add device 0x%02X failed: %s", s_addr, esp_err_to_name(err));
            s_dev = NULL;
            s_ready = false;
            return ESP_ERR_NOT_FOUND;
        }
    }

    /* 功能：发设置命令 */
    err = oled_send_cmds(s_init_cmds, sizeof(s_init_cmds));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send init sequence failed: %s, OLED disabled", esp_err_to_name(err));
        s_ready = false;
        return ESP_ERR_NOT_FOUND;
    }

    /* 功能：先清干净免得出雪花 */
    framebuf_clear();
    s_ready = true;                     /* 功能：先置位再刷新 */
    s_io_err_cnt = 0;
    oled_refresh();

    ESP_LOGI(TAG, "SSD1306 128x64 ready @0x%02X, %dkHz SDA=%d SCL=%d",
             s_addr, (int)(BSP_I2C_FREQ_HZ / 1000),
             (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO);
    return ESP_OK;
}

/* 功能：屏幕能用吗 */
bool oled_is_ready(void)
{
    return s_ready;
}

/* 功能：清空显示 */
void oled_clear(void)
{
    if (!s_ready) {
        return;
    }
    framebuf_clear();
}

/* 功能：写一页并上屏 */
void oled_write_page(uint8_t page, const uint8_t *data)
{
    if (!s_ready || s_dev == NULL || data == NULL) {
        return;
    }
    if (page >= OLED_PAGES) {
        return;
    }

    /* 功能：定位到本页开头 */
    const uint8_t cmds[3] = {
        (uint8_t)(0xB0 | page),
        0x00,                                   /* 功能：列地址低四位是零 */
        0x10,                                   /* 功能：列地址高四位是零 */
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

/* 功能：整屏推上去 */
void oled_refresh(void)
{
    if (!s_ready || s_dev == NULL) {
        return;
    }

    uint8_t buf[1 + OLED_CHUNK_MAX];    /* 功能：控制字节加一页 */
    buf[0] = OLED_CTRL_DATA;

    for (int page = 0; page < OLED_PAGES; page++) {
        /* 功能：先定位到这一页 */
        const uint8_t cmds[3] = {
            (uint8_t)(0xB0 | page),
            0x00,                                   /* 功能：列地址低四位是零 */
            0x10,                                   /* 功能：列地址高四位是零 */
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
