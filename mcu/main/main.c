/* SPDX-License-Identifier: MIT */
#include "apple2.h"
#include "audio.h"
#include "control.h"
#include "cart_commands.h"
#include "cart_storage.h"
#include "handheld_ui.h"
#include "rom_image.h"
#include "shell_link.h"
#include "ssh/ssh_terminal.h"
#include "ssh/ssh_transport.h"
#include "video_link.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_console.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static apple2_t machine;
static SemaphoreHandle_t machine_lock;
static bool running;
static bool remote_typing;
static char console_output[4096];
static uint8_t framebuffer[A2_FRAME_BYTES] __attribute__((aligned(4)));
static atomic_uint video_frames;
static atomic_uint video_crc;
static atomic_int video_error;
static handheld_ui_t ui;
static uint8_t overlay[HANDHELD_UI_PAGE_BYTES];
static char pending_keys[64];
static size_t pending_position;
static bool import_active, import_resume;
static uint16_t remote_buttons,physical_buttons;
static uint8_t physical_menu_sequence, menu_sequence;
static bool launch_pending;
static bool catalog_pending;
static unsigned launch_ready;
static int64_t launch_deadline;
static char launch_name[A2_DOS_NAME_BYTES];
static char ui_status[41];

static void clear_keys(void)
{
    pending_keys[0]=0; pending_position=0;
    launch_pending=false; launch_ready=0;
    catalog_pending=false;
}

static void refresh_programs(void)
{
    static a2_dos_catalog catalog;
    if (cart_storage_saving()) {
        catalog_pending=true;
        handheld_ui_set_programs(&ui,NULL,"FINISHING DISK WRITE - PLEASE WAIT");
        return;
    }
    catalog_pending=false;
    const bool ok=cart_storage_programs(&catalog);
    handheld_ui_set_programs(&ui,&catalog,ok ? NULL : "DISK CATALOG UNAVAILABLE");
}

static bool launch_program(unsigned index)
{
    if (index>=ui.catalog.count) return false;
    snprintf(launch_name,sizeof(launch_name),"%s",ui.catalog.programs[index].name);
    clear_keys();
    machine.buttons=0;
    if (!cart_storage_reset()) return false;
    launch_pending=true; launch_ready=0;
    launch_deadline=esp_timer_get_time()+90000000;
    running=true;
    return true;
}

bool control_import_begin(const char *name)
{
    xSemaphoreTake(machine_lock, portMAX_DELAY);
    clear_keys();
    import_resume=running;
    running=false;
    if (!cart_storage_import_begin(name)) {
        running=import_resume;
        xSemaphoreGive(machine_lock);
        return false;
    }
    import_active=true;
    return true;
}
bool control_import_write(const void *data,size_t size)
{ return import_active && cart_storage_import_write(data,size); }
bool control_import_finish(void)
{
    if (!import_active) return false;
    const bool ok=cart_storage_import_finish();
    if (!ok) cart_storage_import_abort();
    import_active=false;
    running=import_resume;
    xSemaphoreGive(machine_lock);
    return ok;
}
void control_import_abort(void)
{
    if (!import_active) return;
    cart_storage_import_abort();
    import_active=false;
    running=import_resume;
    xSemaphoreGive(machine_lock);
}
int control_storage_error(void) { return cart_storage_error(); }

static void append(char *output, size_t capacity, const char *format, ...)
{
    const size_t used = strlen(output);
    if (used >= capacity) return;
    va_list args;
    va_start(args, format);
    vsnprintf(output + used, capacity - used, format, args);
    va_end(args);
}

static void render_ui_unlocked(void)
{
    char input[41];
    apple2_text_line(&machine,23,input);
    const handheld_ui_info_t info={.ip=network_ip(),
        .free_heap=heap_caps_get_free_size(MALLOC_CAP_8BIT),
        .disk_name=cart_storage_name(),
        .disk_state=cart_storage_error() ? "STORAGE NOT READY" :
                    cart_storage_saving() ? "SAVING" : "READY",
        .status=ui_status[0] ? ui_status : NULL,.input_line=input};
    handheld_ui_render(&ui,&info,overlay);
}

