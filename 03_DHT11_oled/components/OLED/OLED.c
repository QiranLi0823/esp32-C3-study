/**
 * @file    OLED.c
 * @brief   SSD1306 0.96" OLED (SPI) 驱动实现 —— ESP32-C3
 *
 * 与中景园 C8051F020 例程的差异:
 *   1. 原厂用 GPIO 逐位模拟 SPI，这里改用 ESP-IDF 的 spi_master 硬件 SPI，速度快一个数量级;
 *   2. 原厂的 delay_ms() 忙等，换成 FreeRTOS 的 vTaskDelay();
 *   3. y 坐标由「页号」改为「像素行」，调用更直观(见 OLED.h 说明);
 *   4. 初始化命令序列、点阵数据与原厂完全一致，屏幕效果一致。
 */

#include <string.h>
#include <stdbool.h>

#include "OLED.h"
#include "oledfont.h"
#include "bmp.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OLED_SPI_HOST   SPI2_HOST
#define OLED_SPI_HZ     (8 * 1000 * 1000)   /* SSD1306 最高可到 10MHz，杜邦线建议 8MHz 以内 */
#define OLED_SPI_MAX_LEN 128

static const char *TAG = "OLED";

static spi_device_handle_t s_spi_dev;
static bool s_spi_ready;

/* ------------------------------------------------------------------ */
/* 底层: 硬件 SPI 收发                                                */
/* ------------------------------------------------------------------ */

static void oled_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

/**
 * @brief 发送 len 个字节
 * @param buf     数据缓冲区
 * @param len     字节数
 * @param is_data true: 写显存(DC=1), false: 写命令(DC=0)
 *
 * CS 由我们自己控制，这样一整段连续数据(例如清屏的 128 字节)可以只拉一次 CS，
 * 省掉大量事务开销。DC 在 CS 拉低之前设置好，满足 SSD1306 的时序要求。
 */
static void oled_write(const uint8_t *buf, size_t len, bool is_data)
{
    if (!s_spi_ready || len == 0) {
        return;
    }

    gpio_set_level(OLED_PIN_DC, is_data ? 1 : 0);
    gpio_set_level(OLED_PIN_CS, 0);

    spi_transaction_t trans = {
        .length = len * 8,
        .tx_buffer = (void *)buf,
    };
    esp_err_t err = spi_device_polling_transmit(s_spi_dev, &trans);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi transmit failed: %s", esp_err_to_name(err));
    }

    gpio_set_level(OLED_PIN_CS, 1);
}

/**
 * @brief 发送 1 个字节
 */
static void oled_write_byte(uint8_t dat, uint8_t is_data)
{
    oled_write(&dat, 1, is_data);
}

/**
 * @brief 设置显示位置
 * @param x 列 0~127
 * @param y 页号 0~7 (每页 8 个像素行)
 */
static void oled_set_pos(uint8_t x, uint8_t y)
{
    oled_write_byte(0xB0 + y, OLED_CMD);                    /* 设置页地址 */
    oled_write_byte((x & 0xF0) >> 4 | 0x10, OLED_CMD);      /* 列地址高 4 位 */
    oled_write_byte(x & 0x0F, OLED_CMD);                    /* 列地址低 4 位 */
}

/* ------------------------------------------------------------------ */
/* 硬件初始化                                                         */
/* ------------------------------------------------------------------ */

static void oled_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << OLED_PIN_RES) | (1ULL << OLED_PIN_DC) | (1ULL << OLED_PIN_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* 空闲状态: CS / DC 拉高, RES 拉高 */
    gpio_set_level(OLED_PIN_CS, 1);
    gpio_set_level(OLED_PIN_DC, 1);
    gpio_set_level(OLED_PIN_RES, 1);
}

