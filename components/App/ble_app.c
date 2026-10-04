/**
 * @file  ble_app.c
 * @brief BLE (NimBLE) GATT 服务端实现 —— 接口说明见 ble_app.h
 *
 * ===========================================================================
 *  【手机端契约】—— 以下每一条都和 android/app/.../data/Contract.kt 一一对应，
 *  任何一条改动都必须两边同时改，否则"能连上但收不到数据"。
 * ---------------------------------------------------------------------------
 *  广播名        SmartHome-<uid>
 *                uid = MAC 后 3 字节的十六进制小写（如 "a1b2c3"）。
 *                取法和 MQTT topic 里的唯一 ID【完全一致】：
 *                   CONFIG_APP_MQTT_UID_OVERRIDE 非空 → 用它
 *                   否则 → wifi_get_mac_suffix()（wifi_sta.c，内部会退回读 eFuse）
 *                为什么必须一致：一台设备在 MQTT 和 BLE 上必须能被认成同一台，
 *                否则手机端会把它当成两个设备。
 *
 *  Service UUID  a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d
 *  RX  特征      a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5e   WRITE + WRITE_NO_RSP  手机→板子
 *  TX  特征      a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5f   NOTIFY                板子→手机
 *
 *  ⚠⚠ NimBLE 的 ble_uuid128_t.value[16] 是【小端序】（低字节在前），
 *      所以标准 UUID 字符串的 16 个字节要【倒过来】写。举例：
 *        a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d
 *        → {0x5d,0x4c,0x3b,0x2a,0x1f,0x0e,0x9d,0x8c,0x7b,0x4a,0xf6,0xe5,0xd4,0xc3,0xb2,0xa1}
 *      只有第一个字节随最后一段变化（...c5d / ...c5e / ...c5f），后面 15 个字节三者相同。
 *      写成大端（正序）也能编译、也能被手机扫到，但 UUID 会变成另一个值，
 *      手机按原 UUID 过滤就一个设备都找不到 —— 这是最容易踩的坑，故在此写明。
 *
 *  帧格式（两个方向一致）：byte[0] = 类型码，byte[1..] = UTF-8 JSON（不含结尾 '\0'）
 *    手机→板子：0x01 = cmd 控制命令，0x02 = config 阈值配置，0x03 = get 主动查询
 *    板子→手机：直接用 app_msg_type_t 的数值
 *               1=state 2=sensor 3=ack 4=event 5=config
 *    （上行数值就是 app_link.h 里 enum 的值，两端必须对齐，不能改。）
 *
 * ===========================================================================
 *  【为什么广播包这样分配】—— 31 字节的账，必须算清楚
 * ---------------------------------------------------------------------------
 *  传统广播 PDU 最多 31 字节，而"广播名 + 128 位 Service UUID"一起放【装不下】：
 *      Flags(3) + 完整名 "SmartHome-a1b2c3"(15) + 128 位 UUID(18) = 36 > 31
 *  NimBLE 在这种情况下会返回 BLE_HS_EMSGSIZE，广播根本起不来。
 *
 *  所以按"哪个丢了更致命"来分：
 *    · 主广播包放【Service UUID + Flags】
 *        —— 因为手机端（BleManager.kt）的扫描过滤条件是
 *           ScanFilter.setServiceUuid(...)，而且某些机型/某些扫描模式只看主广播包
 *           （扫描响应必须主动扫描才拿得到）。UUID 一旦丢了，设备直接"扫不到"，
 *           这是致命的。反过来名字丢了只是显示成 "SmartHome-?"，还能连。
 *    · 扫描响应包放【完整广播名】
 *        —— 手机端用的是 SCAN_MODE_LOW_LATENCY（主动扫描），一定能拿到扫描响应；
 *           Android 会把主广播包和扫描响应【合并】成一条 ScanRecord，
 *           所以按名字前缀过滤、以及按 UUID 过滤，两种写法都能命中。
 *
 * ===========================================================================
 *  【为什么连接后立刻推一次数据，但可能被丢掉】
 * ---------------------------------------------------------------------------
 *  手机连上后要经过 连接 → 协商 MTU → 发现服务 → 写 CCCD 订阅 → 发 get(0x03)
 *  这一串步骤（见 BleManager.kt 的 Stage）。而我们在"连接建立"这一刻就推
 *  state/sensor/config，此时：
 *    · 协商前的 ATT MTU 只有默认的 23（载荷 20 字节），几百字节的 JSON 发不出去；
 *    · 手机还没写 CCCD，就算发出去了 Android 协议栈也不会往上层递。
 *  所以这次推送是【尽力而为】的问候，【真正的数据以客户端发的 get(0x03) 为准】：
 *  ble_gatt_rx_access() 收到 0x03 会回一份完整现状（state + sensor + config）。
 *  ⚠ 后来的实测证实了这一点，并且暴露过一个真实缺口：配置 config 只有在
 *    0x03 get 里回才收得到（连接时那一轮永远丢），所以 get 的分支【必须】
 *    把 config 也带上。改动那个分支时请务必保留 config，否则 App 的
 *    "阈值设置"界面会永远空白。
 *  之所以连接时仍然要推：① 需求要求；② 若客户端实现改成"连着就一直读"，
 *  也能拿到数据（且此时 MTU 已协商好的话是能成功的）。
 *  发不出去时我们【不硬发】——超长帧只记警告并跳过，理由见 ble_link_send()。
 */

