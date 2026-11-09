/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_MODULES_H
#define RX3_MODULES_H
#include "../api/rx3_indicator_api.h"
unsigned int rx3_modules_start(void);
unsigned int rx3_modules_failures(void);
void rx3_modules_stop(void);
void rx3_modules_track_did_load(unsigned int, void *, const void *);
void rx3_modules_track_will_load(unsigned int, void *, const void *);
struct rx3_text_observation;
void rx3_modules_audio_started(unsigned int);
void rx3_modules_text_observed(const struct rx3_text_observation *);
void rx3_modules_report(void);
const uint16_t *rx3_modules_text_substitute(const struct rx3_text_observation *, unsigned int *);
int rx3_modules_beatfx_touch(int, int);
void rx3_modules_touch_observed(int, int, int);
int rx3_modules_uses_audio(void);
void rx3_indicator_draw(rx3_indicator_rect, void *, int light);
int rx3_indicator_active(void);
unsigned int rx3_header_progress(void);
int rx3_indicator_take_refresh(void);
int rx3_indicator_probe_enabled(void);
void rx3_indicator_probe(rx3_indicator_rect, void *, unsigned int, int, int, unsigned int);
void rx3_modules_bind_writer(int (*)(unsigned long, const void *, const void *, unsigned int));
#endif