static void oled_spi_init(void)
{
    spi_bus_config_t bus_conf = {
        .mosi_io_num = OLED_PIN_MOSI,
        .miso_io_num = -1,              /* 只写不读 */
        .sclk_io_num = OLED_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = OLED_SPI_MAX_LEN,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(OLED_SPI_HOST, &bus_conf, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_conf = {
        .clock_speed_hz = OLED_SPI_HZ,
        .mode = 0,                      /* SSD1306 要求 CPOL=0, CPHA=0 */
        .spics_io_num = -1,             /* CS 由我们自己用 GPIO 控制 */
        .queue_size = 1,
        .flags = 0,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(OLED_SPI_HOST, &dev_conf, &s_spi_dev));

    s_spi_ready = true;
}

/**
 * @brief  在指定页上把 [x0, x1] 这段列清零(含首尾)
 */
static void oled_fill_row(uint8_t page, uint8_t x0, uint8_t x1)
{
    static const uint8_t blank[OLED_SPI_MAX_LEN] = {0};

    if (page >= 8 || x0 >= OLED_WIDTH) {
        return;
    }
    if (x1 >= OLED_WIDTH) {
        x1 = OLED_WIDTH - 1;
    }
    if (x1 < x0) {
        return;
    }

    oled_set_pos(x0, page);
    oled_write(blank, (size_t)(x1 - x0 + 1), true);
}

/**
 * @brief  已绘制文字的锚点记录
 *
 * 记录每个「起点(x, y)」上次画到了哪一列、哪一页。
 * 再次在同一位置写文字时, 先把上次的范围一起清掉,
 * 这样 "led is on..." 和 "led is off..." 这样长短不一的文字交替显示也不会留残影。
 *
 * 注意: 16 号字高 16 像素横跨两页, 所以记录里要连页号一起存,
 * 否则换字号时清不干净。
 */
typedef struct {
    uint8_t used;
    uint8_t x;        /* 起点列 */
    uint8_t y;        /* 起点行(像素) */
    uint8_t x1;       /* 上次画到的最后一列 */
    uint8_t page1;    /* 上次画到的最后一页 */
} oled_anchor_t;

#define OLED_MAX_ANCHORS 8

static oled_anchor_t s_anchors[OLED_MAX_ANCHORS];

static oled_anchor_t *oled_anchor_find(uint8_t x, uint8_t y)
{
    for (uint8_t i = 0; i < OLED_MAX_ANCHORS; i++) {
        if (s_anchors[i].used && s_anchors[i].x == x && s_anchors[i].y == y) {
            return &s_anchors[i];
        }
    }
    return NULL;
}

static void oled_anchor_store(uint8_t x, uint8_t y, uint8_t x1, uint8_t page1)
{
    oled_anchor_t *a = oled_anchor_find(x, y);

    if (a == NULL) {
        for (uint8_t i = 0; i < OLED_MAX_ANCHORS; i++) {
            if (!s_anchors[i].used) {
                a = &s_anchors[i];
                break;
            }
        }
        if (a == NULL) {
            a = &s_anchors[0];     /* 记录满了就复用第一条 */
        }
        a->used = 1;
        a->x = x;
        a->y = y;
    }

    a->x1 = x1;
    a->page1 = page1;
}

/**
 * @brief  在 (x, y) 处画东西之前的准备
 *
 * 如果这个位置之前画过内容, 先把旧的范围清掉, 避免长短不一的文字交替显示时留残影。
 * 第一次在这个位置画则不做清除, 以免误擦同一页上别的内容。
 *
 * @param  x, y   起点(像素)
 * @param  sizey  字号, 8 或 16
 * @param  w      本次内容占用的宽度(像素)
 */
static void oled_prepare(uint8_t x, uint8_t y, uint8_t sizey, uint8_t w)
{
    uint8_t page0 = y / 8;
    uint8_t page1 = (sizey == 8) ? page0 : page0 + 1;
    if (page1 > 7) {
        page1 = 7;
    }

    oled_anchor_t *anchor = oled_anchor_find(x, y);

    if (anchor != NULL) {
        uint8_t clear_x1 = (uint8_t)(x + w - 1);
        uint8_t clear_p1 = page1;

        if (anchor->x1 > clear_x1) {
            clear_x1 = anchor->x1;
        }
        if (anchor->page1 > clear_p1) {
            clear_p1 = anchor->page1;
        }

        for (uint8_t page = page0; page <= clear_p1; page++) {
            oled_fill_row(page, x, clear_x1);
        }
    }

    oled_anchor_store(x, y, (uint8_t)(x + w - 1), page1);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */

void OLED_Init(void)
{
    oled_gpio_init();
    oled_spi_init();

    /* 屏幕刚被清空, 之前的占用记录作废 */
    for (uint8_t i = 0; i < OLED_MAX_ANCHORS; i++) {
        s_anchors[i].used = 0;
    }

    /* 硬件复位: RES 拉低再拉高 */
    gpio_set_level(OLED_PIN_RES, 1);
    oled_delay_ms(10);
    gpio_set_level(OLED_PIN_RES, 0);
    oled_delay_ms(20);
    gpio_set_level(OLED_PIN_RES, 1);
    oled_delay_ms(100);

    /* 初始化命令序列(与原厂例程一致) */
    oled_write_byte(0xAE, OLED_CMD);     /* 关闭显示 */
    oled_write_byte(0x00, OLED_CMD);     /* 列地址低 4 位 = 0 */
    oled_write_byte(0x10, OLED_CMD);     /* 列地址高 4 位 = 0 */
    oled_write_byte(0x40, OLED_CMD);     /* 起始行 = 0 */
    oled_write_byte(0x81, OLED_CMD);     /* 对比度控制 */
    oled_write_byte(0xCF, OLED_CMD);     /* SEG 输出电流(亮度) */
    oled_write_byte(0xA1, OLED_CMD);     /* 列地址方向: 正常 */
    oled_write_byte(0xC8, OLED_CMD);     /* 行扫描方向: 正常 */
    oled_write_byte(0xA6, OLED_CMD);     /* 正常显示(非反显) */
    oled_write_byte(0xA8, OLED_CMD);     /* 多路复用比 */
    oled_write_byte(0x3F, OLED_CMD);     /* 1/64 duty */
    oled_write_byte(0xD3, OLED_CMD);     /* 显示偏移 */
    oled_write_byte(0x00, OLED_CMD);     /* 无偏移 */
    oled_write_byte(0xD5, OLED_CMD);     /* 时钟分频/振荡频率 */
    oled_write_byte(0x80, OLED_CMD);     /* 100 帧/秒 */
    oled_write_byte(0xD9, OLED_CMD);     /* 预充电周期 */
    oled_write_byte(0xF1, OLED_CMD);     /* 预充电 15 clk, 放电 1 clk */
    oled_write_byte(0xDA, OLED_CMD);     /* COM 引脚硬件配置 */
    oled_write_byte(0x12, OLED_CMD);     /* 128x64 交替式 */
    oled_write_byte(0xDB, OLED_CMD);     /* VCOMH 电平 */
    oled_write_byte(0x40, OLED_CMD);     /* VCOM 反选电平 */
    oled_write_byte(0x20, OLED_CMD);     /* 页寻址模式 */
    oled_write_byte(0x02, OLED_CMD);     /* 页起始地址 = 2 */
    oled_write_byte(0x8D, OLED_CMD);     /* 电荷泵使能 */
    oled_write_byte(0x14, OLED_CMD);     /* 打开电荷泵 */
    oled_write_byte(0xA4, OLED_CMD);     /* 跟随显存显示 */
    oled_write_byte(0xA6, OLED_CMD);     /* 正常显示 */

    OLED_Clear();
    oled_write_byte(0xAF, OLED_CMD);     /* 点亮显示 */

    ESP_LOGI(TAG, "SSD1306 init done (128x64, SPI mode 0)");
}

void OLED_Clear(void)
{
    static const uint8_t blank[OLED_SPI_MAX_LEN] = {0};

    for (uint8_t page = 0; page < 8; page++) {
        oled_set_pos(0, page);
        oled_write(blank, OLED_WIDTH, true);    /* 一页 128 字节一次发完 */
    }
}

void OLED_FillAll(uint8_t dat)
{
    static uint8_t buf[OLED_SPI_MAX_LEN];

    memset(buf, dat, sizeof(buf));
    for (uint8_t page = 0; page < 8; page++) {
        oled_set_pos(0, page);
        oled_write(buf, OLED_WIDTH, true);
    }
}

void OLED_ClearArea(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    static const uint8_t blank[OLED_SPI_MAX_LEN] = {0};

    if (x >= OLED_WIDTH || y >= OLED_HEIGHT || w == 0 || h == 0) {
        return;
    }
    if (x + w > OLED_WIDTH) {
        w = OLED_WIDTH - x;
    }
    if (y + h > OLED_HEIGHT) {
        h = OLED_HEIGHT - y;
    }

    uint8_t first_page = y / 8;
    uint8_t last_page = (y + h - 1) / 8;

    for (uint8_t page = first_page; page <= last_page; page++) {
        oled_set_pos(x, page);
        oled_write(blank, w, true);
    }
}


void OLED_Display_On(void)
{
    oled_write_byte(0x8D, OLED_CMD);
    oled_write_byte(0x14, OLED_CMD);
    oled_write_byte(0xAF, OLED_CMD);
}

void OLED_Display_Off(void)
{
    oled_write_byte(0x8D, OLED_CMD);
    oled_write_byte(0x10, OLED_CMD);
    oled_write_byte(0xAE, OLED_CMD);
}

void OLED_ColorTurn(uint8_t i)
{
    oled_write_byte(i == 0 ? 0xA6 : 0xA7, OLED_CMD);
}

void OLED_DisplayTurn(uint8_t i)
{
    if (i == 0) {
        oled_write_byte(0xC8, OLED_CMD);
        oled_write_byte(0xA1, OLED_CMD);
    } else {
        oled_write_byte(0xC0, OLED_CMD);
        oled_write_byte(0xA0, OLED_CMD);
    }
}

void OLED_ShowChar(uint8_t x, uint8_t y, uint8_t chr, uint8_t sizey)
{
    uint8_t page = y / 8;
    uint8_t idx = chr - ' ';

    if (idx > 94) {
        return;                     /* 超出字库范围 */
    }

    if (sizey == 8) {
        /* 6x8: 6 个字节就是 6 列, 都在同一页 */
        oled_set_pos(x, page);
        oled_write(asc2_0806[idx], 6, true);
    } else if (sizey == 16) {
        /* 8x16: 前 8 个字节是上半页的 8 列, 后 8 个字节是下半页的 8 列 */
        oled_set_pos(x, page);
        oled_write(&asc2_1608[idx][0], 8, true);
        if (page + 1 < 8) {         /* 最后一页下面没有页了 */
            oled_set_pos(x, page + 1);
            oled_write(&asc2_1608[idx][8], 8, true);
        }
    }
}

void OLED_ShowString(uint8_t x, uint8_t y, const char *str, uint8_t sizey)
{
    uint8_t width = (sizey == 8) ? 6 : 8;

    if (str == NULL || x >= OLED_WIDTH || y >= OLED_HEIGHT) {
        return;
    }

    size_t len = strlen(str);
    if (len == 0) {
        return;
    }

    /* 本次文字实际占用的列范围 */
    uint8_t w = (uint8_t)(len * width);
    if (x + w > OLED_WIDTH) {
        w = OLED_WIDTH - x;
    }

    /* 同位置写过的话先清掉旧内容, 再画新的 */
    oled_prepare(x, y, sizey, w);

    /* 逐字符输出 */
    uint8_t cx = x;
    for (size_t i = 0; i < len; i++) {
        if (cx > OLED_WIDTH - width) {
            break;
        }
        OLED_ShowChar(cx, y, (uint8_t)str[i], sizey);
        cx += width;
    }
}

void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t len, uint8_t sizey)
{
    uint8_t width = (sizey == 8) ? 6 : 8;
    uint8_t shown = 0;

    if (len == 0 || x >= OLED_WIDTH || y >= OLED_HEIGHT) {
        return;
    }

    /* 本次数字占用的列范围 */
    uint8_t w = (uint8_t)(len * width);
    if (x + w > OLED_WIDTH) {
        w = OLED_WIDTH - x;
    }

    /* 同位置写过的话先清掉旧内容 */
    oled_prepare(x, y, sizey, w);

    for (uint8_t t = 0; t < len; t++) {
        uint32_t divisor = 1;
        for (uint8_t k = 0; k < (len - 1 - t); k++) {
            divisor *= 10;
        }
        uint8_t digit = (num / divisor) % 10;

        /* 去掉前导零: 未出现非零位之前都显示空格 */
        if (shown == 0 && t < (len - 1)) {
            if (digit == 0) {
                OLED_ShowChar(x + width * t, y, ' ', sizey);
                continue;
            }
            shown = 1;
        }
        OLED_ShowChar(x + width * t, y, (uint8_t)(digit + '0'), sizey);
    }
}

void OLED_ShowChinese(uint8_t x, uint8_t y, uint8_t no, uint8_t sizey)
{
    uint8_t page = y / 8;

    if (sizey != 16 || no > 6) {
        return;
    }

    /* 16x16: 前 16 字节上半页, 后 16 字节下半页 */
    oled_set_pos(x, page);
    oled_write(&Hzk[no][0], 16, true);
    oled_set_pos(x, page + 1);
    oled_write(&Hzk[no][16], 16, true);
}

void OLED_DrawBMP(uint8_t x, uint8_t y, uint8_t sizex, uint8_t sizey, const uint8_t *bmp)
{
    uint8_t pages = sizey / 8 + ((sizey % 8) ? 1 : 0);
    const uint8_t *p = bmp;

    for (uint8_t i = 0; i < pages; i++) {
        oled_set_pos(x, y / 8 + i);
        oled_write(p, sizex, true);
        p += sizex;
    }
}
