/******************************************************************************
 * Copyright 2013-2014 Espressif Systems (Wuxi)
 *
 * FileName: uart.c
 *
 * Description: Two UART mode configration and interrupt handler.
 *              Check your hardware connection while use this mode.
 *
 * Modification history:
 *     2014/3/12, v1.0 create this file.
*******************************************************************************/
#include <c_types.h>
#include <ets_sys.h>
#include <osapi.h>
#include "uart.h"
//#include <string.h>
#include <mem.h>

#define UART0   0
#define UART1   1

// UartDev is defined and initialized in rom code.
extern UartDevice UartDev;

void uart0_tx_buffer(uint8 *buf, uint16 len);

LOCAL void uart0_rx_intr_handler(void *para);

/******************************************************************************
 * FunctionName : uart_config
 * Description  : Internal used function
 *                UART0 used for data TX/RX, RX buffer size is 0x100, interrupt enabled
 *                UART1 just used for debug output
 * Parameters   : uart_no, use UART0 or UART1 defined ahead
 * Returns      : NONE
*******************************************************************************/
LOCAL void ICACHE_FLASH_ATTR uart_config(uint8 uart_no) {
    if (uart_no == UART1) {
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_U1TXD_BK);
    } else {
        /* rcv_buff size if 0x100 */
        ETS_UART_INTR_ATTACH(uart0_rx_intr_handler,  &(UartDev.rcv_buff));
        PIN_PULLUP_DIS(PERIPHS_IO_MUX_U0TXD_U);
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD);
    }

    uart_div_modify(uart_no, UART_CLK_FREQ / (UartDev.baut_rate));

    WRITE_PERI_REG(UART_CONF0(uart_no),    UartDev.exist_parity
                   | UartDev.parity
                   | (UartDev.stop_bits << UART_STOP_BIT_NUM_S)
                   | (UartDev.data_bits << UART_BIT_NUM_S));


    //clear rx and tx fifo,not ready
    SET_PERI_REG_MASK(UART_CONF0(uart_no), UART_RXFIFO_RST | UART_TXFIFO_RST);
    CLEAR_PERI_REG_MASK(UART_CONF0(uart_no), UART_RXFIFO_RST | UART_TXFIFO_RST);

    //set rx fifo trigger
    WRITE_PERI_REG(UART_CONF1(uart_no), (UartDev.rcv_buff.TrigLvl & UART_RXFIFO_FULL_THRHD) << UART_RXFIFO_FULL_THRHD_S);

    //clear all interrupt
    WRITE_PERI_REG(UART_INT_CLR(uart_no), 0xffff);
    //enable rx_interrupt
    SET_PERI_REG_MASK(UART_INT_ENA(uart_no), UART_RXFIFO_FULL_INT_ENA);
}

/******************************************************************************
 * FunctionName : uart0_tx_one_char
 * Description  : Internal used function
 *                Use uart1 interface to transfer one char
 * Parameters   : uint8 TxChar - character to tx
 * Returns      : OK
*******************************************************************************/
LOCAL STATUS ICACHE_FLASH_ATTR uart0_tx_one_char(uint8 TxChar) {
    while (true) {
		uint32 fifo_cnt = READ_PERI_REG(UART_STATUS(UART0)) & (UART_TXFIFO_CNT<<UART_TXFIFO_CNT_S);
		if ((fifo_cnt >> UART_TXFIFO_CNT_S & UART_TXFIFO_CNT) < 126) {
			break;
		}
	}

	WRITE_PERI_REG(UART_FIFO(UART0) , TxChar);
	return OK;
}

/******************************************************************************
 * FunctionName : uart0_write_char
 * Description  : Internal used function
 *                Do some special deal while tx char is '\r' or '\n'
 * Parameters   : char c - character to tx
 * Returns      : NONE
*******************************************************************************/
LOCAL void ICACHE_FLASH_ATTR uart0_write_char(char c) {
    if (c == '\n') {
        uart0_tx_one_char('\r');
        uart0_tx_one_char('\n');
    } else if (c == '\r') {
    } else {
        uart0_tx_one_char(c);
    }
}

