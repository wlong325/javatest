/*******************************************************************************
 * 蓝桥杯单片机通用模板
 *
 * 使用思路：
 * 1. PCA 的 CCF1 产生 1ms 系统节拍，只负责给各模块任务计数。
 * 2. PCA 的 CCF2 固定刷新 LED、数码管、继电器/蜂鸣器，保证显示稳定。
 * 3. while(1) 里只做业务：按键、显示内容、模块采样、逻辑判断、串口处理。
 * 4. 主循环不要直接操作 P0/P2/74HC573；只修改 led、seg[]、b_and_r 这些影子变量。
 * 5. 新题来了优先改 Display_Proc、Key_Proc、Logic_Proc、Uart_Proc。
 ******************************************************************************/

#include "STC15F2K60S2.h"
#include "ds1302.h"
#include "iic.h"
#include "onewire.h"
#include "ultrasonic.h"
#include "uart.h"

/********************************* 可调周期区 *********************************
 * 单位都是 ms。系统节拍在 PCA CCF1 中每 1ms 加一次。
 * 显示扫描不走这些任务周期，它由 PCA CCF2 独立高速刷新。
 ******************************************************************************/
#define KEY_TASK_TIME         10      // 按键扫描/消抖周期，通常 10ms
#define RTC_TASK_TIME         300     // DS1302 读取周期，300~1000ms 都可以
#define PCF8591_TASK_TIME     50      // ADC/DAC 处理周期，电位器/光敏建议 50ms 左右
#define DS18B20_TASK_TIME     250     // 温度读取周期，温度变化慢，250~500ms 即可
#define NE555_TASK_TIME       1000    // 频率测量周期，常用 1s 闸门
#define ULTRASONIC_TASK_TIME  200     // 超声波测距周期，建议 150~300ms
#define SEND_TASK_TIME  2000   // 主动上报周期：2 秒发一条
#define UART_IDLE_TIME        10      // 串口不定长接收：超过 10ms 无新字节认为一帧结束
#define LED_BLINK_TIME        200     // LED闪烁模板周期，200ms 翻转一次，效果约为 0.2s 闪烁
#define UART_MULTI_CMD_ENABLE 0       // 连续命令备用开关：0=普通单命令，1=支持 (D,2500)(T,235950) 这类连续配置命令
#define EEPROM_BASE_ADDR      0x00    // EEPROM参数保存起始地址，可按题目自行调整
#define EEPROM_MAGIC          0x5A    // EEPROM有效数据标记，用来判断是否已经保存过参数

/* PCA 定时周期，单位 us。
 * CCF1 = 1ms 系统节拍；CCF2 = 数码管扫描节拍。
 * CCF2 小于 CCF1，8 位数码管每位约 0.666ms，整屏约 5.3ms，显示比较稳。
 */
#define TIME_CCAP1 1000
#define TIME_CCAP2 666

/********************************* 段码与索引 *********************************
 * seg[] 里存的不是段码，而是下面 Code[] 的下标。
 * 0~15: 0~F
 * 16~31: 0.~F.，即带小数点
 * 32: 只有小数点
 * 33: '-'
 * 34: 空白
 * 35~37: n、L、P，可按题目继续扩展字库
 ******************************************************************************/
#define SEG_DOT      32
#define SEG_DASH     33
#define SEG_BLANK    34
#define SEG_N        35
#define SEG_L        36
#define SEG_P        37

unsigned char code Code[38]={
    0XC0,0XF9,0XA4,0XB0,0X99,0X92,0X82,0XF8,0X80,0X90,
    0X88,0X83,0XC6,0XA1,0X86,0X8E,
    0x40,0x79,0x24,0x30,0x19,0x12,0x02,0x78,0x00,0x10,
    0x08,0x03,0x46,0x21,0x06,0x3f,
    0x7F,0xBF,0xFF,0xAB,0xC7,0x8C
};

/********************************* 硬件影子变量 ********************************
 * 主循环只改这些变量；PCA CCF2 中断负责把它们真正写入 74HC573。
 ******************************************************************************/
unsigned char led = 0x00;                                    // bit=1 表示对应 LED 亮
unsigned char b_and_r = 0x00;                                // bit4 继电器，bit6 蜂鸣器
unsigned char seg[8]={SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK,SEG_BLANK};
unsigned char SEG_Count = 0;                                 // 当前扫描到第几位数码管

/********************************* 模块数据缓存 ********************************
 * 各模块读到的数据先放在这里，显示和业务逻辑只读取缓存。
 ******************************************************************************/
unsigned char rtc[3] = {23,59,50};                           // DS1302：时、分、秒
unsigned char adc[5] = {0,0,0,0,128};                        // PCF8591：[0]右侧ADC，[1]光敏，[2]运放，[3]电位器，[4]DAC
unsigned int pot_mv = 0;                                      // 电位器输入电压，单位 mV，范围约 0~5000
unsigned int light_mv = 0;                                    // 光敏电阻输入电压，单位 mV，范围约 0~5000
unsigned int dac_mv = 2500;                                   // DAC希望输出的电压，单位 mV，默认约 2.50V
long int ds18b20 = 22;                                       // 温度，单位：摄氏度整数
unsigned int ne555 = 0;                                      // NE555 频率，单位 Hz，1s 闸门时直接等于计数值
unsigned int wave = 0;                                       // 超声波距离，单位约 cm，驱动和 CCF0 配合更新

/********************************* 按键变量 ************************************/
unsigned char key_val = 0;                                   // 当前按键值
unsigned char key_old = 0;                                   // 上一次按键值
unsigned char key_down = 0;                                  // 按下瞬间，只保持一轮
unsigned char key_up = 0;                                    // 松开瞬间，只保持一轮

