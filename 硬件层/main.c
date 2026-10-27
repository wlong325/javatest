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
#define RX1_LENGTH            80       // 串口接收缓冲长度
#define UART_IDLE_TIME        10       // 串口不定长接收：超 10ms 无新字节判一帧结束

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
unsigned char b_and_r = 0x00;                        // bit4=继电器，bit6=蜂鸣器（写 HC573 通道5）
unsigned char seg[8]={SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK};
unsigned char SEG_Count = 0;

/********************************* 传感器数据 ********************************/
unsigned char adc[5] = {0,0,0,0,128};
unsigned int pot_mv = 0;
unsigned int light_mv = 0;
long int ds18b20 = 22;
unsigned int ne555 = 0;
unsigned int wave = 0;
unsigned int ne555_candidate = 0;
bit ne555_candidate_valid = 0;

/********************************* 业务数据 *********************************/
unsigned long upload_count = 0;      // 上传成功次数（上电从 0 开始，不保存）
unsigned int pot_mv_prev = 0;        // 电位器上次原始换算值（偶发0过滤用）
unsigned int light_mv_prev = 0;      // 光敏上次原始换算值（偶发0过滤用）
bit tx_ok_flag = 0;                  // 1 = 已发出一帧，等待发送完成
bit tx_led_on = 0;                   // L8 点亮状态
unsigned char TX_LED_Task = 0;       // L8 亮灯计时（LED_BLINK_TIME 后熄灭）
unsigned char mode = 4;              // 显示界面 0~4（界面1~5），S4 切换，默认界面5
long int disp_temp = 22;             // 显示用温度（×100），去抖后值
unsigned int disp_light = 0;         // 显示用光敏 mV（去抖后值）
unsigned int disp_pot = 0;           // 显示用电位器 mV（去抖后值）
bit disp_dirty = 1;                  // 1=显示内容有变化，需重算 seg[]（上电先算一次）

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

/********************************* 串口接收缓冲 ********************************/
xdata char RX1_Buffer[RX1_LENGTH];
volatile unsigned char B_RX1_Con = 0;                         // 已接收字节数
volatile unsigned char B_RX1_flag = 0;                                  // 1=一帧接收完成，主循环可解析
volatile unsigned char UART_Idle_Task = 0;                    // 接收空闲计时（10ms 判帧结束）

/********************************* PCA 计数 ********************************/
unsigned int TIME_1 = TIME_CCAP1;
unsigned int TIME_2 = TIME_CCAP2;

/********************************* 函数声明 *********************************/
void System_Init(void);
void HC573_Write(unsigned char channel,unsigned char dat);
void Display_Clear(void);
void Display_Proc(void);
void Seg_ShowFixed(unsigned long val, unsigned char digits, unsigned char point, bit hide_zero);
void Task_Proc(void);
void Led_Proc(void);
void Uart_SendString(char *str);
void Uart_TxClear(void);
void Uart_Proc(void);
void Uart_ClearRxBuffer(void);
bit Uart_ParseOneCommand(unsigned char start,unsigned char end,unsigned char reply_enable);
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
void Uart_Report_Proc(void);                  // 异常上报
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
        Uart_Proc();
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
 * 5 个界面由 S4 循环切换（mode 0~4）：
 *   界面1：温度 xx.xx℃；界面2：光敏 x.xxxV；界面3：电位器 x.xxxV；
 *   界面4：NE555 频率；界面5：上传成功次数。
 * 界面1~3 的值来自最近一次采集（即发送帧内容），每帧刷新。
 * 小数点显示用带点段码（Code[d+16]），point 为从右数第几位带点。
 ******************************************************************************/
void Seg_ShowFixed(unsigned long val, unsigned char digits, unsigned char point, bit hide_zero)
{
    unsigned char i;
    unsigned char d;
    unsigned char show = 0;
    unsigned char pos = 8 - digits;
    unsigned char tmp[8];

    for(i=0;i<digits;i++)            /* 按高位顺序拆出各位（只用除10/取模） */
    {
        tmp[digits - 1 - i] = (unsigned char)(val % 10);
        val /= 10;
    }
    for(i=0;i<8;i++) seg[i] = SEG_BLANK;

    for(i=0;i<digits;i++, pos++)
    {
        d = tmp[i];
        if(hide_zero && show == 0 && d == 0 && i < digits - 1)
            seg[pos] = SEG_BLANK;
        else
        {
            show = 1;
            seg[pos] = ((digits - i) == point) ? (d + 16) : d;
        }
    }
}