static void text_unlocked(char *output, size_t capacity)
{
    output[0] = 0;
    char line[41];
    if (handheld_ui_visible(&ui)) render_ui_unlocked();
    for (unsigned row = 0; row < 24; ++row) {
        if (handheld_ui_visible(&ui)) {
            const unsigned address=((row&7)<<7)+(row>>3)*40;
            for (unsigned col=0;col<40;++col) {
                uint8_t value=overlay[address+col]&0x7f;
                line[col]=value<32 ? value+64 : value;
            }
            line[40]=0;
            if (ui.mode==HANDHELD_UI_MENU && row==4+ui.menu_selection*2) line[0]='>';
            if (ui.mode==HANDHELD_UI_PROGRAMS && ui.catalog.count &&
                row==4+ui.program_selection-ui.program_scroll) line[0]='>';
        } else if (!machine.text_mode && (!machine.mixed_mode || row<20)) {
            snprintf(line,sizeof(line),"%s",row==0 ? "GRAPHICS ON CHROMATIC DISPLAY" :
                     row==1 ? "TERMINAL KEYBOARD REMAINS ACTIVE" : "");
        } else apple2_text_line(&machine, row, line);
        append(output, capacity, "%s\n", line);
    }
}

void control_text(char *output, size_t capacity)
{
    xSemaphoreTake(machine_lock, portMAX_DELAY);
    text_unlocked(output, capacity);
    xSemaphoreGive(machine_lock);
}

bool control_terminal_text(char *output,size_t capacity)
{
    if (!output || !capacity || xSemaphoreTake(machine_lock,pdMS_TO_TICKS(2))!=pdTRUE) return false;
    text_unlocked(output,capacity);
    xSemaphoreGive(machine_lock);
    return true;
}

bool control_terminal_key(unsigned char key)
{
    if (xSemaphoreTake(machine_lock,pdMS_TO_TICKS(2))!=pdTRUE) return false;
    bool accepted=false;
    uint16_t button=0;
    if (key==29) button=HANDHELD_BTN_MENU;
    else if (handheld_ui_visible(&ui)) {
        if (key==11) button=HANDHELD_BTN_UP;
        else if (key==10) button=HANDHELD_BTN_DOWN;
        else if (key==8) button=HANDHELD_BTN_LEFT;
        else if (key==21) button=HANDHELD_BTN_RIGHT;
        else if (key==13 || key==' ') button=HANDHELD_BTN_A;
        else if (key==27) button=HANDHELD_BTN_B;
    }
    if (button) {
        accepted=handheld_ui_press(&ui,button);
    } else if (running && !launch_pending && !remote_typing && !pending_keys[0] &&
               !handheld_ui_pauses_emulation(&ui)) {
        if (key==127) key=8;
        accepted=apple2_key(&machine,(uint8_t)toupper(key));
    } else if (handheld_ui_pauses_emulation(&ui)) accepted=true; /* Unbound menu key. */
    xSemaphoreGive(machine_lock);
    return accepted;
}

void control_status(char *output, size_t capacity)
{
    xSemaphoreTake(machine_lock, portMAX_DELAY);
    a2_audio_status sound;
    audio_status(&sound);
    snprintf(output, capacity,
        "{\"version\":\"apple2-foundation-1\",\"rom\":\"%s\",\"rom_bytes\":%u,"
        "\"ram_bytes\":49152,\"machine_bytes\":%u,\"pc\":%u,\"cycles\":%" PRIu64 ","
        "\"running\":%s,\"key_ready\":%s,\"diagnostic\":[%u,%u,%u],"
        "\"free_heap\":%u,\"minimum_free_heap\":%u,\"video_frames\":%u,\"video_crc\":%u,\"video_error\":%d,\"storage_error\":%d,\"storage_saving\":%s,"
        "\"audio_samples\":%u,\"audio_error\":%d,\"audio_armed\":%s,\"audio_underruns\":%u,\"audio_overflows\":%u,"
        "\"ui_mode\":%u,\"program_selection\":%u,\"program_count\":%u,\"launch_pending\":%s,\"buttons\":%u,\"physical_buttons\":%u,"
        "\"physical_menu_sequence\":%u,\"physical_menu_acknowledged\":%u,\"text_mode\":%s,\"ip\":\"%s\"}\n",
        a2_rom_kind, (unsigned)a2_rom_size, (unsigned)sizeof(machine), machine.cpu.PC,
        machine.cpu.total_cycle, running ? "true" : "false", machine.key_ready ? "true" : "false",
        machine.ram[0x200], machine.ram[0x201], machine.ram[0x202],
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
        atomic_load(&video_frames),atomic_load(&video_crc),atomic_load(&video_error),cart_storage_error(),cart_storage_saving() ? "true" : "false",
        (unsigned)sound.samples,sound.error,sound.armed ? "true" : "false",(unsigned)sound.underruns,(unsigned)sound.overflows,
        (unsigned)ui.mode,ui.program_selection,ui.catalog.count,launch_pending ? "true" : "false",machine.buttons,physical_buttons,
        (unsigned)physical_menu_sequence,(unsigned)menu_sequence,machine.text_mode ? "true" : "false",network_ip());
    xSemaphoreGive(machine_lock);
}