/********************************* 页面与业务变量 *******************************/
unsigned char mode = 0;                                      // 当前页面，按题目定义
unsigned char mode_old = 255;                                // 用于判断是否刚切换页面
unsigned int demo_threshold = 1000;                          // 示例阈值，新题可删除/改名
bit alarm_flag = 0;                                          // 示例报警标志，新题按需求改
bit uart_report_flag = 0;                                    // 主动异常上报标志：Logic_Proc() 置1，Uart_Report_Proc() 发送后清0
unsigned int uart_report_code = 500;                         // 主动上报错误码，按题目改成 500/501/502 等
bit eeprom_save_flag = 0;                                    // 置1后在主循环保存参数，避免在按键处理中直接写EEPROM
bit eeprom_load_flag = 0;                                    // 置1后在主循环读取参数

/********************************* 任务计数器 **********************************/
unsigned char KEY_Task = 0;
unsigned int  RTC_Task = 0;
unsigned char PCF8591_Task = 0;
unsigned char DS18B20_Task = 0;
unsigned int  NE555_Task = 0;
unsigned char ULTRASONIC_Task = 0;
unsigned int  Send_Task = 0;   // 主动上报计数器
unsigned char UART_Idle_Task = 0;
unsigned char LED_Blink_Task = 0;                             // LED闪烁计时，1ms 系统节拍累加到 LED_BLINK_TIME 后翻转
bit led_blink_flag = 0;                                       // LED闪烁状态位，1=本次应该点亮，0=本次应该熄灭

/********************************* 串口缓冲区 **********************************
 * B_TX1_busy = 255 表示发送空闲；非 255 表示正在由串口中断逐字节发送。
 * B_RX1_flag = 1 表示一帧接收完成，主循环可解析 RX1_Buffer。
 ******************************************************************************/
#define TX1_LENGTH 80
#define RX1_LENGTH 80
xdata char TX1_Buffer[TX1_LENGTH];
xdata char RX1_Buffer[RX1_LENGTH];
unsigned char B_TX1_busy = 255;
unsigned char B_RX1_Con = 0;
bit B_RX1_flag = 0;

/********************************* PCA 变量 ************************************/
unsigned int TIME_1 = TIME_CCAP1;
unsigned int TIME_2 = TIME_CCAP2;

/********************************* 函数声明 ************************************/
void System_Init(void);
void HC573_Write(unsigned char channel,unsigned char dat);
void Display_Clear(void);
void Display_Proc(void);
void Key_Proc(void);
void Task_Proc(void);
void Logic_Proc(void);
void Led_Proc(void);
void Relay_Buzz_Proc(void);
void Uart_Proc(void);
void Uart_Report_Proc(void);
bit Uart_ParseOneCommand(unsigned char start,unsigned char end,unsigned char reply_enable);
void Uart_SendString(char *str);
void Uart_ClearRxBuffer(void);
void Uart_TxClear(void);
void Uart_TxStart(void);
void Uart_TxPutChar(unsigned char *idx,char dat);
void Uart_TxPutString(unsigned char *idx,char *str);
void Uart_TxPut2Digit(unsigned char *idx,unsigned char dat);
void Uart_TxPutUInt(unsigned char *idx,unsigned int dat);
void Uart_TxPutVoltage(unsigned char *idx,unsigned int mv);
bit Uart_Parse4Digit(unsigned char start,unsigned int *value);
void EEPROM_LoadParams(void);
void EEPROM_SaveParams(void);
void Timer0Init(void);
void Timer1Init(void);
void PCA_TIME_init(void);
unsigned char Key_Read(void);
long int DS18B20_ToIntC(unsigned int raw);

/********************************* 主函数 **************************************/
void main(void)
{
    System_Init();

    while(1)
    {
        Display_Proc();       // 只修改 seg[]，不要写 P0/P2
        Key_Proc();           // 10ms 按键扫描，产生 key_down/key_up
        Task_Proc();          // 按周期读取 RTC/ADC/温度/频率/超声波
        Logic_Proc();         // 按题目写判断、计算、报警等业务逻辑
        Led_Proc();           // 只修改 led
        Relay_Buzz_Proc();    // 只修改 b_and_r
        Uart_Proc();          // 串口接收解析、发送组织
        Uart_Report_Proc();   // 主动异常上报；不用时可直接注释掉这一行
    }
}

/********************************* 初始化 **************************************/
void System_Init(void)
{
    HC573_Write(4,0xff);      // LED 全灭：板上 LED 通常低电平点亮，所以写 0xff
    HC573_Write(5,0x00);      // 继电器/蜂鸣器关闭
    HC573_Write(6,0x00);      // 位选清零
    HC573_Write(7,0xff);      // 段选全灭

    Display_Clear();
    EEPROM_LoadParams();     // 上电先尝试读取保存过的参数；校验失败会保留默认值
    Set_RTC(rtc);             // 比赛中如果题目要求上电设置初始时间，就保留；否则可注释
    Timer0Init();             // T0：NE555 频率计数
    Timer1Init();             // T1：超声波回波计时，内部顺带启动 PCA
    uart_init();              // 串口使用 T2 作为波特率发生器
    EA = 1;
}

/* 74HC573 写入函数。
 * 关键顺序：先把数据放到 P0，再打开对应锁存器打一拍，最后关闭锁存器。
 * 不建议在主循环随便调用它；显示相关硬件刷新集中放在 PCA CCF2 中断里。
 */
void HC573_Write(unsigned char channel,unsigned char dat)
{
    P2 &= 0x1f;
    P0 = dat;
    switch(channel)
    {
        case 4: P2 = (P2 & 0x1f) | 0x80; break;  // Y4C：LED
        case 5: P2 = (P2 & 0x1f) | 0xa0; break;  // Y5C：继电器/蜂鸣器
        case 6: P2 = (P2 & 0x1f) | 0xc0; break;  // Y6C：数码管位选
        case 7: P2 = (P2 & 0x1f) | 0xe0; break;  // Y7C：数码管段选
        default: break;
    }
    P2 &= 0x1f;
}

void Display_Clear(void)
{
    unsigned char i;
    for(i=0;i<8;i++) seg[i] = SEG_BLANK;
}

/********************************* 显示内容 ************************************
 * 这里只负责决定 seg[0]~seg[7] 显示什么。
 * 真正的动态扫描在 PCA CCF2 中断里完成。
 ******************************************************************************/