void Display_Proc(void)
{
    if(disp_dirty == 0) return;    /* 内容未变化：跳过重算，主循环保持轻快 */
    disp_dirty = 0;

    switch(mode)
    {
        case 0:  /* 界面1：温度 xx.xx ℃ */
            if(disp_temp < 0)
            {
                Seg_ShowFixed((unsigned long)(-disp_temp), 4, 3, 0);
                seg[3] = 33;         /* 负号段码 Code[33]=0xBF（共阳，只亮 g 段） */
            }
            else
                Seg_ShowFixed((unsigned long)disp_temp, 4, 3, 0);
            break;
        case 1:  /* 界面2：光敏电压 x.xxx V */
            Seg_ShowFixed(disp_light, 4, 4, 0);
            break;
        case 2:  /* 界面3：电位器 x.xxx V */
            Seg_ShowFixed(disp_pot, 4, 4, 0);
            break;
        case 3:  /* 界面4：NE555 频率 Hz */
            Seg_ShowFixed(ne555, 8, 0, 1);
            break;
        default: /* 界面5：上传成功次数 */
            Seg_ShowFixed(upload_count, 8, 0, 1);
            break;
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
    unsigned int measured_frequency;
    unsigned int frequency_delta;

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

        /* 去抖：显示值仅当变化超过阈值才更新，防低位抖动导致数码管闪烁 */
        if(light_mv > disp_light)
        {
            if(light_mv - disp_light >= 5) { disp_light = light_mv; disp_dirty = 1; }
        }
        else
        {
            if(disp_light - light_mv >= 5) { disp_light = light_mv; disp_dirty = 1; }
        }
        if(pot_mv > disp_pot)
        {
            if(pot_mv - disp_pot >= 5) { disp_pot = pot_mv; disp_dirty = 1; }
        }
        else
        {
            if(disp_pot - pot_mv >= 5) { disp_pot = pot_mv; disp_dirty = 1; }
        }
    }

    if(DS18B20_Task >= DS18B20_TASK_TIME)
    {
        DS18B20_Task = 0;
        raw_temp = rd_temperature();
        if(raw_temp != 0xFFFF && (raw_temp <= 0x07D0 || raw_temp >= 0xFC90))
        {
            ds18b20 = DS18B20_ToIntC(raw_temp);
            if(ds18b20 > disp_temp)
            {
                if(ds18b20 - disp_temp >= 2) { disp_temp = ds18b20; disp_dirty = 1; }
            }
            else
            {
                if(disp_temp - ds18b20 >= 2) { disp_temp = ds18b20; disp_dirty = 1; }
            }
        }
    }

    if(NE555_Task >= NE555_TASK_TIME)
    {
        NE555_Task = 0;
        TR0 = 0;
        measured_frequency = ((unsigned int)TH0 << 8) | TL0;
        TH0 = 0;
        TL0 = 0;
        TR0 = 1;

        if(ne555_candidate_valid == 0)
        {
            ne555_candidate = measured_frequency;
            ne555_candidate_valid = 1;
        }
        else
        {
            if(measured_frequency > ne555_candidate)
                frequency_delta = measured_frequency - ne555_candidate;
            else
                frequency_delta = ne555_candidate - measured_frequency;

            if(frequency_delta <= 2)
            {
                ne555 = measured_frequency;
                ne555_candidate_valid = 0;
                disp_dirty = 1;
            }
            else
            {
                ne555_candidate = measured_frequency;
            }
        }
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
        disp_dirty = 1;
        tx_led_on = 1;
        TX_LED_Task = 0;
    }
}

/********************************* LED 处理 ***********************************/
void Led_Proc(void)
{
    led = 0x00;
    led |= (unsigned char)(0x01 << mode);  /* L1~L5：显示界面 1~5 时对应灯常亮 */
    if(tx_led_on) led |= 0x80;             /* L8：发送成功后亮 0.2 秒 */
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

    if(key_down == 4)          /* S4：界面 1~5 循环切换 */
    {
        mode++;
        if(mode > 4) mode = 0;
        disp_dirty = 1;
    }
    else if(key_down == 8)     /* S8：按一次后开始发送 */
    {
        send_enable = 1;
        Send_Task = SEND_TASK_TIME;   /* 立即触发第一帧 */
    }
}


