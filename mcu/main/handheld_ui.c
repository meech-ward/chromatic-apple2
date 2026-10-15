/* SPDX-License-Identifier: MIT */
#include "handheld_ui.h"
#include <stdio.h>
#include <string.h>

enum { DEBOUNCE_MS = 20, REPEAT_DELAY_MS = 350, REPEAT_MS = 100,
       KEY_COLUMNS = 10, PROGRAM_ROWS = 12 };
static const char character_keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,:;()+-*/=<>\"'$?^";
enum { CHARACTER_COUNT = sizeof(character_keys) - 1, KEY_COUNT = CHARACTER_COUNT + 3 };
static const char *const menu_items[] = {
    "PROGRAMS", "RESUME", "KEYBOARD", "SAVE BASIC", "LOAD BASIC", "RESTART", "DEVICE INFO",
    "WI-FI SETUP"
};
enum { MENU_COUNT = sizeof(menu_items) / sizeof(menu_items[0]) };

void handheld_ui_init(handheld_ui_t *ui) { memset(ui, 0, sizeof(*ui)); }
bool handheld_ui_visible(const handheld_ui_t *ui) { return ui->mode != HANDHELD_UI_HIDDEN; }
bool handheld_ui_pauses_emulation(const handheld_ui_t *ui)
{
    return ui->mode == HANDHELD_UI_MENU || ui->mode == HANDHELD_UI_PROGRAMS ||
           ui->mode == HANDHELD_UI_INFO || ui->mode == HANDHELD_UI_WIFI_SETUP;
}

bool handheld_ui_press(handheld_ui_t *ui, uint16_t button)
{
    if (!button || (button & ~0x1ffu) || (button & (button - 1)) ||
        ui->injected_button || ((ui->stable_buttons | ui->candidate_buttons) & button))
        return false;
    ui->injected_button = button;
    ui->injected_releasing = false;
    return true;
}

void handheld_ui_set_programs(handheld_ui_t *ui, const a2_dos_catalog *catalog,
                              const char *status)
{
    if (catalog) ui->catalog = *catalog;
    else memset(&ui->catalog, 0, sizeof(ui->catalog));
    if (ui->catalog.count > A2_DOS_MAX_PROGRAMS) ui->catalog.count = A2_DOS_MAX_PROGRAMS;
    for (unsigned i = 0; i < ui->catalog.count; ++i)
        ui->catalog.programs[i].name[A2_DOS_NAME_BYTES - 1] = 0;
    ui->program_selection = ui->program_scroll = 0;
    snprintf(ui->program_status, sizeof(ui->program_status), "%s", status ? status : "");
}

static void mode(handheld_ui_t *ui, handheld_ui_mode_t value)
{
    ui->mode = value;
    ui->blocked_buttons |= ui->stable_buttons;
    ui->repeat_button = 0;
}

static uint8_t selected_key(unsigned selection)
{
    if (selection < CHARACTER_COUNT) return character_keys[selection];
    if (selection == CHARACTER_COUNT) return ' ';
    if (selection == CHARACTER_COUNT + 1) return 8;
    return 13;
}

static void navigate(handheld_ui_t *ui, uint16_t direction)
{
    if (ui->mode == HANDHELD_UI_PROGRAMS) {
        if (!ui->catalog.count) return;
        unsigned selection = ui->program_selection;
        if (direction == HANDHELD_BTN_UP && selection) --selection;
        else if (direction == HANDHELD_BTN_DOWN && selection + 1 < ui->catalog.count) ++selection;
        else if (direction == HANDHELD_BTN_LEFT)
            selection = selection > PROGRAM_ROWS ? selection - PROGRAM_ROWS : 0;
        else if (direction == HANDHELD_BTN_RIGHT) {
            selection += PROGRAM_ROWS;
            if (selection >= ui->catalog.count) selection = ui->catalog.count - 1;
        }
        ui->program_selection = selection;
        if (selection < ui->program_scroll) ui->program_scroll = selection;
        if (selection >= ui->program_scroll + PROGRAM_ROWS)
            ui->program_scroll = selection - PROGRAM_ROWS + 1;
        return;
    }
    if (ui->mode == HANDHELD_UI_MENU) {
        if (direction == HANDHELD_BTN_UP)
            ui->menu_selection = (ui->menu_selection + MENU_COUNT - 1) % MENU_COUNT;
        else if (direction == HANDHELD_BTN_DOWN)
            ui->menu_selection = (ui->menu_selection + 1) % MENU_COUNT;
        return;
    }
    if (ui->mode != HANDHELD_UI_KEYBOARD) return;
    unsigned selection = ui->key_selection;
    if (direction == HANDHELD_BTN_LEFT && selection % KEY_COLUMNS) --selection;
    else if (direction == HANDHELD_BTN_RIGHT && selection % KEY_COLUMNS < KEY_COLUMNS - 1 &&
             selection + 1 < KEY_COUNT) ++selection;
    else if (direction == HANDHELD_BTN_UP && selection >= KEY_COLUMNS) selection -= KEY_COLUMNS;
    else if (direction == HANDHELD_BTN_DOWN && selection / KEY_COLUMNS < (KEY_COUNT - 1) / KEY_COLUMNS) {
        selection += KEY_COLUMNS;
        if (selection >= KEY_COUNT) selection = KEY_COUNT - 1;
    }
    ui->key_selection = selection;
}

