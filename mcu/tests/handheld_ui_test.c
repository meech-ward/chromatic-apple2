/* SPDX-License-Identifier: MIT */
#include "handheld_ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t now;
static handheld_ui_event_t settle(handheld_ui_t *ui, uint16_t buttons)
{
    handheld_ui_update(ui, buttons, now);
    now += 20;
    return handheld_ui_update(ui, buttons, now);
}
static void release(handheld_ui_t *ui) { ++now; settle(ui, 0); ++now; }
static handheld_ui_event_t tap(handheld_ui_t *ui, uint16_t button)
{
    release(ui);
    const handheld_ui_event_t event = settle(ui, button);
    release(ui);
    return event;
}
static void open_menu(handheld_ui_t *ui)
{
    handheld_ui_init(ui);
    now = 100;
    assert(tap(ui, HANDHELD_BTN_MENU).changed);
    assert(ui->mode == HANDHELD_UI_MENU);
}
static void decode_row(const uint8_t *page, unsigned row, char output[41])
{
    const unsigned start = ((row & 7) << 7) + (row >> 3) * 40;
    for (unsigned i = 0; i < 40; ++i) {
        uint8_t c = page[start + i] & 0x7f;
        if (c < 32) c += 64;
        output[i] = c;
    }
    output[40] = 0;
}

static void hidden_controls(void)
{
    handheld_ui_t ui;
    handheld_ui_init(&ui);
    assert(handheld_ui_update(&ui, HANDHELD_BTN_START, 100).action == HANDHELD_ACTION_NONE);
    assert(handheld_ui_update(&ui, HANDHELD_BTN_START, 119).action == HANDHELD_ACTION_NONE);
    handheld_ui_event_t event = handheld_ui_update(&ui, HANDHELD_BTN_START, 120);
    assert(event.action == HANDHELD_ACTION_KEY && event.key == 13 && !handheld_ui_visible(&ui));
    assert(handheld_ui_update(&ui, HANDHELD_BTN_START, 1000).action == HANDHELD_ACTION_NONE);
    now = 1001;
    assert(tap(&ui, HANDHELD_BTN_SELECT).key == 3);
    assert(tap(&ui, HANDHELD_BTN_A | HANDHELD_BTN_B).action == HANDHELD_ACTION_NONE);
    const uint16_t buttons[] = {HANDHELD_BTN_LEFT, HANDHELD_BTN_RIGHT, HANDHELD_BTN_UP, HANDHELD_BTN_DOWN};
    const uint8_t keys[] = {8, 21, 11, 10};
    for (unsigned i = 0; i < 4; ++i) {
        event = tap(&ui, buttons[i]);
        assert(event.action == HANDHELD_ACTION_KEY && event.key == keys[i]);
    }
    assert(tap(&ui, HANDHELD_BTN_LEFT | HANDHELD_BTN_RIGHT).action == HANDHELD_ACTION_NONE);
    settle(&ui, HANDHELD_BTN_LEFT);
    const uint32_t pressed = now;
    assert(handheld_ui_update(&ui, HANDHELD_BTN_LEFT, pressed + 349).action == HANDHELD_ACTION_NONE);
    assert(handheld_ui_update(&ui, HANDHELD_BTN_LEFT, pressed + 350).key == 8);
    assert(handheld_ui_update(&ui, HANDHELD_BTN_LEFT, pressed + 350).action == HANDHELD_ACTION_NONE);
    assert(handheld_ui_update(&ui, HANDHELD_BTN_LEFT, pressed + 450).key == 8);
    assert(handheld_ui_update(&ui, HANDHELD_BTN_LEFT, pressed + 10000).key == 8);
    assert(handheld_ui_update(&ui, HANDHELD_BTN_LEFT, pressed + 10000).action == HANDHELD_ACTION_NONE);
    now = pressed + 10001;
    release(&ui);
    event = settle(&ui, HANDHELD_BTN_MENU | HANDHELD_BTN_START | HANDHELD_BTN_SELECT | HANDHELD_BTN_DOWN);
    assert(event.action == HANDHELD_ACTION_NONE && ui.mode == HANDHELD_UI_MENU && ui.menu_selection == 0);
    assert(handheld_ui_update(&ui, ui.stable_buttons, now + 1000).action == HANDHELD_ACTION_NONE);
    now += 1001;
    release(&ui);
    event = settle(&ui, HANDHELD_BTN_MENU | HANDHELD_BTN_SELECT | HANDHELD_BTN_DOWN);
    assert(event.action == HANDHELD_ACTION_RESUME && !handheld_ui_visible(&ui));
    assert(handheld_ui_update(&ui, ui.stable_buttons, now + 1000).action == HANDHELD_ACTION_NONE);
    now += 1001;
    release(&ui);
    assert(tap(&ui, HANDHELD_BTN_SELECT).key == 3);
    assert(tap(&ui, HANDHELD_BTN_DOWN).key == 10);
}

