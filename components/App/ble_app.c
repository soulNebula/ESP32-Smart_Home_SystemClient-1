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
// 没开蓝牙就空着
// 保证还能编过
static const char *TAG = "ble_app";

// 报一句不可用
esp_err_t ble_app_start(void) {
    ESP_LOGW(TAG, "BLE 未编译进固件（CONFIG_BT_ENABLED=%d, CONFIG_APP_BLE_ENABLE=%d），"
                  "手机蓝牙控制不可用；MQTT 不受影响",
             (int)CONFIG_BT_ENABLED, (int)CONFIG_APP_BLE_ENABLE);
    return ESP_ERR_NOT_SUPPORTED;
}

// 真正实现开始
#else

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
// 取长度宏在这里头
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// 上行走过这里
#include "app_link.h"
// 共用的命令解析口
#include "app_cmd.h"
// 拼 JSON 和上报
#include "mqtt_app.h"
// 取板子号
#include "wifi_sta.h"
// 标命令从哪来
#include "device_model.h"

static const char *TAG = "ble_app";

// 照例程声明一下
void ble_store_config_init(void);

// 广播名前缀要一致
#define BLE_NAME_PREFIX      CONFIG_APP_BLE_DEVICE_NAME_PREFIX

// 名字最长 29 字节
// 砍短了要告警
#define BLE_NAME_MAX_LEN     29

// 三个号要小端写
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x5d, 0x4c, 0x3b, 0x2a, 0x1f, 0x0e, 0x9d, 0x8c,
    0x7b, 0x4a, 0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1);

static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x5e, 0x4c, 0x3b, 0x2a, 0x1f, 0x0e, 0x9d, 0x8c,
    0x7b, 0x4a, 0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1);

static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x5f, 0x4c, 0x3b, 0x2a, 0x1f, 0x0e, 0x9d, 0x8c,
    0x7b, 0x4a, 0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1);

// 手机发来的帧头
// 控制命令
#define BLE_DOWN_CMD         0x01
// 阈值配置
#define BLE_DOWN_CONFIG      0x02
// 要一份现状
#define BLE_DOWN_GET         0x03

// 一包要装下状态
#define BLE_MTU_PREFERRED    512

// 通知多占三字节
#define BLE_ATT_OVERHEAD     3

// 一帧最长这么多
#define BLE_RX_FRAME_MAX     (BLE_MTU_PREFERRED + 1)
// 多留一字节收尾
#define BLE_RX_BUF_LEN       (BLE_RX_FRAME_MAX + 1)

// 防重复启动
static bool     s_started = false;
static char     s_dev_name[BLE_NAME_MAX_LEN + 1] = { 0 };

// 连着就是它
// 断了要清零
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// 发通知要用的号
static uint16_t s_tx_val_handle = 0;

// 广播用哪种地址
static uint8_t  s_own_addr_type = 0;

// 收帧的地方
// 放全局省点栈
static uint8_t  s_rx_buf[BLE_RX_BUF_LEN];

static esp_err_t ble_link_send(app_msg_type_t type, const char *json, size_t len);
static bool      ble_link_is_connected(void);

static const app_link_t s_ble_link = {
    .name         = "ble",
    .send         = ble_link_send,
    .is_connected = ble_link_is_connected,
};

// 地址转成好读的字
static void ble_format_addr(const uint8_t addr[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

// 取号跟那边一样
// 抄一份省得牵扯
static esp_err_t ble_build_uid(char *buf, size_t len) {
    // 号码加结尾符
    if (buf == NULL || len < 7) {
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

// 拼广播名防超长
static esp_err_t ble_build_device_name(void) {
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
        // 已经截好直接用
    }

    return ESP_OK;
}

static int ble_gatt_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);

// 占位，平时不来
// 有人加读才走到
static int ble_gatt_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    // 被调到是改坏了
    ESP_LOGW(TAG, "TX 特征收到不支持的访问 (op=%d)，已拒绝", ctxt->op);
    return BLE_ATT_ERR_UNLIKELY;
}

// 末尾全零收尾
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                // 手机写命令进来
                .uuid      = &s_rx_uuid.u,
                .access_cb = ble_gatt_rx_access,
                // 两种写都放开
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // 板子通知手机
                .uuid       = &s_tx_uuid.u,
                .access_cb  = ble_gatt_tx_access,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                // 让它回填发的号
                .val_handle = &s_tx_val_handle,
            },
            {
                // 本服务到此
                0,
            },
        },
    },
    {
        // 表到此为止
        0,
    },
};

static int ble_gap_event(struct ble_gap_event *event, void *arg);
static void ble_start_advertising(void);

