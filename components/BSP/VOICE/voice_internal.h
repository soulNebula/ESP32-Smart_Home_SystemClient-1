#pragma once

#include "voice.h"

#ifdef __cplusplus
extern "C" {
#endif

// 把命令交出去
void voice_dispatch(voice_cmd_t cmd);

#ifdef __cplusplus
}
#endif