static bool number(const char *text, unsigned maximum, unsigned *value)
{
    if (!text || *text == '-' || *text == '+') return false;
    char *end;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno || end == text || *end || parsed > maximum) return false;
    *value = parsed;
    return true;
}

static void reboot_task(void *unused)
{
    (void)unused;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void run_instructions(unsigned instructions)
{
    while (instructions) {
        const unsigned batch = instructions < 4096 ? instructions : 4096;
        apple2_run(&machine, batch);
        cart_storage_tick();
        instructions -= batch;
        if (instructions) vTaskDelay(1);
    }
}

static bool type_text(const char *text)
{
    for (size_t i = 0; text[i]; ++i) {
        uint8_t key = text[i];
        if (key == '\\' && text[i + 1]) {
            key = text[++i];
            if (key == 'r' || key == 'n') key = 13;
            else if (key == 'b') key = 8;
            else if (key == 'e') key = 27;
        }
        if (key == '\n') key = 13;
        key = toupper(key);
        const int64_t deadline=esp_timer_get_time()+10000000;
        while (machine.key_ready && esp_timer_get_time()<deadline) {
            if (running) {
                xSemaphoreGive(machine_lock);
                vTaskDelay(1);
                xSemaphoreTake(machine_lock,portMAX_DELAY);
            } else run_instructions(5000);
        }
        if (!apple2_key(&machine, key)) return false;
        if (!running) run_instructions(5000);
    }
    const int64_t deadline=esp_timer_get_time()+10000000;
    while (running && machine.key_ready && esp_timer_get_time()<deadline) {
        xSemaphoreGive(machine_lock);
        vTaskDelay(1);
        xSemaphoreTake(machine_lock,portMAX_DELAY);
    }
    if (machine.key_ready) return false;
    return true;
}

bool control_command(const char *command, char *output, size_t capacity)
{
    if (!capacity) return false;
    output[0] = 0;
    if (!command || strlen(command) > 255) { append(output, capacity, "ERROR command too long\n"); return false; }
    xSemaphoreTake(machine_lock, portMAX_DELAY);
    bool ok = true;
    if (!strncmp(command, "a2 type ", 8)) {
        if (handheld_ui_pauses_emulation(&ui)) {
            append(output,capacity,"ERROR close the handheld menu before typing\n");
            xSemaphoreGive(machine_lock); return false;
        }
        remote_typing=true;
        ok = type_text(command + 8);
        remote_typing=false;
        append(output, capacity, ok ? "OK typed\n" : "ERROR keyboard latch not consumed\n");
        xSemaphoreGive(machine_lock);
        return ok;
    }
    char copy[256];
    strcpy(copy, command);
    char *argv[6], *save;
    int argc = 0;
    for (char *word = strtok_r(copy, " \t\r\n", &save); word && argc < 6;
         word = strtok_r(NULL, " \t\r\n", &save)) argv[argc++] = word;
    unsigned value = 0;
    if (argc == 2 && !strcmp(argv[0], "a2") && !strcmp(argv[1], "reset")) {
        clear_keys();
        ok=cart_storage_reset();
        if (ok) run_instructions(2000);
        append(output, capacity, "%s reset pc=%04x storage=%d\n",ok ? "OK" : "ERROR",
               machine.cpu.PC,cart_storage_error());
    } else if (argc == 3 && !strcmp(argv[0], "a2") && !strcmp(argv[1], "run") &&
               number(argv[2], 1000000, &value)) {
        const uint64_t before = machine.cpu.total_cycle;
        const int64_t start = esp_timer_get_time();
        run_instructions(value);
        append(output, capacity, "OK instructions=%u cycles=%" PRIu64 " elapsed_us=%" PRId64 " pc=%04x\n",
               value, machine.cpu.total_cycle - before, esp_timer_get_time() - start, machine.cpu.PC);
    } else if (argc==3 && !strcmp(argv[0],"a2") && !strcmp(argv[1],"buttons") && number(argv[2],511,&value)) {
        remote_buttons=value;
        append(output,capacity,"OK buttons=%u\n",value);
    } else if (argc==2 && !strcmp(argv[0],"a2") && !strcmp(argv[1],"programs")) {
        refresh_programs();
        ok=cart_storage_error()==0;
        for (unsigned i=0;i<ui.catalog.count;++i)
            append(output,capacity,"%u %s\n",i,ui.catalog.programs[i].name);
        if (!ui.catalog.count) append(output,capacity,ok ? "NO BASIC PROGRAMS\n" : "ERROR catalog unavailable\n");
    } else if (argc == 3 && !strcmp(argv[0], "a2") && !strcmp(argv[1], "key") && number(argv[2], 127, &value)) {
        ok = apple2_key(&machine, value);
        if (ok) run_instructions(5000);
        append(output, capacity, ok ? "OK key\n" : "ERROR keyboard latch busy\n");
    } else if (argc == 2 && !strcmp(argv[0], "a2") && !strcmp(argv[1], "text")) {
        text_unlocked(output, capacity);
    } else if (argc == 2 && !strcmp(argv[0], "a2") && !strcmp(argv[1], "stats")) {
        xSemaphoreGive(machine_lock);
        control_status(output, capacity);
        return true;
    } else if (argc == 2 && !strcmp(argv[0], "a2") &&
               (!strcmp(argv[1], "start") || !strcmp(argv[1], "stop"))) {
        if (!strcmp(argv[1],"stop")) ok=cart_storage_sync();
        if (ok) running = !strcmp(argv[1], "start");
        append(output, capacity, "%s running=%u storage=%d\n",ok ? "OK" : "ERROR",
               running,cart_storage_error());
    } else if (argc == 2 && !strcmp(argv[0], "a2") && !strcmp(argv[1], "reboot")) {
        ok=cart_storage_command(2,(char *[]){"storage","sync"},output,capacity);
        if (ok) ok=xTaskCreate(reboot_task,"reboot",2048,NULL,3,NULL)==pdPASS;
    } else if (argc==2 && !strcmp(argv[0],"ssh") && !strcmp(argv[1],"status")) {
        append(output,capacity,"SSH enabled=%u listening=%u connected=%u fingerprint=%s\n",
               ssh_transport_enabled(),ssh_transport_listening(),ssh_transport_connected(),ssh_transport_fingerprint());
    } else if (argc == 2 && !strcmp(argv[0], "audio") && !strcmp(argv[1], "status")) {
        a2_audio_status sound;
        audio_status(&sound);
        append(output,capacity,"AUDIO samples=%u buffered=%u armed=%u error=%d underruns=%u overflows=%u resets=%u\n",
               sound.samples,sound.buffered,sound.armed,sound.error,sound.underruns,sound.overflows,sound.resets);
        for (unsigned i=15;i<=32;++i) {
            uint8_t byte=0;
            if (!shell_link_diag(i,&byte)) { ok=false; break; }
            append(output,capacity," %u=%02x",i,byte);
        }
        append(output,capacity,"\n");
    } else if (argc == 2 && !strcmp(argv[0], "cart") && !strcmp(argv[1], "lcd")) {
        append(output, capacity, "LCD_DIAG");
        for (unsigned i=5; i<=14; ++i) {
            uint8_t byte=0;
            if (!shell_link_diag(i,&byte)) { ok=false; break; }
            append(output,capacity," %u=%02x",i,byte);
        }
        append(output,capacity,"\n");
    } else if (argc && !strcmp(argv[0], "cart")) {
        ok=cart_command(argc,argv,output,capacity);
    } else if (argc && !strcmp(argv[0], "storage")) {
        clear_keys();
        ok=cart_storage_command(argc,argv,output,capacity);
    } else {
        ok = false;
        append(output, capacity, "ERROR commands: a2 reset/run <n>/key <ascii>/type <text>/text/stats/start/stop; cart diag/header/lcd/read/bank/ram-digest/ram-test/release; storage probe/format/mount/catalog/boot/eject/sync\n");
    }
    if (!ok && !output[0]) append(output, capacity, "ERROR %s\n", shell_link_error());
    xSemaphoreGive(machine_lock);
    return ok;
}

static int console_command(int argc, char **argv)
{
    char command[256] = "";
    for (int i = 0; i < argc; ++i) {
        if (strlen(command) + strlen(argv[i]) + 2 > sizeof(command)) return 1;
        if (i) strcat(command, " ");
        strcat(command, argv[i]);
    }
    const bool ok = control_command(command, console_output, sizeof(console_output));
    printf("%s", console_output);
    return ok ? 0 : 1;
}

static void cpu_task(void *unused)
{
    (void)unused;
    int64_t target = esp_timer_get_time();
    for (;;) {
        xSemaphoreTake(machine_lock, portMAX_DELAY);
        /* Let DOS finish an in-progress write before the menu freezes it.
         * Pausing midway through a sector must not force an incomplete flush. */
        if (running && (!handheld_ui_pauses_emulation(&ui) || cart_storage_saving())) {
            if (launch_pending) {
                const bool ready=!machine.key_ready && machine.cpu.PC>=0xc27d && machine.cpu.PC<=0xc28e;
                launch_ready=ready ? launch_ready+1 : 0;
                if (launch_ready>=3) {
                    snprintf(pending_keys,sizeof(pending_keys),"\x18\x04" "RUN %s\r",launch_name);
                    pending_position=0; launch_pending=false;
                } else if (esp_timer_get_time()>launch_deadline) {
                    launch_pending=false;
                    snprintf(ui_status,sizeof(ui_status),"DOS START TIMEOUT - USE RESTART");
                }
            }
            if (!remote_typing && !machine.key_ready && pending_keys[pending_position]) {
                apple2_key(&machine,pending_keys[pending_position++]);
                if (!pending_keys[pending_position]) pending_keys[pending_position=0]=0;
            }
            const uint64_t before = machine.cpu.total_cycle;
            apple2_run(&machine, 4096);
            cart_storage_tick();
            target += (machine.cpu.total_cycle - before) * 1000000 / 1023000;
            // Host flash service pauses emulated time; resume at normal speed
            // instead of replaying the pause as a burst of CPU/audio activity.
            if (target<esp_timer_get_time()-20000) target=esp_timer_get_time();
        } else {
            target=esp_timer_get_time();
            if (launch_pending) launch_deadline=target+90000000;
        }
        xSemaphoreGive(machine_lock);
        const int64_t ahead = target - esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(ahead > 1000 ? (ahead + 999) / 1000 : 1));
    }
}

