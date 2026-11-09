/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_INPUT_H
#define RX3_INPUT_H
#include "../api/rx3_input_api.h"
/* The action the core runs when a physical pad-mode key is pressed. */
void rx3_input_bind_mode_keys(void (*action)(void));
/* Diagnostic: describe this many SEND_KEY events in the log. */
void rx3_input_trace_keys(unsigned int limit);
/* Diagnostic: trace only this key code; 0 traces every key. */
void rx3_input_trace_only(unsigned int key);
unsigned int rx3_input_count(void);
#endif
