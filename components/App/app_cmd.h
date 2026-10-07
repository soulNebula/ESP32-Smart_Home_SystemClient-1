#pragma once

#include "device_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// 处理一条控制命令
void app_cmd_handle_json(const char *json, int len, ctrl_source_t src);

// 处理一条改配置的命令
void app_cmd_handle_config_json(const char *json, int len, ctrl_source_t src);

#ifdef __cplusplus
}
#endif
