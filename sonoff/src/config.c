/* Configuration stored in flash */

#include <osapi.h>
#include <string.h>
#include "mqtt_client.h"
#include "network.h"
#include "injectorcleaner.h"
#include "config.h"

// DO NOT DISABLE CONFIG_DBG (It will crash esp on save... to fix)
#define CONFIG_DBG

#ifdef CONFIG_DBG
#define DBG(format, ...) do { os_printf("%s: ", __FUNCTION__); os_printf(format, ## __VA_ARGS__); os_printf("\n"); } while(0)
#else
#define DBG(format, ...) do { } while(0)
#endif
#define PRINTNET(format, ...) do { if (pTXdata) { TXdatalen+=os_sprintf(pTXdata+TXdatalen, format, ## __VA_ARGS__ ); TXdatalen+=os_sprintf(pTXdata+TXdatalen, "\n");} } while(0)


FlashConfig flashConfig;

/* CITT CRC16 polynomial ^16 + ^12 + ^5 + 1 */
/*---------------------------------------------------------------------------*/
unsigned short ICACHE_FLASH_ATTR crc16_add(unsigned char b, unsigned short acc) {
  /*
    acc  = (unsigned char)(acc >> 8) | (acc << 8);
    acc ^= b;
    acc ^= (unsigned char)(acc & 0xff) >> 4;
    acc ^= (acc << 8) << 4;
    acc ^= ((acc & 0xff) << 4) << 1;
  */

  acc ^= b;
  acc  = (acc >> 8) | (acc << 8);
  acc ^= (acc & 0xff00) << 4;
  acc ^= (acc >> 8) >> 4;
  acc ^= (acc & 0xff00) >> 5;
  return acc;
}
/*---------------------------------------------------------------------------*/
unsigned short ICACHE_FLASH_ATTR crc16_data(const unsigned char *data, int len, unsigned short acc){
  int i;
  
  for(i = 0; i < len; ++i) {
    acc = crc16_add(*data, acc);
    ++data;
  }
  return acc;
}
/*---------------------------------------------------------------------------*/

#if 1
bool ICACHE_FLASH_ATTR parse_ip(char *buff, ip_addr_t *ip_ptr) {
  char *next = buff; // where to start parsing next integer
  int i, found = 0;     // number of integers parsed
  uint32_t ip = 0;   // the ip addres parsed

  for (i=0; i<32; i++) { // 32 is just a safety limit
    char c = buff[i];
    if (c == '.' || c == 0) {
      // parse the preceding integer and accumulate into IP address
      bool last = c == 0;
      buff[i] = 0;
      uint32_t v = atoi(next);
      ip = ip | ((v&0xff)<<(found*8));
      next = buff+i+1; // next integer starts after the '.'
      found++;
      if (last) { // if at end of string we better got 4 integers
        ip_ptr->addr = ip;
        if (found == 4) {
          return true;
          }
        else {
          DBG("found %d next %s", found, next);
          }
        }
      continue;
      }
    if (c < '0' || c > '9') return false;
    }
  return false;
}
#else

bool parse_ip (char * ip, ip_addr_t *ip_ptr) {
    /* The count of the number of bytes processed. */
    int i, n;
    /* A pointer to the next digit to process. */
    const char * start;
    /* The digit being processed. */
    char c;
    /* I have not all the time in my life, folks :-) */
    uint8_t reverse[4];
    uint32_t fatica;

    start = ip;
    for (i = 0; i < 4; i++) {
        /* The value of this byte. */
        n = 0;
        while (1) {
            c = *start;
            start++;
            if (c >= '0' && c <= '9') {
              n *= 10;
              n += c - '0';
              }
            /* We insist on stopping at "." if we are still parsing
               the first, second, or third numbers. If we have reached
               the end of the numbers, we will allow any character. */
            else if ((i < 3 && c == '.') || i == 3) {
                  break;
                  }
                 else {
                  return false;
                  }
          }
        if (n >= 256) {
            return false;
          }
        fatica *= 256;
        fatica += n;
      }

    /* rotate */
    reverse[3]=(uint8_t)(fatica & 0x000000FF);
    reverse[2]=(uint8_t)(fatica & 0x0000FF00);
    reverse[1]=(uint8_t)(fatica & 0x00FF0000);
    reverse[0]=(uint8_t)(fatica & 0xFF000000);

    ip_ptr->addr=(uint32_t)((uint8_t)(reverse[3] << 24) | (uint8_t)(reverse[2] << 16) | (uint8_t)(reverse[1] << 8) | (uint8_t)reverse[0]);

    //ip_ptr->addr=fatica;
    return true;
}

