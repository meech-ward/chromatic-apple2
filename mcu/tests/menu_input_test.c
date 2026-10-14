/* SPDX-License-Identifier: MIT */
#include "handheld_ui.h"
#include <assert.h>
#include <stdio.h>

static handheld_ui_t ui;
static uint16_t physical_buttons, remote_buttons;
static uint8_t physical_menu_sequence, menu_sequence, diagnostic_sequence;
static bool diagnostic_valid=true;

static bool shell_link_diag(uint16_t address, uint8_t *value)
{
    assert(address==33);
    if (!diagnostic_valid) return false;
    *value=diagnostic_sequence;
    return true;
}

/* Generated from main.c by the test runner, so the integration path itself is
 * exercised without starting ESP tasks or mocking the complete application. */
#include "menu_input_impl.inc"

static void reset(void)
{
    handheld_ui_init(&ui);
    physical_buttons=remote_buttons=0;
    physical_menu_sequence=menu_sequence=diagnostic_sequence=0;
    diagnostic_valid=true;
}

int main(void)
{
    reset();
    diagnostic_sequence=1; /* A complete tap ended long before the first poll. */
    assert(!sample_physical_buttons(0,0,10000).changed);
    assert(menu_sequence==1 && ui.injected_button==HANDHELD_BTN_MENU);
    assert(sample_physical_buttons(0,0,20000).changed && ui.mode==HANDHELD_UI_MENU);
    assert(!sample_physical_buttons(0,0,30000).changed);
    assert(!sample_physical_buttons(0,0,30020).changed && !ui.injected_button);
    /* Keeping the raw physical level high cannot duplicate the retained press. */
    assert(!sample_physical_buttons(0,1,40000).changed);
    assert(physical_buttons==HANDHELD_BTN_MENU);
    assert(!sample_physical_buttons(0,1,50000).changed && ui.mode==HANDHELD_UI_MENU);
    assert(!ui.stable_buttons);

    reset();
    diagnostic_sequence=7; /* Boot-early presses are coalesced, never baselined away. */
    sample_physical_buttons(0,0,0);
    assert(menu_sequence==7);
    assert(sample_physical_buttons(0,0,20).changed && ui.mode==HANDHELD_UI_MENU);
    sample_physical_buttons(0,0,40);
    sample_physical_buttons(0,0,60);
    assert(!sample_physical_buttons(0,0,10000).changed);

    reset();
    ui.mode=HANDHELD_UI_MENU;
    assert(handheld_ui_press(&ui,HANDHELD_BTN_DOWN));
    diagnostic_sequence=1;
    sample_physical_buttons(0,0,0);
    assert(menu_sequence==0); /* Another injection owns the debounce cycle. */
    assert(physical_menu_sequence==1);
    assert(sample_physical_buttons(0,0,20).changed && ui.menu_selection==1);
    assert(menu_sequence==0);
    sample_physical_buttons(0,0,40);
    sample_physical_buttons(0,0,60);
    assert(menu_sequence==0 && !ui.injected_button);
    sample_physical_buttons(0,0,80);
    assert(menu_sequence==1);
    assert(sample_physical_buttons(0,0,100).action==HANDHELD_ACTION_RESUME);
    sample_physical_buttons(0,0,120);
    sample_physical_buttons(0,0,140);
    assert(!sample_physical_buttons(0,0,10000).changed);

    reset();
    menu_sequence=255;
    diagnostic_sequence=0;
    sample_physical_buttons(0,0,0);
    assert(menu_sequence==0);
    assert(sample_physical_buttons(0,0,20).changed); /* Ordinary 8-bit wrap. */
    reset();
    menu_sequence=7;
    assert(!sample_physical_buttons(0,0,0).changed && menu_sequence==0);
    assert(!sample_physical_buttons(0,0,10000).changed); /* Live reset is quiet. */
    diagnostic_sequence=1;
    sample_physical_buttons(0,0,10001);
    assert(sample_physical_buttons(0,0,10021).changed);

    reset();
    diagnostic_valid=false;
    diagnostic_sequence=1;
    sample_physical_buttons(HANDHELD_BTN_START,1,0);
    handheld_ui_event_t event=sample_physical_buttons(HANDHELD_BTN_START,1,20);
    assert(event.action==HANDHELD_ACTION_KEY && event.key==13);
    assert(menu_sequence==0 && ui.mode==HANDHELD_UI_HIDDEN);
    assert(physical_buttons==(HANDHELD_BTN_START|HANDHELD_BTN_MENU));
    diagnostic_valid=true;
    sample_physical_buttons(0,0,40);
    assert(sample_physical_buttons(0,0,60).changed && ui.mode==HANDHELD_UI_MENU);

    reset();
    diagnostic_valid=false;
    remote_buttons=HANDHELD_BTN_MENU; /* All nine remote bits remain functional. */
    sample_physical_buttons(0,0,0);
    assert(sample_physical_buttons(0,0,20).changed && ui.mode==HANDHELD_UI_MENU);
    puts("PASS production MENU sampling: retained early/late presses, coalescing, busy-injection retention, raw-level deduplication, wrap/reset, diagnostic errors and remote controls");
}
