/*******************************************************************************
 * 硬件层主程序：CT107D 传感器数据主动上报 + 上传计数显示
 *
 * 功能：
 *   1. 每 2 秒通过串口上报一帧：S,温度,光敏mV,电位器mV,频率Hz\r\n
 *   2. 数码管显示上传成功次数（前导零不显示，上电从 0 开始）
 *   3. 每次发送成功后 L1 亮 0.2 秒
 *
 * 未使用的模板功能已移除实现，仅保留函数声明作为接口预留。
 ******************************************************************************/

#include "STC15F2K60S2.h"
#include "ds1302.h"
#include "iic.h"
#include "onewire.h"
#include "ultrasonic.h"
#include "uart.h"

/********************************* 可调参数 *********************************/
#define PCF8591_TASK_TIME     50
#define DS18B20_TASK_TIME     250
#define NE555_TASK_TIME       1000
#define SEND_TASK_TIME        2000
#define KEY_TASK_TIME         10
#define LED_BLINK_TIME        200

#define TIME_CCAP1 1000
#define TIME_CCAP2 666

/********************************* 段码定义 *********************************/
#define SEG_BLANK    34

unsigned char code Code[38]={
    0XC0,0XF9,0XA4,0XB0,0X99,0X92,0X82,0XF8,0X80,0X90,
    0X88,0X83,0XC6,0XA1,0X86,0X8E,
    0x40,0x79,0x24,0x30,0x19,0x12,0x02,0x78,0x00,0x10,
    0x08,0x03,0x46,0x21,0x06,0x3f,
    0x7F,0xBF,0xFF,0xAB,0xC7,0x8C
};

/********************************* 硬件影子变量 ********************************/
unsigned char led = 0x00;
unsigned char seg[8]={SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK};
unsigned char SEG_Count = 0;

/********************************* 传感器数据 ********************************/
unsigned char adc[5] = {0,0,0,0,128};
unsigned int pot_mv = 0;
unsigned int light_mv = 0;
long int ds18b20 = 22;
unsigned int ne555 = 0;

/********************************* 业务数据 *********************************/
unsigned long upload_count = 0;      // 上传成功次数（上电从 0 开始，不保存）
unsigned int pot_mv_prev = 0;        // 电位器上次原始换算值（偶发0过滤用）
unsigned int light_mv_prev = 0;      // 光敏上次原始换算值（偶发0过滤用）
bit tx_ok_flag = 0;                  // 1 = 已发出一帧，等待发送完成
bit tx_led_on = 0;                   // L1 点亮状态
unsigned char TX_LED_Task = 0;       // L1 亮灯计时（LED_BLINK_TIME 后熄灭）

/********************************* 任务计数器 ********************************/
unsigned char PCF8591_Task = 0;
unsigned char DS18B20_Task = 0;
unsigned int  NE555_Task = 0;
unsigned int  Send_Task = 0;

/********************************* 按键与发送使能 ********************************/
unsigned char key_val = 0;      // 当前按键值
unsigned char key_old = 0;      // 上一次按键值
unsigned char key_down = 0;     // 按下瞬间，保持一轮
unsigned char KEY_Task = 0;     // 按键扫描节流（10ms）
bit send_enable = 0;            // 0=未启动；按 S4 后置 1

/********************************* 串口发送缓冲 ********************************/
#define TX1_LENGTH 80
xdata char TX1_Buffer[TX1_LENGTH];
unsigned char B_TX1_busy = 255;

/********************************* PCA 计数 ********************************/
unsigned int TIME_1 = TIME_CCAP1;
unsigned int TIME_2 = TIME_CCAP2;

/********************************* 函数声明 *********************************/
void System_Init(void);
void HC573_Write(unsigned char channel,unsigned char dat);
void Display_Clear(void);
void Display_Proc(void);
void Task_Proc(void);
void Led_Proc(void);
void Uart_SendString(char *str);
void Uart_TxClear(void);
void Uart_TxStart(void);
void Uart_TxPutChar(unsigned char *idx,char dat);
void Uart_TxPutString(unsigned char *idx,char *str);
void Uart_TxPutUInt(unsigned char *idx,unsigned int dat);
void Timer0Init(void);
void Timer1Init(void);
void PCA_TIME_init(void);
long int DS18B20_ToIntC(unsigned int raw);