static void injected_buttons(void)
{
    handheld_ui_t ui;
    handheld_ui_init(&ui);
    assert(!handheld_ui_press(&ui, 0));
    assert(!handheld_ui_press(&ui, 1u << 9));
    assert(!handheld_ui_press(&ui, HANDHELD_BTN_MENU | HANDHELD_BTN_A));
    assert(handheld_ui_press(&ui, HANDHELD_BTN_MENU));
    assert(!handheld_ui_press(&ui, HANDHELD_BTN_A));
    /* Neither a late first poll nor a late second poll can expire the press. */
    assert(!handheld_ui_update(&ui, 0, 10000).changed);
    assert(!handheld_ui_update(&ui, 0, 10019).changed);
    assert(!handheld_ui_press(&ui, HANDHELD_BTN_MENU));
    assert(handheld_ui_update(&ui, 0, 20000).changed);
    assert(ui.mode == HANDHELD_UI_MENU && ui.injected_releasing);
    assert(!handheld_ui_press(&ui, HANDHELD_BTN_A));
    /* The MENU early return must still have advanced the pulse to release. */
    assert(!handheld_ui_update(&ui, 0, 30000).changed);
    assert(!handheld_ui_press(&ui, HANDHELD_BTN_A));
    assert(!handheld_ui_update(&ui, 0, 30019).changed);
    assert(!handheld_ui_update(&ui, 0, 30020).changed);
    assert(!ui.injected_button && !ui.injected_releasing && !ui.stable_buttons);

    assert(handheld_ui_press(&ui, HANDHELD_BTN_DOWN));
    handheld_ui_update(&ui, 0, 40000);
    assert(handheld_ui_update(&ui, 0, 41000).changed && ui.menu_selection == 1);
    /* A long delay before release processing must not turn one pulse into repeats. */
    assert(!handheld_ui_update(&ui, 0, 42000).changed && ui.menu_selection == 1);
    assert(!handheld_ui_update(&ui, 0, 43000).changed && ui.menu_selection == 1);
    assert(!handheld_ui_update(&ui, 0, 44000).changed && ui.menu_selection == 1);
    assert(!ui.injected_button);

    /* A physical button held across a synthetic mode change stays blocked. */
    handheld_ui_init(&ui);
    handheld_ui_update(&ui, HANDHELD_BTN_A, 50000);
    handheld_ui_update(&ui, HANDHELD_BTN_A, 50020);
    assert(!handheld_ui_press(&ui, HANDHELD_BTN_A));
    assert(handheld_ui_press(&ui, HANDHELD_BTN_MENU));
    handheld_ui_update(&ui, HANDHELD_BTN_A, 50021);
    assert(handheld_ui_update(&ui, HANDHELD_BTN_A, 50041).changed);
    assert(ui.mode == HANDHELD_UI_MENU && (ui.blocked_buttons & HANDHELD_BTN_A));
    assert(handheld_ui_update(&ui, HANDHELD_BTN_A, 60000).action == HANDHELD_ACTION_NONE);
    assert(handheld_ui_update(&ui, HANDHELD_BTN_A, 60020).action == HANDHELD_ACTION_NONE);
    assert(ui.mode == HANDHELD_UI_MENU && !ui.injected_button);
    assert(!handheld_ui_press(&ui, HANDHELD_BTN_A));
    handheld_ui_update(&ui, 0, 60021);
    assert(!handheld_ui_press(&ui, HANDHELD_BTN_A)); /* Stable state is still held. */
    handheld_ui_update(&ui, 0, 60041);
    assert(handheld_ui_press(&ui, HANDHELD_BTN_A));
    handheld_ui_update(&ui, 0, 60042);
    assert(handheld_ui_update(&ui, 0, 60062).action == HANDHELD_ACTION_OPEN_PROGRAMS);
    assert(handheld_ui_update(&ui, 0, 70000).action == HANDHELD_ACTION_NONE);
    assert(handheld_ui_update(&ui, 0, 70020).action == HANDHELD_ACTION_NONE);
    assert(!ui.injected_button);

    /* A physical candidate that has not settled also prevents duplicate injection. */
    handheld_ui_init(&ui);
    handheld_ui_update(&ui, HANDHELD_BTN_START, 80000);
    assert(!handheld_ui_press(&ui, HANDHELD_BTN_START));
    handheld_ui_update(&ui, 0, 80001);
    assert(handheld_ui_press(&ui, HANDHELD_BTN_START));
    assert(handheld_ui_update(&ui, 0, 80002).action == HANDHELD_ACTION_NONE);
    handheld_ui_event_t event = handheld_ui_update(&ui, 0, 80022);
    assert(event.action == HANDHELD_ACTION_KEY && event.key == 13);
    assert(handheld_ui_update(&ui, 0, 90000).action == HANDHELD_ACTION_NONE);
    assert(handheld_ui_update(&ui, 0, 90020).action == HANDHELD_ACTION_NONE);
    assert(handheld_ui_press(&ui, HANDHELD_BTN_START));
}

