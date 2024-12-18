#include "esp8266.h"
#include "gpio.h"

// Make a pin be GPIO, i.e. set the mux so the pin has the gpio function
void ICACHE_FLASH_ATTR makeGpio(uint8_t pin) {
  uint32_t addr;
  uint8_t func = 3;
  switch (pin) {
  case 0:
    addr = PERIPHS_IO_MUX_GPIO0_U;
    func = 0;
    break;
  case 1:
    addr = PERIPHS_IO_MUX_U0TXD_U;
    break;
  case 2:
    addr = PERIPHS_IO_MUX_GPIO2_U;
    func = 0;
    break;
  case 3:
    addr = PERIPHS_IO_MUX_U0RXD_U;
    break;
  case 4:
    addr = PERIPHS_IO_MUX_GPIO4_U;
    func = 0;
    break;
  case 5:
    addr = PERIPHS_IO_MUX_GPIO5_U;
    func = 0;
    break;
  case 6:
  case 7:
  case 8:
  case 9:
  case 10:
  case 11:
    addr = PERIPHS_IO_MUX_SD_CMD_U - 4 * (11-pin);
    break;
  case 12:
  case 13:
  case 14:
  case 15:
    addr = PERIPHS_IO_MUX_MTDO_U - 4 * (15-pin);
    break;
  default:
    return;
  }
  PIN_FUNC_SELECT(addr, func);
}