#include "ble_app.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !CONFIG_BT_ENABLED || !CONFIG_APP_BLE_ENABLE
/* =========================================================================
 *  蓝牙栈没打开（或本模块被 menuconfig 关掉）时的空实现
 * -------------------------------------------------------------------------
 *  为什么要有这一段：main.c 里是 `#if CONFIG_APP_BLE_ENABLE` 包着调用的，
 *  但为了"菜单里关掉 BLE 也一定能编过、链接过"，这里给出不依赖 NimBLE 头文件
 *  的桩函数。这样即使有人在 menuconfig 里只关了 BT_ENABLED 而忘了关
 *  APP_BLE_ENABLE，也只会打印一条警告，而不是一堆"找不到 host/ble_hs.h"。
 * ========================================================================= */
static const char *TAG = "ble_app";

esp_err_t ble_app_start(void)
{
    ESP_LOGW(TAG, "BLE 未编译进固件（CONFIG_BT_ENABLED=%d, CONFIG_APP_BLE_ENABLE=%d），"
                  "手机蓝牙控制不可用；MQTT 不受影响",
             (int)CONFIG_BT_ENABLED, (int)CONFIG_APP_BLE_ENABLE);
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_app_is_connected(void)
{
    return false;
}

int ble_app_get_mtu(void)
{
    return 0;
}

#else  /* ---------------- 以下是真正的实现 ---------------- */

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
/* 显式包含 os_mbuf.h：OS_MBUF_PKTLEN() 宏定义在这里，
 * 而 host/ble_hs_mbuf.h 只前向声明了 struct os_mbuf（不展开宏），
 * 少这一行会得到 "implicit declaration" / "undefined macro" 类的编译错误。 */
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "app_link.h"        /* 传输链路注册表：上行 JSON 从这儿过来 */
#include "app_cmd.h"         /* 下行命令解析入口（实现在 mqtt_app.c） */
#include "mqtt_app.h"        /* mqtt_publish_state/sensor/config：拼 JSON + 广播 */
#include "wifi_sta.h"        /* wifi_get_mac_suffix()：取 MAC 后 3 字节 */
#include "device_model.h"    /* SRC_BLE：命令来源标记 */

static const char *TAG = "ble_app";

/* 手机端有义务实现这两个函数（NimBLE 要求应用提供 store 回调，否则配对时会空指针）。
 * 本工程不用配对（特征都不要求加密），但按 IDF 官方例程的习惯还是初始化一下，
 * 免得以后加 SMP 时踩坑。声明照抄 IDF 例程（头文件在私有 include 里）。 */
void ble_store_config_init(void);

/* ========================================================================= */
/*  1. 契约常量                                                              */
/* ========================================================================= */

/** 广播名前缀。默认 "SmartHome"，改 menuconfig 要同步改手机端 Contract.kt */
#define BLE_NAME_PREFIX      CONFIG_APP_BLE_DEVICE_NAME_PREFIX

/**
 * 广播名的最大长度。
 * 扫描响应包总共 31 字节，名字这一条 AD 结构占 1(长度) + 1(类型) + N(名字)，
 * 所以 N 最大 29。超了 NimBLE 会返回 EMSGSIZE、广播起不来，所以这里主动截断
 * 并告警 —— 截断后名字仍然以 "SmartHome-" 开头，手机端的前缀过滤还能命中。
 */
#define BLE_NAME_MAX_LEN     29

/* Service UUID 与两个特征 UUID（小端序，见文件头注释） */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x5d, 0x4c, 0x3b, 0x2a, 0x1f, 0x0e, 0x9d, 0x8c,
    0x7b, 0x4a, 0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1);

static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x5e, 0x4c, 0x3b, 0x2a, 0x1f, 0x0e, 0x9d, 0x8c,
    0x7b, 0x4a, 0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1);

static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x5f, 0x4c, 0x3b, 0x2a, 0x1f, 0x0e, 0x9d, 0x8c,
    0x7b, 0x4a, 0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1);

/* 手机→板子 的帧类型码（板子→手机用 app_msg_type_t 的数值，见 app_link.h） */
#define BLE_DOWN_CMD         0x01   /**< 控制命令 JSON */
#define BLE_DOWN_CONFIG      0x02   /**< 阈值配置 JSON */
#define BLE_DOWN_GET         0x03   /**< 主动查询：立刻回 state + sensor */

/**
 * 我们希望的 ATT MTU。
 * 手机端（BleManager.kt）请求 517，我们回 512，最终协商为 min(517, 512) = 512，
 * 单包净载荷 = MTU - 3 = 509 字节。state JSON 约 250~330 字节，一份就装得下。
 * 为什么不用 517：留一点余量给不同协议栈的实现差异，512 已经远超需求。
 * 同步在 sdkconfig 里设了 CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=512；
 * 这里再调一次 ble_att_set_preferred_mtu() 是为了"代码自证"——
 * 就算有人把 sdkconfig 改了，代码里的值仍然是权威。
 */
#define BLE_MTU_PREFERRED    512

/* ATT 通知/写入 的 3 字节协议开销：1 字节 opcode + 2 字节 attribute handle */
#define BLE_ATT_OVERHEAD     3

/** 一帧最多多少字节（帧头 1 + JSON） */
#define BLE_RX_FRAME_MAX     (BLE_MTU_PREFERRED + 1)
/** 接收缓冲：帧最长 513，再留 1 字节放 '\0'（只为日志好打，解析一律用长度） */
#define BLE_RX_BUF_LEN       (BLE_RX_FRAME_MAX + 1)

/* ========================================================================= */
/*  2. 模块状态                                                              */
/* ========================================================================= */

static bool     s_started = false;    /**< ble_app_start() 幂等标志 */
static char     s_dev_name[BLE_NAME_MAX_LEN + 1] = { 0 };

