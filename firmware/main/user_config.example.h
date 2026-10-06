/* user_config.example.h - your own settings for the muma build.
 *
 * Copy this file to user_config.h (same folder) and edit it. user_config.h is
 * in .gitignore, so your key and your home stops never end up on GitHub.
 * If user_config.h is missing, the build uses this example as-is.
 *
 * Wi-Fi is NOT set here: on first boot the board opens a "PocketTank-Setup"
 * hotspot and you pick your network from your phone. */
#ifndef USER_CONFIG_H
#define USER_CONFIG_H

/* LTA DataMall AccountKey (free: https://datamall.lta.gov.sg/content/datamall/en/request-for-api.html).
 * Optional. Leave "" to use the keyless arrivelah API instead, or type the key
 * into the setup page on your phone (it is then kept on the board, not in code). */
#define USER_LTA_KEY ""

/* Your bus stops (up to 6 fit nicely; the first page, LEAVE SOON, merges them all).
 * code = the 5-digit stop number on the pole, name/road = what the screen shows
 * (UPPER CASE, keep names to ~12 characters), walk = minutes on foot from home. */
#define USER_STOPS \
    { "01012", "HOTEL GRAND PAC", "VICTORIA ST", 3 }, \
    { "01013", "ST JOSEPH'S CH",  "VICTORIA ST", 4 },

/* AIR mode, page 3 (alerts): the 2-hour forecast area name exactly as data.gov.sg
 * spells it (e.g. "City", "Tampines", "Jurong West"), the nearest heat-stress (WBGT)
 * station id, and the subtitle the page shows. */
#define USER_FORECAST_AREA "City"
#define USER_WBGT_STATION  "S128"   /* station ids are listed in the API response */
#define USER_AREA_LABEL    "NEAR HOME"

#endif