// 配好并开始广播
// 一直播到有人连
static void ble_start_advertising(void) {
    struct ble_hs_adv_fields adv = { 0 };
    struct ble_hs_adv_fields rsp = { 0 };
    struct ble_gap_adv_params params = { 0 };
    int rc;

    // 主广播放服务号
    // 能扫到就行
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    adv.uuids128 = &s_svc_uuid;
    adv.num_uuids128 = 1;
    // 标成完整表才认
    adv.uuids128_is_complete = 1;

    // 省下字节留给以后

    rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播数据失败 rc=%d（字段超 31 字节？）", rc);
        return;
    }

    // 名字放扫描响应里
    rsp.name = (uint8_t *)s_dev_name;
    rsp.name_len = (uint8_t)strlen(s_dev_name);
    rsp.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置扫描响应失败 rc=%d（名字 %u 字节太长？）",
                 rc, (unsigned)rsp.name_len);
        // 名字失败也接着走
    }

    // 广播参数
    // 能连的普通广播
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    // 谁都能扫到
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // 发快些，插电不怕
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

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            // 失败了重开广播
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

        // 声明能收多大包
        // 再设一次保险
        int rc = ble_att_set_preferred_mtu(BLE_MTU_PREFERRED);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_att_set_preferred_mtu(%d) 失败 rc=%d", BLE_MTU_PREFERRED, rc);
        }

        // 连上先推一遍数据
        // 这轮多半收不到
        // 真正靠手机来要
        // 两条链路都会收到
        // 别在这抢回调位
        (void)mqtt_publish_state();
        (void)mqtt_publish_sensor(NULL);
        (void)mqtt_publish_config();
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "手机已断开：handle=%d reason=%d",
                 event->disconnect.conn.conn_handle, event->disconnect.reason);
        // 清掉句柄别乱发
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        // 重开广播等它回来
        ble_start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        // 出错就重开广播
        ESP_LOGW(TAG, "广播结束 reason=%d，重新开始广播", event->adv_complete.reason);
        ble_start_advertising();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 已协商：handle=%d cid=%d mtu=%d（单包净载荷 %d 字节）",
                 event->mtu.conn_handle, event->mtu.channel_id, event->mtu.value,
                 event->mtu.value - BLE_ATT_OVERHEAD);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        // 这事件不止管我们
        // 只记日志不当开关
        ESP_LOGI(TAG, "CCCD 写入：handle=%d notify=%d->%d indicate=%d->%d"
                      "（handle=%d 是 TX 特征，其它 handle 是 GAP Service Changed 之类）",
                 event->subscribe.attr_handle,
                 event->subscribe.prev_notify, event->subscribe.cur_notify,
                 event->subscribe.prev_indicate, event->subscribe.cur_indicate,
                 s_tx_val_handle);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        // 参数让手机定
        ESP_LOGI(TAG, "连接参数已更新 status=%d", event->conn_update.status);
        return 0;

    default:
        // 别的事件不理
        return 0;
    }
}

