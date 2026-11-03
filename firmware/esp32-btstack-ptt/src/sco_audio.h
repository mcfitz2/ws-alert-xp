#ifndef SCO_AUDIO_H
#define SCO_AUDIO_H

#include <stdbool.h>
#include "btstack.h"

// Embedded commentary clip -> SCO (mSBC or CVSD), standing in for FM audio.
void sco_audio_init(void);
void sco_audio_start(uint8_t negotiated_codec);
void sco_audio_stop(void);
void sco_audio_send(hci_con_handle_t h);
// Ramp the commentary to silence (true) or back to full level (false).
void sco_audio_mute(bool mute);

#endif
