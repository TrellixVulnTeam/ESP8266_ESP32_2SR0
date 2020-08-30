/////////////////////////////////////////////////////////////////////////////////////////////////////////
// Include Files                                                                                       //
/////////////////////////////////////////////////////////////////////////////////////////////////////////
//
#include <osapi.h>
#include <ets_sys.h>
#include <c_types.h>
#include <mem.h>
#include <ip_addr.h>
#include <espconn.h>
#include <errno.h>
#include <limits.h>

#include "gpio16.h"
#include "mqtt_client.h"
#include "config.h"
#include "uart.h"
#include "gtn_hpr.h"

//#define GTN_HPR_DBG

#ifdef GTN_HPR_DBG
#define DBG(format, ...) do { os_printf("%s: ", __FUNCTION__); os_printf(format, ## __VA_ARGS__); os_printf("\n"); } while(0)
#define PRINTNET(format, ...) do { if (pTXdata) { TXdatalen+=os_sprintf(pTXdata+TXdatalen, format, ## __VA_ARGS__ ); TXdatalen+=os_sprintf(pTXdata+TXdatalen, "\n");} } while(0)
#else
#define DBG(format, ...) do { } while(0)
#define PRINTNET(format, ...) do { } while(0)
#endif


// UartDev is defined and initialized in rom code.
extern UartDevice UartDev;
gtn_hpr_t * gtn_hpr_data;
unsigned int nGTN_HPRStatem;
struct espconn * pGTN_HPRConn;
sys_mutex_t RxBuf_lock;

//mosquitto_sub -v -p 5800 -h 192.168.1.6 -t 'sonoff_pow/221/+'
//mosquitto_sub -v -p 5800 -h 192.168.1.6 -t 'sonoff_th10/215/+'
//mosquitto_sub -v -p 5800 -h 192.168.1.6 -t 'esp_mains/115/+'
static void ICACHE_FLASH_ATTR uart0_rx_handler(void *para) {
	  /* uart0 and uart1 intr combine together, when interrupt occur, see reg 0x3ff20020, bit2, bit0 represents uart1 and uart0 respectively */
    RcvMsgBuff *pRxBuff = (RcvMsgBuff *)para;
    uint8 RcvChar;

    if (UART_RXFIFO_FULL_INT_ST != (READ_PERI_REG(UART_INT_ST(UART0)) & UART_RXFIFO_FULL_INT_ST)) { DBG("UARTFIFO"); return; }
    WRITE_PERI_REG(UART_INT_CLR(UART0), UART_RXFIFO_FULL_INT_CLR);

    while (READ_PERI_REG(UART_STATUS(UART0)) & (UART_RXFIFO_CNT << UART_RXFIFO_CNT_S)) {
		    RcvChar = READ_PERI_REG(UART_FIFO(UART0)) & 0xFF;
        sys_mutex_lock(&RxBuf_lock);
        *(pRxBuff->pWritePos++)=RcvChar;
        UartDev.received=pRxBuff->pWritePos - pRxBuff->pRcvMsgBuff;
        sys_mutex_unlock(&RxBuf_lock);

        switch (nGTN_HPRStatem) {
          case SM_WAITING_GTN1000_POLL:
            if ( UartDev.received == GTN1000_RX_MSG_LEN) { 
              ETS_UART_INTR_DISABLE();
              espconn_connect(pGTN_HPRConn);
              espconn_set_opt(pGTN_HPRConn, ESPCONN_REUSEADDR|ESPCONN_NODELAY);
              }
            break;            

          case SM_DISABLE_SOCKET:
            espconn_disconnect(pGTN_HPRConn);
	          ETS_UART_INTR_DISABLE();
            pGTN_HPRConn=NULL;
            msleep(10000);
            break;
              
        }
        /// boudary checks
        if ( UartDev.received > (RX_BUFF_SIZE-1) ) {
          ResetRxBuff();
          nGTN_HPRStatem=SM_WAITING_GTN1000_POLL;
          }

      }
}