static handheld_ui_event_t sample_physical_buttons(uint8_t buttons, uint8_t menu,
                                                    uint32_t now_ms)
{
    physical_buttons=buttons | ((menu&1)<<8);
    uint8_t sequence;
    if (shell_link_diag(33,&sequence)) {
        physical_menu_sequence=sequence;
        if (sequence!=menu_sequence) {
            /* Coalesce presses missed during storage service into one UI event.
             * Zero normally means a live FPGA reset; 255 -> 0 is a valid wrap. */
            if (!sequence && menu_sequence!=UINT8_MAX) menu_sequence=0;
            else if (handheld_ui_press(&ui,HANDHELD_BTN_MENU)) menu_sequence=sequence;
        }
    }
    /* Physical MENU is delivered only by the retained sequence. Its raw level
     * remains in status, while remote controls retain their complete bit mask. */
    return handheld_ui_update(&ui,buttons | remote_buttons,now_ms);
}

static void input_task(void *unused)
{
    (void)unused;
    for (;;) {
        xSemaphoreTake(machine_lock, portMAX_DELAY);
        uint8_t buttons=0,menu=0;
        if (shell_link_diag(1,&buttons) && shell_link_diag(2,&menu)) {
            const handheld_ui_event_t event=sample_physical_buttons(buttons,menu,
                                                                   esp_timer_get_time()/1000);
            machine.buttons=handheld_ui_visible(&ui) ? 0 : ui.stable_buttons & ~ui.blocked_buttons & 255;
            if (event.action==HANDHELD_ACTION_OPEN_PROGRAMS) refresh_programs();
            if (catalog_pending && !cart_storage_saving()) refresh_programs();
            if (event.action==HANDHELD_ACTION_RUN_PROGRAM) {
                if (!launch_program(event.program_index)) {
                    ui.mode=HANDHELD_UI_PROGRAMS;
                    handheld_ui_set_programs(&ui,&ui.catalog,"DISK ERROR - NOT STARTED");
                } else ui_status[0]=0;
            }
            const char *keys=NULL;
            if (event.action==HANDHELD_ACTION_CATALOG) keys="\x18\x04" "CATALOG\r";
            if (event.action==HANDHELD_ACTION_SAVE_BASIC) keys="\x18\x04" "SAVE PROGRAM\r";
            if (event.action==HANDHELD_ACTION_LOAD_BASIC) keys="\x18\x04" "LOAD PROGRAM\r";
            if (keys) { snprintf(pending_keys,sizeof(pending_keys),"%s",keys); pending_position=0; }
            if (event.action==HANDHELD_ACTION_KEY && !pending_keys[0] && !launch_pending && !remote_typing) {
                pending_keys[0]=event.key; pending_keys[1]=0; pending_position=0;
            }
            if (event.action==HANDHELD_ACTION_RESTART) {
                clear_keys();
                cart_storage_reset();
            }
        }
        xSemaphoreGive(machine_lock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void video_task(void *unused)
{
    (void)unused;
    uint32_t previous_crc=0;
    int64_t last_send=0;
    for (;;) {
        const int64_t now=esp_timer_get_time();
        xSemaphoreTake(machine_lock, portMAX_DELAY);
        cart_storage_autoboot();
        if (handheld_ui_visible(&ui)) {
            render_ui_unlocked();
            a2_render_page(overlay,(now/500000)&1,15,framebuffer);
        } else a2_render_screen(machine.ram,machine.text_mode,machine.page_two,
                     machine.mixed_mode,machine.hires_mode,(now/500000)&1,12,framebuffer);
        xSemaphoreGive(machine_lock);
        const uint32_t crc=esp_crc32_le(0, framebuffer, sizeof(framebuffer));
        if (!last_send || crc!=previous_crc || now-last_send>=2000000) {
            const esp_err_t result=video_link_frame(framebuffer);
            atomic_store(&video_error, result);
            if (result==ESP_OK) {
                atomic_fetch_add(&video_frames, 1);
                atomic_store(&video_crc,crc);
                previous_crc=crc;
                last_send=now;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    machine_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(machine_lock ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(apple2_init(&machine, a2_rom_image, a2_rom_size) ? ESP_OK : ESP_ERR_INVALID_SIZE);
    apple2_run(&machine, 2000);
    running = true;
    cart_storage_init(&machine,a2_disk_rom,a2_disk_rom_size);
    handheld_ui_init(&ui);
    shell_link_init();
    (void)audio_start(&machine);
    video_link_init();
    esp_console_repl_t *repl;
    esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    config.prompt = "apple2> ";
    config.max_cmdline_length = 256;
    config.task_stack_size = 6144;
    esp_console_dev_uart_config_t uart = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    uart.rx_gpio_num = 3;
    uart.tx_gpio_num = 1;
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart, &config, &repl));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, 1, 3, -1, -1));
    const esp_console_cmd_t commands[] = {
        {.command = "a2", .help = "Apple II CPU/text control", .func = console_command},
        {.command = "cart", .help = "Original FPGA cartridge controls", .func = console_command},
        {.command = "storage", .help = "Cartridge floppy disk storage", .func = console_command},
        {.command = "audio", .help = "Speaker diagnostics", .func = console_command},
        {.command = "ssh", .help = "SSH terminal status", .func = console_command},
    };
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        ESP_ERROR_CHECK(esp_console_cmd_register(commands + i));
    ESP_ERROR_CHECK(esp_console_register_help_command());
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    printf("CHROMATIC_APPLE2_READY rom=%s ram=49152 machine=%u diagnostic=%02x free_heap=%u\n",
           a2_rom_kind, (unsigned)sizeof(machine), machine.ram[0x200],
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ESP_ERROR_CHECK(xTaskCreate(cpu_task, "apple2_cpu", 6144, NULL, 4, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(input_task, "apple2_input", 6144, NULL, 3, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(video_task, "apple2_video", 6144, NULL, 2, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    network_start();
    ssh_terminal_start();
}
