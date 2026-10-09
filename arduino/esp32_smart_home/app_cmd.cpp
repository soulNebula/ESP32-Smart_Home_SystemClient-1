#include "app_cmd.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "automation.h"
#include "device_model.h"

// ======================================================================
// 命令分发：网络和蓝牙收到的是同一套 JSON，所以只用一份解析
//
// 对应 ESP-IDF 工程的 components/App/mqtt_app.c 里那段 app_cmd_handle_json()，
// 动作名和字段名用 app_cmd.h 里的 CMD_*（原工程是 mqtt_protocol.h 的 MQTT_*）。
//
// 原版拿 cJSON 解报文，这版是自己用 const char * 扫的：
// 手写解析是为了不让甲方多装一个 JSON 库。报文就 app_cmd.h 顶上列的那几种，
// 只认这几种写法，不做完整 JSON 解析：嵌套对象、数组、转义键名一概不管。
// 全程不动态分配内存，缓冲都在栈上。
//
// 和原版还有一处不一样：原版这条函数顺手把 ack 和状态发出去了，
// 这版只把成没成返回给调用者，发不发、发什么由 net_mqtt / net_ble 那边定。
// ======================================================================

// 放收到的命令，跟原版 RX_BUF_LEN 一个量级
#define CMD_RX_BUF_LEN    256
// 设备名长度上限，和原版 DEV_NAME_MAX 一样
#define CMD_DEV_NAME_MAX  32
// 动作名长度上限，和原版 ACTION_MAX 一样
#define CMD_ACTION_MAX    24

// 设备名里的 all，头文件里没给这个宏，本文件自己定一个，别散着写裸字符串
// （device_from_name() 认的也是这三个字母）
#define CMD_DEV_ALL       "all"

// 外面给的字符串未必有结尾符（蓝牙那帧是按长度收的），
// 最多扫这么多字节还没扫到结尾就放弃，免得把内存读飞
#define CMD_SCAN_MAX      512

static const char *const TAG = "app_cmd";

// 文档到头的位置：有结尾符按结尾符，没有就按扫描上限
static const char *json_doc_end(const char *json) {
    size_t n = 0;

    if (json == NULL) {
        return json;
    }
    while ((n < CMD_SCAN_MAX) && (json[n] != '\0')) {
        n++;
    }
    return json + n;
}

// 跳过空白，报文里有空格换行都正常
static const char *json_skip_space(const char *p, const char *end) {
    while ((p < end) && ((*p == ' ') || (*p == '\t') || (*p == '\n') || (*p == '\r'))) {
        p++;
    }
    return p;
}

// 这一段引号里的名字是不是要找的键，长短和内容都得一样
static bool json_name_is(const char *s, const char *e, const char *key) {
    const size_t n = (size_t)(e - s);

    if (strlen(key) != n) {
        return false;
    }
    return (memcmp(s, key, n) == 0);
}

// 找到 key 后面那一格值的开头，找不到给 NULL
//
// 这里就是"先定位键再取值"：扫到一个引号串，只有它后面跟冒号才算键，
// 后面跟逗号或右括号的都是值，所以值里出现 "value" 这种字样不会被当成字段。
// 键名里的转义不认（字段名都是死的那几个），值的转义交给下面取值的人处理。
static const char *json_find_value(const char *json, const char *key) {
    const char *end;
    const char *p;
    const char *s;
    const char *e;
    const char *q;

    if ((json == NULL) || (key == NULL)) {
        return NULL;
    }

    end = json_doc_end(json);
    p   = json;

    while (p < end) {
        if (*p != '"') {
            p++;
            continue;
        }

        s = p + 1;
        e = s;
        // 到下一个引号为止，前面带反斜杠的算转义，不当结尾
        while ((e < end) && !((*e == '"') && (e[-1] != '\\'))) {
            e++;
        }
        if (e >= end) {
            // 引号没配上，这报文是坏的，后面不用看了
            break;
        }

        q = json_skip_space(e + 1, end);
        if ((q < end) && (*q == ':')) {
            if (json_name_is(s, e, key)) {
                return json_skip_space(q + 1, end);
            }
        }
        // 不是要找的键就跳过这一格，接着往后扫
        p = e + 1;
    }

    return NULL;
}

