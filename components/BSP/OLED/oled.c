/**
 * @file  oled.c
 * @brief SSD1306 128x64 OLED 驱动（I2C / ESP-IDF v5.4 新版 i2c_master API）
 *
 * ============================ 设计要点 ============================
 * 1. 只用新版 I2C API：i2c_master_probe / i2c_master_bus_add_device /
 *    i2c_master_transmit。不碰已废弃的 i2c_cmd_link_* 老接口。
 * 2. 显存（framebuf）布局和 SSD1306 GDDRAM 完全一致：
 *        buf[page * 128 + x]，每字节 8 行，bit0 = 该列最上面那一行。
 * 3. 【没插屏也能跑】：探测/加设备/首次传输任一失败 → ready=false，
 *    oled_is_ready() 返回 false，之后所有刷新函数都是空操作，
 *    绝不 ESP_ERROR_CHECK / assert / abort。
 * 4. 【文字渲染不在这里】—— 2026-09-29 清理：
 *    本文件原先自带一套 6x8 / 8x16 / 16x16 中文字库 + 仪表盘/开机画面绘制，
 *    这些在换成 astra UI（u8g2 画布）之后【全工程零调用】，已删除（连 oled_font.c
 *    和被它生成的 tools/gen_font.ps1 一起删了）。
 *    现在屏幕内容由 astra UI 用 u8g2 画进画布，再按页交给 oled_write_page()。
 *    本模块只保留：初始化 / 探测 / 清屏 / 整屏刷新 / 按页写入。
 */
#include "oled.h"
#include "i2c_bus.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "board_config.h"

static const char *TAG = "OLED";

/* ============================ 常量 ============================ */

#define OLED_PAGES          (OLED_HEIGHT / 8)                 /* 64 行 = 8 页 */
#define OLED_BUF_SIZE       (OLED_WIDTH * OLED_HEIGHT / 8)    /* 128*64/8 = 1024 字节 */

/* SSD1306 I2C 控制字节：Co=0, D/C#=0 → 后续全是命令；D/C#=1 → 后续全是数据 */
#define OLED_CTRL_CMD       0x00
#define OLED_CTRL_DATA      0x40

/* 一次 I2C 传输的最大数据量：128 字节 = 一整页，避免单次传输过长 */
#define OLED_CHUNK_MAX      OLED_WIDTH

/* I2C 出错日志最多打几条，避免没插屏时刷屏 */
#define OLED_IO_ERR_LOG_MAX 3

/* ============================ 静态状态 ============================ */

/** 显存：1024 字节，布局 = SSD1306 GDDRAM（buf[page*128 + x]，bit0 在上） */
static uint8_t s_framebuf[OLED_BUF_SIZE];

static i2c_master_dev_handle_t s_dev = NULL;    /* OLED 从机句柄 */
static uint8_t s_addr = BSP_I2C_ADDR_SSD1306;   /* 实际探测到的地址（0x3C 或 0x3D） */
static bool s_ready = false;                    /* 屏是否可用（false → 全部空操作） */
static uint32_t s_io_err_cnt = 0;               /* 运行期 I2C 错误计数（只用于限制日志） */

/**
 * @brief SSD1306 128x64 标准初始化序列
 * 参数按常见模块（0.96" 128x64）取值，见注释。
 */
static const uint8_t s_init_cmds[] = {
    0xAE,               /* display off */
    0x20, 0x00,         /* memory addressing mode = horizontal */
    0xB0,               /* page start address = 0 */
    0xC8,               /* COM output scan direction = remapped（上下不倒） */
    0x00,               /* lower column address = 0 */
    0x10,               /* higher column address = 0 */
    0x40,               /* display start line = 0 */
    0x81, 0xCF,         /* contrast = 0xCF */
    0xA1,               /* segment remap（左右不倒） */
    0xA6,               /* normal display（非反白） */
    0xA8, 0x3F,         /* multiplex ratio = 64 */
    0xA4,               /* entire display ON 跟随 RAM 内容 */
    0xD3, 0x00,         /* display offset = 0 */
    0xD5, 0x80,         /* clock divide ratio / oscillator frequency */
    0xD9, 0xF1,         /* pre-charge period */
    0xDA, 0x12,         /* COM pins hardware configuration */
    0xDB, 0x40,         /* VCOMH deselect level */
    0x8D, 0x14,         /* charge pump enable（模块内部升压，必须开）*/
    0xAF,               /* display on */
};

/* ============================ 底层 I2C / 显存工具 ============================ */

