#pragma config FPR = XT_PLL16          // Fosc = 64 MHz -> 4MHz crystal * 16 PLL multiplier
#pragma config FOS = PRI               // Oscillator Source Primary
#pragma config WDT = WDT_OFF            // Watchdog Timer (Disabled)

#define FCY 16000000UL
#include <xc.h>
#include <string.h>
#include <stdio.h>
#include <libpic30.h>

#define DIST_MSG_ID 0x5d7
#define BUTTON_STATE (PORTDbits.RD0)
#define BUZZ_PIN (PORTDbits.RD1)

#define can_msg_available() (C1RX0CONbits.RXFUL)
#define can_capture_set_state(state) (C1CTRLbits.CANCAP = state)

#define DBG

struct CAN_MSG
{
    int sid;
    int dlc;
    unsigned char data[8];
};

void can_set_accept_mask(int sid)
{
    C1RXM0SIDbits.MIDE = 1;
    C1RXM0SIDbits.SID = 0x7ff; // Fill mask with 1
    C1RXF0SIDbits.EXIDE = 0; //only standard identifiers
    C1RXF0SIDbits.SID = sid;
    return;
}

void can_init()
{
    /* Tq = 125ns, Baud = 16 * Tq = 500KHz */
    C1CTRLbits.REQOP = 4; // configuration mode
    C1CTRLbits.CANCKS = 0; // FCAN = 4 * FCY
    C1CFG1bits.SJW = 1; // SJW = 2Tq
    C1CFG1bits.BRP = 3; // Tq = 8 / Fcan
    C1CFG2 = 0;
    // Sync segment = 1Tq, doesn't seem to be configurable
    C1CFG2bits.PRSEG = 3; // PR_SEG = 4Tq
    C1CFG2bits.SEG1PH = 5; // PH1_SEG = 6Tq
    C1CFG2bits.SEG2PHTS = 1; // PR_SEG2 value programmable
    C1CFG2bits.SEG2PH = 4; // PH2_SEG = 5Tq
    can_set_accept_mask(DIST_MSG_ID);
    C1RX0CON = 0;
    can_capture_set_state(1); //enable capture
    C1CTRLbits.REQOP = 0; // normal operation
    return;
}

void can_read(struct CAN_MSG *can_msg)
{
    C1RX0CONbits.RXFUL = 0; // sw clear bit necessary
    can_msg->sid = C1RX0SIDbits.SID;
    can_msg->dlc = C1RX0DLCbits.DLC;
    can_msg->data[0] = C1RX0B1;
    can_msg->data[1] = C1RX0B1 >> 8;
    can_msg->data[2] = C1RX0B2;
    can_msg->data[3] = C1RX0B2 >> 8;
    can_msg->data[4] = C1RX0B3;
    can_msg->data[5] = C1RX0B3 >> 8;
    can_msg->data[6] = C1RX0B4;
    can_msg->data[7] = C1RX0B4 >> 8;
    return;
}

void uart_send_str(char *str)
{
    while(*str != 0)
    {
        while(U1STAbits.TRMT == 0);
        U1TXREG = *str++;
    }
}

void uart_init(void)
{
	U1MODE = 0;
	U1STA = 0;
	U1BRG = 8; //Baudrate 115200
	U1MODEbits.ALTIO = 1;
	U1MODEbits.UARTEN = 1;
	U1STAbits.UTXEN = 1;
	U1STAbits.FERR = 0;
	U1STAbits.OERR = 0;
	U1STAbits.URXDA = 0;
	return;
}

char * byte_to_hex_ascii(char byte)
{
    static char hex_str[4];
    char x;
    x = (byte >> 4) & 0x0f;  
    if(x > 9)
        x += 55;
    else
        x += 48;
    hex_str[0] = x;
    x = byte & 0x0f;  
    if(x > 9)
        x += 55;
    else
        x += 48;
    hex_str[1] = x;
    hex_str[2] = ' ';
    hex_str[3] = 0;
    return hex_str;
}

char __attribute__((space(eedata), aligned(_EE_ROW))) dat[_EE_ROW];

void eeprom_write_dist(unsigned long dist)
{
  int source[_EE_ROW];
  _prog_addressT p = 0x7FFC00;
  source[0] = dist >> 8;
  source[1] = dist & 0xff;
  
  _init_prog_address(p, dat);/* get address in program space */

  _erase_eedata(p, _EE_ROW);  /* erase a row */

  _wait_eedata();   /* wait for operation to complete */
  _write_eedata_row(p, source);/* write a row */
  _wait_eedata();   /* wait for operation to complete */
}


