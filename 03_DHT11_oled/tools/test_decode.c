/* ===========================================================================
 * test_decode.c —— DHT11 位解码器的主机端单元测试
 *
 * 目的: 不接硬件就能验证 components/DHT11/dht11.c 里 dht11_decode() 的逻辑,
 *       包括 0/1 脉宽判定、应答段定位、校验和验证、末位缺下降沿、时序抖动等。
 *
 * 做法: 合成一段符合 DHT11 时序的边沿时间戳序列, 直接喂给真正的解码函数。
 *       (硬件相关调用用宏屏蔽, 见下面)
 *
 * 编译运行(在 PC 的 Linux/macOS/Git-Bash 下, 需要 gcc):
 *     make            或者
 *     gcc -std=gnu11 -Wall -I stubs -I ../components/DHT11/include \
 *         -o test_decode test_decode.c && ./test_decode
 *
 * 注意: 这里 include 的是 dht11.c 本体, 所以编译期能看到它的 static 函数。
 * ===========================================================================*/
#include <stdio.h>
#include <string.h>

/* 先包含真实头文件(拿到类型定义), 再用宏屏蔽硬件相关调用 */
#include "dht11.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define gpio_get_level(p)        0
#define gpio_set_level(p, l)     ESP_OK
#define gpio_set_direction(p, m) ESP_OK
#define gpio_config(c)           ESP_OK
#define gpio_install_isr_service(f) ESP_OK
#define gpio_isr_handler_add(p, h, a) ESP_OK
#define gpio_set_pull_mode(p, m)  ESP_OK
#define esp_timer_get_time()     0

/* 让 dht11.c 里的 static 函数可见 */
#include "../components/DHT11/dht11.c"

/* ---- 链接用的桩函数 ---- */
const char *esp_err_to_name(esp_err_t code) { (void)code; return "STUB"; }
void       vTaskDelay(TickType_t t)        { (void)t; }
void       taskYIELD(void)                 { }

/* 直接复用驱动里的边沿结构体, 避免类型不一致 */
typedef dht11_edge_t edge_t;

static int failed = 0;

static void check(const char *name, int cond)
{
    printf("%-46s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) {
        failed = 1;
    }
}

/**
 * @brief 按 DHT11 时序合成一段边沿序列
 *
 *   主机释放总线(上升沿) -> 从机应答(低80us + 高80us) -> 40 个数据位
 *   每个数据位: 低 50us, 然后高 26us('0') 或 70us('1')
 *
 * @param drop_last_fall  置 1 时, 最后一位之后不产生下降沿(传感器直接释放总线)
 */
static int build_frame(edge_t *e, const uint8_t data[5], int drop_last_fall)
{
    int     n = 0;
    int64_t t = 0;

    e[n].ts = t; e[n].level = 1; n++;      /* 主机释放总线 */

    t += 200; e[n].ts = t; e[n].level = 0; n++;   /* 从机开始应答 */
    t += 80;  e[n].ts = t; e[n].level = 1; n++;   /* 应答低 80us 结束 */
    t += 80;  e[n].ts = t; e[n].level = 0; n++;   /* 应答高 80us 结束 */

    for (int bit = 0; bit < 40; bit++) {
        int b = (data[bit / 8] >> (7 - (bit % 8))) & 1;

        t += 50;                             /* 低电平 50us */
        e[n].ts = t; e[n].level = 1; n++;    /* 上升沿 = 高电平起点 */
        t += (b ? 70 : 26);                  /* '1' 高 70us, '0' 高 26us */

        if (bit == 39 && drop_last_fall) {
            break;
        }
        e[n].ts = t; e[n].level = 0; n++;
    }

    return n;
}

int main(void)
{
    uint8_t out[5];
    edge_t  e[200];

    /* 典型值: 湿度 60% 温度 25℃ */
    uint8_t good[5] = {60, 0, 25, 0, (uint8_t)(60 + 0 + 25 + 0)};
    int n = build_frame(e, good, 0);
    check("40 位帧, 校验通过", dht11_decode(e, n, out) && memcmp(out, good, 5) == 0);

    n = build_frame(e, good, 1);
    check("最后一位无下降沿(39 位)也能解出", dht11_decode(e, n, out) && memcmp(out, good, 5) == 0);

    uint8_t zero[5] = {0, 0, 0, 0, 0};
    n = build_frame(e, zero, 0);
    check("全 0 数据", dht11_decode(e, n, out) && memcmp(out, zero, 5) == 0);

    uint8_t ones[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFC};
    n = build_frame(e, ones, 0);
    check("全 1 数据(校验和 0xFC)", dht11_decode(e, n, out) && memcmp(out, ones, 5) == 0);

    uint8_t bad[5] = {60, 0, 25, 0, 99};
    n = build_frame(e, bad, 0);
    check("校验和错误时拒绝解码", !dht11_decode(e, n, out));

    n = build_frame(e, good, 0);
    check("截断的帧解码失败", !dht11_decode(e, n / 2, out));

    /* 帧前有多余边沿(起点偏移), 应仍能定位到应答段 */
    {
        edge_t e2[200];
        int m = build_frame(e2, good, 0);
        for (int i = m; i > 0; i--) {
            e2[i] = e2[i - 1];
        }
        e2[0].ts = 5; e2[0].level = 1;
        check("帧前有多余边沿仍能定位应答段", dht11_decode(e2, m + 1, out) && memcmp(out, good, 5) == 0);
    }

    /* 时序抖动: 高电平在 20us / 90us 之间抖动, 仍应正确区分 0 和 1 */
    {
        edge_t e3[200];
        int     m = 0;
        int64_t t = 0;
        uint8_t d[5] = {60, 0, 25, 0, 85};

        e3[m].ts = t; e3[m].level = 1; m++;
        t += 200; e3[m].ts = t; e3[m].level = 0; m++;
        t += 80;  e3[m].ts = t; e3[m].level = 1; m++;
        t += 80;  e3[m].ts = t; e3[m].level = 0; m++;

        for (int bit = 0; bit < 40; bit++) {
            int b = (d[bit / 8] >> (7 - (bit % 8))) & 1;
            t += 50; e3[m].ts = t; e3[m].level = 1; m++;
            t += (b ? 90 : 20);
            e3[m].ts = t; e3[m].level = 0; m++;
        }
        check("±10us 抖动下仍正确解码", dht11_decode(e3, m, out) && memcmp(out, d, 5) == 0);
    }

    printf("\n%s\n", failed ? "==== 有测试失败 ====" : "==== 全部通过 ====");
    return failed;
}
