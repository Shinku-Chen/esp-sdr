#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Show the "open the web viewer and connect" hint on the built-in 240x320
 * panel of the FoloToy AI Passport. Best effort: the receiver keeps working
 * if the panel cannot be initialized. */
void passport_display_show_hint(void);

#ifdef __cplusplus
}
#endif