handheld_ui_event_t handheld_ui_update(handheld_ui_t *ui, uint16_t buttons, uint32_t now_ms)
{
    handheld_ui_event_t event = {0};
    const uint16_t releasing_button = ui->injected_releasing ? ui->injected_button : 0;
    buttons &= 0x1ff;
    if (!ui->injected_releasing) buttons |= ui->injected_button;
    if (buttons != ui->candidate_buttons) {
        ui->candidate_buttons = buttons;
        ui->candidate_since_ms = now_ms;
    }
    uint16_t edges = 0;
    if (ui->stable_buttons != ui->candidate_buttons &&
        (uint32_t)(now_ms - ui->candidate_since_ms) >= DEBOUNCE_MS) {
        edges = ui->candidate_buttons & ~ui->stable_buttons;
        ui->stable_buttons = ui->candidate_buttons;
    }
    if (ui->injected_button) {
        if (!ui->injected_releasing && (ui->stable_buttons & ui->injected_button))
            ui->injected_releasing = true;
        else if (ui->injected_releasing &&
                 !((ui->stable_buttons | ui->candidate_buttons) & ui->injected_button)) {
            ui->injected_button = 0;
            ui->injected_releasing = false;
        }
    }
    ui->blocked_buttons &= ui->stable_buttons;
    edges &= ~ui->blocked_buttons;
    if (edges & HANDHELD_BTN_MENU) {
        if (handheld_ui_visible(ui)) {
            event.action = ui->mode == HANDHELD_UI_WIFI_SETUP ?
                HANDHELD_ACTION_STOP_WIFI_SETUP : HANDHELD_ACTION_RESUME;
            mode(ui, HANDHELD_UI_HIDDEN);
        } else {
            ui->menu_selection = 0;
            mode(ui, HANDHELD_UI_MENU);
        }
        event.changed = true;
        return event;
    }
    const bool hidden = !handheld_ui_visible(ui);
    if (hidden && (edges & (HANDHELD_BTN_SELECT | HANDHELD_BTN_START))) {
        event.action = HANDHELD_ACTION_KEY;
        event.key = edges & HANDHELD_BTN_SELECT ? 3 : 13;
        event.changed = true;
        return event;
    }
    if (!hidden && (edges & HANDHELD_BTN_B)) {
        if (ui->mode == HANDHELD_UI_WIFI_SETUP)
            event.action = HANDHELD_ACTION_STOP_WIFI_SETUP;
        if (ui->mode == HANDHELD_UI_MENU) {
            mode(ui, HANDHELD_UI_HIDDEN);
            event.action = HANDHELD_ACTION_RESUME;
        } else mode(ui, HANDHELD_UI_MENU);
        event.changed = true;
        return event;
    }
    if (!hidden && (edges & HANDHELD_BTN_A)) {
        if (ui->mode == HANDHELD_UI_PROGRAMS) {
            if (ui->program_selection < ui->catalog.count) {
                event.action = HANDHELD_ACTION_RUN_PROGRAM;
                event.program_index = ui->program_selection;
                mode(ui, HANDHELD_UI_HIDDEN);
                event.changed = true;
            }
        } else if (ui->mode == HANDHELD_UI_KEYBOARD) {
            event.action = HANDHELD_ACTION_KEY;
            event.key = selected_key(ui->key_selection);
            ui->last_key = event.key;
            ui->has_last_key = true;
            event.changed = true;
        } else if (ui->mode == HANDHELD_UI_MENU) {
            switch (ui->menu_selection) {
            case 0:
                handheld_ui_set_programs(ui, NULL, "READING PROGRAMS...");
                mode(ui, HANDHELD_UI_PROGRAMS);
                event.action = HANDHELD_ACTION_OPEN_PROGRAMS;
                break;
            case 1: event.action = HANDHELD_ACTION_RESUME; mode(ui, HANDHELD_UI_HIDDEN); break;
            case 2: mode(ui, HANDHELD_UI_KEYBOARD); break;
            case 3: event.action = HANDHELD_ACTION_SAVE_BASIC; mode(ui, HANDHELD_UI_HIDDEN); break;
            case 4: event.action = HANDHELD_ACTION_LOAD_BASIC; mode(ui, HANDHELD_UI_HIDDEN); break;
            case 5: event.action = HANDHELD_ACTION_RESTART; mode(ui, HANDHELD_UI_HIDDEN); break;
            case 6: mode(ui, HANDHELD_UI_INFO); break;
            case 7:
                mode(ui, HANDHELD_UI_WIFI_SETUP);
                event.action = HANDHELD_ACTION_START_WIFI_SETUP;
                break;
            default: break;
            }
            event.changed = true;
        }
        return event;
    }
    const uint16_t direction = ui->stable_buttons & ~ui->blocked_buttons & ~releasing_button &
        (HANDHELD_BTN_UP | HANDHELD_BTN_DOWN | HANDHELD_BTN_LEFT | HANDHELD_BTN_RIGHT);
    if (!direction || (direction & (direction - 1))) {
        ui->repeat_button = 0;
        return event;
    }
    bool move = false;
    if (direction != ui->repeat_button) {
        ui->repeat_button = direction;
        ui->repeat_at_ms = now_ms + REPEAT_DELAY_MS;
        move = true;
    } else if ((int32_t)(now_ms - ui->repeat_at_ms) >= 0) {
        ui->repeat_at_ms = now_ms + REPEAT_MS;
        move = true;
    }
    if (move) {
        if (hidden) {
            event.action = HANDHELD_ACTION_KEY;
            event.key = direction == HANDHELD_BTN_LEFT ? 8 :
                        direction == HANDHELD_BTN_RIGHT ? 21 :
                        direction == HANDHELD_BTN_UP ? 11 : 10;
            event.changed = true;
            return event;
        }
        const unsigned before = ui->mode == HANDHELD_UI_MENU ? ui->menu_selection :
                                ui->mode == HANDHELD_UI_PROGRAMS ? ui->program_selection : ui->key_selection;
        navigate(ui, direction);
        const unsigned after = ui->mode == HANDHELD_UI_MENU ? ui->menu_selection :
                               ui->mode == HANDHELD_UI_PROGRAMS ? ui->program_selection : ui->key_selection;
        event.changed = before != after;
    }
    return event;
}

