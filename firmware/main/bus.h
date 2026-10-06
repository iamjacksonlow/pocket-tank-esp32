/* bus.h - Bus mode for the muma board: live Singapore bus arrivals for the
 * stops near home (user_config.h), drawn on the native 240x240 screen. */
#ifndef BUS_H
#define BUS_H
#include <stdbool.h>
#include <stdint.h>

void bus_init(void);                 /* once at boot: NVS creds read, nothing radio-side */
void bus_enter(void);                /* Wi-Fi on (or the setup portal), fetching starts */
void bus_leave(void);                /* fetching stops, Wi-Fi off */
bool bus_active(void);
void bus_next_page(void);            /* the button's double press */
void bus_scroll(int dir);            /* voice "next" (+1) / "previous" (-1): keep scrolling that way */
void bus_hold(void);                 /* voice "stop": stay on this page */
void bus_goto(int page);             /* 0 = LEAVE SOON */
void bus_render(uint16_t *fb240, float clock);   /* RGB565 native endian, 240x240 */
void bus_refresh(void);              /* fetch now + back to LEAVE SOON */
/* AIR mode (PSI + alerts from data.gov.sg); shares the Wi-Fi with BUS */
void air_enter(void);
void air_leave(void);
bool air_active(void);
void air_next_page(void);
void air_refresh(void);              /* fetch now + back to page 1 */
void air_render_screen(uint16_t *fb240);
#endif