/** @brief 纯 I2C 写（不做 ready 判断，初始化过程中也要用） */
static esp_err_t oled_i2c_write(const uint8_t *buf, size_t len)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* xfer_timeout_ms = BSP_I2C_TIMEOUT_MS（board_config.h，不许硬编码） */
    return i2c_master_transmit(s_dev, buf, len, BSP_I2C_TIMEOUT_MS);
}

/** @brief 运行期 I2C 失败计数 + 限流日志（屏被拔掉时不要刷屏） */
static void oled_note_io_err(const char *what, esp_err_t err)
{
    if (s_io_err_cnt < OLED_IO_ERR_LOG_MAX) {
        ESP_LOGW(TAG, "%s failed: %s (%u)", what, esp_err_to_name(err),
                 (unsigned)(s_io_err_cnt + 1));
    }
    s_io_err_cnt++;
}

/**
 * @brief 发一串 SSD1306 命令（会自动在前面补控制字节 0x00）
 * @param cmds 命令字节数组
 * @param n    命令字节数（不含控制字节）
 */
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

/** @brief 只清显存（不上屏），初始化内部使用，不受 ready 影响 */
static void framebuf_clear(void)
{
    memset(s_framebuf, 0x00, sizeof(s_framebuf));
}

/* ============================ 初始化 ============================ */

esp_err_t oled_init(void)
{
    /* 幂等：已经起来了就直接返回 */
    if (s_ready) {
        ESP_LOGD(TAG, "oled already initialized");
        return ESP_OK;
    }

    /* 1. I2C 总线（幂等，OLED/传感器共用；失败说明总线都建不起来） */
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

    /* 2. 探测地址。
     *    SSD1306 模块常见两种 I2C 地址：0x3C（绝大多数）和 0x3D（ADDR 脚被拉高）。
     *    两个都试一下，省得买到 0x3D 的模块还得手动改 board_config.h。 */
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

    /* 3. 添加 OLED 从机设备（重复调用时复用已有句柄） */
    if (s_dev == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,      /* 7 位地址 */
            .device_address  = s_addr,                  /* 探测到的地址 */
            .scl_speed_hz    = BSP_I2C_FREQ_HZ,         /* 400kHz，来自 board_config.h */
        };
        err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add device 0x%02X failed: %s", s_addr, esp_err_to_name(err));
            s_dev = NULL;
            s_ready = false;
            return ESP_ERR_NOT_FOUND;
        }
    }

    /* 4. 发初始化序列（这一步失败 = 屏其实不在 / 接线有问题） */
    err = oled_send_cmds(s_init_cmds, sizeof(s_init_cmds));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send init sequence failed: %s, OLED disabled", esp_err_to_name(err));
        s_ready = false;
        return ESP_ERR_NOT_FOUND;
    }

    /* 5. 清屏并刷一次，避免上电出现雪花 */
    framebuf_clear();
    s_ready = true;                     /* refresh 内部要检查 ready，所以先置位 */
    s_io_err_cnt = 0;
    oled_refresh();

    ESP_LOGI(TAG, "SSD1306 128x64 ready @0x%02X, %dkHz SDA=%d SCL=%d",
             s_addr, (int)(BSP_I2C_FREQ_HZ / 1000),
             (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO);
    return ESP_OK;
}

bool oled_is_ready(void)
{
    return s_ready;
}

/* ============================ 显存刷新 ============================ */

void oled_clear(void)
{
    if (!s_ready) {
        return;
    }
    framebuf_clear();
}

void oled_write_page(uint8_t page, const uint8_t *data)
{
    if (!s_ready || s_dev == NULL || data == NULL) {
        return;
    }
    if (page >= OLED_PAGES) {
        return;
    }

    /* 光标定位到 (x=0, page)，然后整页突发写入（与 oled_refresh 同一套时序） */
    const uint8_t cmds[3] = {
        (uint8_t)(0xB0 | page),
        0x00,                                   /* 低 4 位列地址 = 0（SSD1306 列偏移 0） */
        0x10,                                   /* 高 4 位列地址 = 0 */
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

void oled_refresh(void)
{
    if (!s_ready || s_dev == NULL) {
        return;
    }

    uint8_t buf[1 + OLED_CHUNK_MAX];    /* [控制字节][128 字节数据] = 129 字节 */
    buf[0] = OLED_CTRL_DATA;

    for (int page = 0; page < OLED_PAGES; page++) {
        /* 光标定位到 (x=0, page)：0xB0|page、低 4 位列地址、高 4 位列地址 */
        const uint8_t cmds[3] = {
            (uint8_t)(0xB0 | page),
            0x00,                                   /* 0x00 | (0 & 0x0F) */
            0x10,                                   /* 0x10 | ((0 >> 4) & 0x0F) */
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