// 值那一格是不是裸的 true / false，是的话给 1 / 0，不是给 -1
// 原版这儿走的是 cJSON_IsBool，手写版就比这几个字符
static int json_bool_at(const char *p, const char *end) {
    if ((p == NULL) || (p >= end)) {
        return -1;
    }
    if (((size_t)(end - p) >= 4) && (memcmp(p, "true", 4) == 0)) {
        return 1;
    }
    if (((size_t)(end - p) >= 5) && (memcmp(p, "false", 5) == 0)) {
        return 0;
    }
    return -1;
}

// 某个键的值是不是布尔，找不到键或者不是布尔都给 -1
static int json_get_bool(const char *json, const char *key) {
    return json_bool_at(json_find_value(json, key), json_doc_end(json));
}

// 抠一个整数，布尔不算数
// 原版取 r / g / b 走的是 cJSON_IsNumber，布尔会被忽略，这里跟着一样
static bool json_get_int(const char *json, const char *key, int *out) {
    double num = 0.0;

    if (out == NULL) {
        return false;
    }
    if (json_get_bool(json, key) >= 0) {
        return false;
    }
    if (!json_get_num(json, key, &num)) {
        return false;
    }

    // 先粗夹一下再取整，免得天外飞来的大数把 int 撑爆
    // 真正该在哪个范围是调用者的事，这儿只管别出事
    if (num > 100000.0) {
        num = 100000.0;
    } else if (num < -100000.0) {
        num = -100000.0;
    }
    *out = (int)num;
    return true;
}

// 抠一个字符串字段，抠到给 true
// 只认 "key":"value" 这一种，值里的 \" 转义简单处理成引号
bool json_get_str(const char *json, const char *key, char *out, size_t len) {
    const char *end;
    const char *v;
    size_t      used = 0;

    if ((out == NULL) || (len == 0)) {
        return false;
    }
    // 先说好是空串，失败时调用者看到的也是干净的
    out[0] = '\0';

    v = json_find_value(json, key);
    if (v == NULL) {
        return false;
    }
    end = json_doc_end(json);
    if ((v >= end) || (*v != '"')) {
        // 值不是引号串，这个键就当没有
        return false;
    }

    v++;
    while ((v < end) && (*v != '"')) {
        // 转义的写法只把反斜杠丢掉，转出来是啥就是啥，够这种报文用
        if ((*v == '\\') && ((v + 1) < end)) {
            v++;
        }
        if ((used + 1) >= len) {
            // 装不下就当没抠到：宁可整条不认，也别拿半截名字去查设备
            out[0] = '\0';
            return false;
        }
        out[used++] = *v;
        v++;
    }
    if (v >= end) {
        // 结尾的引号都没有，这报文断了
        out[0] = '\0';
        return false;
    }

    out[used] = '\0';
    return true;
}

// 抠一个数字字段，抠到给 true
// 认负号和小数点，布尔也认（true=1 false=0，原版配置那条路就这么收的）
bool json_get_num(const char *json, const char *key, double *out) {
    const char *end;
    const char *v;
    double      val = 0.0;
    double      div = 10.0;
    bool        neg = false;
    bool        any = false;
    int         b;

    if (out == NULL) {
        return false;
    }

    end = json_doc_end(json);
    v   = json_find_value(json, key);
    if (v == NULL) {
        return false;
    }

    b = json_bool_at(v, end);
    if (b >= 0) {
        *out = (b != 0) ? 1.0 : 0.0;
        return true;
    }

    // 负号小数都从这儿开始认
    if ((v < end) && ((*v == '-') || (*v == '+'))) {
        neg = (*v == '-');
        v++;
    }

    while ((v < end) && (*v >= '0') && (*v <= '9')) {
        val = (val * 10.0) + (double)(*v - '0');
        any = true;
        v++;
    }

    if ((v < end) && (*v == '.')) {
        v++;
        // 小数点后面一位位加上去
        while ((v < end) && (*v >= '0') && (*v <= '9')) {
            val += (double)(*v - '0') / div;
            div *= 10.0;
            any = true;
            v++;
        }
    }

    if (!any) {
        // 连一位数字都没有，那就不是数字
        return false;
    }

    *out = neg ? -val : val;
    return true;
}