void uart_0(void) interrupt 4
{
    unsigned char rx_byte;

    if(RI)
    {
        rx_byte = SBUF;
        RI = 0;
        if(B_RX1_flag == 0)
        {
            if(B_RX1_Con < RX1_LENGTH-1)
            {
                RX1_Buffer[B_RX1_Con] = rx_byte;
                B_RX1_Con++;
                UART_Idle_Task = 0;
            }
            else
            {
                B_RX1_flag = 2;
            }
        }
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

        /* 串口不定长接收判帧：10ms 无新字节认为一帧接收完成 */
        if(B_RX1_Con != 0 && B_RX1_flag == 0)
        {
            if(UART_Idle_Task < UART_IDLE_TIME) UART_Idle_Task++;
            else B_RX1_flag = 1;
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
        HC573_Write(5,b_and_r);          // 继电器/蜂鸣器：bit4=继电器，bit6=蜂鸣器
    }
}

/********************************* 串口接收处理 ********************************
 * 双向控制：命令帧由桥通过串口下发，10ms 空闲判帧结束，主循环解析执行。
 * 命令表：? → OK；R1/R0 → 继电器开/关；B1/B0 → 蜂鸣器开/关。
 * 执行后回 OK；命令不识别回 ERROR。继电器/蜂鸣器改影子变量 b_and_r，
 * 由 CCF2 中断统一写入 74HC573 通道 5（bit4=继电器，bit6=蜂鸣器）。
 ******************************************************************************/
void Uart_ClearRxBuffer(void)
{
    unsigned char i;
    for(i=0;i<RX1_LENGTH;i++) RX1_Buffer[i] = 0;
    B_RX1_Con = 0;
    UART_Idle_Task = 0;
    B_RX1_flag = 0;
}

void Uart_Proc(void)
{
    unsigned char rx_len;
    unsigned char frame_state;

    frame_state = B_RX1_flag;
    if(frame_state == 0) return;

    rx_len = B_RX1_Con;
    while(rx_len > 0 && (RX1_Buffer[rx_len-1] == '\r' || RX1_Buffer[rx_len-1] == '\n'))
    {
        rx_len--;
    }
    RX1_Buffer[rx_len] = '\0';

    if(frame_state == 2)
        Uart_SendString("ERROR\r\n");
    else if(rx_len > 0)
        Uart_ParseOneCommand(0,rx_len-1,1);
    else
        Uart_SendString("ERROR\r\n");

    Uart_ClearRxBuffer();
}

bit Uart_ParseOneCommand(unsigned char start,unsigned char end,unsigned char reply_enable)
{
    unsigned char len;

    if(end < start) return 0;
    len = end - start + 1;

    /* 通信测试：? → OK */
    if(len == 1 && RX1_Buffer[start] == '?')
    {
        if(reply_enable) Uart_SendString("OK\r\n");
        return 1;
    }
    /* R1/R0：继电器开/关（b_and_r bit4） */
    else if(len == 2 && RX1_Buffer[start] == 'R')
    {
        if(RX1_Buffer[start+1] == '1')
        {
            b_and_r |= 0x10;          /* 继电器吸合 */
            if(reply_enable) Uart_SendString("OK\r\n");
            return 1;
        }
        else if(RX1_Buffer[start+1] == '0')
        {
            b_and_r &= ~0x10;         /* 继电器释放 */
            if(reply_enable) Uart_SendString("OK\r\n");
            return 1;
        }
    }
    /* B1/B0：蜂鸣器开/关（b_and_r bit6） */
    else if(len == 2 && RX1_Buffer[start] == 'B')
    {
        if(RX1_Buffer[start+1] == '1')
        {
            b_and_r |= 0x40;          /* 蜂鸣器鸣响 */
            if(reply_enable) Uart_SendString("OK\r\n");
            return 1;
        }
        else if(RX1_Buffer[start+1] == '0')
        {
            b_and_r &= ~0x40;         /* 蜂鸣器关闭 */
            if(reply_enable) Uart_SendString("OK\r\n");
            return 1;
        }
    }

    if(reply_enable) Uart_SendString("ERROR\r\n");
    return 0;
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
    CCAPM0 = 0x00;
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
