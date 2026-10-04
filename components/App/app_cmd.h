/**
 * @file  app_cmd.h
 * @brief 共用的下行命令解析接口 —— MQTT 与 BLE 走同一套
 *
 * ===========================================================================
 *  设计意图
 * ===========================================================================
 *  "解析命令 JSON → 调 device_model" 这段逻辑【必须只有一份】。手机 App
 *  无论走 MQTT 还是 BLE，下发的都是同一套 JSON（见 mqtt_protocol.h），
 *  如果两条链路各解析一遍，早晚会漂移（改了一边忘了另一边）。
 *
 *  src 参数用于标记命令来源，会一路传进 device_model：
 *    · 影响日志显示（ctrl_source_name）
 *    · 影响"自动模式下手动操作优先"的判定（device_model.c 里判 src 是否为人工来源）
 *  所以 MQTT 传 SRC_MQTT、BLE 传 SRC_BLE，不要图省事混用。
 *
 * ===========================================================================
 *  ⚠ 实现位置说明
 * ===========================================================================
 *  这两个函数目前【实现在 mqtt_app.c 里】，而不是本文件对应的 app_cmd.c。
 *  原因是它们与 MQTT 侧的以下状态耦合：
 *    · s_last_pub_config —— 用来识别 broker 回显（仅 MQTT 需要，见函数内注释）
 *    · copy_bounded() / RX_BUF_LEN —— 原有的定长拷贝工具
 *  后续若要把它们原样迁到独立的 app_cmd.c，这里的接口不需要变。
 */
#pragma once

#include "device_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 处理一条【控制命令】JSON
 * @param json  如 {"dev":"led_living","action":"on"}
 * @param len   json 字节数（不含结尾 '\0'）
 * @param src   命令来源：MQTT 传 SRC_MQTT，BLE 传 SRC_BLE
 * @note 会通过 app_link_broadcast() 把 ack / state / config 回给所有已连接链路
 */
void app_cmd_handle_json(const char *json, int len, ctrl_source_t src);

/**
 * @brief 处理一条【阈值配置】JSON
 * @param json  如 {"temp_fan_on_c":28,"enabled":true}
 * @param len   json 字节数（不含结尾 '\0'）
 * @param src   命令来源（回显识别只对 SRC_MQTT 生效）
 */
void app_cmd_handle_config_json(const char *json, int len, ctrl_source_t src);

#ifdef __cplusplus
}
#endif
