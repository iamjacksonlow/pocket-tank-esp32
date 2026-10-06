#ifndef RTC_PORT_H
#define RTC_PORT_H
#include <stdbool.h>
#include "driver/i2c_master.h"
/* call after the board I2C bus exists (display_port_init creates it) */
bool rtc_port_init(i2c_master_bus_handle_t bus);
#endif