/**
 * 当前连接句柄，未连接为 BLE_HS_CONN_HANDLE_NONE。
 *
 * volatile 的理由：它由 NimBLE 主机任务（GAP 事件回调）写，由其它任务
 * （app_loop / MQTT 任务 / 按键任务）通过 ble_app_is_connected()、ble_link_send()
 * 读。严格说 volatile 不是内存屏障，但这里的竞争是【良性】的：
 *   · 读到一个刚断开的旧句柄 → ble_att_mtu() / ble_gatts_notify_custom() 内部
 *     自己会校验句柄是否存在，返回 0 / BLE_HS_ENOTCONN，不会崩；
 *   · 读到 NONE → 直接当成"没连接"，不发。
 * 为了这一处竞争上临界区反而要冒在中断/高优先级上下文里争锁的风险，不划算。
 */
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

/** TX 特征的 value handle，由 NimBLE 在注册服务时回填（见 s_gatt_svcs） */
static uint16_t s_tx_val_handle = 0;

/** 本机蓝牙地址类型（ble_hs_id_infer_auto 的结果），广播时要用 */
static uint8_t  s_own_addr_type = 0;

/**
 * 收帧缓冲。
 *
 * 为什么可以用 static 而不是局部变量：
 *   GATT 访问回调【只在 NimBLE 主机任务里】被调用（单任务串行），所以这个缓冲
 *   不存在并发访问。放 static 是为了把 NimBLE 主机任务的栈（默认 4096 字节）
 *   省下来 —— 一帧 514 字节的局部数组，加上下面还要调 cJSON 解析，
 *   4KB 的栈很容易爆。爆栈是那种"偶发重启、很难查"的故障，不值得为优雅去赌。
 */
static uint8_t  s_rx_buf[BLE_RX_BUF_LEN];

/* ========================================================================= */
/*  3. app_link：让 BLE 成为一条上行链路                                      */
/* ========================================================================= */

static esp_err_t ble_link_send(app_msg_type_t type, const char *json, size_t len);
static bool      ble_link_is_connected(void);

static const app_link_t s_ble_link = {
    .name         = "ble",
    .send         = ble_link_send,
    .is_connected = ble_link_is_connected,
};

/* ========================================================================= */
/*  4. 小工具                                                                */
/* ========================================================================= */

/** 把 6 字节蓝牙地址格式化成 "AA:BB:CC:DD:EE:FF"（只用于日志） */
static void ble_format_addr(const uint8_t addr[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

/**
 * @brief 生成唯一 ID（"a1b2c3" 之类）
 *
 * 刻意和 mqtt_protocol.c::build_uid() 取同一套值：
 *   优先 CONFIG_APP_MQTT_UID_OVERRIDE（用户在生产环境里给每台板子指定 ID），
 *   否则用 wifi_get_mac_suffix()。
 * 为什么不直接调 mqtt_protocol.c 里的函数：那个 build_uid() 是 static，
 * 而且它属于 MQTT 模块的私有实现；为了一个字符串去改它的可见性、
 * 让 BLE 依赖 MQTT 的头文件不划算。这里 6 行复制换来两个模块解耦。
 */
static esp_err_t ble_build_uid(char *buf, size_t len)
{
    if (buf == NULL || len < 7) {          /* "a1b2c3" + '\0' */
        return ESP_ERR_INVALID_ARG;
    }

    if (CONFIG_APP_MQTT_UID_OVERRIDE[0] != '\0') {
        int n = snprintf(buf, len, "%s", CONFIG_APP_MQTT_UID_OVERRIDE);
        if (n < 0 || (size_t)n >= len) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }

    return wifi_get_mac_suffix(buf, len);
}

/**
 * @brief 拼广播名 "<前缀>-<uid>"，并做长度保护
 * @note  截断时必须告警：名字被截断后手机端"按名字过滤"可能失配，
 *        这是能连上/扫不到的分界点，不能静默。
 */
static esp_err_t ble_build_device_name(void)
{
    char uid[16] = { 0 };
    esp_err_t err;
    int n;

    err = ble_build_uid(uid, sizeof(uid));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "取唯一 ID 失败: %s", esp_err_to_name(err));
        return err;
    }

    n = snprintf(s_dev_name, sizeof(s_dev_name), "%s-%s", BLE_NAME_PREFIX, uid);
    if (n < 0 || (size_t)n >= sizeof(s_dev_name)) {
        ESP_LOGW(TAG, "广播名过长（前缀 \"%s\" + uid \"%s\" > %d 字节），已截断为 \"%s\"",
                 BLE_NAME_PREFIX, uid, BLE_NAME_MAX_LEN, s_dev_name);
        /* snprintf 已经安全截断并加了 '\0'，直接用即可（仍以前缀开头） */
    }

    return ESP_OK;
}

/* ========================================================================= */
/*  5. GATT 服务定义                                                          */
/* ========================================================================= */

static int ble_gatt_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);

/**
 * TX 特征的访问回调。
 *
 * TX 只有 NOTIFY，没有任何 READ/WRITE 属性，所以正常流程下 NimBLE 永远不会
 * 调用它（读写都会被协议栈以"属性不支持"拒掉）。那为什么还要给？
 *   ① NimBLE 的 ble_gatts_add_svcs() 要求每个特征都有 access_cb 才能注册；
 *   ② 万一以后给 TX 加上 READ（有些 App 喜欢开机先读一次最后状态），
 *      这里能直接返回"未实现"而不是空指针崩溃。
 * 所以它存在的意义是"占位 + 兜底"，不是功能。
 */
static int ble_gatt_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    /* 真走到了说明有人给 TX 加了 READ/WRITE 属性但没实现回调，写清楚原因 */
    ESP_LOGW(TAG, "TX 特征收到不支持的访问 (op=%d)，已拒绝", ctxt->op);
    return BLE_ATT_ERR_UNLIKELY;
}

