/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "a2_dos_catalog.h"

enum {
    HANDHELD_BTN_START = 1u << 0, HANDHELD_BTN_SELECT = 1u << 1,
    HANDHELD_BTN_B = 1u << 2, HANDHELD_BTN_A = 1u << 3,
    HANDHELD_BTN_UP = 1u << 4, HANDHELD_BTN_RIGHT = 1u << 5,
    HANDHELD_BTN_LEFT = 1u << 6, HANDHELD_BTN_DOWN = 1u << 7,
    HANDHELD_BTN_MENU = 1u << 8,
    HANDHELD_UI_PAGE_BYTES = 2048,
};

typedef enum {
    HANDHELD_UI_HIDDEN, HANDHELD_UI_MENU, HANDHELD_UI_KEYBOARD, HANDHELD_UI_INFO,
    HANDHELD_UI_PROGRAMS, HANDHELD_UI_WIFI_SETUP, HANDHELD_UI_WIFI,
} handheld_ui_mode_t;

typedef enum {
    HANDHELD_ACTION_NONE, HANDHELD_ACTION_RESUME, HANDHELD_ACTION_KEY,
    HANDHELD_ACTION_CATALOG, HANDHELD_ACTION_SAVE_BASIC,
    HANDHELD_ACTION_LOAD_BASIC, HANDHELD_ACTION_RESTART,
    HANDHELD_ACTION_OPEN_PROGRAMS, HANDHELD_ACTION_RUN_PROGRAM,
    HANDHELD_ACTION_START_WIFI_SETUP, HANDHELD_ACTION_STOP_WIFI_SETUP,
    HANDHELD_ACTION_TOGGLE_WIFI,
} handheld_ui_action_t;

typedef struct {
    handheld_ui_action_t action;
    uint8_t key;                    /* ASCII; Delete=8, Return=13. */
    bool changed;
    unsigned program_index;
} handheld_ui_event_t;

typedef struct {
    const char *ip;
    size_t free_heap;
    const char *disk_name;
    const char *disk_state;
    const char *status;
    const char *input_line;         /* Optional current Apple II input row. */
    bool wifi_enabled;
    bool wifi_hotspot;
    const char *wifi_ssid;
    const char *wifi_status;
    const char *ssh_user;
    const char *ssh_password;       /* Caller supplies uppercase or a displayable hint. */
    const char *wifi_setup_ssid;    /* Uppercase, matching the text display. */
    const char *wifi_setup_password;
    const char *wifi_setup_url;
    const char *wifi_setup_status;
} handheld_ui_info_t;

typedef struct {
    handheld_ui_mode_t mode;
    uint8_t menu_selection;
    uint8_t wifi_selection;
    uint8_t key_selection;
    uint8_t last_key;
    bool has_last_key;
    uint16_t stable_buttons, candidate_buttons, blocked_buttons, repeat_button;
    uint32_t candidate_since_ms, repeat_at_ms;
    uint16_t injected_button;
    bool injected_releasing;
    a2_dos_catalog catalog;
    unsigned program_selection, program_scroll;
    char program_status[41];
} handheld_ui_t;

void handheld_ui_init(handheld_ui_t *ui);
bool handheld_ui_visible(const handheld_ui_t *ui);
bool handheld_ui_pauses_emulation(const handheld_ui_t *ui);
/* Latch one synthetic button until a debounced press and release are observed.
 * Returns false while another pulse is pending or this button is already held.
 * Caller must serialize this with update, as with all other UI state access. */
bool handheld_ui_press(handheld_ui_t *ui, uint16_t button);
/* Copy a refreshed catalog (NULL clears it), reset selection and keep an
 * optional short error/status message. This does not change the current mode. */
void handheld_ui_set_programs(handheld_ui_t *ui, const a2_dos_catalog *catalog,
                              const char *status);
/* Poll regularly using pressed=1. Debounce=20ms; directional hold repeats after
 * 350ms at 100ms intervals, with at most one movement per call. A/B/MENU never
 * repeat. With the overlay hidden, START=Return, SELECT=Ctrl-C/Break, and the
 * D-pad emits Apple cursor codes (left8/right21/up11/down10); A/B remain joystick
 * buttons. Buttons held during a mode transition must be released before reuse. */
handheld_ui_event_t handheld_ui_update(handheld_ui_t *ui, uint16_t buttons, uint32_t now_ms);
/* Output is a 2048-byte block corresponding to Apple addresses $0400..$0BFF.
 * The active overlay uses page $0400, 40x24 normal/inverse uppercase text.
 * Caller owns the buffer and may pass it to the original text renderer. */
void handheld_ui_render(const handheld_ui_t *ui, const handheld_ui_info_t *info,
                        uint8_t page[HANDHELD_UI_PAGE_BYTES]);