/* ===== 预留接口（当前未使用，仅保留声明） ===== */
void Key_Proc(void);                          // 按键扫描
unsigned char Key_Read(void);                 // 按键读取
void Logic_Proc(void);                        // 业务逻辑
void Relay_Buzz_Proc(void);                   // 继电器/蜂鸣器
void Uart_Proc(void);                         // 串口命令接收处理
void Uart_Report_Proc(void);                  // 异常上报
bit Uart_ParseOneCommand(unsigned char start,unsigned char end,unsigned char reply_enable); // 命令解析
void Uart_TxPut2Digit(unsigned char *idx,unsigned char dat);
void Uart_TxPutVoltage(unsigned char *idx,unsigned int mv);
void EEPROM_LoadParams(void);
void EEPROM_SaveParams(void);

/********************************* 主函数 ************************************/
void main(void)
{
    System_Init();

    while(1)
    {
        Display_Proc();
        Key_Proc();
        Task_Proc();
        Led_Proc();
    }
}

/********************************* 初始化 ************************************/
void System_Init(void)
{
    HC573_Write(4,0xff);
    HC573_Write(5,0x00);
    HC573_Write(6,0x00);
    HC573_Write(7,0xff);

    Display_Clear();
    Timer0Init();
    Timer1Init();
    uart_init();
    EA = 1;
}

void HC573_Write(unsigned char channel,unsigned char dat)
{
    P2 &= 0x1f;
    P0 = dat;
    switch(channel)
    {
        case 4: P2 = (P2 & 0x1f) | 0x80; break;
        case 5: P2 = (P2 & 0x1f) | 0xa0; break;
        case 6: P2 = (P2 & 0x1f) | 0xc0; break;
        case 7: P2 = (P2 & 0x1f) | 0xe0; break;
        default: break;
    }
    P2 &= 0x1f;
}

void Display_Clear(void)
{
    unsigned char i;
    for(i=0;i<8;i++) seg[i] = SEG_BLANK;
}

/********************************* 数码管显示 *********************************
 * 显示内容：上传成功次数 upload_count。
 * 前导零不显示；全为 0 时显示单个 0；每帧刷新（无需保存）。
 ******************************************************************************/
void Display_Proc(void)
{
    unsigned char i;
    unsigned char d;
    unsigned char show = 0;
    unsigned long num = upload_count;
    unsigned long weight = 10000000;

    for(i=0;i<8;i++)
    {
        d = (unsigned char)(num / weight);
        num %= weight;
        weight /= 10;
        if(show == 0 && d == 0 && i < 7) seg[i] = SEG_BLANK;
        else
        {
            show = 1;
            seg[i] = d;
        }
    }
}

/********************************* 任务调度 ***********************************
 * 各传感器按周期采集；
 * 每 SEND_TASK_TIME 拼一帧发到串口；
 * 发送完成后计数 +1 并点亮 L1。
 ******************************************************************************/