void Display_Proc(void)
{
    if(mode != mode_old)
    {
        mode_old = mode;
        Display_Clear();      // 切换页面时清一次，避免上个页面残留
    }

    switch(mode)
    {
        case 0:               // 示例页面0：显示时间 HH-MM-SS
            seg[0] = rtc[0] / 10;
            seg[1] = rtc[0] % 10;
            seg[2] = SEG_DASH;
            seg[3] = rtc[1] / 10;
            seg[4] = rtc[1] % 10;
            seg[5] = SEG_DASH;
            seg[6] = rtc[2] / 10;
            seg[7] = rtc[2] % 10;
        break;

        case 1:               // 示例页面1：显示温度和电位器电压 x.xxV
            seg[0] = SEG_BLANK;
            if(ds18b20 < 0)
            {
                seg[1] = SEG_DASH;
                seg[2] = (unsigned char)(-ds18b20 / 10);
                seg[3] = (unsigned char)(-ds18b20 % 10);
            }
            else
            {
                seg[1] = ds18b20 >= 100 ? (unsigned char)(ds18b20 / 100) : SEG_BLANK;
                seg[2] = ds18b20 >= 10  ? (unsigned char)(ds18b20 / 10 % 10) : SEG_BLANK;
                seg[3] = (unsigned char)(ds18b20 % 10);
            }
            seg[4] = SEG_DASH;
            seg[5] = pot_mv / 1000;
            seg[6] = pot_mv / 100 % 10 + 16;
            seg[7] = pot_mv / 10 % 10;
        break;

        case 2:               // 示例页面2：显示 NE555 频率，前导 0 消隐
            seg[0] = SEG_N;
            seg[1] = SEG_DASH;
            seg[2] = ne555 >= 10000 ? ne555 / 10000 : SEG_BLANK;
            seg[3] = ne555 >= 1000  ? ne555 / 1000 % 10 : SEG_BLANK;
            seg[4] = ne555 >= 100   ? ne555 / 100 % 10 : SEG_BLANK;
            seg[5] = ne555 >= 10    ? ne555 / 10 % 10 : SEG_BLANK;
            seg[6] = ne555 % 10;
            seg[7] = SEG_BLANK;
        break;

        case 3:               // 示例页面3：显示超声波距离
            seg[0] = SEG_BLANK;
            seg[1] = SEG_BLANK;
            seg[2] = SEG_BLANK;
            seg[3] = wave >= 1000 ? wave / 1000 : SEG_BLANK;
            seg[4] = wave >= 100  ? wave / 100 % 10 : SEG_BLANK;
            seg[5] = wave >= 10   ? wave / 10 % 10 : SEG_BLANK;
            seg[6] = wave % 10;
            seg[7] = SEG_BLANK;
        break;

        default:
            mode = 0;
        break;
    }
}

/********************************* 长按/双击模板 ********************************
 * 下面只是注释模板，默认不参与编译。题目需要长按/双击时，再按需复制到变量区和 Key_Proc()。
 * 当前 Key_Proc() 每 10ms 执行一次，所以计数值 100 约等于 1 秒，30 约等于 300ms。
 *
 * 一、需要加到“按键变量”区域的变量示例：
 * unsigned char long_key = 0;          // 长按触发的按键号，0表示无
 * unsigned int key_hold_cnt = 0;       // 当前按键已按住多久，单位约10ms
 * bit long_key_sent = 0;               // 防止一次长按重复触发
 *
 * unsigned char click_key = 0;         // 等待第二次点击的按键号
 * unsigned char click_wait_cnt = 0;    // 双击等待时间，单位约10ms
 * unsigned char double_key = 0;        // 双击触发的按键号，0表示无
 * bit single_click_flag = 0;           // 如果题目要区分单击/双击，可用它延迟确认单击
 *
 * 二、长按检测模板，放在 key_old = key_val; 后面：
 * if(key_val != 0)                     // 有按键一直按着
 * {
 *     if(key_hold_cnt < 65535) key_hold_cnt++;
 *     if(key_hold_cnt >= 100 && long_key_sent == 0) // 约1秒长按
 *     {
 *         long_key_sent = 1;
 *         long_key = key_val;          // 在后面根据 long_key 执行长按功能
 *     }
 * }
 * else                                // 没有按键，清除长按状态
 * {
 *     key_hold_cnt = 0;
 *     long_key_sent = 0;
 * }
 *
 * 三、双击检测模板，放在 key_down 处理附近：
 * if(key_down != 0)
 * {
 *     if(click_key == key_down && click_wait_cnt > 0)
 *     {
 *         double_key = key_down;       // 第二次点击在时间窗内，认为双击
 *         click_key = 0;
 *         click_wait_cnt = 0;
 *         single_click_flag = 0;
 *     }
 *     else
 *     {
 *         click_key = key_down;        // 第一次点击，开始等待第二次
 *         click_wait_cnt = 30;         // 约300ms双击窗口
 *         single_click_flag = 1;
 *     }
 * }
 *
 * if(click_wait_cnt > 0)
 * {
 *     click_wait_cnt--;
 *     if(click_wait_cnt == 0 && single_click_flag)
 *     {
 *         single_click_flag = 0;       // 到这里才确认是单击，不是双击
 *         // 在这里处理单击 click_key
 *         click_key = 0;
 *     }
 * }
 *
 * 四、双键组合/组合长按模板：
 * 注意：上面的 key_val 只能表示一个按键号，不适合判断“两个键同时按下”。
 * 如果题目要求 S4+S5 同时按下 1 秒，需要额外写一个“按键状态位图”函数。
 * 位图思想：每个按键占一个 bit，按下则置 1。例如 bit0 表示 S4，bit1 表示 S5。
 *
 * 变量示例：
 * unsigned char key_map = 0;          // 当前按键位图
 * unsigned int combo_cnt = 0;         // 组合键保持时间，单位约10ms
 * bit combo_sent = 0;                 // 防止组合长按重复触发
 * bit combo_45_flag = 0;              // 示例：S4+S5 同时长按事件
 *
 * 位图读取函数示例，按题目需要自己补全更多按键：
 * unsigned char Key_Map_Read(void)
 * {
 *     unsigned char map = 0;
 *
 *     P44=0; P42=1; P35=1;
 *     P33=1; P32=1;
 *     if(!P33) map |= 0x01;           // S4
 *     if(!P32) map |= 0x02;           // S5
 *
 *     P44=1; P42=0;
 *     if(!P33) map |= 0x04;           // S8
 *     if(!P32) map |= 0x08;           // S9
 *
 *     return map;
 * }
 *
 * 组合长按检测示例，放在 10ms 的 Key_Proc() 里：
 * key_map = Key_Map_Read();
 * if((key_map & 0x03) == 0x03)        // S4 和 S5 同时按下
 * {
 *     if(combo_cnt < 65535) combo_cnt++;
 *     if(combo_cnt >= 100 && combo_sent == 0) // 同时按住约1秒
 *     {
 *         combo_sent = 1;
 *         combo_45_flag = 1;          // 在后面处理 S4+S5 长按事件
 *     }
 * }
 * else
 * {
 *     combo_cnt = 0;
 *     combo_sent = 0;
 * }
 *
 * 使用示例：
 * if(combo_45_flag) { combo_45_flag = 0; 处理S4+S5同时长按; }
 *
 * 五、事件使用示例：
 * if(long_key == 8) { long_key = 0; 处理S8长按; }
 * if(double_key == 4) { double_key = 0; 处理S4双击/二次点击; }
 ******************************************************************************/
