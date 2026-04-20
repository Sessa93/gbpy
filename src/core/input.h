/*
 * Game Boy Input (Joypad)
 *
 * Reference: Game Boy CPU Manual v1.01 (resources/gba-manual.md)
 * Section 2.12: Joypad Input
 *
 * Register FF00 - P1/JOYP:
 *   Bit 5: P15 Select Button Keys   (0=Select)
 *   Bit 4: P14 Select Direction Keys (0=Select)
 *   Bit 3: P13 Input Down  or Start  (0=Pressed)
 *   Bit 2: P12 Input Up    or Select (0=Pressed)
 *   Bit 1: P11 Input Left  or B      (0=Pressed)
 *   Bit 0: P10 Input Right or A      (0=Pressed)
 *
 * Note: Low = pressed / selected
 */

#ifndef GBPY_INPUT_H
#define GBPY_INPUT_H

#include "types.h"

/* Button bit indices (active = pressed) */
typedef enum {
    GB_BTN_RIGHT  = 0,
    GB_BTN_LEFT   = 1,
    GB_BTN_UP     = 2,
    GB_BTN_DOWN   = 3,
    GB_BTN_A      = 4,
    GB_BTN_B      = 5,
    GB_BTN_SELECT = 6,
    GB_BTN_START  = 7,
} GbButton;

typedef struct GbInput {
    uint8_t button_state;   /* Bitmask: bit=1 means pressed */
    uint8_t select;         /* P1 register write value (bits 4-5) */
} GbInput;

void input_init(GbInput *input);

/* Set/clear a button press */
void input_press(GbInput *input, GbButton btn);
void input_release(GbInput *input, GbButton btn);

/* Read P1 register (FF00) */
uint8_t input_read(const GbInput *input);

/* Write P1 register (FF00) - only bits 4-5 writable */
void input_write(GbInput *input, uint8_t val);

/* Check if joypad interrupt should fire (any button in selected group goes low) */
bool input_check_interrupt(const GbInput *input);

#endif /* GBPY_INPUT_H */