void Task_Proc(void)
{
    unsigned int raw_temp;
    unsigned char tx_i;
    unsigned int pot_raw;
    unsigned int light_raw;

    if(PCF8591_Task >= PCF8591_TASK_TIME)
    {
        PCF8591_Task = 0;
        PCF8591_Adc(adc);
        pot_raw = (unsigned int)((unsigned long)adc[3] * 5000 / 255);
        light_raw = (unsigned int)((unsigned long)adc[1] * 5000 / 255);
        /* B方案：偶发读到 0（IIC 偶发错误）时沿用上次值；连续两次 0 才认为是真实归零 */
        if(pot_raw != 0 || pot_mv_prev == 0) pot_mv = pot_raw;
        pot_mv_prev = pot_raw;
        if(light_raw != 0 || light_mv_prev == 0) light_mv = light_raw;
        light_mv_prev = light_raw;
    }

    if(DS18B20_Task >= DS18B20_TASK_TIME)
    {
        DS18B20_Task = 0;
        raw_temp = rd_temperature();
        ds18b20 = DS18B20_ToIntC(raw_temp);
    }

    if(NE555_Task >= NE555_TASK_TIME)
    {
        NE555_Task = 0;
        TR0 = 0;
        ne555 = ((unsigned int)TH0 << 8) | TL0;
        TH0 = 0;
        TL0 = 0;
        TR0 = 1;
    }

    if(Send_Task >= SEND_TASK_TIME)
    {
        tx_i = 0;
        Send_Task = 0;
        if(send_enable == 0) return;   /* 未按 S4：不上报 */
        if(B_TX1_busy != 255) return;

        Uart_TxClear();
        Uart_TxPutChar(&tx_i,'S');
        Uart_TxPutChar(&tx_i,',');
        if(ds18b20 < 0)
        {
            Uart_TxPutChar(&tx_i,'-');
            Uart_TxPutUInt(&tx_i,(unsigned int)(-ds18b20));
        }
        else
        {
            Uart_TxPutUInt(&tx_i,(unsigned int)ds18b20);
        }
        Uart_TxPutChar(&tx_i,',');
        Uart_TxPutUInt(&tx_i,light_mv);
        Uart_TxPutChar(&tx_i,',');
        Uart_TxPutUInt(&tx_i,pot_mv);
        Uart_TxPutChar(&tx_i,',');
        Uart_TxPutUInt(&tx_i,ne555);
        Uart_TxPutString(&tx_i,"\r\n");
        Uart_TxStart();
        tx_ok_flag = 1;
    }

    /* 发送完成：帧已全部发出 → 计数 +1，L1 亮起 */
    if(tx_ok_flag && B_TX1_busy == 255)
    {
        tx_ok_flag = 0;
        upload_count++;
        tx_led_on = 1;
        TX_LED_Task = 0;
    }
}

/********************************* LED 处理 ***********************************/
void Led_Proc(void)
{
    led = 0x00;
    if(tx_led_on) led |= 0x01;   // L1：发送成功后亮 0.2 秒
}

/********************************* 按键扫描 ***********************************/
unsigned char Key_Read(void)
{
    P44=0;P42=1;P35=1;
    P33=1;P32=1;
    if(!P33)return 4;      /* S4：启动发送 */
    else if(!P32)return 5; /* S5 */
    P44=1;P42=0;
    if(!P33)return 8;      /* S8 */
    else if(!P32)return 9; /* S9 */
    P42=1;P35=0;
    if(!P33)return 12;     /* S12 */
    else if(!P32)return 13;/* S13 */
    return 0;
}

void Key_Proc(void)
{
    if(KEY_Task < KEY_TASK_TIME) return;
    KEY_Task = 0;

    key_val = Key_Read();
    key_down = key_val & (key_val ^ key_old);
    key_old = key_val;

    if(key_down == 4)          /* S4：按一次后开始发送 */
    {
        send_enable = 1;
        Send_Task = SEND_TASK_TIME;   /* 立即触发第一帧 */
    }
}

/********************************* 串口中断 ***********************************
 * 仅发送方向：TI 中断逐字节发送 TX1_Buffer；RI 丢弃（本机只发不收）。
 ******************************************************************************/
void uart_0(void) interrupt 4
{
    if(RI)
    {
        RI = 0;                  // 丢弃接收字节
    }

    if(TI)
    {
        TI = 0;
        if(B_TX1_busy != 255 && TX1_Buffer[B_TX1_busy] != '\0')
        {
            SBUF = TX1_Buffer[B_TX1_busy];
            B_TX1_busy++;
        }
        else
        {
            B_TX1_busy = 255;
        }
    }
}