/* 服务表。注意 characteristics 是【数组字面量】，最后一个元素必须全 0 收尾 */
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                /* RX：手机 → 板子。写命令 */
                .uuid      = &s_rx_uuid.u,
                .access_cb = ble_gatt_rx_access,
                /* WRITE = 带响应的写（手机端会优先用它，见 BleManager.writeFrame），
                 * WRITE_NO_RSP = 不带响应的写。两个都给：手机端代码里判断
                 * "有 NO_RSP 且没有 WRITE" 时才用 NO_RSP，所以最终会走带响应那条，
                 * 我们能拿到写结果去回 ack；但如果手机端以后改成 NO_RSP，
                 * 也能直接跑，不用改固件。 */
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* TX：板子 → 手机。通知 */
                .uuid       = &s_tx_uuid.u,
                .access_cb  = ble_gatt_tx_access,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                /* 让 NimBLE 把实际分配的 value handle 写回来，
                 * ble_gatts_notify_custom() 要用它 */
                .val_handle = &s_tx_val_handle,
            },
            {
                0,   /* 本服务特征结束 */
            },
        },
    },
    {
        0,           /* 本服务表结束 */
    },
};

/* ========================================================================= */
/*  6. 广播                                                                  */
/* ========================================================================= */

static int ble_gap_event(struct ble_gap_event *event, void *arg);
static void ble_start_advertising(void);

/**
 * @brief 配置并启动广播
 * @note  在 NimBLE 主机任务（sync 回调 / 断开事件）里调用。
 *        广播参数用 BLE_HS_FOREVER：一直广播到有人连上，或出错。
 */
static void ble_start_advertising(void)
{
    struct ble_hs_adv_fields adv = { 0 };
    struct ble_hs_adv_fields rsp = { 0 };
    struct ble_gap_adv_params params = { 0 };
    int rc;

    /* ---------- 主广播包：Flags + 128 位 Service UUID ---------- */
    /* DISC_GEN：通用可发现；BREDR_UNSUP：告诉手机"我不支持经典蓝牙"，
     * 免得 Android 去试 BR/EDR 连接白白多花几百毫秒 */
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    adv.uuids128 = &s_svc_uuid;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;   /* 用"完整 UUID 列表"(0x07)，
                                     * Android 的 ScanFilter.setServiceUuid 才认 */

    /* 为什么这里【不放】tx_pwr_lvl / appearance：
     *   主广播包只有 31 字节，已经用掉 3 + 18 = 21。这两个字段都是"锦上添花"，
     *   而手机端用不到（它不看 TX Power，也不看 appearance）。
     *   把字节留给以后真要加的字段（比如厂商自定义数据），比现在塞满更稳。 */

    rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播数据失败 rc=%d（字段超 31 字节？）", rc);
        return;
    }

    /* ---------- 扫描响应包：完整广播名 ---------- */
    /* 名字放扫描响应的原因见文件头"31 字节的账" */
    rsp.name = (uint8_t *)s_dev_name;
    rsp.name_len = (uint8_t)strlen(s_dev_name);
    rsp.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置扫描响应失败 rc=%d（名字 %u 字节太长？）",
                 rc, (unsigned)rsp.name_len);
        /* 名字设置失败不致命：主广播包里的 Service UUID 够手机扫到了，
         * 只是名字会显示不出来。继续往下走，别让整个 BLE 功能因为一个名字挂掉。 */
    }

    /* ---------- 广播参数 ---------- */
    params.conn_mode = BLE_GAP_CONN_MODE_UND;   /* 可连接的非定向广播 */
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;   /* 通用可发现 */
    /* 100ms / 110ms：手机端要"秒发现"，本机是市电供电，不在乎这点功耗。
     * 再快没意义（BLE 规范最小 20ms），再慢用户会觉得"连不上"。 */
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(110);

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "启动广播失败 rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "广播已开始：名 \"%s\"（在扫描响应里），Service UUID 在广播包里",
             s_dev_name);
}