static void program_browser(void)
{
    handheld_ui_t ui;
    a2_dos_catalog catalog = {.count = 25, .truncated = true};
    for (unsigned i = 0; i < catalog.count; ++i) {
        snprintf(catalog.programs[i].name, A2_DOS_NAME_BYTES, "PROGRAM %02u", i);
        catalog.programs[i].type = i == 1 ? 1 : 2;
        catalog.programs[i].locked = i & 1;
    }
    open_menu(&ui);
    assert(handheld_ui_pauses_emulation(&ui) && ui.menu_selection == 0);
    handheld_ui_event_t event = settle(&ui, HANDHELD_BTN_A);
    assert(event.action == HANDHELD_ACTION_OPEN_PROGRAMS && ui.mode == HANDHELD_UI_PROGRAMS);
    assert(handheld_ui_pauses_emulation(&ui));
    handheld_ui_set_programs(&ui, &catalog, NULL);
    assert(handheld_ui_update(&ui, HANDHELD_BTN_A, now + 1000).action == HANDHELD_ACTION_NONE);
    assert(ui.mode == HANDHELD_UI_PROGRAMS);
    now += 1001;
    release(&ui);
    struct { uint8_t before; uint8_t page[2048]; uint8_t after; } output = {.before=0x12, .after=0x34};
    char row[41];
    handheld_ui_render(&ui, NULL, output.page);
    decode_row(output.page, 0, row); assert(strstr(row, "PROGRAMS"));
    decode_row(output.page, 2, row); assert(strstr(row, "1/25") && strstr(row, "OMITTED"));
    decode_row(output.page, 4, row); assert(strstr(row, "A PROGRAM 00"));
    decode_row(output.page, 5, row); assert(strstr(row, "* I PROGRAM 01"));
    decode_row(output.page, 15, row); assert(strstr(row, "PROGRAM 11"));
    decode_row(output.page, 16, row); assert(!strstr(row, "PROGRAM"));
    assert(output.before == 0x12 && output.after == 0x34);
    assert(output.page[4 * 128] < 0x40 && output.page[5 * 128] >= 0x80);
    settle(&ui, HANDHELD_BTN_DOWN);
    const uint32_t pressed = now;
    assert(ui.program_selection == 1);
    handheld_ui_update(&ui, HANDHELD_BTN_DOWN, pressed + 349); assert(ui.program_selection == 1);
    handheld_ui_update(&ui, HANDHELD_BTN_DOWN, pressed + 350); assert(ui.program_selection == 2);
    handheld_ui_update(&ui, HANDHELD_BTN_DOWN, pressed + 10000); assert(ui.program_selection == 3);
    handheld_ui_update(&ui, HANDHELD_BTN_DOWN, pressed + 10000); assert(ui.program_selection == 3);
    now = pressed + 10001;
    release(&ui);
    handheld_ui_set_programs(&ui, &catalog, NULL);
    for (unsigned i = 0; i < 12; ++i) tap(&ui, HANDHELD_BTN_DOWN);
    assert(ui.program_selection == 12 && ui.program_scroll == 1);
    tap(&ui, HANDHELD_BTN_RIGHT);
    assert(ui.program_selection == 24 && ui.program_scroll == 13);
    assert(!tap(&ui, HANDHELD_BTN_DOWN).changed);
    handheld_ui_render(&ui, NULL, output.page);
    decode_row(output.page, 4, row); assert(strstr(row, "PROGRAM 13"));
    decode_row(output.page, 15, row); assert(strstr(row, "PROGRAM 24"));
    tap(&ui, HANDHELD_BTN_LEFT);
    assert(ui.program_selection == 12 && ui.program_scroll == 12);
    tap(&ui, HANDHELD_BTN_RIGHT);
    event = settle(&ui, HANDHELD_BTN_A | HANDHELD_BTN_DOWN);
    assert(event.action == HANDHELD_ACTION_RUN_PROGRAM && event.program_index == 24);
    assert(!handheld_ui_visible(&ui) && !handheld_ui_pauses_emulation(&ui));
    assert(handheld_ui_update(&ui, ui.stable_buttons, now + 1000).action == HANDHELD_ACTION_NONE);
    now += 1001;
    release(&ui);
    assert(tap(&ui, HANDHELD_BTN_DOWN).key == 10);

    open_menu(&ui);
    tap(&ui, HANDHELD_BTN_A);
    handheld_ui_set_programs(&ui, NULL, NULL);
    assert(tap(&ui, HANDHELD_BTN_A).action == HANDHELD_ACTION_NONE);
    assert(ui.mode == HANDHELD_UI_PROGRAMS);
    handheld_ui_render(&ui, NULL, output.page);
    decode_row(output.page, 6, row); assert(strstr(row, "NO BASIC PROGRAMS FOUND"));
    handheld_ui_set_programs(&ui, NULL, "DISK NOT READY");
    handheld_ui_render(&ui, NULL, output.page);
    decode_row(output.page, 6, row); assert(strstr(row, "UNAVAILABLE"));
    decode_row(output.page, 8, row); assert(strstr(row, "DISK NOT READY"));
    assert(tap(&ui, HANDHELD_BTN_B).action == HANDHELD_ACTION_NONE && ui.mode == HANDHELD_UI_MENU);
    assert(handheld_ui_pauses_emulation(&ui));
    tap(&ui, HANDHELD_BTN_DOWN); tap(&ui, HANDHELD_BTN_DOWN); tap(&ui, HANDHELD_BTN_A);
    assert(ui.mode == HANDHELD_UI_KEYBOARD && !handheld_ui_pauses_emulation(&ui));
    catalog.count = A2_DOS_MAX_PROGRAMS + 1;
    memset(catalog.programs[A2_DOS_MAX_PROGRAMS - 1].name, 'X', A2_DOS_NAME_BYTES);
    handheld_ui_set_programs(&ui, &catalog, "STATUS");
    assert(ui.catalog.count == A2_DOS_MAX_PROGRAMS && ui.program_selection == 0 && ui.program_scroll == 0);
    assert(ui.catalog.programs[A2_DOS_MAX_PROGRAMS - 1].name[A2_DOS_NAME_BYTES - 1] == 0);
    assert(catalog.programs[A2_DOS_MAX_PROGRAMS - 1].name[A2_DOS_NAME_BYTES - 1] == 'X');
}