#endif
// address where to flash the settings: if we have >512KB flash then there are 16KB of reserved
// space at the end of the first flash partition, we use the upper 8KB (2 sectors). If we only
// have 512KB then that space is used by the SDK and we use the 8KB just before that.
static uint32_t ICACHE_FLASH_ATTR flashAddr(void) {
  enum flash_size_map map = system_get_flash_size_map();
  return map >= FLASH_SIZE_8M_MAP_512_512
    ? FLASH_SECT + ESP_FLASH_MAX + FLASH_SECT // bootloader + maxfirmwaresize + 4KB free
    : FLASH_SECT + ESP_FLASH_MAX - FLASH_SECT;// bootloader + firmware - 4KB (risky...)
}

#ifdef CONFIG_MEMDBG
static void memDump(void *addr, int len) {
  os_printf("cksum of: ");
  for (int i=0; i<len; i++) {
    os_printf("0x%02x", ((uint8_t *)addr)[i]);
	}
  os_printf("\n");
}
#else
static void memDump(void *addr, int len) {}
#endif

bool ICACHE_FLASH_ATTR configSave() {
  bool bRet=true;
  uint16_t crc;

  crc=crc16_data((unsigned char*)&flashConfig+sizeof(crc), sizeof(FlashConfig)-sizeof(crc), 0);	
  flashConfig.crc=crc;

	ETS_GPIO_INTR_DISABLE();
  ETS_UART_INTR_DISABLE();    // when ModBus is enabled it MUST be disabled
  ETS_FRC1_INTR_DISABLE();

  if (spi_flash_erase_sector(flashAddr()>>12) == SPI_FLASH_RESULT_OK) {
    if (spi_flash_write(flashAddr(), (uint32 *)&flashConfig, sizeof(FlashConfig)) != SPI_FLASH_RESULT_OK) {
      DBG("Failed to save config.");
      bRet=false;
      goto en;
      }
    DBG("config saved, crc 0x%04X..", flashConfig.crc);
    }
  else DBG("Failed to erase.");

en:	//enable global interrupt
  ETS_FRC1_INTR_ENABLE();
  ETS_UART_INTR_ENABLE();
	ETS_GPIO_INTR_ENABLE();

  return bRet;
}

void ICACHE_FLASH_ATTR LoadDefaultAPConfig(void) {
  os_sprintf(flashConfig.apconf.ssid, "%s", AP_SSID);
  flashConfig.apconf.ssid_len = 0;
  os_sprintf(flashConfig.apconf.password, AP_PSW);
  parse_ip(AP_IPADDRESS, &flashConfig.ip1.ip);
  parse_ip(AP_NETMASK, &flashConfig.ip1.netmask);
  parse_ip(AP_GATEWAY, &flashConfig.ip1.gw);
  //flashConfig.apconf.authmode = AUTH_WPA2_PSK;
  flashConfig.apconf.authmode = AUTH_WPA_WPA2_PSK;  
  flashConfig.apconf.beacon_interval = 200;
  flashConfig.apconf.channel=5;
  flashConfig.apconf.max_connection=2;
  flashConfig.apconf.ssid_hidden = 0;
}