bool app_cmd_handle_json(const char *json, int len, ctrl_source_t src) {
    char        buf[CMD_RX_BUF_LEN];
    char        dev[CMD_DEV_NAME_MAX] = { 0 };
    char        action[CMD_ACTION_MAX] = { 0 };
    const char *head;
    device_id_t id;
    double      num = 0.0;
    bool        ok = true;
    // 存布尔值那一格，和数字分开认
    int         flag;
    // 默认亮度，和原版一样
    int value = 100;
    // 默认白色，和原版一样
    int r = 255, g = 255, b = 255;

    if ((json == NULL) || (len <= 0)) {
        Serial.printf("%s: cmd 空报文，丢掉\n", TAG);
        return false;
    }
    if (len >= (int)sizeof(buf)) {
        // 原版这儿是 copy_bounded 没成，一样是整条拒掉
        Serial.printf("%s: cmd 报文 %d 字节太长（最多 %u），丢掉\n",
                      TAG, len, (unsigned)sizeof(buf) - 1);
        return false;
    }

    // 先抄一份带结尾符的：蓝牙那帧是按长度给的，未必有结尾符
    memcpy(buf, json, (size_t)len);
    buf[len] = '\0';

    // 只看开头像不像对象，不做完整 JSON 校验，坏报文后面自然抠不到字段
    head = json_skip_space(buf, buf + len);
    if ((head >= (buf + len)) || (*head != '{')) {
        Serial.printf("%s: cmd 不是 JSON 对象 \"%s\"\n", TAG, buf);
        return false;
    }

    // 缺动作就整条不认，动作名太长也算没有
    if (!json_get_str(buf, CMD_KEY_ACTION, action, sizeof(action))) {
        Serial.printf("%s: cmd 缺 \"%s\" 或者动作名太长，不认\n", TAG, CMD_KEY_ACTION);
        return false;
    }

    // 切自动总开关，这条不用 dev
    if (strcmp(action, CMD_ACTION_AUTO) == 0) {
        // 不给 value 就是打开，和原版一样
        bool on = true;

        flag = json_get_bool(buf, CMD_KEY_VALUE);
        if (flag >= 0) {
            on = (flag != 0);
        } else if (json_get_num(buf, CMD_KEY_VALUE, &num)) {
            // 数字就是零关非零开
            on = (num != 0.0);
        } else if (json_find_value(buf, CMD_KEY_VALUE) != NULL) {
            // 值和数都不沾边，原版当没给，这里也一样，只是说一声
            Serial.printf("%s: cmd \"%s\" 不是布尔也不是数字，auto 按默认走\n",
                          TAG, CMD_KEY_VALUE);
        }

        if (!automation_set_enabled(on)) {
            Serial.printf("%s: cmd failed: action=%s (automation_set_enabled 没成)\n",
                          TAG, action);
            return false;
        }
        if (!automation_save()) {
            Serial.printf("%s: cmd failed: action=%s (automation_save 没成)\n", TAG, action);
            return false;
        }

        // 原版这行日志照留，日志里好找
        Serial.printf("%s: auto mode -> %s\n", TAG, on ? "ON" : "OFF");
        Serial.printf("%s: cmd ok: action=%s value=%d auto=%s by %s\n",
                      TAG, action, on ? 1 : 0, on ? "ON" : "OFF", ctrl_source_name(src));
        return true;
    }

    // auto 之外都得说清是哪台
    if (!json_get_str(buf, CMD_KEY_DEV, dev, sizeof(dev))) {
        Serial.printf("%s: cmd failed: action=%s (缺 \"%s\" 或者设备名太长)\n",
                      TAG, action, CMD_KEY_DEV);
        return false;
    }

    // 先单独认 all
    if (strcmp(dev, CMD_DEV_ALL) == 0) {
        if (strcmp(action, CMD_ACTION_OFF) == 0) {
            ok = device_all_off(src);
        } else if (strcmp(action, CMD_ACTION_ON) == 0) {
            // 全开只开灯和风扇
            // 门窗不动，怕出事
            for (int i = (int)DEV_LED_LIVING; i <= (int)DEV_FAN; i++) {
                if (!device_set_power((device_id_t)i, true, src)) {
                    ok = false;
                }
            }
        } else {
            // all 只支持开关，调档没意义
            Serial.printf("%s: cmd failed: dev=%s action=%s (all 只认 on/off)\n",
                          TAG, dev, action);
            return false;
        }

        if (ok) {
            Serial.printf("%s: cmd ok: dev=%s action=%s value=%d rgb=%d,%d,%d by %s\n",
                          TAG, dev, action, value, r, g, b, ctrl_source_name(src));
        } else {
            Serial.printf("%s: cmd failed: dev=%s action=%s (有设备没设成)\n", TAG, dev, action);
        }
        return ok;
    }

    id = device_from_name(dev);
    if (id >= DEV_COUNT) {
        Serial.printf("%s: cmd failed: dev=%s action=%s (设备名不认)\n", TAG, dev, action);
        return false;
    }

    // 取数值参数：给了布尔就当满档或零，给了别的数就取整
    if (json_find_value(buf, CMD_KEY_VALUE) != NULL) {
        flag = json_get_bool(buf, CMD_KEY_VALUE);
        if (flag >= 0) {
            value = (flag != 0) ? 100 : 0;
        } else if (json_get_num(buf, CMD_KEY_VALUE, &num)) {
            // 和原版一样先夹再取整，别让大数把 int 撑爆
            if (num > 100.0) {
                num = 100.0;
            } else if (num < 0.0) {
                num = 0.0;
            }
            value = (int)num;
        } else {
            // 不是数字就当没给，原版也这么放过
            Serial.printf("%s: cmd \"%s\" 不是数字，按默认 %d 走\n",
                          TAG, CMD_KEY_VALUE, value);
        }
    }

    // 颜色没给就保持白色，和原版一样
    // json_get_int 抠不到时不碰 out，所以能直接写进 r / g / b
    (void)json_get_int(buf, CMD_KEY_R, &r);
    (void)json_get_int(buf, CMD_KEY_G, &g);
    (void)json_get_int(buf, CMD_KEY_B, &b);

    // 值超范围就卡住，别让设备收奇怪的值
    if (value < 0) {
        value = 0;
    } else if (value > 100) {
        value = 100;
    }
    // 颜色最多 255，负的当零
    if (r < 0) { r = 0; }
    if (r > 255) { r = 255; }
    if (g < 0) { g = 0; }
    if (g > 255) { g = 255; }
    if (b < 0) { b = 0; }
    if (b > 255) { b = 255; }

    // 按动作去执行
    if (strcmp(action, CMD_ACTION_ON) == 0) {
        ok = device_set_power(id, true, src);
    } else if (strcmp(action, CMD_ACTION_OFF) == 0) {
        ok = device_set_power(id, false, src);
    } else if (strcmp(action, CMD_ACTION_TOGGLE) == 0) {
        ok = device_toggle(id, src);
    } else if (strcmp(action, CMD_ACTION_SET) == 0) {
        ok = device_set_level(id, (uint8_t)value, src);
    } else if (strcmp(action, CMD_ACTION_OPEN) == 0) {
        // 开就是给满值
        ok = device_set_level(id, 100, src);
    } else if (strcmp(action, CMD_ACTION_CLOSE) == 0) {
        // 关就是给零
        ok = device_set_level(id, 0, src);
    } else if (strcmp(action, CMD_ACTION_COLOR) == 0) {
        ok = device_set_color(id, (uint8_t)r, (uint8_t)g, (uint8_t)b, src);
    } else {
        Serial.printf("%s: cmd failed: dev=%s action=%s (不支持的动作)\n", TAG, dev, action);
        return false;
    }

    if (ok) {
        Serial.printf("%s: cmd ok: dev=%s action=%s value=%d rgb=%d,%d,%d by %s\n",
                      TAG, dev, action, value, r, g, b, ctrl_source_name(src));
    } else {
        // 走到这儿失败就是设备那步没成，比如窗帘舵机停用了
        Serial.printf("%s: cmd failed: dev=%s action=%s value=%d (设备那步没成)\n",
                      TAG, dev, action, value);
    }
    return ok;
}