/********************************* PCA 中断 ***********************************/
void CCP_IRQHandler(void) interrupt 7
{
    if(CCF1)                 // 1ms 系统节拍
    {
        CCF1 = 0;
        CCAP1L = TIME_1;
        CCAP1H = TIME_1 >> 8;
        TIME_1 += TIME_CCAP1;

        if(PCF8591_Task < PCF8591_TASK_TIME) PCF8591_Task++;
        if(DS18B20_Task < DS18B20_TASK_TIME) DS18B20_Task++;
        if(NE555_Task < NE555_TASK_TIME) NE555_Task++;
        if(Send_Task < SEND_TASK_TIME) Send_Task++;
        if(KEY_Task < KEY_TASK_TIME) KEY_Task++;

        if(tx_led_on)
        {
            if(TX_LED_Task < LED_BLINK_TIME) TX_LED_Task++;
            else
            {
                TX_LED_Task = 0;
                tx_led_on = 0;
            }
        }
    }

    if(CCF2)                 // 显示刷新
    {
        CCF2 = 0;
        CCAP2L = TIME_2;
        CCAP2H = TIME_2 >> 8;
        TIME_2 += TIME_CCAP2;

        HC573_Write(4,~led);
        HC573_Write(7,0xff);
        HC573_Write(6,1 << (SEG_Count % 8));
        HC573_Write(7,Code[seg[SEG_Count % 8]]);
        SEG_Count++;
    }
}

/********************************* 串口发送函数 ********************************/
void Uart_SendString(char *str)
{
    unsigned char i = 0;

    if(B_TX1_busy != 255) return;
    Uart_TxClear();
    while(str[i] != '\0' && i < TX1_LENGTH-1)
    {
        TX1_Buffer[i] = str[i];
        i++;
    }
    TX1_Buffer[i] = '\0';
    Uart_TxStart();
}

void Uart_TxClear(void)
{
    unsigned char i;
    for(i=0;i<TX1_LENGTH;i++) TX1_Buffer[i] = 0;
}

void Uart_TxStart(void)
{
    if(B_TX1_busy != 255) return;
    if(TX1_Buffer[0] == '\0') return;
    SBUF = TX1_Buffer[0];
    B_TX1_busy = 1;
}

void Uart_TxPutChar(unsigned char *idx,char dat)
{
    if(*idx < TX1_LENGTH-1)
    {
        TX1_Buffer[*idx] = dat;
        (*idx)++;
        TX1_Buffer[*idx] = '\0';
    }
}

void Uart_TxPutString(unsigned char *idx,char *str)
{
    while(*str != '\0')
    {
        Uart_TxPutChar(idx,*str);
        str++;
    }
}

void Uart_TxPutUInt(unsigned char *idx,unsigned int dat)
{
    bit started = 0;
    unsigned int div;

    for(div=10000; div>0; div/=10)
    {
        if(dat >= div || started || div == 1)
        {
            started = 1;
            Uart_TxPutChar(idx,dat/div+'0');
            dat %= div;
        }
    }
}

/********************************* 定时器初始化 ********************************/
void Timer0Init(void)
{
    AUXR |= 0x80;
    TMOD &= 0xF0;
    TMOD |= 0x04;
    TL0 = 0;
    TH0 = 0;
    TF0 = 0;
    TR0 = 1;
}

void Timer1Init(void)
{
    AUXR &= 0xBF;
    TMOD &= 0x0F;
    TL1 = 0x00;
    TH1 = 0x00;
    TF1 = 0;
    TR1 = 0;
    PCA_TIME_init();
}

void PCA_TIME_init(void)
{
    P_SW1 &= 0xCF;
    CCAPM0 = 0x11;
    PPCA = 1;
    CMOD = 0x00;
    CCON = 0x00;
    CH = 0;
    CL = 0;

    CCAP1L = TIME_1;
    CCAP1H = TIME_1 >> 8;
    CCAPM1 = 0x49;
    TIME_1 += TIME_CCAP1;

    CCAP2L = TIME_2;
    CCAP2H = TIME_2 >> 8;
    CCAPM2 = 0x49;
    TIME_2 += TIME_CCAP2;

    CR = 1;
}

/********************************* 温度换算 ************************************/
long int DS18B20_ToIntC(unsigned int raw)
{
    /* 返回温度×100，例 2706 = 27.06℃（DS18B20 每 LSB = 0.0625℃） */
    if(raw & 0xF000)    /* 负温度（12 位补码） */
    {
        return -((long int)((0x10000 - raw) & 0xFFFF) * 625 / 100);
    }
    return (long int)raw * 625 / 100;
}