/* ========================================================================= */
/*  7. GAP 事件：连接 / 断开 / MTU / 订阅                                     */
/* ========================================================================= */

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    /* ---------------- 连接建立 / 失败 ---------------- */
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            /* 连接失败（比如手机中途走了）。必须重新开广播，否则板子就"消失"了 */
            ESP_LOGW(TAG, "连接失败 status=%d，重新开始广播", event->connect.status);
            ble_start_advertising();
            return 0;
        }

        struct ble_gap_conn_desc desc;
        char addr[18] = "??";

        s_conn_handle = event->connect.conn_handle;

        if (ble_gap_conn_find(s_conn_handle, &desc) == 0) {
            ble_format_addr(desc.peer_id_addr.val, addr);
        }
        ESP_LOGI(TAG, "手机已连接：handle=%d addr=%s", s_conn_handle, addr);

        /* 主动把我们要用的 MTU 报给协议栈。
         * 注意：作为 peripheral，MTU 交换是【手机发起】的（Android 的 requestMtu），
         * 我们只能声明"我最多接受 512"，实际用 min(双方)。这里再设一次是保险，
         * 保证即使有人漏改了 sdkconfig，协商结果也不会退化成默认的 23。 */
        int rc = ble_att_set_preferred_mtu(BLE_MTU_PREFERRED);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_att_set_preferred_mtu(%d) 失败 rc=%d", BLE_MTU_PREFERRED, rc);
        }

        /* 立刻推一次 state + sensor + config（尽力而为，理由见文件头）。
         * ⚠ 实测：这一轮【必然收不到】—— 此刻客户端还没写 CCCD 订阅，
         *   而且 MTU 还是默认的 23（下面那三条 "帧 > 当前 MTU ... 本条不上报"
         *   警告就是它），所以它只是"礼貌性的问候"。
         *   真正让客户端拿到首屏数据的是它随后发的 get(0x03)。
         *   保留这段是为了：① 需求如此；② 某类客户端可能不订阅、只是周期读，
         *   那它们至少能在 MTU 协商好后收到推送。
         * ★ 这里用的是 mqtt_publish_xxx()，它们现在是"拼 JSON + app_link_broadcast()"，
         *   所以这一推会同时发给 MQTT 和 BLE 两条链路，不会漏掉任何一边。
         *   我们【绝对不能】在这里再注册一个 device_register_cb —— 那个回调槽位
         *   已经被 mqtt_app_bind_device_events() 独占（"只保留最后一个"），
         *   再注册会把 MQTT 的状态上报顶掉。 */
        (void)mqtt_publish_state();
        (void)mqtt_publish_sensor(NULL);
        (void)mqtt_publish_config();
        return 0;
    }

    /* ---------------- 断开 ---------------- */
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "手机已断开：handle=%d reason=%d",
                 event->disconnect.conn.conn_handle, event->disconnect.reason);
        /* 必须置回 NONE：否则 app_link 仍以为"连着"，会一直往一个不存在的
         * 连接上发通知，日志刷屏且白费 CPU */
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        /* 重新广播，让手机能重连。不做这一步的话，断开一次就得重启板子。 */
        ble_start_advertising();
        return 0;

    /* ---------------- 广播意外结束 ---------------- */
    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* 用 BLE_HS_FOREVER 启动的广播正常不会走到这里，只有出错才会。
         * 重新拉起，保证设备一直可被发现。 */
        ESP_LOGW(TAG, "广播结束 reason=%d，重新开始广播", event->adv_complete.reason);
        ble_start_advertising();
        return 0;

    /* ---------------- MTU 协商完成 ---------------- */
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 已协商：handle=%d cid=%d mtu=%d（单包净载荷 %d 字节）",
                 event->mtu.conn_handle, event->mtu.channel_id, event->mtu.value,
                 event->mtu.value - BLE_ATT_OVERHEAD);
        return 0;

    /* ---------------- 有人写了某个 CCCD（订阅/取消订阅） ---------------- */
    case BLE_GAP_EVENT_SUBSCRIBE:
        /* ⚠ 这个事件【不只】对应我们 TX 特征的通知订阅：
         *   手机（Android 协议栈自己）还会去写 GAP 服务的 Service Changed 描述符
         *   （UUID 0x2A05，通常是很小的 handle，如 8），而且用的是【indication】
         *   而不是 notification。实测日志里出现过
         *       "手机已取消订阅 TX 通知（handle=8）"
         *   这句话是【误导】的 —— handle=8 不是我们的 TX（我们的 TX value handle
         *   实测是 18），cur_notify=false 只是说明对方启用的是 indication。
         *   所以这里把 notify/indicate 两个标志都打出来，并标明 handle，
         *   免得以后有人照着日志去排查一个不存在的"订阅被取消"问题。
         *
         * 只记日志，【不】拿订阅状态当"要不要发通知"的开关：
         *   手机没订阅时 BLE 协议栈本来就收不到通知（Android 必须写了 CCCD 才会
         *   把数据往上层递），所以订阅与否不影响"手机能不能看到数据"这个结果；
         *   但如果拿它当开关，一旦哪台手机的协议栈没有上报这个事件，
         *   功能就会【彻底】失效 —— 而我们手上没有手机可以验证这一点。
         *   宁可多花一点空口时间，也不要把功能挂在一个我无法验证的事件上。 */
        ESP_LOGI(TAG, "CCCD 写入：handle=%d notify=%d->%d indicate=%d->%d"
                      "（handle=%d 是 TX 特征，其它 handle 是 GAP Service Changed 之类）",
                 event->subscribe.attr_handle,
                 event->subscribe.prev_notify, event->subscribe.cur_notify,
                 event->subscribe.prev_indicate, event->subscribe.cur_indicate,
                 s_tx_val_handle);
        return 0;

    /* ---------------- 连接参数更新（由手机决定，我们只记日志） ---------------- */
    case BLE_GAP_EVENT_CONN_UPDATE:
        /* 为什么不在这里主动 ble_gap_update_params()：
         *   Android 连上后自己会请求一套连接参数（通常 30~50ms 间隔），
         *   外设再去改会和它打架，还可能被手机拒绝。让手机做主更省事。
         *   如果以后发现延迟偏高，再在这里发起更新也不迟。 */
        ESP_LOGI(TAG, "连接参数已更新 status=%d", event->conn_update.status);
        return 0;

    default:
        /* 其它事件（扫描报告、扩展广播等）我们不用，静默忽略 */
        return 0;
    }
}

/* ========================================================================= */
/*  8. 收帧：手机 → 板子                                                      */
/* ========================================================================= */

/**
 * @brief RX 特征的写回调：拆帧 → 分发
 *
 * 帧结构：[0]=类型码，[1..]=UTF-8 JSON（不含结尾 '\0'）
 * 这里【逐条校验】：长度 0、超过缓冲、类型码非法，全部记日志后安全返回，
 * 绝不能让一个畸形包把板子搞崩（手机端也可能有 bug，甚至是恶意扫描器）。
 */