/********************************* 按键处理 ************************************
 * key_down 是“刚按下”的瞬间，适合切换页面、参数加减。
 ******************************************************************************/
void Key_Proc(void)
{
    if(KEY_Task < KEY_TASK_TIME) return;
    KEY_Task = 0;

    key_val = Key_Read();
    key_down = key_val & (key_val ^ key_old);
    key_up = (~key_val) & (key_val ^ key_old);
    key_old = key_val;

    if(key_down == 4)         // S4：切换页面
    {
        mode++;
        if(mode > 3) mode = 0;
    }
    else if(key_down == 8)    // S8：示例参数加
    {
        demo_threshold += 100;
    }
    else if(key_down == 9)    // S9：示例参数减
    {
        if(demo_threshold >= 100) demo_threshold -= 100;
    }
    else if(key_down == 12)   // S12：保存示例参数到 EEPROM
    {
        eeprom_save_flag = 1;
    }
    else if(key_down == 13)   // S13：从 EEPROM 重新读取示例参数
    {
        eeprom_load_flag = 1;
    }
}

/********************************* 周期任务 ************************************
 * 所有耗时模块都放在主循环里按周期执行，不要放进中断。
 ******************************************************************************/
void Task_Proc(void)
{
    unsigned int raw_temp;

    if(RTC_Task >= RTC_TASK_TIME)
    {
        RTC_Task = 0;
        Read_RTC(rtc);
    }

    if(PCF8591_Task >= PCF8591_TASK_TIME)
    {
        PCF8591_Task = 0;
        PCF8591_Adc(adc);    // 驱动会轮流更新 adc[0]~adc[3]，不是一次读完四路
        pot_mv = (unsigned int)((unsigned long)adc[3] * 5000 / 255);   // 电位器：AD值换算为 0~5000mV
        light_mv = (unsigned int)((unsigned long)adc[1] * 5000 / 255); // 光敏电阻：AD值换算为 0~5000mV
        if(dac_mv > 5000) dac_mv = 5000;
        adc[4] = (unsigned char)((unsigned long)dac_mv * 255 / 5000);  // DAC：目标mV反算为 0~255
        PCF8591_Dac(adc[4]);  // 不需要 DAC 时可注释
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

    if(ULTRASONIC_Task >= ULTRASONIC_TASK_TIME)
    {
        ULTRASONIC_Task = 0;
        Wave_Recv();

    if(Send_Task >= SEND_TASK_TIME)
    {
        unsigned char tx_i = 0;
        Send_Task = 0;
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
    }
    }

    if(eeprom_save_flag)
    {
        eeprom_save_flag = 0;
        EEPROM_SaveParams();
    }

    if(eeprom_load_flag)
    {
        eeprom_load_flag = 0;
        EEPROM_LoadParams();
    }
}


/********************************* EEPROM参数保存 *******************************
 * AT24C02 掉电不丢失，适合保存阈值、模式、校准值等少量参数。
 * 模板示例保存 6 个字节：
 * [0] 魔术字 EEPROM_MAGIC，表示这片 EEPROM 已经写过有效参数。
 * [1][2] demo_threshold，高字节在前，低字节在后。
 * [3][4] dac_mv，高字节在前，低字节在后。
 * [5] 简单校验和，防止 EEPROM 随机值被当成有效参数。
 *
 * 新题扩展方法：继续往 buf[] 后面加参数，同时更新校验和和读写长度。
 ******************************************************************************/
void EEPROM_LoadParams(void)
{
    unsigned char buf[6];
    unsigned char sum;

    EEPROM_Read(buf,EEPROM_BASE_ADDR,6);
    sum = buf[0] + buf[1] + buf[2] + buf[3] + buf[4];

    if(buf[0] == EEPROM_MAGIC && buf[5] == sum)
    {
        demo_threshold = ((unsigned int)buf[1] << 8) | buf[2];
        dac_mv = ((unsigned int)buf[3] << 8) | buf[4];
        if(dac_mv > 5000) dac_mv = 5000;
    }
}

void EEPROM_SaveParams(void)
{
    unsigned char buf[6];

    if(dac_mv > 5000) dac_mv = 5000;

    buf[0] = EEPROM_MAGIC;
    buf[1] = demo_threshold >> 8;
    buf[2] = demo_threshold & 0xff;
    buf[3] = dac_mv >> 8;
    buf[4] = dac_mv & 0xff;
    buf[5] = buf[0] + buf[1] + buf[2] + buf[3] + buf[4];

    EEPROM_Write(buf,EEPROM_BASE_ADDR,6);
}
/********************************* 业务逻辑 ************************************
 * 新题的大部分计算和报警判断写在这里。
 * 示例：NE555 超过阈值就报警。
 * 主动异常上报建议只在“异常刚发生”或“状态变化”时置 uart_report_flag，避免每轮循环都重复发送。
 ******************************************************************************/
void Logic_Proc(void)
{
    if(ne555 > demo_threshold) alarm_flag = 1;
    else alarm_flag = 0;

//    主动上报使用示例：
    if(ne555 > demo_threshold)
    {
         uart_report_code = 500;
         uart_report_flag = 1;
     }
//    这里默认不自动置位，防止模板上电后一直向串口发送示例异常。
    
}

/********************************* LED 与继电器/蜂鸣器 *************************
 * 这里只改影子变量 led / b_and_r，不直接写硬件。
 * b_and_r 写入 74HC573 的通道 5：bit4=继电器，bit6=蜂鸣器。
 * 常用写法：b_and_r |= 0x10 打开继电器；b_and_r |= 0x40 打开蜂鸣器。
 * 关闭时清对应位：b_and_r &= ~0x10；b_and_r &= ~0x40。
 ******************************************************************************/
void Led_Proc(void)
{
    led = 0x00;
    led |= (1 << mode);       // L1~L4 指示当前页面
    if(alarm_flag && led_blink_flag) led |= 0x10; // L5 报警闪烁灯：alarm_flag=1 时每 0.2s 翻转一次
}

void Relay_Buzz_Proc(void)
{
    b_and_r = 0x00;

    if(alarm_flag)
    {
        b_and_r |= 0x10;          // bit4：继电器吸合
        /* 如果题目要求报警时蜂鸣器响，就打开下一行。
         * 注意蜂鸣器是 bit6，也就是 0x40；不要和继电器的 0x10 混淆。
         */
        // b_and_r |= 0x40;       // bit6：蜂鸣器鸣响
    }
}

/********************************* 串口处理 ************************************
 * 串口模板采用“中断收发 + 主循环解析”的结构，和标准答案思路一致。
 *
 * 接收流程：
 * 1. 串口中断每收到 1 个字节，就放入 RX1_Buffer，并把 UART_Idle_Task 清零。
 * 2. PCA 的 1ms 节拍发现 10ms 没有新字节后，置 B_RX1_flag=1。
 * 3. 主循环 Uart_Proc() 看到 B_RX1_flag=1，认为一帧字符串接收完成，然后解析。
 *
 * 发送流程：
 * 1. 主循环先把完整字符串放入 TX1_Buffer。
 * 2. 写 SBUF=TX1_Buffer[0] 启动发送。
 * 3. 后续字节由串口发送中断 TI 自动一个一个发出。
 *
 * 普通模式与连续命令模式：
 * 1. UART_MULTI_CMD_ENABLE = 0：默认模式，一帧只当作一条命令解析，最通用也最安全。
 * 2. UART_MULTI_CMD_ENABLE = 1：备用模式，会在一帧里寻找多组 (...)，适合 (D,2500)(T,235950)。
 * 3. 连续命令模式主要用于“连续配置参数”；如果题目要求连续查询，建议再扩展发送队列或拼接总回复。
 *
 * 当前示例命令：
 * ?             -> 回复 OK\r\n
 * (T?)          -> 查询时间，回复 (T,HHMMSS)\r\n
 * (V?)          -> 查询电位器和光敏电压，回复 (V,x.xx,x.xx)\r\n
 * (F?)          -> 查询 NE555 频率，回复 (F,1234)\r\n
 * (D,2500)      -> 设置 DAC 目标输出 2500mV，范围 0~5000
 * (T,235950)    -> 设置时间为 23:59:50
 ******************************************************************************/
void Uart_Proc(void)
{
    unsigned char rx_len;        // 本帧接收到的有效长度，去掉末尾 \r\n 后再解析

#if UART_MULTI_CMD_ENABLE
    unsigned char pos;           // 扫描 RX1_Buffer 的当前位置
    unsigned char start;         // 当前命令左括号 '(' 的位置
    unsigned char end;           // 当前命令右括号 ')' 的位置
    bit has_cmd = 0;             // 是否至少找到一条完整命令
    bit all_ok = 1;              // 连续命令全部成功才统一回复 OK
#endif

    if(B_RX1_flag == 0) return;  // 没有完整一帧时直接返回，主循环继续做其他任务
    B_RX1_flag = 0;              // 先清标志，防止同一帧被重复解析

    rx_len = B_RX1_Con;
    while(rx_len > 0 && (RX1_Buffer[rx_len-1] == '\r' || RX1_Buffer[rx_len-1] == '\n'))
    {
        rx_len--;                // 兼容串口助手自动添加的回车换行
    }
    RX1_Buffer[rx_len] = '\0';   // 让接收内容成为 C 字符串，便于后续扩展

#if UART_MULTI_CMD_ENABLE
    /* 连续命令备用解析：
     * 例如收到 (D,2500)(T,235950)，这里会先找到第一对括号，再找到第二对括号。
     * 每找到一条完整命令，就交给 Uart_ParseOneCommand() 执行。
     * reply_enable 传 0，是为了避免每条命令都立刻发送 OK，导致串口发送忙时后面的回复丢失。
     */
    pos = 0;
    while(pos < rx_len)
    {
        if(RX1_Buffer[pos] != '(')      // 连续命令必须紧挨着写成 (...)(...)，中间混入无关字符就报错
        {
            all_ok = 0;
            break;
        }

        start = pos;
        while(pos < rx_len && RX1_Buffer[pos] != ')') pos++;
        if(pos >= rx_len)              // 找到 '(' 但没找到 ')'，说明格式不完整
        {
            all_ok = 0;
            break;
        }

        end = pos;
        has_cmd = 1;
        if(Uart_ParseOneCommand(start,end,0) == 0) all_ok = 0;
        pos++;                         // 跳过当前 ')'，继续找下一条命令
    }

    if(has_cmd && all_ok) Uart_SendString("OK\r\n");
    else Uart_SendString("ERROR\r\n");
#else
    /* 普通单命令解析：
     * 这是模板默认模式，逻辑最直观，查询命令也能立即回复。
     * 后续如果题目明确要求连续配置命令，只需要把 UART_MULTI_CMD_ENABLE 改为 1。
     */
    if(rx_len > 0) Uart_ParseOneCommand(0,rx_len-1,1);
    else Uart_SendString("ERROR\r\n");
#endif

    Uart_ClearRxBuffer();              // 一帧处理完后清空接收区，准备下一帧
}


/* 主动异常上报。
 * 用法：Logic_Proc() 判断到异常后，只需要设置 uart_report_code 和 uart_report_flag。
 * 本函数放在主循环中执行，串口空闲时才真正发送，避免打断正在回复的查询命令。
 * 默认格式：[错误码,HHMMSS]\r\n，例如 [500,123456]。
 * 如果题目要求携带更多数据，可以继续在这里追加字段，例如频率、液位、状态位等。
 */
void Uart_Report_Proc(void)
{
    unsigned char tx_i = 0;

    if(uart_report_flag == 0) return;   // 没有上报请求，直接返回
    if(B_TX1_busy != 255) return;       // 串口正在发送时先等待，下一轮主循环再试

    uart_report_flag = 0;               // 先清标志，避免同一条异常被重复发送

    Uart_TxClear();
    Uart_TxPutChar(&tx_i,'[');
    Uart_TxPutUInt(&tx_i,uart_report_code);
    Uart_TxPutChar(&tx_i,',');
    Uart_TxPut2Digit(&tx_i,rtc[0]);
    Uart_TxPut2Digit(&tx_i,rtc[1]);
    Uart_TxPut2Digit(&tx_i,rtc[2]);
    Uart_TxPutString(&tx_i,"]\r\n");
    Uart_TxStart();
}

/* 解析一条命令。
 * start/end 是这条命令在 RX1_Buffer 里的起止位置，普通模式通常是 0 和 rx_len-1。
 * reply_enable = 1：单命令模式，函数内部直接回复 OK/ERROR/查询结果。
 * reply_enable = 0：连续命令模式，只执行配置并返回成功/失败，最后由 Uart_Proc() 统一回复一次。
 * 返回值：1 表示命令格式正确且执行成功；0 表示命令不认识或参数非法。
 */
bit Uart_ParseOneCommand(unsigned char start,unsigned char end,unsigned char reply_enable)
{
    unsigned char tx_i = 0;      // 发送缓冲区下标，组包时从 TX1_Buffer[0] 开始写
    unsigned char len;           // 当前命令长度，方便判断固定格式
    unsigned int temp_value;      // 临时保存解析出的数字参数，例如 DAC 的 2500

    if(end < start) return 0;
    len = end - start + 1;

    // 通信测试命令：串口助手发送 ?，单片机回复 OK。
    if(len == 1 && RX1_Buffer[start] == '?')
    {
        if(reply_enable) Uart_SendString("OK\r\n");
        return reply_enable ? 1 : 0;   // 连续模式只支持括号配置命令，避免无回复查询被误认为成功
    }
    // 查询时间命令：(T?)。回复格式固定为 (T,HHMMSS)。
    else if(len == 4 && RX1_Buffer[start] == '(' && RX1_Buffer[start+1] == 'T' && RX1_Buffer[start+2] == '?' && RX1_Buffer[start+3] == ')')
    {
        if(reply_enable)
        {
            Uart_TxClear();                // 清空发送缓冲区，准备重新组包
            Uart_TxPutString(&tx_i,"(T,"); // 以下几行依次拼出回复字符串\r\nUart_TxPut2Digit(&tx_i,rtc[0]);
            Uart_TxPut2Digit(&tx_i,rtc[1]);
            Uart_TxPut2Digit(&tx_i,rtc[2]);
            Uart_TxPutString(&tx_i,")\r\n");
            Uart_TxStart();                // 启动中断发送
            return 1;
        }
        return 0;
    }
    // 查询电压命令：(V?)。回复电位器电压和光敏电压，单位 V，格式 x.xx。
    else if(len == 4 && RX1_Buffer[start] == '(' && RX1_Buffer[start+1] == 'V' && RX1_Buffer[start+2] == '?' && RX1_Buffer[start+3] == ')')
    {
        if(reply_enable)
        {
            Uart_TxClear();
            Uart_TxPutString(&tx_i,"(V,");
            Uart_TxPutVoltage(&tx_i,pot_mv);
            Uart_TxPutChar(&tx_i,',');
            Uart_TxPutVoltage(&tx_i,light_mv);
            Uart_TxPutString(&tx_i,")\r\n");
            Uart_TxStart();
            return 1;
        }
        return 0;
    }
    // 查询频率命令：(F?)。回复 NE555 当前频率，整数形式。
    else if(len == 4 && RX1_Buffer[start] == '(' && RX1_Buffer[start+1] == 'F' && RX1_Buffer[start+2] == '?' && RX1_Buffer[start+3] == ')')
    {
        if(reply_enable)
        {
            Uart_TxClear();
            Uart_TxPutString(&tx_i,"(F,");
            Uart_TxPutUInt(&tx_i,ne555);
            Uart_TxPutString(&tx_i,")\r\n");
            Uart_TxStart();
            return 1;
        }
        return 0;
    }
    // 设置 DAC 命令：(D,2500)。这里只接收 4 位数字，所以范围写成 0000~5000。
    else if(len == 8 && RX1_Buffer[start] == '(' && RX1_Buffer[start+1] == 'D' && RX1_Buffer[start+2] == ',' && RX1_Buffer[start+7] == ')')
    {
        if(Uart_Parse4Digit(start+3,&temp_value) && temp_value <= 5000)
        {
            dac_mv = temp_value;        // Task_Proc() 会把 dac_mv 换算为 adc[4] 并输出到 DAC
            if(reply_enable) Uart_SendString("OK\r\n");
            return 1;
        }
        else
        {
            if(reply_enable) Uart_SendString("ERROR\r\n");
            return 0;
        }
    }
    // 设置时间命令：(T,235950)。依次表示 HH MM SS。
    else if(len == 10 && RX1_Buffer[start] == '(' && RX1_Buffer[start+1] == 'T' && RX1_Buffer[start+2] == ',' && RX1_Buffer[start+9] == ')')
    {
        if(RX1_Buffer[start+3]>='0' && RX1_Buffer[start+3]<='9' && RX1_Buffer[start+4]>='0' && RX1_Buffer[start+4]<='9' &&
           RX1_Buffer[start+5]>='0' && RX1_Buffer[start+5]<='9' && RX1_Buffer[start+6]>='0' && RX1_Buffer[start+6]<='9' &&
           RX1_Buffer[start+7]>='0' && RX1_Buffer[start+7]<='9' && RX1_Buffer[start+8]>='0' && RX1_Buffer[start+8]<='9')
        {
            rtc[0] = (RX1_Buffer[start+3]-'0')*10 + (RX1_Buffer[start+4]-'0');
            rtc[1] = (RX1_Buffer[start+5]-'0')*10 + (RX1_Buffer[start+6]-'0');
            rtc[2] = (RX1_Buffer[start+7]-'0')*10 + (RX1_Buffer[start+8]-'0');
            if(rtc[0] < 24 && rtc[1] < 60 && rtc[2] < 60)
            {
                Set_RTC(rtc);           // 合法才真正写入 DS1302
                if(reply_enable) Uart_SendString("OK\r\n");
                return 1;
            }
        }

        if(reply_enable) Uart_SendString("ERROR\r\n");
        return 0;
    }

    if(reply_enable) Uart_SendString("ERROR\r\n"); // 命令格式不认识，统一回复 ERROR
    return 0;
}

/* 发送固定字符串。适合回复 "OK\r\n"、"ERROR\r\n" 这种不需要拼变量的内容。 */
void Uart_SendString(char *str)
{
    unsigned char i = 0;

    if(B_TX1_busy != 255) return;       // 上一帧还没发完，本帧放弃；需要排队时可自行扩展
    Uart_TxClear();
    while(str[i] != '\0' && i < TX1_LENGTH-1)
    {
        TX1_Buffer[i] = str[i];
        i++;
    }
    TX1_Buffer[i] = '\0';              // 结尾补 0，串口中断靠它判断发送结束
    Uart_TxStart();
}

/* 清空接收缓冲区。每处理完一帧都要调用，否则下一帧可能和旧内容混在一起。 */
void Uart_ClearRxBuffer(void)
{
    unsigned char i;
    for(i=0;i<RX1_LENGTH;i++) RX1_Buffer[i] = 0;
    B_RX1_Con = 0;                      // 接收下标归零
    UART_Idle_Task = 0;                 // 空闲计时也归零
}

/* 清空发送缓冲区。手动组包前先调用，避免残留上一次发送内容。 */
void Uart_TxClear(void)
{
    unsigned char i;
    for(i=0;i<TX1_LENGTH;i++) TX1_Buffer[i] = 0;
}

/* 启动发送。只需要写第一个字节到 SBUF，后续字节由 TI 中断自动发送。 */
void Uart_TxStart(void)
{
    if(B_TX1_busy != 255) return;       // 串口忙时不打断当前发送
    if(TX1_Buffer[0] == '\0') return;   // 空字符串不发送
    SBUF = TX1_Buffer[0];
    B_TX1_busy = 1;                     // 下次 TI 中断从 TX1_Buffer[1] 开始发
}

/* 向 TX1_Buffer 追加 1 个字符，idx 会自动后移，并始终补 \0 作为字符串结束。 */
void Uart_TxPutChar(unsigned char *idx,char dat)
{
    if(*idx < TX1_LENGTH-1)
    {
        TX1_Buffer[*idx] = dat;
        (*idx)++;
        TX1_Buffer[*idx] = '\0';        // 保持缓冲区随时都是一个合法字符串
    }
}

/* 向 TX1_Buffer 追加一个字符串，例如 "(T," 或 ")\r\n"。 */
void Uart_TxPutString(unsigned char *idx,char *str)
{
    while(*str != '\0')
    {
        Uart_TxPutChar(idx,*str);
        str++;
    }
}

/* 追加两位十进制数字。常用于时间：3 会被发送成 03。 */
void Uart_TxPut2Digit(unsigned char *idx,unsigned char dat)
{
    Uart_TxPutChar(idx,dat/10+'0');
    Uart_TxPutChar(idx,dat%10+'0');
}

/* 追加 unsigned int，不带前导 0。常用于频率、计数值、阈值。 */
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

/* 追加电压字符串，输入单位 mV，输出格式 x.xx，例如 3260 -> 3.26。 */
void Uart_TxPutVoltage(unsigned char *idx,unsigned int mv)
{
    if(mv > 5000) mv = 5000;
    Uart_TxPutChar(idx,mv/1000+'0');
    Uart_TxPutChar(idx,'.');
    Uart_TxPutChar(idx,mv/100%10+'0');
    Uart_TxPutChar(idx,mv/10%10+'0');
}

/* 从 RX1_Buffer[start] 开始解析 4 位数字，成功返回 1，失败返回 0。 */
bit Uart_Parse4Digit(unsigned char start,unsigned int *value)
{
    unsigned char i;
    unsigned int v = 0;

    for(i=0;i<4;i++)
    {
        if(RX1_Buffer[start+i] < '0' || RX1_Buffer[start+i] > '9') return 0;
        v = v*10 + (RX1_Buffer[start+i]-'0');
    }
    *value = v;
    return 1;
}
/********************************* 工具函数 ************************************/
long int DS18B20_ToIntC(unsigned int raw)
{
    if(raw & 0xF000)
    {
        if(((raw ^ 0xFFFF) & 0x000F) == 0x000F)
            return -((long int)((raw ^ 0xFFFF) >> 4)) - 1;
        else
            return -((long int)((raw ^ 0xFFFF) >> 4));
    }
    return (long int)(raw >> 4);
}

/********************************* 键盘扫描 ************************************
 * 当前保留蓝桥杯常用矩阵键盘部分按键：S4/S5/S8/S9/S12/S13。
 * 如果题目需要更多按键，取消下面注释并补全对应行。
 ******************************************************************************/
unsigned char Key_Read(void)
{
    P44=0;P42=1;P35=1;//P34=1;
    P33=1;P32=1;//P30=1;P31=1;
    if(!P33)return 4;
    else if(!P32)return 5;
//    else if(!P31)return 6;
//    else if(!P30)return 7;
    P44=1;P42=0;
    if(!P33)return 8;
    else if(!P32)return 9;
//    else if(!P31)return 10;
//    else if(!P30)return 11;
    P42=1;P35=0;
    if(!P33)return 12;
    else if(!P32)return 13;
//    else if(!P31)return 14;
//    else if(!P30)return 15;
//    P35=1;P34=0;
//    if(!P33)return 16;
//    else if(!P32)return 17;
//    else if(!P31)return 18;
//    else if(!P30)return 19;
    return 0;
}

/********************************* 定时器初始化 ********************************/
void Timer0Init(void)
{
    AUXR |= 0x80;            // T0 1T 模式
    TMOD &= 0xF0;
    TMOD |= 0x04;            // T0 计数器模式，NE555 输入接 T0/P3.4
    TL0 = 0;
    TH0 = 0;
    TF0 = 0;
    TR0 = 1;
}

void Timer1Init(void)
{
    AUXR &= 0xBF;            // T1 12T 模式，用于超声波回波计时
    TMOD &= 0x0F;
    TL1 = 0x00;
    TH1 = 0x00;
    TF1 = 0;
    TR1 = 0;
    PCA_TIME_init();
}

void PCA_TIME_init(void)
{
    P_SW1 &= 0xCF;           // PCA 使用默认引脚
    CCAPM0 = 0x11;           // CCF0：超声波捕获
    PPCA = 1;                // 提高 PCA 中断优先级，保护显示刷新实时性
    CMOD = 0x00;             // PCA 时钟 Fosc/12
    CCON = 0x00;
    CH = 0;
    CL = 0;

    CCAP1L = TIME_1;
    CCAP1H = TIME_1 >> 8;
    CCAPM1 = 0x49;           // CCF1：比较匹配 + 中断，1ms 系统节拍
    TIME_1 += TIME_CCAP1;

    CCAP2L = TIME_2;
    CCAP2H = TIME_2 >> 8;
    CCAPM2 = 0x49;           // CCF2：比较匹配 + 中断，显示刷新
    TIME_2 += TIME_CCAP2;

    CR = 1;
}

/********************************* 串口中断 ************************************/
void uart_0(void) interrupt 4
{
    if(RI)
    {
        RI = 0;
        if(B_RX1_Con < RX1_LENGTH-1)
        {
            RX1_Buffer[B_RX1_Con] = SBUF;
            B_RX1_Con++;
            UART_Idle_Task = 0;           // 收到新字节后重新计算空闲时间
        }
    }

    if(TI)
    {
        TI = 0;
        if(B_TX1_busy != 255 && TX1_Buffer[B_TX1_busy] != '\0')
        {
            SBUF = TX1_Buffer[B_TX1_busy]; // 发送下一个字节
            B_TX1_busy++;
        }
        else
        {
            B_TX1_busy = 255;             // 发送结束，恢复空闲状态
        }
    }
}

/********************************* PCA 中断 ************************************/
void CCP_IRQHandler(void) interrupt 7
{
    unsigned int t;

    if(CCF0)                 // 超声波回波捕获
    {
        CCF0 = 0;
        if(RX_Wait_FLAG)
        {
            TR1 = 0;
            if(TF1)
            {
                TF1 = 0;
                wave = 999;
            }
            else
            {
                t = ((unsigned int)TH1 << 8) | TL1;
                wave = (unsigned int)(t * 0.017); // 12MHz/12T 下常用近似换算，单位 cm
            }
            RX_Wait_FLAG = 0;
        }
    }

    if(CCF1)                 // 1ms 系统节拍：只计数，不执行耗时模块
    {
        CCF1 = 0;
        CCAP1L = TIME_1;
        CCAP1H = TIME_1 >> 8;
        TIME_1 += TIME_CCAP1;

        if(KEY_Task < KEY_TASK_TIME) KEY_Task++;
        if(RTC_Task < RTC_TASK_TIME) RTC_Task++;
        if(PCF8591_Task < PCF8591_TASK_TIME) PCF8591_Task++;
        if(DS18B20_Task < DS18B20_TASK_TIME) DS18B20_Task++;
        if(NE555_Task < NE555_TASK_TIME) NE555_Task++;
        if(ULTRASONIC_Task < ULTRASONIC_TASK_TIME) ULTRASONIC_Task++;
        if(Send_Task < SEND_TASK_TIME) Send_Task++;

        /* LED闪烁模板：
         * 这里只更新“闪烁状态位”，真正点亮哪一盏灯仍然放在 Led_Proc() 里决定。
         * alarm_flag 清零时同步复位，保证下次报警从灭灯状态重新开始闪烁。
         */
        if(alarm_flag)
        {
            if(LED_Blink_Task < LED_BLINK_TIME) LED_Blink_Task++;
            else
            {
                LED_Blink_Task = 0;
                led_blink_flag = !led_blink_flag;
            }
        }
        else
        {
            LED_Blink_Task = 0;
            led_blink_flag = 0;
        }

        if(B_RX1_Con != 0 && B_RX1_flag == 0)
        {
            if(UART_Idle_Task < UART_IDLE_TIME) UART_Idle_Task++;
            else B_RX1_flag = 1;
        }
    }

    if(CCF2)                 // 高实时显示刷新：LED、数码管、继电器/蜂鸣器统一在这里写硬件
    {
        CCF2 = 0;
        CCAP2L = TIME_2;
        CCAP2H = TIME_2 >> 8;
        TIME_2 += TIME_CCAP2;

        HC573_Write(4,~led);                       // LED：led bit=1 表示亮，硬件低电平点亮，所以取反
        HC573_Write(7,0xff);                       // 消隐，防止位切换时串影
        HC573_Write(6,1 << (SEG_Count % 8));       // 位选
        HC573_Write(7,Code[seg[SEG_Count % 8]]);   // 段选
        SEG_Count++;
        HC573_Write(5,b_and_r);                    // 继电器/蜂鸣器
    }
}