void ICACHE_FLASH_ATTR LoadDefaultConfig(void) {
  uint16_t crc;
  os_memset(&flashConfig, 0, sizeof(FlashConfig));	
  DBG("Setting defaults in flashConfig..."); 
  os_sprintf(flashConfig.hostname, PROJ_NAME"_%06x", system_get_chip_id());
  
  LoadDefaultAPConfig();
  DBG("ssid %s, FlashConfig size %d", AP_SSID, sizeof(FlashConfig));

  os_sprintf(flashConfig.stat_conf.ssid, STA_SSID);
  os_sprintf(flashConfig.stat_conf.password, STA_PASS);

  if (!parse_ip(STA_IPADDRESS, &flashConfig.ip0.ip)) {
    DBG("STA_IPADDRESS %u", flashConfig.ip0.ip.addr);
    }
  //os_printf("STA_IPADDRESS %s, flashConfig.ip0.ip 0x%08X, "IPSTR"\n", STA_IPADDRESS, flashConfig.ip0.ip.addr, IP2STR(&flashConfig.ip0.ip.addr));

  if (!parse_ip(STA_NETMASK, &flashConfig.ip0.netmask)) {
    DBG("STA_NETMASK %u", flashConfig.ip0.netmask.addr);
    }
  //os_printf("STA_NETMASK %s, flashConfig.ip0.netmask 0x%08X, "IPSTR"\n", STA_NETMASK, flashConfig.ip0.netmask.addr, IP2STR(&flashConfig.ip0.netmask.addr));

  if (!parse_ip(STA_GATEWAY, &flashConfig.ip0.gw)) {
    DBG("STA_GATEWAY %u", flashConfig.ip0.gw.addr);
    }
  //os_printf("STA_GATEWAY %s, flashConfig.ip0.gw 0x%08X, "IPSTR"\n", STA_GATEWAY, flashConfig.ip0.gw.addr, IP2STR(&flashConfig.ip0.gw.addr));

  //flashConfig.netmask      = 0x00ffffff,
  flashConfig.mqtt_timeout = 4;
  
  flashConfig.mqtt_clean_session = 1;
  flashConfig.mqtt_port    = MQTT_PORT;
  flashConfig.mqtt_keepalive = 60;
  os_sprintf(flashConfig.mqtt_host, MQTT_HOST);
  os_sprintf(flashConfig.mqtt_clientid, flashConfig.hostname);  
  #if defined(HOUSE_POW_METER_RX)
  flashConfig.WattOffset=200;
  #endif
  #if defined(HOUSE_POW_METER_TX) || defined(SONOFFPOW_DDS238_2)
  parse_ip(HPMETER_RX_IP, &flashConfig.HPRx_IP);
  #endif
  #if defined(GASINJECTORCLEANER)
    flashConfig.dutycycle=DUTY_CYCLE;
    flashConfig.interval=INJECTION_PERIOD;
  #endif

  flashConfig.map2.Enable.doorBell=1;
  crc=crc16_data((unsigned char*)&flashConfig+sizeof(crc), sizeof(FlashConfig)-sizeof(crc), 0);	
  flashConfig.crc=crc;
  DBG("Defaults settled, crc 0x%04X..", flashConfig.crc);
}
  
void ICACHE_FLASH_ATTR LoadConfigFromFlash(void) {
  uint16_t crc;
	
	ETS_GPIO_INTR_DISABLE();
  ETS_UART_INTR_DISABLE();    // when ModBus is enabled it MUST be disabled
  ETS_FRC1_INTR_DISABLE();

  if (spi_flash_read(flashAddr(), (uint32 *)&flashConfig, sizeof(FlashConfig)) == SPI_FLASH_RESULT_OK ) {
    crc=crc16_data((unsigned char*)&flashConfig+sizeof(crc), sizeof(FlashConfig)-sizeof(crc), 0);	
    if ( flashConfig.crc == crc ) {
      DBG("crc 0x%04X", flashConfig.crc);
      }
    else {
      DBG("FlashConfig CRC 0x%04X, crc 0x%04X. Clearing Flash...", flashConfig.crc, crc);
      if (spi_flash_erase_sector(flashAddr()>>12) != SPI_FLASH_RESULT_OK) {
        DBG("Cant erase flash!");
        }
      LoadDefaultConfig();
      }
    }
  ETS_FRC1_INTR_ENABLE();
  ETS_UART_INTR_ENABLE();
	ETS_GPIO_INTR_ENABLE();
  
  DBG("config loaded.");
}

// returns the flash chip's size, in BYTES
const size_t ICACHE_FLASH_ATTR getFlashSize() {
  uint32_t id = spi_flash_get_id();
  uint8_t mfgr_id = id & 0xff;
  //uint8_t type_id = (id >> 8) & 0xff; // not relevant for size calculation
  uint8_t size_id = (id >> 16) & 0xff; // lucky for us, WinBond ID's their chips as a form that lets us calculate the size
  if (mfgr_id != 0xEF) // 0xEF is WinBond; that's all we care about (for now)
    return 0;
  return 1 << size_id;
}


bool ICACHE_FLASH_ATTR isFlashconfig_actual() {
  uint16_t crc;
  crc=crc16_data((unsigned char*)&flashConfig+sizeof(crc), sizeof(FlashConfig)-sizeof(crc), 0);	
  if (flashConfig.crc==crc) return true;
  return false;
}