/******************************************************************************
 * FunctionName : uart0_rx_intr_handler
 * Description  : Internal used function
 *                UART0 interrupt handler, add self handle code inside
 * Parameters   : void *para - point to ETS_UART_INTR_ATTACH's arg
 * Returns      : NONE
*******************************************************************************/
extern void ProcessCommand(char* str);
LOCAL void uart0_rx_intr_handler(void *para) {
    /* uart0 and uart1 intr combine togther, when interrupt occur, see reg 0x3ff20020, bit2, bit0 represents
     * uart1 and uart0 respectively
     */
    RcvMsgBuff *pRxBuff = (RcvMsgBuff *)para;
    uint8 RcvChar;

    if (UART_RXFIFO_FULL_INT_ST != (READ_PERI_REG(UART_INT_ST(UART0)) & UART_RXFIFO_FULL_INT_ST)) {
        return;
    }

    WRITE_PERI_REG(UART_INT_CLR(UART0), UART_RXFIFO_FULL_INT_CLR);

    while (READ_PERI_REG(UART_STATUS(UART0)) & (UART_RXFIFO_CNT << UART_RXFIFO_CNT_S)) {
        RcvChar = READ_PERI_REG(UART_FIFO(UART0)) & 0xFF;

        /* you can add your handle code below.*/

        // insert here for get one command line from uart
		uart_tx_one_char(RcvChar);
        if (RcvChar == '\r') continue;
		if (RcvChar == '\n') {

			int len;
			char *str;
			len = pRxBuff->pWritePos - pRxBuff->pReadPos;
			if (len == 0) continue;
			if (len < 0) len += RX_BUFF_SIZE;
			str = (char*)os_zalloc(len+1);
			if (str) {
				uint8 loop;
				for (loop = 0; loop < len; loop++) str[loop] = uart0_rx_one_char();
				str[len] = '\0';
				ProcessCommand(str);
				os_free(str);
			}

        } else {
			*(pRxBuff->pWritePos) = RcvChar;
			pRxBuff->pWritePos++;
		}

        // if we hit the end of the buffer, loop back to the beginning
        if (pRxBuff->pWritePos == (pRxBuff->pRcvMsgBuff + RX_BUFF_SIZE)) {
            // overflow ...we may need more error handle here.
            pRxBuff->pWritePos = pRxBuff->pRcvMsgBuff ;
        }
    }
}

ICACHE_FLASH_ATTR int uart0_rx_one_char() {
  if(UartDev.rcv_buff.pReadPos == UartDev.rcv_buff.pWritePos) return -1;
  int ret = *UartDev.rcv_buff.pReadPos;
  UartDev.rcv_buff.pReadPos++;
  if(UartDev.rcv_buff.pReadPos == (UartDev.rcv_buff.pRcvMsgBuff + RX_BUFF_SIZE)) {
    UartDev.rcv_buff.pReadPos = UartDev.rcv_buff.pRcvMsgBuff;
  }

  return ret;
}

/******************************************************************************
 * FunctionName : uart0_tx_buffer
 * Description  : use uart0 to transfer buffer
 * Parameters   : uint8 *buf - point to send buffer
 *                uint16 len - buffer len
 * Returns      :
*******************************************************************************/
void ICACHE_FLASH_ATTR uart0_tx_buffer(uint8 *buf, uint16 len) {
    uint16 i;

    for (i = 0; i < len; i++) {
        uart_tx_one_char(buf[i]);
    }
}

void ICACHE_FLASH_ATTR uart0_send(char *str) {
	uint8 i = 0;
	while (str[i]) {
        uart_tx_one_char(str[i]);
		i++;
    }
}

/******************************************************************************
 * FunctionName : uart_init
 * Description  : user interface for init uart
 * Parameters   : UartBautRate uart0_br - uart0 bautrate
 *                UartBautRate uart1_br - uart1 bautrate
 * Returns      : NONE
*******************************************************************************/
void ICACHE_FLASH_ATTR uart_init(UartBautRate uart0_br) {
    // rom use 74880 baut_rate, here reinitialize
    UartDev.baut_rate = uart0_br;
    uart_config(UART0);
    ETS_UART_INTR_ENABLE();

    // install uart0 putc callback
    os_install_putc1((void *)uart0_write_char);
//    UartDev.rcv_buff.pWritePos = UartDev.rcv_buff.pRcvMsgBuff;
//    UartDev.rcv_buff.pReadPos  = UartDev.rcv_buff.pRcvMsgBuff;
}