static int ble_gatt_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    /* 只接受"写特征"操作。READ 之类的走到这里说明服务表被改坏了 */
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGW(TAG, "RX 特征收到非写操作 op=%d，已拒绝", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }

    /* ① 长度检查。
     * ctxt->om 是 mbuf 链，一帧超过一个 mbuf 块时是【多块】的，
     * 所以不能像官方小例子那样只看 ctxt->om->om_len（那只有第一块的长度），
     * 必须用 PKTLEN 拿整链长度，并用 ble_hs_mbuf_to_flat() 安全地拷出来。 */
    uint16_t pkt_len = OS_MBUF_PKTLEN(ctxt->om);

    if (pkt_len == 0) {
        ESP_LOGW(TAG, "收到空帧，已忽略");
        return 0;    /* 空帧不是协议错误，回 0 让协议栈正常完成这次写 */
    }
    if (pkt_len > BLE_RX_FRAME_MAX) {
        /* 超过我们声明的 MTU 上限：不正常，拒绝并让手机端拿到错误码 */
        ESP_LOGE(TAG, "帧长 %u 超过上限 %u 字节，已拒绝",
                 (unsigned)pkt_len, (unsigned)BLE_RX_FRAME_MAX);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf, (uint16_t)(BLE_RX_BUF_LEN - 1), &copied);
    if (rc != 0) {
        /* ble_hs_mbuf_to_flat 内部会自己夹紧长度，这里走到说明底层拷贝失败 */
        ESP_LOGE(TAG, "从 mbuf 取数据失败 rc=%d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }
    /* 只为日志好打；下面的 JSON 解析一律用显式长度，不依赖这个 '\0' */
    s_rx_buf[copied] = '\0';

    uint8_t     type     = s_rx_buf[0];
    const char *json     = (const char *)(s_rx_buf + 1);
    int         json_len = (int)copied - 1;      /* 可能为 0：只有帧头的空命令 */

    ESP_LOGI(TAG, "收到帧 type=0x%02X len=%d payload=%.*s",
             (unsigned)type, (int)copied, json_len > 0 ? json_len : 0, json);

    switch (type) {
    case BLE_DOWN_CMD:
        /* 控制命令。app_cmd_handle_json 内部会做 JSON 解析、值域收敛、
         * 执行、并通过 app_link_broadcast() 回 ack/state —— MQTT 和 BLE 都收到。
         * 传 SRC_BLE（不是 SRC_MQTT）：device_model 靠它判断"自动模式下手动优先"
         * 以及打印来源日志，混用会让日志和行为都失真。 */
        if (json_len <= 0) {
            ESP_LOGW(TAG, "cmd 帧没有 JSON 内容，已忽略");
            break;
        }
        app_cmd_handle_json(json, json_len, SRC_BLE);
        break;

    case BLE_DOWN_CONFIG:
        if (json_len <= 0) {
            ESP_LOGW(TAG, "config 帧没有 JSON 内容，已忽略");
            break;
        }
        app_cmd_handle_config_json(json, json_len, SRC_BLE);
        break;

    case BLE_DOWN_GET:
        /* 主动查询。手机在订阅完 CCCD 之后会发这个（payload 是 {"get":"state"}，
         * 我们不看内容，收到就回一份【完整现状】），这也是连接后真正拿到
         * 首屏数据的路径。
         *
         * ★ 为什么必须【连 config 一起回】（实测踩过的坑）：
         *   客户端（Android App 与 tools/ble_probe.py）的标准流程是
         *     连接 → 协商 MTU → 发现服务 → 写 CCCD 订阅 → 发 get(0x03)
         *   而"连接建立"那一刻我们推的 state/sensor/config 是在【订阅之前】，
         *   手机协议栈根本不会把通知递到 App 层 —— 那一轮等于推给空气。
         *   所以 get 才是真正可靠的第一份数据，必须把 config 也带上，
         *   否则 App 的"阈值设置"界面永远是空的（实测确认过：
         *   只回 state+sensor 时，客户端收得到 state/sensor，永远收不到 config）。
         *   语义上也自洽：get = "把当前完整现状给我一份"。
         *
         * 为什么直接调 mqtt_publish_xxx 而不是自己拼 JSON 再 app_link_broadcast()：
         *   拼 JSON 的活儿必须只有一份（在 mqtt_app.c 里），这里再拼一遍就是重复实现，
         *   以后加字段必然漂移。mqtt_publish_state() 语义上就是"上报一次全量状态"，
         *   名字里的 mqtt_ 只是历史遗留，它现在是"拼 JSON + 广播给所有链路"。 */
        ESP_LOGI(TAG, "手机请求刷新：回 state + sensor + config");
        (void)mqtt_publish_state();
        (void)mqtt_publish_sensor(NULL);
        (void)mqtt_publish_config();
        break;

    default:
        /* 非法类型码：只记日志，不崩、不断连。断开是惩罚错发，而不是惩罚我们 */
        ESP_LOGW(TAG, "未知帧类型 0x%02X，已忽略（len=%d）", (unsigned)type, (int)copied);
        break;
    }

    /* ---- 栈余量自检（★ 这是踩过坑之后加的，别删） ----
     * 背景：本回调【跑在 NimBLE 主机任务里】，而 0x01/0x02 这两条路径会一路
     * 调进业务层：cJSON 解析 → device_model → 设备事件回调 → mqtt_publish_state()
     * （它自己就有一个 1024 字节的栈缓冲）→ esp-mqtt 发送。
     * 实测在 CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096（IDF 默认值）下，
     * 收到一条 {"dev":"led_kitchen","action":"on"} 就会：
     *     ***ERROR*** A stack overflow in task nimble_host has been detected.
     * 然后整块板子重启。所以 sdkconfig 里把这个栈调到了 8192。
     *
     * uxTaskGetStackHighWaterMark(NULL) = 本任务【历史最小剩余栈】，
     * 也就是刚刚这条帧处理过程中最深的那一点剩了多少。
     * 正常情况它远大于 1KB，完全静默；一旦有人以后又往这条链路上加
     * 重量级调用（比如在回调里再套一层 JSON 解析），余量掉到 1KB 以下就会告警，
     * 而不是等到某天现场"随机重启"才发现。 */
    UBaseType_t stack_left = uxTaskGetStackHighWaterMark(NULL);
    if (stack_left < 1024) {
        ESP_LOGW(TAG, "NimBLE 主机任务栈余量只剩 %u 字节（type=0x%02X）——"
                      "请调大 CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE",
                 (unsigned)stack_left, (unsigned)type);
    }

    return 0;
}

/* ========================================================================= */
/*  9. 发帧：板子 → 手机（app_link 的 send 实现）                             */
/* ========================================================================= */

static bool ble_link_is_connected(void)
{
    return (s_conn_handle != BLE_HS_CONN_HANDLE_NONE);
}

/**
 * @brief 把一条 JSON 作为 BLE 通知发出去
 *
 * 帧 = [类型码 1 字节] + [JSON len 字节]，然后 notify 给当前连接的手机。
 *
 * @return ESP_OK               已交给协议栈
 *         ESP_ERR_INVALID_STATE 没连手机 / 帧超长（见下），app_link_broadcast()
 *                               对 INVALID_STATE 是【静默跳过】的，不会刷日志
 *         ESP_ERR_INVALID_ARG   参数非法
 *         ESP_FAIL              协议栈拒绝（句柄失效等）
 */
static esp_err_t ble_link_send(app_msg_type_t type, const char *json, size_t len)
{
    if (json == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t conn = s_conn_handle;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t frame_len = len + 1;      /* +1 = 类型码 */

    /* ---- MTU 检查：超过单包净载荷时【记警告并放弃】，不硬发 ----
     * 为什么不是截断后发：截断出来的 JSON 一定解析失败，手机端要么报错要么显示
     * 一半状态，比"这条没收到"更糟。
     * 为什么不是硬发整包：ATT 明文规定单条通知不得超过 MTU-3，
     * 超过的部分会被对端 L2CAP/ATT 层直接丢弃（我们白占空口），
     * 个别协议栈还会因此报错断开。所以最好的选择是：这条不发，但把话说清楚。
     * 什么时候会看到这条警告：
     *   · 正常情况不会（手机协商到 512，state JSON 只有 ~300 字节）；
     *   · 手机连上但还没完成 MTU 协商的那几百毫秒里，MTU 还是默认 23，
     *     连接瞬间那次 state/sensor/config 的推送会命中这里 —— 这是预期的，
     *     手机随后会发 get(0x03) 重新要一遍。
     * 返回值用 INVALID_STATE 而不是 INVALID_SIZE，是为了让 app_link_broadcast()
     * 走"静默跳过"分支，避免同一条问题被记两遍日志（上面这条 ESP_LOGW 才是诊断信息）。 */
    int mtu = ble_att_mtu(conn);
    if (mtu > 0 && frame_len > (size_t)(mtu - BLE_ATT_OVERHEAD)) {
        ESP_LOGW(TAG, "%s 帧 %u 字节 > 当前 MTU %d 的净载荷 %d 字节，本条不上报",
                 app_msg_type_name(type), (unsigned)frame_len, mtu,
                 mtu - BLE_ATT_OVERHEAD);
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- 组帧 ----
     * 为什么先 malloc 成一段连续内存再交给 ble_hs_mbuf_from_flat()：
     *   ble_hs_mbuf_from_flat() 会自动把数据拆进 mbuf 链（超过一块也能处理），
     *   比手工 os_mbuf_append 两段（帧头 + JSON）简单得多，也更不容易写错。
     *   代价是一次 malloc —— 一帧最多 500 多字节、最多每秒几次，可以忽略。 */
    uint8_t *frame = (uint8_t *)malloc(frame_len);
    if (frame == NULL) {
        ESP_LOGE(TAG, "组帧 malloc(%u) 失败", (unsigned)frame_len);
        return ESP_ERR_NO_MEM;
    }
    frame[0] = (uint8_t)type;               /* 帧头 = app_msg_type_t 的数值 */
    memcpy(frame + 1, json, len);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)frame_len);
    free(frame);                            /* om 已经拿到副本，源缓冲可以释放 */
    if (om == NULL) {
        ESP_LOGE(TAG, "ble_hs_mbuf_from_flat(%u) 失败（msys 池不够？）",
                 (unsigned)frame_len);
        return ESP_ERR_NO_MEM;
    }

    /* ⚠ ble_gatts_notify_custom() 无论成功失败都会【吃掉并释放】om
     *   （见 nimble/host/src/ble_gattc.c 末尾的 os_mbuf_free_chain(txom)），
     *   所以这里不需要、也绝不能自己再 free 一次。 */
    int rc = ble_gatts_notify_custom(conn, s_tx_val_handle, om);
    if (rc != 0) {
        /* 句柄刚失效（手机正在断开）是最常见的原因，属于正常竞争，只警告不报错 */
        ESP_LOGW(TAG, "notify(%s) 失败 rc=%d（连接可能刚断开）",
                 app_msg_type_name(type), rc);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "已 notify %s（%u 字节，MTU %d）",
             app_msg_type_name(type), (unsigned)frame_len, mtu);
    return ESP_OK;
}

/* ========================================================================= */
/*  10. NimBLE 主机回调与启动                                                 */
/* ========================================================================= */

/** 协议栈复位（内部错误）时的回调：只记日志，NimBLE 会自己重新同步 */
static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE 协议栈复位，reason=%d", reason);
}

