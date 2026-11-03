/*******************************************
 * ESP-01S 连 WiFi 测试（E:\javatest\wifi\蓝桥杯模板\main.c）
 * 固定 115200 波特率（已验证模块出厂 115200）
 * 上电自动执行：AT -> CWMODE=1 -> CWJAP 连热点
 *
 * 数码管显示：
 *   1111  = 正在连接（AT/CWMODE/CWJAP 阶段）
 *   8888  = 连上 WiFi 拿到 IP 了！
 *   0000  = 连接失败
 *******************************************/

#include "STC15F2K60S2.h"


/* 共阳数码管段码 0-F */
unsigned char code Code[16] = {
    0xC0,0xF9,0xA4,0xB0,0x99,0x92,0x82,0xF8,
    0x80,0x90,0x88,0x83,0xC6,0xA1,0x86,0x8E
};
#define BLANK 0x0A

/* 热点配置 */
#define SSID  "Xiaomi 15"
#define PWD   "machunjie"

unsigned char seg[8] = {BLANK,BLANK,BLANK,BLANK,BLANK,BLANK,BLANK,BLANK};
unsigned char seg_cnt = 0;
unsigned int  tick_ms = 0;

/* 状态机 */
unsigned char state = 0;        /* 0=发AT 1=等OK 2=发CWMODE 3=发CWJAP 4=已连接 5=失败 */
unsigned char b_and_r = 0x00;  /* bit4=继电器 bit6=蜂鸣器 */
unsigned int  state_timer = 0;   /* 当前状态计时 */
bit           got_ok = 0;        /* 收到 OK */
bit           got_ip = 0;        /* 收到 WIFI GOT IP */
unsigned char rx_buf[64];
unsigned char rx_len = 0;

/********************************* HC573 锁存 *********************************/
void HC573_Write(unsigned char channel, unsigned char dat)
{
    P2 &= 0x1f;
    P0 = dat;
    switch(channel)
    {
        case 4: P2 |= 0x80; break;
        case 5: P2 |= 0xa0; break;
        case 6: P2 |= 0xc0; break;
        case 7: P2 |= 0xe0; break;
        default: break;
    }
}

/********************************* 串口（固定 115200） *********************************/
void Uart_Init(void)
{
    SCON = 0x50;
    AUXR |= 0x01;
    AUXR |= 0x04;
    T2H = 0xFF; T2L = 0xE8;      /* 115200 @11.0592MHz */
    AUXR |= 0x10;
    ES = 1;
    EA = 1;
}

void Uart_SendChar(unsigned char c)
{
    SBUF = c;
    while(TI == 0);
    TI = 0;
}

void Uart_SendStr(char *s)
{
    while(*s) Uart_SendChar(*s++);
}

/* 串口中断：收字节进缓冲 */
void Uart_ISR(void) interrupt 4
{
    unsigned char c;
    if(RI)
    {
        RI = 0;
        c = SBUF;
        if(rx_len < 63) rx_buf[rx_len++] = c;
    }
}

/* 在 rx_buf 里找子串 */
bit FindInBuf(char *sub)
{
    unsigned char i, j;
    for(i = 0; i + 2 < rx_len; i++)
    {
        for(j = 0; sub[j] != 0; j++)
        {
            if(rx_buf[i+j] != sub[j]) break;
        }
        if(sub[j] == 0) return 1;
    }
    return 0;
}

/********************************* Timer0：1ms 数码管扫描 *********************************/
void Timer0_Init(void)
{
    TMOD &= 0xF0;
    TH0 = 0xFC; TL0 = 0x66;      /* 1ms @11.0592MHz 12T */
    ET0 = 1;
    TR0 = 1;
    EA = 1;
}

void Timer0_ISR(void) interrupt 1
{
    TH0 = 0xFC; TL0 = 0x66;
    tick_ms++;
    state_timer++;

    HC573_Write(7, 0xFF);
    HC573_Write(6, 1 << seg_cnt);
    HC573_Write(7, Code[seg[seg_cnt]]);
    seg_cnt = (seg_cnt + 1) & 0x07;
}

