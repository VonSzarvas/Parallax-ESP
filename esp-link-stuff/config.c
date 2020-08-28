// Copyright 2015 by Thorsten von Eicken, see LICENSE.txt
/* Configuration stored in flash */

#include <esp8266.h>
#include <osapi.h>
#include "config.h"
#include "espfs.h"
#include "crc16.h"
#include "uart.h"
#include "sscp.h"

#define MCU_RESET_PIN       12
#define LED_CONN_PIN        5
#define LOADER_BAUD_RATE    115200
#define BAUD_RATE           115200

// magic number to recognize thet these are our flash settings as opposed to some random stuff
#define FLASH_MAGIC         0x55aa
#define FLASH_VERSION       8

// size of the setting sector
#define FLASH_SECT          4096

#define FIRMWARE_SIZE       0x7b000
#define CHIP_IN_MODULE_NAME

FlashConfig flashConfig;
FlashConfig flashDefault = {
  .seq = 33, .magic = 0, .crc = 0,
  .version              = FLASH_VERSION,
  .reset_pin            = MCU_RESET_PIN,
  .conn_led_pin         = LED_CONN_PIN,
  .loader_baud_rate     = LOADER_BAUD_RATE,
  .baud_rate            = BAUD_RATE,
  .stop_bits            = ONE_STOP_BIT,
  .dbg_baud_rate        = BAUD_RATE,
  .dbg_stop_bits        = ONE_STOP_BIT,
  .module_descr 	    = "",
  .rx_pullup	        = 0,
  .sscp_enable          = 0,
  .sscp_start           = SSCP_TKN_START,
  .sscp_need_pause      = ":,",
  .sscp_need_pause_cnt  = 2,
  .sscp_pause_time_ms   = 0,
  .sscp_events          = 0,
  .dbg_enable           = 0,
  .sscp_loader          = 0
};

typedef union {
  FlashConfig fc;
  uint8_t     block[1024];
} FlashFull;

// address where to flash the settings: if we have >512KB flash then there are 16KB of reserved
// space at the end of the first flash partition, we use the upper 8KB (2 sectors). If we only
// have 512KB then that space is used by the SDK and we use the 8KB just before that.
static uint32_t ICACHE_FLASH_ATTR flashAddr(void) {
  enum flash_size_map map = system_get_flash_size_map();
  return map >= FLASH_SIZE_8M_MAP_512_512
    ? FLASH_SECT + FIRMWARE_SIZE + 2*FLASH_SECT // bootloader + firmware + 8KB free
    : FLASH_SECT + FIRMWARE_SIZE - 2*FLASH_SECT;// bootloader + firmware - 8KB (risky...)
}

static int flash_pri; // primary flash sector (0 or 1, or -1 for error)

#if 0
static void memDump(void *addr, int len) {
  for (int i=0; i<len; i++) {
    os_printf("0x%02x", ((uint8_t *)addr)[i]);
  }
  os_printf("\n");
}
#endif

bool ICACHE_FLASH_ATTR configSave(void) {
  FlashFull ff;
  os_memset(&ff, 0, sizeof(ff));
  os_memcpy(&ff, &flashConfig, sizeof(FlashConfig));
  uint32_t seq = ff.fc.seq+1;
  // erase secondary
  uint32_t addr = flashAddr() + (1-flash_pri)*FLASH_SECT;
  if (spi_flash_erase_sector(addr>>12) != SPI_FLASH_RESULT_OK)
    goto fail; // no harm done, give up
  // calculate CRC
  ff.fc.seq = seq;
  ff.fc.magic = FLASH_MAGIC;
  ff.fc.version = FLASH_VERSION;
  ff.fc.crc = 0;
  //os_printf("cksum of: ");
  //memDump(&ff, sizeof(ff));
  ff.fc.crc = crc16_data((unsigned char*)&ff, sizeof(ff), 0);
  //os_printf("cksum is %04x\n", ff.fc.crc);
  // write primary with incorrect seq
  ff.fc.seq = 0xffffffff;
  if (spi_flash_write(addr, (void *)&ff, sizeof(ff)) != SPI_FLASH_RESULT_OK)
    goto fail; // no harm done, give up
  // fill in correct seq
  ff.fc.seq = seq;
  if (spi_flash_write(addr, (void *)&ff, sizeof(uint32_t)) != SPI_FLASH_RESULT_OK)
    goto fail; // most likely failed, but no harm if successful
  // now that we have safely written the new version, erase old primary
  addr = flashAddr() + flash_pri*FLASH_SECT;
  flash_pri = 1-flash_pri;
  if (spi_flash_erase_sector(addr>>12) != SPI_FLASH_RESULT_OK)
    return true; // no back-up but we're OK
  // write secondary
  ff.fc.seq = 0xffffffff;
  if (spi_flash_write(addr, (void *)&ff, sizeof(ff)) != SPI_FLASH_RESULT_OK)
    return true; // no back-up but we're OK
  ff.fc.seq = seq;
  spi_flash_write(addr, (void *)&ff, sizeof(uint32_t));
  return true;
fail:
#ifdef CONFIG_DBG
  os_printf("*** Failed to save config ***\n");
#endif
  return false;
}

