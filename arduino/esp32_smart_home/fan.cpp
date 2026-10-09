#include "fan.h"

#include "pwm_compat.h"

// ======================================================================
// 风扇：低边 MOS 管 + PWM 调速
//
// 对应 ESP-IDF 工程的 components/BSP/FAN/fan.c。
// 25kHz 超出人耳听觉，调速时不会有啸叫。
// 没买 MOS 管的话，一颗 NPN 三极管（S8050/SS8050/2S2222/S9013）当低边
// 开关一样能跑，风扇会有 0.2~0.3V 的压降，转速略降一点点，看不出来。
// ======================================================================

// 有位宽算满值，十位就是 1023
#define FAN_FULL_SCALE  ((1u << BSP_FAN_RES_BITS) - 1u)

static bool    s_inited = false;
// 记住当前风速
static uint8_t s_speed  = 0;

// 把硬件准备好
bool fan_init(void) {
    if (s_inited) {
        Serial.println("FAN: already initialized");
        return true;
    }

    if (!pwm_attach(BSP_FAN_CHANNEL, BSP_FAN_GPIO_PWM, BSP_FAN_FREQ_HZ, BSP_FAN_RES_BITS)) {
        Serial.printf("FAN: gpio%d PWM 挂不上\n", BSP_FAN_GPIO_PWM);
        return false;
    }

    // 开机先停转
    pwm_write(BSP_FAN_CHANNEL, BSP_FAN_GPIO_PWM, 0);
    s_speed  = 0;
    s_inited = true;

    Serial.printf("FAN: init ok: %dHz / %d bit, pwm=gpio%d, stopped\n",
                  BSP_FAN_FREQ_HZ, BSP_FAN_RES_BITS, BSP_FAN_GPIO_PWM);
    return true;
}

// 按百分比调风速
bool fan_set_speed(uint8_t percent) {
    if (!s_inited) {
        return false;
    }

    if (percent > 100) {
        percent = 100;
    }

    const uint32_t duty = ((uint32_t)percent * FAN_FULL_SCALE) / 100u;

    pwm_write(BSP_FAN_CHANNEL, BSP_FAN_GPIO_PWM, duty);

    s_speed = percent;
    Serial.printf("FAN: speed %u%% (duty %u/%u)\n",
                  (unsigned)percent, (unsigned)duty, (unsigned)FAN_FULL_SCALE);
    return true;
}

// 查现在多大风
uint8_t fan_get_speed(void) {
    return s_speed;
}

// 直接把风扇关掉
bool fan_off(void) {
    return fan_set_speed(0);
}

// 只认开和关，开就满速
bool fan_set_power(bool on) {
    return fan_set_speed(on ? 100 : 0);
}

// 查风扇转没转
bool fan_get_power(void) {
    return s_speed > 0;
}
