#ifndef SCO_AUDIO_H
#define SCO_AUDIO_H

#include <stdbool.h>
#include "btstack.h"

// Embedded commentary clip -> SCO (mSBC or CVSD), standing in for FM audio.
void sco_audio_init(void);
void sco_audio_start(uint8_t negotiated_codec);
void sco_audio_stop(void);
// Fill one outbound SCO payload (ws6_ptt sco_fill hook).
void sco_audio_fill(uint8_t *payload, uint16_t len);
// Ramp the commentary to silence (true) or back to full level (false).
void sco_audio_mute(bool mute);

#endif
