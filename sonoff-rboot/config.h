#ifndef CONFIG_H
#define CONFIG_H

// Flash configuration settings. When adding new items always add them at the end and formulate
// them such that a value of zero is an appropriate default or backwards compatible. Existing
// modules that are upgraded will have zero in the new fields. This ensures that an upgrade does
// not wipe out the old settings.
typedef struct {
  uint32_t seq; // flash write sequence number
  uint16_t magic, crc;
  int32_t  baud_rate;
  char     hostname[32];               // if using DHCP
  uint32_t staticip, netmask, gateway; // using DHCP if staticip==0
  uint8_t  log_mode;                   // UART log debug mode
  uint8_t  tcp_enable, rssi_enable;    // TCP client settings
  uint8_t  mqtt_enable,   // SLIP protocol, MQTT client
           mqtt_status_enable,         // MQTT status reporting
           mqtt_timeout,               // MQTT send timeout
           mqtt_clean_session;         // MQTT clean session
  uint16_t mqtt_port, mqtt_keepalive;  // MQTT Host port, MQTT Keepalive timer
  char     mqtt_host[32], 
           mqtt_clientid[48], 
           mqtt_username[32], 
           mqtt_password[32];
  uint8_t  IOPort_bit0: 1;
  uint8_t  IOPort_bit1: 1;
  uint8_t  IOPort_bit2: 1;
  uint8_t  IOPort_bit3: 1;
  uint8_t  IOPort_bit4: 1;
  uint8_t  IOPort_bit5: 1;
  uint8_t  IOPort_bit6: 1;
  uint8_t  IOPort_bit7: 1;
  
} FlashConfig;
extern FlashConfig flashConfig;

bool configSave(void);
bool configRestore(void);
void configWipe(void);
const size_t getFlashSize();

void ICACHE_FLASH_ATTR set_blink_timer(uint16_t interval);
#define sleepms(n) os_delay_us(n*1000L);

#endif
