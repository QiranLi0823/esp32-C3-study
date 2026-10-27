/**
 * @file    OLED.h
 * @brief   SSD1306 0.96" OLED (SPI 接口) 驱动 —— ESP32-C3
 *
 * 移植自中景园 C8051F020 SPI 例程 (SRC/oled.c, SRC/oled.h)，
 * 保留原厂 API 名称与点阵数据，底层由「GPIO 位操作模拟 SPI」改为「硬件 SPI」。
 *
 * 接线 (ESP32-C3 SuperMini):
 *   VCC -> 3V3      GND  -> GND
 *   DO  -> GPIO4    (MOSI)
 *   D1  -> GPIO6    (SCLK / SPI CLK)
 *   RES -> GPIO1
 *   DC  -> GPIO3
 *   CS  -> GPIO7
 *
 * 坐标约定: x 为列(0~127)，y 为像素行(0~63)。
 * sizey=16 的字符必须让 y 处于 8 的倍数上(整页对齐)，否则字形会跨页错位。
 */
#ifndef OLED_H
#define OLED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 写命令 / 写数据 */
#define OLED_CMD  0
#define OLED_DATA 1

/* 屏幕尺寸 */
#define OLED_WIDTH   128
#define OLED_HEIGHT  64

/* 接线引脚定义
 *
 * 注意: 中景园这块模块的丝印是 D0 / D1, 而厂商例程的注释写的是
 *       "D0 接 P02(SCL)"、"D1 接 P03(SDA)"
 * 也就是 D0 是时钟线、D1 是数据线, 与常见的 "D0=数据/D1=时钟" 相反。
 * 下面用 OLED_SPI_SWAPPED 一个开关控制两种接法, 换模块时改这里即可。
 *
 *   OLED_SPI_SWAPPED = 1 : D0=时钟(SCLK), D1=数据(MOSI)  ← 厂商例程的接法
 *   OLED_SPI_SWAPPED = 0 : D0=数据(MOSI), D1=时钟(SCLK)  ← 常规接法
 */
#define OLED_SPI_SWAPPED  1

#if OLED_SPI_SWAPPED
#define OLED_PIN_SCLK   4     /* D0 */
#define OLED_PIN_MOSI   6     /* D1 */
#else
#define OLED_PIN_MOSI   4     /* D0 */
#define OLED_PIN_SCLK   6     /* D1 */
#endif

#define OLED_PIN_CS     7
#define OLED_PIN_DC     3
#define OLED_PIN_RES    1

/**
 * @brief  初始化 OLED(复位 + 发送初始化命令 + 点亮屏幕)
 * @note   必须在使用其它函数之前调用一次
 */
void OLED_Init(void);

/**
 * @brief  清屏(整屏写 0, 效果等同熄灭)
 */
void OLED_Clear(void);

/**
 * @brief  调试用: 整屏填充指定数据
 * @param  dat  0x00 全黑, 0xFF 全白(用来快速验证 SPI 是否通了)
 */
void OLED_FillAll(uint8_t dat);

/**
 * @brief  清除一块矩形区域(写 0)
 * @param  x, y  左上角坐标(像素)
 * @param  w     宽(像素)
 * @param  h     高(像素)
 */
void OLED_ClearArea(uint8_t x, uint8_t y, uint8_t w, uint8_t h);

/**
 * @brief  开关显示
 */
void OLED_Display_On(void);
void OLED_Display_Off(void);

/**
 * @brief  反色显示
 * @param  i  0: 正常显示, 1: 反色显示
 */
void OLED_ColorTurn(uint8_t i);

/**
 * @brief  屏幕旋转 180 度
 * @param  i  0: 正常显示, 1: 旋转 180 度
 */
void OLED_DisplayTurn(uint8_t i);

/**
 * @brief  显示一个 ASCII 字符
 * @param  x     列坐标 0~127
 * @param  y     像素行坐标 0~63
 * @param  chr   字符
 * @param  sizey 字号, 只支持 8(6x8) 和 16(8x16)
 */
void OLED_ShowChar(uint8_t x, uint8_t y, uint8_t chr, uint8_t sizey);

/**
 * @brief  显示字符串
 * @param  x     列坐标 0~127
 * @param  y     像素行坐标 0~63
 * @param  str   以 '\0' 结尾的字符串
 * @param  sizey 字号, 只支持 8(6x8) 和 16(8x16)
 *
 * 在同一个 (x, y) 位置第二次调用时会自动把上次写的内容清掉,
 * 因此 "led is on..." 和 "led is off..." 这样长短不一的文字交替显示不会留残影。
 * 第一次调用不做清除, 不会影响同一行上其它位置的内容。
 */
void OLED_ShowString(uint8_t x, uint8_t y, const char *str, uint8_t sizey);

/**
 * @brief  显示数字
 * @param  x     列坐标 0~127
 * @param  y     像素行坐标 0~63
 * @param  num   要显示的数值
 * @param  len   显示位数(不足前面补空格)
 * @param  sizey 字号, 只支持 8(6x8) 和 16(8x16)
 */
void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t len, uint8_t sizey);

/**
 * @brief  显示 16x16 汉字(字库为原厂内置的 7 个字: 中景园电子技术)
 * @param  x     列坐标 0~111
 * @param  y     像素行坐标 0~47, 需 8 对齐
 * @param  no    字库编号 0~6
 * @param  sizey 只能填 16
 */
void OLED_ShowChinese(uint8_t x, uint8_t y, uint8_t no, uint8_t sizey);

/**
 * @brief  显示一张 128x64 的点阵图片
 * @param  x, y  左上角坐标(像素)
 * @param  sizex, sizey 图片宽高(像素)
 * @param  bmp   点阵数据, 每行 sizex/8 个字节, 纵向排列
 */
void OLED_DrawBMP(uint8_t x, uint8_t y, uint8_t sizex, uint8_t sizey, const uint8_t *bmp);

#ifdef __cplusplus
}
#endif

#endif /* OLED_H */
