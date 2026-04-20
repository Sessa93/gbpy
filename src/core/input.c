/*
 * Game Boy Input (Joypad) Implementation
 * Section 2.12 of Game Boy CPU Manual
 */

#include "input.h"

void input_init(GbInput *input) {
    input->button_state = 0;
    input->select = 0x30; /* Both deselected */
}

void input_press(GbInput *input, GbButton btn) {
    input->button_state |= (1 << btn);
}

void input_release(GbInput *input, GbButton btn) {
    input->button_state &= ~(1 << btn);
}

/*
 * Read P1 register (Section 2.12):
 *   When P14 (bit 4) is 0: read direction keys (bits 0-3)
 *   When P15 (bit 5) is 0: read button keys (bits 0-3)
 *   Buttons active LOW (0 = pressed)
 */
uint8_t input_read(const GbInput *input) {
    uint8_t result = input->select | 0xC0; /* bits 6-7 always 1 */

    if (!(input->select & 0x10)) {
        /* P14 selected: direction keys */
        uint8_t dirs = input->button_state & 0x0F; /* right/left/up/down */
        result |= (~dirs) & 0x0F;
    }
    if (!(input->select & 0x20)) {
        /* P15 selected: button keys */
        uint8_t btns = (input->button_state >> 4) & 0x0F; /* A/B/select/start */
        result |= (~btns) & 0x0F;
    }

    /* If neither selected, bits 0-3 are all 1 */
    if ((input->select & 0x30) == 0x30) {
        result |= 0x0F;
    }

    return result;
}

void input_write(GbInput *input, uint8_t val) {
    input->select = val & 0x30;
}

bool input_check_interrupt(const GbInput *input) {
    /* Interrupt fires if any selected button is pressed */
    if (!(input->select & 0x10)) {
        if (input->button_state & 0x0F) return true;
    }
    if (!(input->select & 0x20)) {
        if (input->button_state & 0xF0) return true;
    }
    return false;
}