// 收帧，先查再分发
static int ble_gatt_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;

    // 只收写，别的拒掉
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGW(TAG, "RX 特征收到非写操作 op=%d，已拒绝", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }

    // 整链长度都算上
    uint16_t pkt_len = OS_MBUF_PKTLEN(ctxt->om);

    if (pkt_len == 0) {
        ESP_LOGW(TAG, "收到空帧，已忽略");
        // 空帧不算错
        return 0;
    }
    if (pkt_len > BLE_RX_FRAME_MAX) {
        // 太长就拒收
        ESP_LOGE(TAG, "帧长 %u 超过上限 %u 字节，已拒绝",
                 (unsigned)pkt_len, (unsigned)BLE_RX_FRAME_MAX);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf, (uint16_t)(BLE_RX_BUF_LEN - 1), &copied);
    if (rc != 0) {
        // 拷贝失败就报错
        ESP_LOGE(TAG, "从 mbuf 取数据失败 rc=%d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }
    // 只为打日志补的
    s_rx_buf[copied] = '\0';

    uint8_t     type     = s_rx_buf[0];
    const char *json     = (const char *)(s_rx_buf + 1);
    // 可能只剩帧头
    int         json_len = (int)copied - 1;

    ESP_LOGI(TAG, "收到帧 type=0x%02X len=%d payload=%.*s",
             (unsigned)type, (int)copied, json_len > 0 ? json_len : 0, json);

    switch (type) {
    case BLE_DOWN_CMD:
        // 交给共用的解析
        // 标上是手机来的
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
        // 回一份完整现状
        // 必须连阈值一起回
        // 不订阅就收不到
        // 拼数据只有一份
        ESP_LOGI(TAG, "手机请求刷新：回 state + sensor + config");
        (void)mqtt_publish_state();
        (void)mqtt_publish_sensor(NULL);
        (void)mqtt_publish_config();
        break;

    default:
        // 不认识的号不理
        ESP_LOGW(TAG, "未知帧类型 0x%02X，已忽略（len=%d）", (unsigned)type, (int)copied);
        break;
    }

    // 看一眼还剩多少栈
    // 掉太多就告警
    UBaseType_t stack_left = uxTaskGetStackHighWaterMark(NULL);
    if (stack_left < 1024) {
        ESP_LOGW(TAG, "NimBLE 主机任务栈余量只剩 %u 字节（type=0x%02X）——"
                      "请调大 CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE",
                 (unsigned)stack_left, (unsigned)type);
    }

    return 0;
}

static bool ble_link_is_connected(void) {
    return (s_conn_handle != BLE_HS_CONN_HANDLE_NONE);
}

// 加帧头后通知手机
static esp_err_t ble_link_send(app_msg_type_t type, const char *json, size_t len) {
    if (json == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t conn = s_conn_handle;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    // 多一字节放帧头
    size_t frame_len = len + 1;

    // 太长就不发
    // 截半截更糟
    int mtu = ble_att_mtu(conn);
    if (mtu > 0 && frame_len > (size_t)(mtu - BLE_ATT_OVERHEAD)) {
        ESP_LOGW(TAG, "%s 帧 %u 字节 > 当前 MTU %d 的净载荷 %d 字节，本条不上报",
                 app_msg_type_name(type), (unsigned)frame_len, mtu,
                 mtu - BLE_ATT_OVERHEAD);
        return ESP_ERR_INVALID_STATE;
    }

    // 拼成一整块再交
    // 让底层自己拆包
    uint8_t *frame = (uint8_t *)malloc(frame_len);
    if (frame == NULL) {
        ESP_LOGE(TAG, "组帧 malloc(%u) 失败", (unsigned)frame_len);
        return ESP_ERR_NO_MEM;
    }
    // 头一字节放类型
    frame[0] = (uint8_t)type;
    memcpy(frame + 1, json, len);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)frame_len);
    // 拷完就能释放
    free(frame);
    if (om == NULL) {
        ESP_LOGE(TAG, "ble_hs_mbuf_from_flat(%u) 失败（msys 池不够？）",
                 (unsigned)frame_len);
        return ESP_ERR_NO_MEM;
    }

    // 它自己会释放
    int rc = ble_gatts_notify_custom(conn, s_tx_val_handle, om);
    if (rc != 0) {
        // 多半是刚断开，正常
        ESP_LOGW(TAG, "notify(%s) 失败 rc=%d（连接可能刚断开）",
                 app_msg_type_name(type), rc);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "已 notify %s（%u 字节，MTU %d）",
             app_msg_type_name(type), (unsigned)frame_len, mtu);
    return ESP_OK;
}

// 复位只记日志
static void ble_on_reset(int reason) {
    ESP_LOGE(TAG, "NimBLE 协议栈复位，reason=%d", reason);
}

// 同步好了开广播
static void ble_on_sync(void) {
    int rc;

    // 先保证有地址可用
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "本机没有可用的蓝牙地址 rc=%d", rc);
        return;
    }

    // 问清该用哪种地址
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

// 常驻的蓝牙任务
static void ble_host_task(void *param) {
    (void)param;
    // 进去就不出来
    nimble_port_run();
    vTaskDelete(NULL);
}

esp_err_t ble_app_start(void) {
    esp_err_t err;
    int rc;

    // 重复调就直接返回
    if (s_started) {
        ESP_LOGI(TAG, "BLE 已经启动过，跳过");
        return ESP_OK;
    }

    err = ble_build_device_name();
    if (err != ESP_OK) {
        // 名字取不到就退出
        return err;
    }

    // 起蓝牙要在存储后
    // 不用管和 WiFi 抢
    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    // 注册基础服务
#if CONFIG_BT_NIMBLE_GAP_SERVICE
    ble_svc_gap_init();
    rc = ble_svc_gap_device_name_set(s_dev_name);
    if (rc != 0) {
        // 广播名不受影响
        ESP_LOGW(TAG, "设置 GAP 设备名失败 rc=%d", rc);
    }
#endif
    ble_svc_gatt_init();

    // 声明服务和特征
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

    // 登记成上行链路
    // 要赶在首次上报前
    err = app_link_register(&s_ble_link);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_link_register(ble) 失败: %s", esp_err_to_name(err));
        return err;
    }

    // 只挂自己的回调
    // 设备回调位别碰
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb  = ble_on_sync;

    // 配对信息照例程存
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // 再声明一次包大小
    rc = ble_att_set_preferred_mtu(BLE_MTU_PREFERRED);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_att_set_preferred_mtu(%d) 失败 rc=%d，将使用默认 MTU",
                 BLE_MTU_PREFERRED, rc);
    }

    // 起任务，广播随后开
    nimble_port_freertos_init(ble_host_task);

    s_started = true;

    ESP_LOGI(TAG, "BLE 链路已启动：广播名 \"%s\"，等待手机连接", s_dev_name);
    return ESP_OK;
}

// 蓝牙总开关到此
#endif
