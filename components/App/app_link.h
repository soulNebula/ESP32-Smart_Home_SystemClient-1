/**
 * @file  app_link.h
 * @brief 传输链路注册表 —— 让 MQTT / BLE 共用同一套上行 JSON
 *
 * ===========================================================================
 *  为什么需要这一层
 * ===========================================================================
 *  改造前，上行 JSON 是在 mqtt_app.c 里"拼 JSON + 直接 esp_mqtt_client_publish"
 *  焊死在一起的。现在手机 App 除了 MQTT 还要走 BLE，如果照抄一份 JSON 拼装
 *  逻辑，两边迟早会漂移（改了一个字段忘了另一个）。
 *
 *  所以把这两件事拆开：
 *    · 【拼 JSON】由各 mqtt_publish_xxx() 负责，拼完调用 app_link_broadcast()
 *    · 【发出去】由各链路自己的 send() 回调负责（MQTT 发 topic，BLE 发 notify）
 *
 *  加一条新链路（比如以后加 WebSocket）只需要：
 *      1) 实现一个 app_link_t（send + is_connected）
 *      2) 开机时 app_link_register() 一下
 *  业务代码一行都不用改。
 *
 * ===========================================================================
 *  消息类型
 * ===========================================================================
 *  类型码同时用作【BLE 帧的第一个字节】，所以数值一旦定下就不能改，
 *  否则手机端会解析错位。MQTT 侧用不到类型码（它靠 topic 区分），
 *  所以 BLE 的引入【完全没有改动 MQTT 的 JSON 内容】。
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 上行消息类型 —— 数值即 BLE 帧头字节，⚠ 不可随意改动 */
typedef enum {
    APP_MSG_STATE  = 1,  /**< 全设备状态（对应 MQTT <base>/state） */
    APP_MSG_SENSOR = 2,  /**< 传感器数据（对应 <base>/sensor） */
    APP_MSG_ACK    = 3,  /**< 命令执行结果（对应 <base>/ack） */
    APP_MSG_EVENT  = 4,  /**< 按键/语音事件（对应 <base>/event） */
    APP_MSG_CONFIG = 5,  /**< 联动阈值配置（对应 <base>/config） */
} app_msg_type_t;

/** 一条传输链路 */
typedef struct app_link {
    const char *name;   /**< 日志用，如 "mqtt" / "ble" */

    /**
     * @brief 把一条 JSON 发出去
     * @param type 消息类型（BLE 用它填帧头；MQTT 用它选 topic）
     * @param json UTF-8 JSON 文本，以 '\0' 结尾
     * @param len  json 的字节长度（不含结尾 '\0'）
     * @return ESP_OK 表示已交给链路；链路自己未连接时请返回 ESP_ERR_INVALID_STATE
     * @note 运行在调用者任务上下文（可能是 BLE 主机任务 / MQTT 任务 / app_loop）
     */
    esp_err_t (*send)(app_msg_type_t type, const char *json, size_t len);

    /** @brief 该链路当前是否可用（未连接返回 false，broadcast 会跳过它） */
    bool (*is_connected)(void);
} app_link_t;

/**
 * @brief 注册一条链路（表满返回 ESP_ERR_NO_MEM）
 * @note 请在任何广播发生【之前】注册完（通常在各模块 init 里）
 */
esp_err_t app_link_register(const app_link_t *link);

/** @brief 注销一条链路（断开/反初始化时用） */
esp_err_t app_link_unregister(const app_link_t *link);

/**
 * @brief 把一条 JSON 广播给所有已连接链路
 * @return 成功投递的链路条数（0 = 一个都没连上，属正常情况）
 * @note 某条链路 send() 失败只记日志，不影响其它链路
 */
int app_link_broadcast(app_msg_type_t type, const char *json, size_t len);

/** @brief 是否至少有一条链路在线（用来判断"有没有必要拼 JSON"） */
bool app_link_any_connected(void);

/** @brief 类型名，日志用 */
const char *app_msg_type_name(app_msg_type_t type);

#ifdef __cplusplus
}
#endif