static void wifi_controls(void)
{
    handheld_ui_t ui;
    assert(HANDHELD_UI_WIFI_SETUP == 5 && HANDHELD_UI_WIFI == 6);
    open_menu(&ui);
    tap(&ui, HANDHELD_BTN_UP);
    assert(ui.menu_selection == 7); /* Existing menu indices remain unchanged. */
    struct { uint8_t before; uint8_t page[2048]; uint8_t after; } guarded = {
        .before = 0x12, .after = 0x34
    };
    char row[41];
    handheld_ui_render(&ui, NULL, guarded.page);
    decode_row(guarded.page, 18, row); assert(strstr(row, "WI-FI") && !strstr(row, "SETUP"));
    handheld_ui_event_t event = settle(&ui, HANDHELD_BTN_A);
    assert(event.action == HANDHELD_ACTION_NONE && event.changed);
    assert(ui.mode == HANDHELD_UI_WIFI && handheld_ui_pauses_emulation(&ui));
    assert(handheld_ui_update(&ui, HANDHELD_BTN_A, now + 1000).action == HANDHELD_ACTION_NONE);
    now += 1001;
    release(&ui);
    handheld_ui_render(&ui, NULL, guarded.page);
    decode_row(guarded.page, 2, row); assert(strstr(row, "JOIN NETWORK") && strstr(row, "OFF"));
    decode_row(guarded.page, 5, row); assert(strstr(row, "NOT SET"));
    decode_row(guarded.page, 7, row); assert(strstr(row, "NOT CONNECTED"));
    decode_row(guarded.page, 11, row); assert(strstr(row, "SEE PRIVATE SETTINGS"));
    decode_row(guarded.page, 17, row); assert(strstr(row, "TURN WI-FI ON"));
    decode_row(guarded.page, 19, row); assert(strstr(row, "CHANGE WI-FI / PASSWORD"));
    decode_row(guarded.page, 21, row); assert(strstr(row, "KEPT WHEN OFF OR RESTARTED"));
    assert(tap(&ui, HANDHELD_BTN_A).action == HANDHELD_ACTION_TOGGLE_WIFI);
    assert(ui.mode == HANDHELD_UI_WIFI && ui.wifi_selection == 0);
    handheld_ui_info_t info = {
        .wifi_enabled = true, .wifi_hotspot = true, .wifi_ssid = "MY-CHROMATIC",
        .ip = "192.168.4.1", .wifi_status = "HOTSPOT READY", .ssh_user = "apple",
        .ssh_password = "MINT-LAKE-4826", .wifi_setup_ssid = "CHROMATIC-123ABC",
        .wifi_setup_password = "PINE-BOAT-1234", .wifi_setup_url = "http://192.168.4.1",
        .wifi_setup_status = "READY FOR SETUP", .status = "UNRELATED BASIC STATUS"
    };
    handheld_ui_render(&ui, &info, guarded.page);
    decode_row(guarded.page, 2, row); assert(strstr(row, "HOTSPOT") && strstr(row, "ON"));
    decode_row(guarded.page, 5, row); assert(strstr(row, "MY-CHROMATIC"));
    decode_row(guarded.page, 7, row); assert(strstr(row, "IP: 192.168.4.1"));
    decode_row(guarded.page, 9, row); assert(strstr(row, "SSH USER: APPLE"));
    decode_row(guarded.page, 11, row); assert(strstr(row, "MINT-LAKE-4826"));
    decode_row(guarded.page, 12, row); assert(strspn(row, " ") == 40);
    decode_row(guarded.page, 14, row); assert(strstr(row, "HOTSPOT READY"));
    decode_row(guarded.page, 17, row); assert(strstr(row, "TURN WI-FI OFF"));
    decode_row(guarded.page, 21, row); assert(!strstr(row, "UNRELATED"));
    assert(tap(&ui, HANDHELD_BTN_LEFT).action == HANDHELD_ACTION_NONE && !ui.wifi_selection);
    assert(tap(&ui, HANDHELD_BTN_DOWN).changed && ui.wifi_selection == 1);
    handheld_ui_render(&ui, &info, guarded.page);
    const unsigned selected_address = ((19 & 7) << 7) + (19 >> 3) * 40;
    assert(guarded.page[selected_address] < 0x40);
    assert(tap(&ui, HANDHELD_BTN_DOWN).changed && ui.wifi_selection == 0);
    assert(tap(&ui, HANDHELD_BTN_UP).changed && ui.wifi_selection == 1);
    char long_password[64];
    memset(long_password, 'A', 39);
    memset(long_password + 39, 'B', 24);
    long_password[63] = 0;
    info.ssh_password = long_password;
    handheld_ui_render(&ui, &info, guarded.page);
    decode_row(guarded.page, 11, row); assert(row[0] == ' ' && strspn(row + 1, "A") == 39);
    decode_row(guarded.page, 12, row); assert(strspn(row + 1, "B") == 24 && row[25] == ' ');
    assert(guarded.before == 0x12 && guarded.after == 0x34);
    event = settle(&ui, HANDHELD_BTN_A);
    assert(event.action == HANDHELD_ACTION_START_WIFI_SETUP && event.changed);
    assert(ui.mode == HANDHELD_UI_WIFI_SETUP && handheld_ui_pauses_emulation(&ui));
    assert(handheld_ui_update(&ui, HANDHELD_BTN_A, now + 1000).action == HANDHELD_ACTION_NONE);
    now += 1001;
    release(&ui);
    assert(tap(&ui, HANDHELD_BTN_A).action == HANDHELD_ACTION_NONE);
    assert(tap(&ui, HANDHELD_BTN_LEFT).action == HANDHELD_ACTION_NONE);
    handheld_ui_render(&ui, NULL, guarded.page);
    decode_row(guarded.page, 0, row); assert(strstr(row, "WI-FI SETUP"));
    decode_row(guarded.page, 4, row); assert(strstr(row, "STARTING"));
    decode_row(guarded.page, 7, row); assert(strstr(row, "PLEASE WAIT"));
    decode_row(guarded.page, 11, row); assert(strstr(row, "HTTP://192.168.4.1"));
    decode_row(guarded.page, 16, row); assert(strstr(row, "KEPT UNTIL SAVE"));
    handheld_ui_render(&ui, &info, guarded.page);
    decode_row(guarded.page, 2, row); assert(strstr(row, "JOIN THIS WI-FI NETWORK"));
    decode_row(guarded.page, 4, row); assert(strstr(row, "CHROMATIC-123ABC"));
    decode_row(guarded.page, 7, row); assert(strstr(row, "PINE-BOAT-1234"));
    decode_row(guarded.page, 9, row); assert(strstr(row, "PHONE BROWSER"));
    decode_row(guarded.page, 13, row); assert(strstr(row, "CHOOSE WI-FI AND SAVE"));
    decode_row(guarded.page, 16, row); assert(strstr(row, "READY FOR SETUP"));
    decode_row(guarded.page, 20, row); assert(strstr(row, "RETURN TO WI-FI"));
    decode_row(guarded.page, 21, row); assert(!strstr(row, "UNRELATED"));
    assert(guarded.before == 0x12 && guarded.after == 0x34);
    char long_value[100]; memset(long_value, 'X', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = 0;
    info.wifi_setup_ssid = info.wifi_setup_password = info.wifi_setup_url =
        info.wifi_setup_status = long_value;
    handheld_ui_render(&ui, &info, guarded.page);
    assert(guarded.before == 0x12 && guarded.after == 0x34);
    assert(tap(&ui, HANDHELD_BTN_B).action == HANDHELD_ACTION_STOP_WIFI_SETUP);
    assert(ui.mode == HANDHELD_UI_WIFI && ui.wifi_selection == 1);
    assert(tap(&ui, HANDHELD_BTN_A).action == HANDHELD_ACTION_START_WIFI_SETUP);
    assert(tap(&ui, HANDHELD_BTN_MENU).action == HANDHELD_ACTION_STOP_WIFI_SETUP);
    assert(!handheld_ui_visible(&ui) && !handheld_ui_pauses_emulation(&ui));

    /* Browsing status does not start or stop setup, and B preserves main selection. */
    assert(tap(&ui, HANDHELD_BTN_MENU).action == HANDHELD_ACTION_NONE);
    tap(&ui, HANDHELD_BTN_UP);
    assert(tap(&ui, HANDHELD_BTN_A).action == HANDHELD_ACTION_NONE);
    assert(ui.mode == HANDHELD_UI_WIFI && !ui.wifi_selection);
    assert(tap(&ui, HANDHELD_BTN_B).action == HANDHELD_ACTION_NONE);
    assert(ui.mode == HANDHELD_UI_MENU && ui.menu_selection == 7);
    tap(&ui, HANDHELD_BTN_A);
    assert(tap(&ui, HANDHELD_BTN_MENU).action == HANDHELD_ACTION_RESUME);
    assert(!handheld_ui_visible(&ui));
}

int main(void)
{
    hidden_controls();
    injected_buttons();
    program_browser();
    wifi_controls();
    handheld_ui_t ui;
    handheld_ui_init(&ui);
    assert(!handheld_ui_visible(&ui));
    handheld_ui_update(&ui, HANDHELD_BTN_MENU, 0);
    handheld_ui_update(&ui, 0, 10);
    assert(!handheld_ui_update(&ui, 0, 30).changed);
    assert(!handheld_ui_visible(&ui));
    handheld_ui_update(&ui, HANDHELD_BTN_MENU, 40);
    assert(!handheld_ui_update(&ui, HANDHELD_BTN_MENU, 59).changed);
    assert(handheld_ui_update(&ui, HANDHELD_BTN_MENU, 60).changed);
    assert(ui.mode == HANDHELD_UI_MENU);
    assert(handheld_ui_update(&ui, HANDHELD_BTN_MENU, 1000).action == HANDHELD_ACTION_NONE);
    assert(ui.mode == HANDHELD_UI_MENU);
    now = 1001;
    assert(tap(&ui, HANDHELD_BTN_MENU).action == HANDHELD_ACTION_RESUME);
    assert(!handheld_ui_visible(&ui));

    open_menu(&ui);
    settle(&ui, HANDHELD_BTN_DOWN);
    const uint32_t pressed = now;
    assert(ui.menu_selection == 1);
    handheld_ui_update(&ui, HANDHELD_BTN_DOWN, pressed + 349);
    assert(ui.menu_selection == 1);
    handheld_ui_update(&ui, HANDHELD_BTN_DOWN, pressed + 350);
    assert(ui.menu_selection == 2);
    handheld_ui_update(&ui, HANDHELD_BTN_DOWN, pressed + 350);
    assert(ui.menu_selection == 2);
    handheld_ui_update(&ui, HANDHELD_BTN_DOWN, pressed + 450);
    assert(ui.menu_selection == 3);
    handheld_ui_update(&ui, HANDHELD_BTN_DOWN, pressed + 10000);
    assert(ui.menu_selection == 4); // A late poll never catches up in a burst.
    now = pressed + 10001;
    release(&ui);

    open_menu(&ui);
    assert(tap(&ui, HANDHELD_BTN_UP | HANDHELD_BTN_DOWN).action == HANDHELD_ACTION_NONE);
    assert(ui.menu_selection == 0);
    assert(tap(&ui, HANDHELD_BTN_B).action == HANDHELD_ACTION_RESUME);

    open_menu(&ui);
    tap(&ui, HANDHELD_BTN_DOWN);
    tap(&ui, HANDHELD_BTN_DOWN);
    const handheld_ui_event_t opened = settle(&ui, HANDHELD_BTN_A);
    assert(opened.action == HANDHELD_ACTION_NONE && ui.mode == HANDHELD_UI_KEYBOARD);
    assert(handheld_ui_update(&ui, HANDHELD_BTN_A, now + 1000).action == HANDHELD_ACTION_NONE);
    now += 1001;
    release(&ui);
    handheld_ui_event_t event = tap(&ui, HANDHELD_BTN_A);
    assert(event.action == HANDHELD_ACTION_KEY && event.key == 'A');
    tap(&ui, HANDHELD_BTN_RIGHT);
    assert(tap(&ui, HANDHELD_BTN_A).key == 'B');
    tap(&ui, HANDHELD_BTN_DOWN);
    assert(tap(&ui, HANDHELD_BTN_A).key == 'L');
    tap(&ui, HANDHELD_BTN_UP);
    tap(&ui, HANDHELD_BTN_LEFT);
    for (unsigned i = 0; i < 5; ++i) tap(&ui, HANDHELD_BTN_DOWN);
    for (unsigned i = 0; i < 4; ++i) tap(&ui, HANDHELD_BTN_RIGHT);
    assert(tap(&ui, HANDHELD_BTN_A).key == ' ');
    tap(&ui, HANDHELD_BTN_RIGHT);
    assert(tap(&ui, HANDHELD_BTN_A).key == 8);
    tap(&ui, HANDHELD_BTN_RIGHT);
    assert(tap(&ui, HANDHELD_BTN_A).key == 13);
    for (unsigned i = 0; i < 20; ++i) tap(&ui, HANDHELD_BTN_RIGHT);
    assert(tap(&ui, HANDHELD_BTN_A).key == 13); // Right edge is bounded.
    assert(tap(&ui, HANDHELD_BTN_B).action == HANDHELD_ACTION_NONE && ui.mode == HANDHELD_UI_MENU);
    assert(tap(&ui, HANDHELD_BTN_B).action == HANDHELD_ACTION_RESUME);

    const handheld_ui_action_t actions[] = {
        HANDHELD_ACTION_OPEN_PROGRAMS, HANDHELD_ACTION_RESUME, HANDHELD_ACTION_NONE,
        HANDHELD_ACTION_SAVE_BASIC, HANDHELD_ACTION_LOAD_BASIC, HANDHELD_ACTION_RESTART,
        HANDHELD_ACTION_NONE, HANDHELD_ACTION_NONE
    };
    for (unsigned selection = 0; selection < 8; ++selection) {
        open_menu(&ui);
        for (unsigned i = 0; i < selection; ++i) tap(&ui, HANDHELD_BTN_DOWN);
        event = tap(&ui, HANDHELD_BTN_A);
        assert(event.action == actions[selection]);
        if (selection == 0) assert(ui.mode == HANDHELD_UI_PROGRAMS);
        else if (selection == 2) assert(ui.mode == HANDHELD_UI_KEYBOARD);
        else if (selection == 6) assert(ui.mode == HANDHELD_UI_INFO && handheld_ui_pauses_emulation(&ui));
        else if (selection == 7) assert(ui.mode == HANDHELD_UI_WIFI && handheld_ui_pauses_emulation(&ui));
        else assert(!handheld_ui_visible(&ui));
    }

    tap(&ui, HANDHELD_BTN_B);
    tap(&ui, HANDHELD_BTN_UP);
    tap(&ui, HANDHELD_BTN_A);
    assert(ui.mode == HANDHELD_UI_INFO);
    struct { uint8_t before; uint8_t page[2048]; uint8_t after; } guarded = {.before = 0x12, .after = 0x34};
    handheld_ui_info_t info = {.ip = "192.0.2.1", .free_heap = 137 * 1024,
        .disk_name = "Workshop.dsk", .disk_state = "Ready", .status = "BASIC READY",
        .input_line = "]PRINT 2+2"};
    handheld_ui_render(&ui, &info, guarded.page);
    char row[41];
    decode_row(guarded.page, 0, row); assert(strstr(row, "DEVICE INFO"));
    decode_row(guarded.page, 4, row); assert(strstr(row, "192.0.2.1"));
    decode_row(guarded.page, 7, row); assert(strstr(row, "137 KIB"));
    decode_row(guarded.page, 11, row); assert(strstr(row, "WORKSHOP.DSK"));
    assert(guarded.before == 0x12 && guarded.after == 0x34);
    tap(&ui, HANDHELD_BTN_B);
    handheld_ui_render(&ui, NULL, guarded.page);
    decode_row(guarded.page, 10, row); assert(strstr(row, "SAVE BASIC"));
    open_menu(&ui);
    tap(&ui, HANDHELD_BTN_DOWN); tap(&ui, HANDHELD_BTN_DOWN); tap(&ui, HANDHELD_BTN_A);
    handheld_ui_render(&ui, &info, guarded.page);
    decode_row(guarded.page, 2, row); assert(strstr(row, "]PRINT 2+2"));
    decode_row(guarded.page, 14, row); assert(strstr(row, "[SP][BS][CR]"));
    assert(guarded.before == 0x12 && guarded.after == 0x34);

    handheld_ui_init(&ui);
    handheld_ui_update(&ui, HANDHELD_BTN_MENU | HANDHELD_BTN_DOWN, 0xfffffff0u);
    event = handheld_ui_update(&ui, HANDHELD_BTN_MENU | HANDHELD_BTN_DOWN, 4);
    assert(event.changed && ui.mode == HANDHELD_UI_MENU && ui.menu_selection == 0);
    handheld_ui_update(&ui, HANDHELD_BTN_MENU | HANDHELD_BTN_DOWN, 1000);
    assert(ui.menu_selection == 0); // Transition blocks already-held directions.
    printf("PASS handheld UI: latched injected press/release/backpressure, program browser selection/scroll/run/empty/errors, Wi-Fi persistent-controls/toggle/setup/back/resume/password wrapping, pause policy, hidden Return/Break/cursor keys, debounce, bounded repeat, keyboard/menu actions, transition priority, text bounds, timer wrap; state=%zu bytes\n", sizeof(ui));
    return 0;
}