/********************************* 主函数 *********************************/
void main(void)
{
    HC573_Write(5, 0x00);
    HC573_Write(4, 0xFF);

    Timer0_Init();
    Uart_Init();

    /* 显示：1111 连接中 */
    seg[0] = seg[1] = seg[2] = seg[3] = 1;

    while(1)
    {
        /* ---- 状态机 ---- */
        /* ---- 下行命令处理（所有状态都处理） ---- */
        if(FindInBuf("R1=1")) { b_and_r |= 0x10;  P2 &= 0x1f; P0 = b_and_r; P2 |= 0xa0; rx_len = 0; }
        if(FindInBuf("R1=0")) { b_and_r &= ~0x10; P2 &= 0x1f; P0 = b_and_r; P2 |= 0xa0; rx_len = 0; }
        if(FindInBuf("B1=1")) { b_and_r |= 0x40;  P2 &= 0x1f; P0 = b_and_r; P2 |= 0xa0; rx_len = 0; }
        if(FindInBuf("B1=0")) { b_and_r &= ~0x40; P2 &= 0x1f; P0 = b_and_r; P2 |= 0xa0; rx_len = 0; }

        switch(state)
        {
            case 0:   /* 上电 1 秒后发 AT */
                if(state_timer >= 1000)
                {
                    state_timer = 0;
                    rx_len = 0;
                    Uart_SendStr("AT\r\n");
                    state = 1;
                }
                break;

            case 1:   /* 等 OK（3 秒超时） */
                if(FindInBuf("OK"))
                {
                    state_timer = 0;
                    rx_len = 0;
                    Uart_SendStr("AT+CWMODE=1\r\n");
                    state = 2;
                }
                else if(state_timer >= 3000)
                {
                    state = 5;   /* 失败 */
                }
                break;

            case 2:   /* 等 CWMODE 的 OK，发 CWJAP */
                if(FindInBuf("OK"))
                {
                    state_timer = 0;
                    rx_len = 0;
                    Uart_SendStr("AT+CWJAP=\"" SSID "\",\"" PWD "\"\r\n");
                    state = 3;
                }
                else if(state_timer >= 3000)
                {
                    state = 5;
                }
                break;

            case 3:   /* 等 WIFI GOT IP（15 秒超时） */
                if(FindInBuf("WIFI GOT IP"))
                {
                    state = 4;   /* 成功 */
                }
                else if(state_timer >= 15000)
                {
                    state = 5;
                }
                break;

            case 4:   /* WiFi 已连上，发 CIPMUX */
                seg[0] = seg[1] = seg[2] = seg[3] = 8;
                if(state_timer >= 500)
                {
                    state_timer = 0;
                    rx_len = 0;
                    Uart_SendStr("AT+CIPMUX=0\r\n");
                    state = 9;
                }
                break;

            case 9:   /* 等 CIPMUX OK，直接发 CIPSTART（普通模式） */
                if(FindInBuf("OK"))
                {
                    state_timer = 0;
                    rx_len = 0;
                    Uart_SendStr("AT+CIPSTART=\"TCP\",\"172.16.49.111\",8080\r\n");
                    state = 7;
                }
                else if(state_timer >= 3000) { state = 8; }
                break;

            case 7:   /* 等 CIPSTART 连上 "CONNECT" */
                if(FindInBuf("CONNECT"))
                {
                    state = 5;
                }
                else if(state_timer >= 8000) { state = 8; }
                break;

            case 5:   /* TCP 已连，等 5 秒发 CIPSEND */
                seg[0] = seg[1] = seg[2] = seg[3] = 7;
                if(state_timer >= 5000)
                {
                    state_timer = 0;
                    rx_len = 0;
                    Uart_SendStr("AT+CIPSEND=7\r\n");
                    state = 10;
                }
                break;

            case 10:  /* 等 1 秒直接发 hello（不等 >） */
                if(state_timer >= 1000)
                {
                    rx_len = 0;
                    Uart_SendStr("hello\r\n");
                    state = 11;
                }
                break;

            case 11:  /* 等 "SEND OK"，回 state 5 等下一轮 */
                if(FindInBuf("SEND OK"))
                {
                    state_timer = 0;
                    rx_len = 0;
                    state = 5;
                }
                else if(state_timer >= 3000) { state = 8; }
                break;

            case 12:  /* 调试：显示 rx_len 的十六进制（00xx） */
                seg[0] = 0;
                seg[1] = 0;
                seg[2] = (rx_len >> 4) & 0x0F;
                seg[3] = rx_len & 0x0F;
                break;

            case 8:   /* 失败 */
                seg[0] = seg[1] = seg[2] = seg[3] = 0;
                break;
        }
    }
}