unsigned long eeprom_read_dist(void)
{
    int read_val[2] = {0};
    unsigned long read_dist = 0;
    TBLPAG = 0x7f;
    asm("mov #0xfc00, w0");
    asm("tblrdl [w0], w1");
    read_val[0] = WREG1;
    asm("mov #0xfc02, w0");
    asm("tblrdl [w0], w1");
    read_val[1] = WREG1;
    read_dist = (unsigned long) read_val[0] << 8;
    read_dist |= read_val[1];
    read_dist &= 0x00ffffff;
    return read_dist;
    
}

void main(void) {
    struct CAN_MSG can_msg;
    int i = 0, bttn_press_count = 0, buzzed = 0;
    unsigned long travelled_dist = 0xab000000;
    unsigned long stored_dist = 0;
    memset(&can_msg, 0, sizeof(can_msg));
    TRISDbits.TRISD1 = 0;
    TRISDbits.TRISD0 = 1;
    uart_init();
    can_init();

    stored_dist = eeprom_read_dist();
#ifdef DBG
    char stored_str[10] = {0};
    uart_send_str("Stored dist: ");
    sprintf(stored_str, "%lu\n\r", stored_dist);
    uart_send_str(stored_str);
#endif
    while(1)
    {
        if(can_msg_available())
        {
            memset(&can_msg, 0, sizeof(can_msg));
            can_read(&can_msg);
#ifdef DBG
            char id_str[8] = {0};
            sprintf(id_str, "[%03X]: ", can_msg.sid);
            uart_send_str(id_str);
            for(i = 0; i < 8; i++)
            {
                char can_str[4] = {0};
                memcpy(can_str, byte_to_hex_ascii(can_msg.data[i]), 3);
                uart_send_str(can_str);
            }
            uart_send_str("\n\r");
#endif
            travelled_dist = (unsigned long) can_msg.data[2] << 24;
            travelled_dist |= (unsigned long) can_msg.data[3] << 16;
            travelled_dist |= (unsigned long) can_msg.data[4] << 8;
            travelled_dist |= can_msg.data[5];
            travelled_dist = (travelled_dist >> 4) / 100;
            
            unsigned long dist_diff = travelled_dist - stored_dist;
            if(dist_diff >= 1000 && dist_diff < 2000)
            {
                
            }
            else if(dist_diff >= 2000 && dist_diff < 3000)
            {
                
            }
            else if(dist_diff >= 3000 && dist_diff < 4000)
            {
                
            }
            else if(dist_diff >= 4000 && dist_diff < 5000)
            {
                
            }
            else if(dist_diff >= 5000 && dist_diff < 6000)
            {
                
            }
            else if(dist_diff >= 6000 && dist_diff < 7000)
            {
                
            }
            else if(dist_diff >= 7000 && dist_diff < 8000)
            {
                
            }
            else if(dist_diff >= 8000 && dist_diff < 9000)
            {
                
            }
            else if(dist_diff >= 9000)
            {
                if(buzzed == 0) /* buzz only at first start*/
                {
                    can_capture_set_state(0);
                    for(i = 0; i < 1000; i++) /* 2 seconds 500Hz buzz */
                    {
                        BUZZ_PIN = 1;
                        __delay_ms(1)
                        BUZZ_PIN = 0;
                        __delay_ms(1);
                    }
                    /* buzz buzzer to notify user */
                    buzzed++;
                    can_capture_set_state(1);
                }
            }
#ifdef DBG
            char travel_str[10] = {0};
            uart_send_str("Travelled dist: ");
            sprintf(travel_str, "%lu\n\r", travelled_dist);
            uart_send_str(travel_str);
            memset(travel_str, 0, 10);
            uart_send_str("Dist diff: ");
            sprintf(travel_str, "%lu\n\r", dist_diff);
            uart_send_str(travel_str);
#endif
        }
        
        if(BUTTON_STATE == 0)
            bttn_press_count++;
        else
            bttn_press_count = 0;
        
        if(bttn_press_count == 300)
        {
            /* button was pressed for >3s, store the new distance in memory */
            /* light up some LEDs to notify user */
            if(travelled_dist != 0xab000000)
            {
#ifdef DBG
                uart_send_str("Storing in EEPROM\n\r");
#endif
                eeprom_write_dist(travelled_dist);
                stored_dist = eeprom_read_dist();
            }
            bttn_press_count = 0;
        } 
        __delay_ms(10);
    }
}