static unsigned row_address(unsigned row)
{
    return ((row & 7) << 7) + (row >> 3) * 40;
}

static uint8_t encode(unsigned char value, bool inverse)
{
    if (value >= 'a' && value <= 'z') value -= 'a' - 'A';
    if (value < 32 || value > 126) value = ' ';
    return inverse ? value & 0x3f : value | 0x80;
}

static void text(uint8_t *page, unsigned row, unsigned column, const char *value, bool inverse)
{
    if (row >= 24 || column >= 40 || !value) return;
    unsigned address = row_address(row) + column;
    while (*value && column++ < 40) page[address++] = encode((unsigned char)*value++, inverse);
}

static void line(uint8_t *page, unsigned row, const char *value, bool inverse)
{
    if (row >= 24) return;
    memset(page + row_address(row), encode(' ', inverse), 40);
    text(page, row, 1, value, inverse);
}

void handheld_ui_render(const handheld_ui_t *ui, const handheld_ui_info_t *info,
                        uint8_t page[HANDHELD_UI_PAGE_BYTES])
{
    memset(page, 0xa0, HANDHELD_UI_PAGE_BYTES);
    if (!handheld_ui_visible(ui)) return;
    char buffer[80];
    if (ui->mode == HANDHELD_UI_MENU) {
        line(page, 0, "APPLE II", true);
        for (unsigned i = 0; i < MENU_COUNT; ++i)
            line(page, 4 + i * 2, menu_items[i], ui->menu_selection == i);
        text(page, 20, 1, "A SELECT   B RESUME", false);
    } else if (ui->mode == HANDHELD_UI_PROGRAMS) {
        line(page, 0, "PROGRAMS", true);
        if (!ui->catalog.count) {
            line(page, 6, ui->program_status[0] ? "PROGRAM LIST UNAVAILABLE" :
                                                "NO BASIC PROGRAMS FOUND", false);
            if (ui->program_status[0]) line(page, 8, ui->program_status, false);
        } else {
            snprintf(buffer, sizeof(buffer), "%u/%u%s", ui->program_selection + 1,
                     ui->catalog.count, ui->catalog.truncated ? "  MORE PROGRAMS OMITTED" : "");
            line(page, 2, buffer, false);
            for (unsigned row = 0; row < PROGRAM_ROWS; ++row) {
                const unsigned index = ui->program_scroll + row;
                if (index >= ui->catalog.count) break;
                const a2_dos_program *program = &ui->catalog.programs[index];
                snprintf(buffer, sizeof(buffer), "%c %c %.30s", program->locked ? '*' : ' ',
                         program->type == 2 ? 'A' : program->type == 1 ? 'I' : '?', program->name);
                line(page, 4 + row, buffer, index == ui->program_selection);
            }
            if (ui->program_status[0]) line(page, 18, ui->program_status, false);
        }
        text(page, 20, 1, "A RUN   B MENU   UP/DOWN SELECT", false);
        text(page, 21, 1, "LEFT/RIGHT PAGE", false);
    } else if (ui->mode == HANDHELD_UI_KEYBOARD) {
        line(page, 0, "APPLE II KEYBOARD", true);
        if (info && info->input_line) text(page, 2, 0, info->input_line, false);
        else if (ui->has_last_key) {
            const char *name = ui->last_key == ' ' ? "SPACE" : ui->last_key == 8 ? "DELETE" :
                               ui->last_key == 13 ? "RETURN" : NULL;
            if (name) snprintf(buffer, sizeof(buffer), "LAST KEY: %s", name);
            else snprintf(buffer, sizeof(buffer), "LAST KEY: %c", ui->last_key);
            line(page, 2, buffer, false);
        }
        for (unsigned i = 0; i < KEY_COUNT; ++i) {
            char label[5];
            if (i < CHARACTER_COUNT) snprintf(label, sizeof(label), "[%c] ", character_keys[i]);
            else strcpy(label, i == CHARACTER_COUNT ? "[SP]" : i == CHARACTER_COUNT + 1 ? "[BS]" : "[CR]");
            text(page, 4 + (i / KEY_COLUMNS) * 2, (i % KEY_COLUMNS) * 4, label, ui->key_selection == i);
        }
        text(page, 18, 1, "SP SPACE  BS DELETE  CR RETURN", false);
        text(page, 20, 1, "A TYPE     B MENU", false);
    } else if (ui->mode == HANDHELD_UI_WIFI_SETUP) {
        line(page, 0, "WI-FI SETUP", true);
        line(page, 2, "1 JOIN THIS WI-FI NETWORK", false);
        line(page, 4, info && info->wifi_setup_ssid ? info->wifi_setup_ssid :
                                                   "STARTING...", false);
        line(page, 6, "PASSWORD:", false);
        line(page, 7, info && info->wifi_setup_password ? info->wifi_setup_password :
                                                       "PLEASE WAIT", false);
        line(page, 9, "2 OPEN IN YOUR PHONE BROWSER", false);
        line(page, 11, info && info->wifi_setup_url ? info->wifi_setup_url :
                                                   "HTTP://192.168.4.1", false);
        line(page, 13, "3 CHOOSE WI-FI AND SAVE", false);
        line(page, 16, info && info->wifi_setup_status ? info->wifi_setup_status :
                    "EXISTING SETTINGS KEPT UNTIL SAVE", false);
        text(page, 20, 1, "B CLOSE SETUP AND RETURN TO MENU", false);
    } else {
        line(page, 0, "DEVICE INFO", true);
        snprintf(buffer, sizeof(buffer), "IP: %s", info && info->ip ? info->ip : "NOT CONNECTED");
        line(page, 4, buffer, false);
        snprintf(buffer, sizeof(buffer), "FREE MEMORY: %u KIB", info ? (unsigned)(info->free_heap / 1024) : 0);
        line(page, 7, buffer, false);
        line(page, 10, "DISK:", false);
        line(page, 11, info && info->disk_name ? info->disk_name : "NO DISK", false);
        line(page, 14, "DISK STATE:", false);
        line(page, 15, info && info->disk_state ? info->disk_state : "UNKNOWN", false);
        text(page, 20, 1, "B MENU", false);
    }
    if (ui->mode != HANDHELD_UI_WIFI_SETUP && info && info->status)
        line(page, 21, info->status, false);
    text(page, 23, 1, "MENU RESUME", false);
}