/**
 * @brief 协议栈与控制器同步完成 —— 此时才能开始广播
 * @note  跑在 NimBLE 主机任务里
 */
static void ble_on_sync(void)
{
    int rc;

    /* 确保本机有一个可用的蓝牙地址（优先用 eFuse 里的 public 地址） */
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "本机没有可用的蓝牙地址 rc=%d", rc);
        return;
    }

    /* 推断广播时该用哪种地址类型（public / random），结果给 ble_gap_adv_start 用 */
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "推断蓝牙地址类型失败 rc=%d", rc);
        return;
    }

    {
        uint8_t addr[6] = { 0 };
        char    str[18] = { 0 };
        if (ble_hs_id_copy_addr(s_own_addr_type, addr, NULL) == 0) {
            ble_format_addr(addr, str);
            ESP_LOGI(TAG, "本机蓝牙地址 %s（类型 %d）", str, s_own_addr_type);
        }
    }

    ESP_LOGI(TAG, "NimBLE 已就绪：广播名 \"%s\"，MTU 期望 %d，Service "
                  "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
             s_dev_name, BLE_MTU_PREFERRED);

    ble_start_advertising();
}

/**
 * @brief NimBLE 主机任务入口
 * @note  nimble_port_run() 直到 nimble_port_stop() 才返回；本工程不做停止，
 *        所以这个任务会一直活着（和 MQTT 客户端任务一样的常驻角色）。
 */