void ICACHE_FLASH_ATTR ResetRxBuff() {
  sys_mutex_lock(&RxBuf_lock);
  UartDev.rcv_buff.pWritePos = UartDev.rcv_buff.pRcvMsgBuff;
  UartDev.received=0;
  //clear rx fifo
  SET_PERI_REG_MASK(UART_CONF0(UART0), UART_RXFIFO_RST);
  CLEAR_PERI_REG_MASK(UART_CONF0(UART0), UART_RXFIFO_RST);
  sys_mutex_unlock(&RxBuf_lock);
  ETS_UART_INTR_ENABLE();
}

int ICACHE_FLASH_ATTR gtn_hprInit() {
  UartDev.baut_rate 	 = 4800;
  UartDev.data_bits    = EIGHT_BITS;
  UartDev.flow_ctrl    = NONE_CTRL;
  UartDev.parity       = NONE_BITS;
  UartDev.stop_bits    = ONE_STOP_BIT;
  //UartDev.rcv_buff.TrigLvl=50;
  uart_config(uart0_rx_handler);
  ResetRxBuff();

  pGTN_HPRConn = (struct espconn *)os_zalloc(sizeof(struct espconn));  
  pGTN_HPRConn->type = ESPCONN_TCP;
  pGTN_HPRConn->state = ESPCONN_NONE;
  pGTN_HPRConn->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
  pGTN_HPRConn->proto.tcp->local_port = 5001;   // listening port
  StrToIP(DEBUG_IP_ADDRESS, (void*)&pGTN_HPRConn->proto.tcp->remote_ip);
  
  espconn_regist_connectcb(pGTN_HPRConn, pGTN_HPR_connect_cb);
  espconn_accept(pGTN_HPRConn);
  espconn_tcp_set_max_con_allow(pGTN_HPRConn, 1);
  espconn_regist_time(pGTN_HPRConn, 15, 0);
  //pTCPConn->reverse = pMQTTclient;

  nGTN_HPRStatem=SM_WAITING_MERDANERA;
	msleep(10);
	return TRUE;
}

void ICACHE_FLASH_ATTR pGTN_HPR_connect_cb(void *arg) {
  struct espconn * pCon = (struct espconn *)arg;
  espconn_regist_recvcb(pCon, pGTN_HPR_rx_cb);
  espconn_regist_disconcb(pCon, pGTN_HPR_disc_cb);
  espconn_regist_reconcb(pCon, pGTN_HPR_recon_cb);

  // try to send any rx data to test server
  espconn_sent(pCon, UartDev.rcv_buff.pRcvMsgBuff, UartDev.received );
  ResetRxBuff();
  espconn_disconnect(pCon);

}

void ICACHE_FLASH_ATTR pGTN_HPR_rx_cb(void *arg, char *data, uint16_t len) {
    struct espconn *pCon = (struct espconn *)arg;
    uint8_t *addr_array = NULL;
    addr_array = pCon->proto.tcp->remote_ip;
    ip_addr_t addr;
    IP4_ADDR(&addr, pCon->proto.tcp->remote_ip[0], pCon->proto.tcp->remote_ip[1], pCon->proto.tcp->remote_ip[2], pCon->proto.tcp->remote_ip[3]);
    /*
    if (ota_ip == 0) {
      // There is no previously stored IP address, so we have it.
      ota_ip = addr.addr;
      ota_port = pCon->proto.tcp->remote_port;
      ota_state = CONNECTION_ESTABLISHED;
      }
    */
    uart0_write(data, len);
}
void ICACHE_FLASH_ATTR pGTN_HPR_disc_cb(void *arg) {
}
void ICACHE_FLASH_ATTR pGTN_HPR_recon_cb(void *arg) {
}


int ICACHE_FLASH_ATTR uart0_tx_one_char(uint8 TxChar) {
    while (true) {
		  uint32 fifo_cnt = READ_PERI_REG(UART_STATUS(UART0)) & (UART_TXFIFO_CNT<<UART_TXFIFO_CNT_S);
		  if ((fifo_cnt >> UART_TXFIFO_CNT_S & UART_TXFIFO_CNT) < 126) {
			  break;
		    }
	    }
	WRITE_PERI_REG(UART_FIFO(UART0), TxChar);
	return TRUE;
}

void ICACHE_FLASH_ATTR uart0_write(char *c, int len) {
    int nlen;

    for (nlen=0; nlen<len; nlen++) {
        uart0_tx_one_char(c[nlen]);
        }
}