void ICACHE_FLASH_ATTR configWipe(void) {
  spi_flash_erase_sector(flashAddr()>>12);
  spi_flash_erase_sector((flashAddr()+FLASH_SECT)>>12);
}

static int ICACHE_FLASH_ATTR selectPrimary(FlashFull *fc0, FlashFull *fc1);

bool ICACHE_FLASH_ATTR configRestore(void) {
  FlashFull ff0, ff1;
  // read both flash sectors
  if (spi_flash_read(flashAddr(), (void *)&ff0, sizeof(ff0)) != SPI_FLASH_RESULT_OK)
    os_memset(&ff0, 0, sizeof(ff0)); // clear in case of error
  if (spi_flash_read(flashAddr()+FLASH_SECT, (void *)&ff1, sizeof(ff1)) != SPI_FLASH_RESULT_OK)
    os_memset(&ff1, 0, sizeof(ff1)); // clear in case of error
  // figure out which one is good
  flash_pri = selectPrimary(&ff0, &ff1);
  // if neither is OK, we revert to defaults
  if (flash_pri < 0) {
    if (!configRestoreDefaults())
      return false;
    flash_pri = 0;
    return false;
  }
  // copy good one into global var and return
  os_memcpy(&flashConfig, flash_pri == 0 ? &ff0.fc : &ff1.fc, sizeof(FlashConfig));
  softap_get_ssid(flashConfig.module_name, sizeof(flashConfig.module_name));
  return true;
}

bool ICACHE_FLASH_ATTR softap_set_ssid(const char *ssid, int size)
{
    struct softap_config apconf;
    if (size > sizeof(apconf.ssid))
        return false;
    if (!wifi_softap_get_config(&apconf))
        return false;
    os_memset(apconf.ssid, 0, sizeof(apconf.ssid));
    os_memcpy(apconf.ssid, ssid, size);
    apconf.ssid_len = size;
    os_memset(apconf.password, 0, sizeof(apconf.password));
    apconf.authmode = AUTH_OPEN;
    apconf.ssid_hidden = 0;
    return wifi_softap_set_config(&apconf);
}

bool ICACHE_FLASH_ATTR softap_get_ssid(char *ssid, int size)
{
    struct softap_config apconf;
    if (size < sizeof(apconf.ssid) + 1)
        return false;
    if (!wifi_softap_get_config(&apconf))
        return false;
    os_memcpy(ssid, apconf.ssid, sizeof(apconf.ssid));
    ssid[sizeof(apconf.ssid)] = '\0';
    return true;
}

bool ICACHE_FLASH_ATTR configRestoreDefaults(void) {
    os_memcpy(&flashConfig, &flashDefault, sizeof(FlashConfig));
    char chipIdStr[6];
    os_sprintf(chipIdStr, "%06x", system_get_chip_id());
#ifdef CHIP_IN_MODULE_NAME
    char module_name[32];
    os_strcpy(module_name, "wx-");
    os_strcat(module_name, chipIdStr);
    os_memcpy(&flashConfig.module_name, module_name, os_strlen(module_name) + 1); // include terminating zero
    softap_set_ssid(flashConfig.module_name, os_strlen(flashConfig.module_name));
#endif
    return true;
}

static int ICACHE_FLASH_ATTR selectPrimary(FlashFull *ff0, FlashFull *ff1) {
  // check CRC of ff0
  uint16_t crc = ff0->fc.crc;
  ff0->fc.crc = 0;
  bool ff0_crc_ok = crc16_data((unsigned char*)ff0, sizeof(FlashFull), 0) == crc;
#ifdef CONFIG_DBG
  os_printf("FLASH chk=0x%04x crc=0x%04x full_sz=%d sz=%d chip_sz=%d\n",
      crc16_data((unsigned char*)ff0, sizeof(FlashFull), 0),
      crc,
      sizeof(FlashFull),
      sizeof(FlashConfig),
      getFlashSize());
#endif

  // check CRC of ff1
  crc = ff1->fc.crc;
  ff1->fc.crc = 0;
  bool ff1_crc_ok = crc16_data((unsigned char*)ff1, sizeof(FlashFull), 0) == crc;

  // decided which we like better
  if (ff0_crc_ok && ff0->fc.magic == FLASH_MAGIC && ff0->fc.version == FLASH_VERSION)
    if (!ff1_crc_ok || ff1->fc.magic != FLASH_MAGIC || ff1->fc.version != FLASH_VERSION || ff0->fc.seq >= ff1->fc.seq)
      return 0; // use first sector as primary
    else
      return 1; // second sector is newer
  else
    return ff1_crc_ok && ff1->fc.magic == FLASH_MAGIC && ff1->fc.version == FLASH_VERSION? 1 : -1;
}

// returns the flash chip's size, in BYTES
const size_t ICACHE_FLASH_ATTR
getFlashSize() {
  uint32_t id = spi_flash_get_id();
  uint8_t mfgr_id = id & 0xff;
  //uint8_t type_id = (id >> 8) & 0xff; // not relevant for size calculation
  uint8_t size_id = (id >> 16) & 0xff; // lucky for us, WinBond ID's their chips as a form that lets us calculate the size
  if (mfgr_id != 0xEF) // 0xEF is WinBond; that's all we care about (for now)
    return 0;
  return 1 << size_id;
}