static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();     /* 不返回 */
    vTaskDelete(NULL);
}

esp_err_t ble_app_start(void)
{
    esp_err_t err;
    int rc;

    /* ---- 幂等：重复调用直接返回，避免起两个主机任务/重复注册链路 ---- */
    if (s_started) {
        ESP_LOGI(TAG, "BLE 已经启动过，跳过");
        return ESP_OK;
    }

    err = ble_build_device_name();
    if (err != ESP_OK) {
        return err;        /* 名字都拼不出来（MAC 读不到），就别往下走了 */
    }

    /* ---- ① 初始化协议栈（含控制器 + 主机）----
     * 这一步必须在 NVS 初始化【之后】调用（main.c 里已经先做了），
     * 因为 NimBLE 的 store 依赖 NVS。
     * 顺带一提：这里【不】需要单独初始化 WiFi/coex —— esp_coexist 由
     * WiFi 与 BT 的初始化各自触发，只要 menuconfig 里
     * CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y 就会自动协商射频时分复用。 */
    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- ② 注册 GAP / GATT 基础服务 ---- */
#if CONFIG_BT_NIMBLE_GAP_SERVICE
    ble_svc_gap_init();
    rc = ble_svc_gap_device_name_set(s_dev_name);
    if (rc != 0) {
        /* 名字写不进去只影响"连接后读 Device Name 特征"，
         * 广播名走的是扫描响应，不受影响 —— 所以只警告不失败 */
        ESP_LOGW(TAG, "设置 GAP 设备名失败 rc=%d", rc);
    }
#endif
    ble_svc_gatt_init();

    /* ---- ③ 声明我们的服务和特征 ---- */
    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg 失败 rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs 失败 rc=%d", rc);
        return ESP_FAIL;
    }

    /* ---- ④ 把 BLE 注册成一条上行链路 ----
     * 必须在任何一次 app_link_broadcast() 【之前】注册好，
     * 否则开机早期的 state/sensor 就漏掉 BLE 了。
     * 注册之后：MQTT 和 BLE 各拿一份 JSON，互不影响。 */
    err = app_link_register(&s_ble_link);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_link_register(ble) 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- ⑤ 主机回调 ----
     * ⚠ 只设置 sync_cb / reset_cb / store_status_cb。
     *   千万不要碰 ble_hs_cfg.gatts_register_cb 之外的"设备状态回调"——
     *   本工程的 device_register_cb() 只保留最后一个回调，
     *   mqtt_app_bind_device_events() 已经独占它，BLE 靠 app_link 广播拿数据。 */
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb  = ble_on_sync;

    /* store 回调（配对信息持久化用）。本工程特征都不要求加密，
     * 正常不会触发配对；初始化它是为了不让空指针在"手机主动发起配对"时崩掉。
     * store_status_cb 给 ble_store_util_status_rr（IDF 官方例程的用法）：
     * 存储满了就按默认策略淘汰最旧的一条，而不是直接报错。 */
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* ---- ⑥ 声明本机期望的 ATT MTU ----
     * 放在 sync 之前调用也可以（它只改一个全局变量），
     * 效果和连接时再设一次相同，两处都写是为了双保险。 */
    rc = ble_att_set_preferred_mtu(BLE_MTU_PREFERRED);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_att_set_preferred_mtu(%d) 失败 rc=%d，将使用默认 MTU",
                 BLE_MTU_PREFERRED, rc);
    }

    /* ---- ⑦ 起主机任务（非阻塞返回），广播在 on_sync 里开始 ---- */
    nimble_port_freertos_init(ble_host_task);

    s_started = true;

    ESP_LOGI(TAG, "BLE 链路已启动：广播名 \"%s\"，等待手机连接", s_dev_name);
    return ESP_OK;
}

bool ble_app_is_connected(void)
{
    return ble_link_is_connected();
}

int ble_app_get_mtu(void)
{
    uint16_t conn = s_conn_handle;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return 0;         /* 未连接：ble_att_mtu() 这时会返回 0，但语义上直接给 0 更清楚 */
    }
    return (int)ble_att_mtu(conn);
}

#endif /* CONFIG_BT_ENABLED && CONFIG_APP_BLE_ENABLE */
