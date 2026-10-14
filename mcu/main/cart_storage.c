/* SPDX-License-Identifier: MIT */
#include "cart_storage.h"
#include "shell_link.h"
#include "a2_is29gl032.h"
#include "a2_import.h"
#include "a2_disk_overlay.h"
#include "disk2.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>

static apple2_t *machine;
static const uint8_t *slot_rom;
static size_t slot_size;
static a2_is29gl032 flash;
static a2_storage filesystem;
static a2_disk_image image;
static a2_disk_overlay journal;
static uint8_t track_scratch[4096];
static a2_import upload;
static disk2_t disk;
static bool initialized, auto_attempted;
static char active_name[65], upload_name[65];
static int last_error;
static int commit_error;
static bool pending_sync;

static bool image_character(char c)
{
    return (c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9');
}

/* Match the importer's root-level .dsk names, and retain the complete path. */
static bool image_path(const char *name,char path[65])
{
    if (!name) return false;
    if (*name=='/') ++name;
    size_t length=0;
    while (length<=A2_IMPORT_NAME_MAX && name[length]) ++length;
    if (length<5 || length>A2_IMPORT_NAME_MAX || !image_character(name[0])) return false;
    for (size_t i=1;i<length;++i)
        if (!(image_character(name[i]) || name[i]==' ' || name[i]=='_' || name[i]=='-' || name[i]=='.') ||
            (name[i]=='.' && name[i-1]=='.')) return false;
    if (name[length-4]!='.' || (name[length-3]!='d' && name[length-3]!='D') ||
        (name[length-2]!='s' && name[length-2]!='S') || (name[length-1]!='k' && name[length-1]!='K'))
        return false;
    path[0]='/';
    memcpy(path+1,name,length+1);
    return true;
}

static int selected_image(char path[65])
{
    nvs_handle_t settings;
    esp_err_t error=nvs_open("a2_disk",NVS_READONLY,&settings);
    if (error==ESP_ERR_NVS_NOT_FOUND) { strcpy(path,"/Workshop.dsk"); return 0; }
    if (error!=ESP_OK) return error;
    char saved[65];
    size_t length=sizeof(saved);
    error=nvs_get_str(settings,"image",saved,&length);
    nvs_close(settings);
    if (error==ESP_ERR_NVS_NOT_FOUND) { strcpy(path,"/Workshop.dsk"); return 0; }
    if (error!=ESP_OK) return error;
    return image_path(saved,path) ? 0 : LFS_ERR_INVAL;
}

static int remember_image(const char *path)
{
    nvs_handle_t settings;
    esp_err_t error=nvs_open("a2_disk",NVS_READWRITE,&settings);
    if (error!=ESP_OK) return error;
    char saved[65];
    size_t length=sizeof(saved);
    error=nvs_get_str(settings,"image",saved,&length);
    if (error==ESP_OK && !strcmp(saved,path)) { nvs_close(settings); return 0; }
    if (error==ESP_OK || error==ESP_ERR_NVS_NOT_FOUND || error==ESP_ERR_NVS_INVALID_LENGTH)
        error=nvs_set_str(settings,"image",path);
    if (error==ESP_OK) error=nvs_commit(settings);
    nvs_close(settings);
    return error;
}

static int select_bank(void *unused, uint16_t bank)
{
    (void)unused;
    return bank <= 255 && shell_link_mapper_write(0x3000,0) &&
           shell_link_mapper_write(0x2000,bank) ? 0 : LFS_ERR_IO;
}
static int bus_read(void *unused, uint16_t address, void *data, size_t size)
{
    (void)unused;
    uint8_t *out=data;
    while (size) {
        const size_t count=size<32 ? size : 32;
        if (!shell_link_read_block(address,out,count)) return LFS_ERR_IO;
        out+=count; address+=count; size-=count;
    }
    return 0;
}
static int bus_write(void *unused, uint16_t address, uint8_t value)
{
    (void)unused;
    return shell_link_flash_command(address,value) ? 0 : LFS_ERR_IO;
}
static int bus_write_block(void *unused,uint16_t address,const void *data,size_t size)
{
    (void)unused;
    return shell_link_flash_write_block(address,data,size) ? 0 : LFS_ERR_IO;
}
static int bus_sync(void *unused) { (void)unused; return 0; }
static uint64_t now_ms(void *unused) { (void)unused; return esp_timer_get_time()/1000; }
static void wait_ms(void *unused, uint32_t ms)
{
    (void)unused;
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

void cart_storage_init(apple2_t *m, const uint8_t *rom, size_t size)
{
    machine=m; slot_rom=rom; slot_size=size;
    const a2_is29gl032_bus bus={.select_bank=select_bank,.read=bus_read,
        .raw_write=bus_write,.sync=bus_sync,.now_ms=now_ms,.wait_ms=wait_ms,
        .raw_write_block=bus_write_block};
    last_error=a2_is29gl032_init(&flash,&bus,1000,30000);
    initialized=last_error==0;
}

static int probe(void)
{
    if (!initialized) return LFS_ERR_INVAL;
    if (flash.identified) return 0;
    uint8_t build=0,flags=0;
    if (!shell_link_diag(0,&build) || build!=0xa2 ||
        !shell_link_diag(3,&flags) || !(flags&2)) return LFS_ERR_IO;
    return a2_is29gl032_probe(&flash,NULL);
}

static int prepare_fs(void)
{
    int error=probe();
    if (error || filesystem.mounted) return error;
    a2_storage_backend backend;
    error=a2_is29gl032_backend(&flash,&backend);
    if (!error) error=a2_storage_init(&filesystem,&backend);
    return error;
}

static int mount_fs(void)
{
    int error=prepare_fs();
    if (!error) error=a2_is29gl032_allow_writes(&flash);
    if (!error && !filesystem.mounted) error=a2_storage_mount(&filesystem);
    return error;
}

static bool read_track(void *unused,unsigned track,uint8_t data[4096])
{
    (void)unused;
    last_error=a2_disk_overlay_read_track(&journal,track,data);
    return last_error==0;
}
static bool write_track(void *unused,unsigned track,const uint8_t data[4096])
{
    (void)unused;
    if (commit_error) return false;
    last_error=a2_disk_overlay_write_track(&journal,track,data);
    if (!last_error) pending_sync=true;
    else commit_error=last_error;
    return last_error==0;
}

bool cart_storage_sync(void)
{
    if (commit_error) return false;
    last_error=image.opened && !disk2_flush(&disk) ? LFS_ERR_IO : 0;
    if (!last_error && journal.opened) last_error=a2_disk_overlay_sync(&journal);
    if (last_error) { commit_error=last_error; disk.error=true; return false; }
    pending_sync=false;
    return true;
}
void cart_storage_tick(void)
{
    if (image.opened && pending_sync && !disk.motor && !commit_error)
        (void)cart_storage_sync();
}
bool cart_storage_saving(void) { return pending_sync || disk.dirty; }

bool cart_storage_programs(a2_dos_catalog *catalog)
{
    if (!catalog) return false;
    memset(catalog,0,sizeof(*catalog));
    if (!image.opened) { last_error=LFS_ERR_BADF; return false; }
    if (!cart_storage_sync()) return false;
    last_error=a2_dos_catalog_read(read_track,NULL,catalog,track_scratch);
    /* This 48K machine has Applesoft in ROM, without an Integer BASIC
     * language card. Only offer programs this configuration can launch. */
    if (!last_error) {
        unsigned count=0;
        for (unsigned i=0;i<catalog->count;++i)
            if (catalog->programs[i].type==2) catalog->programs[count++]=catalog->programs[i];
        catalog->count=count;
    }
    return last_error==0;
}

static int eject(void)
{
    if (image.opened) {
        if (!cart_storage_sync()) return cart_storage_error();
        int error=journal.opened ? a2_disk_overlay_close(&journal) : 0;
        if (!error) error=a2_disk_image_close(&image);
        if (error) return error;
    }
    apple2_attach_disk(machine,NULL);
    active_name[0]=0;
    return 0;
}

static int image_hash(uint8_t digest[32],bool logical)
{
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    int error=mbedtls_sha256_starts(&hash,0);
    for (unsigned track=0; !error && track<35; ++track) {
        error=logical ? a2_disk_overlay_read_track(&journal,track,track_scratch) :
                       a2_disk_image_read_at(&image,track*4096,track_scratch,sizeof(track_scratch));
        if (!error) error=mbedtls_sha256_update(&hash,track_scratch,sizeof(track_scratch));
    }
    if (!error) error=mbedtls_sha256_finish(&hash,digest);
    mbedtls_sha256_free(&hash);
    return error;
}

static int boot_image(const char *name,bool reset_journal,bool remember)
{
    char path[65];
    if (slot_size!=256 || !image_path(name,path)) return LFS_ERR_INVAL;
    int error=mount_fs();
    if (!error) error=eject();
    if (!error) error=a2_disk_image_open(&filesystem,&image,path,false);
    uint8_t hash[32];
    if (!error) error=image_hash(hash,false);
    const char *basename=path+1;
    if (!error) error=a2_disk_overlay_open(&journal,&image,basename,hash);
    if (!error && reset_journal) error=a2_disk_overlay_reset_journal(&journal);
    if (error) return error;
    if (!disk2_init(&disk,slot_rom,read_track,write_track,NULL)) {
        (void)a2_disk_overlay_close(&journal);
        (void)a2_disk_image_close(&image);
        return LFS_ERR_IO;
    }
    if (remember && (error=remember_image(path))) {
        (void)a2_disk_overlay_close(&journal);
        (void)a2_disk_image_close(&image);
        return error;
    }
    snprintf(active_name,sizeof(active_name),"%s",path);
    apple2_attach_disk(machine,&disk);
    apple2_reset(machine,true);
    return 0;
}

bool cart_storage_reset(void)
{
    if (!cart_storage_sync()) return false;
    if (image.opened && !disk2_init(&disk,slot_rom,read_track,write_track,NULL)) {
        last_error=LFS_ERR_IO;
        return false;
    }
    apple2_reset(machine,true);
    return true;
}

void cart_storage_autoboot(void)
{
    if (auto_attempted || image.opened) return;
    uint8_t build=0;
    if (!shell_link_diag(0,&build) || build!=0xa2) return;
    auto_attempted=true;
    char path[65];
    last_error=selected_image(path);
    if (!last_error) last_error=boot_image(path,false,false);
}

const char *cart_storage_name(void) { return active_name[0] ? active_name : "NO DISK"; }
int cart_storage_error(void) { return commit_error ? commit_error : last_error; }

typedef struct { char *output; size_t capacity; } catalog_output;
static int catalog_entry(void *context,const a2_storage_entry *entry)
{
    catalog_output *out=context;
    const size_t used=strlen(out->output);
    if (used>=out->capacity) return 1;
    snprintf(out->output+used,out->capacity-used,"%s %lu\n",entry->name,(unsigned long)entry->size);
    return 0;
}

static int disk_digest(char *output,size_t capacity)
{
    if (!image.opened) return LFS_ERR_BADF;
    if (!cart_storage_sync()) return cart_storage_error();
    uint8_t digest[32];
    const int error=image_hash(digest,true);
    if (!error) {
        char hex[65];
        for (unsigned i=0;i<32;++i) snprintf(hex+i*2,3,"%02x",digest[i]);
        snprintf(output,capacity,"DISK_SHA256 bytes=%u sha256=%s\n",A2_DISK_IMAGE_BYTES,hex);
    }
    return error;
}

bool cart_storage_command(int argc,char **argv,char *output,size_t capacity)
{
    int error=LFS_ERR_INVAL;
    output[0]=0;
    if (argc==2 && !strcmp(argv[1],"probe")) {
        error=probe();
        snprintf(output,capacity,"FLASH result=%d id=%02x:%02x:%02x:%02x bytes=%lu sectors=%lu sector_bytes=%lu\n",
            error,flash.info.manufacturer,flash.info.device1,flash.info.device2,flash.info.device3,
            (unsigned long)flash.info.bytes,(unsigned long)flash.info.sectors,(unsigned long)flash.info.sector_bytes);
    } else if (argc==2 && !strcmp(argv[1],"format")) {
        error=eject();
        if (!error && filesystem.mounted) error=a2_storage_unmount(&filesystem);
        if (!error) error=prepare_fs();
        if (!error) error=a2_is29gl032_allow_writes(&flash);
        if (!error) error=a2_storage_format(&filesystem);
        if (!error) error=a2_storage_mount(&filesystem);
    } else if (argc==2 && !strcmp(argv[1],"mount")) error=mount_fs();
    else if (argc==2 && !strcmp(argv[1],"catalog")) {
        error=mount_fs();
        catalog_output context={output,capacity};
        if (!error) error=a2_storage_catalog(&filesystem,"/",catalog_entry,&context);
        if (!error && !output[0]) snprintf(output,capacity,"EMPTY\n");
    } else if (argc==2 && !strcmp(argv[1],"digest")) error=disk_digest(output,capacity);
    else if (argc==2 && !strcmp(argv[1],"eject")) error=eject();
    else if ((argc==2 || argc==3) && !strcmp(argv[1],"boot"))
        error=boot_image(argc==3 ? argv[2] : "/Workshop.dsk",false,true);
    else if (argc==2 && !strcmp(argv[1],"sync")) {
        error=cart_storage_sync() ? 0 : cart_storage_error();
    }
    last_error=error;
    if (!output[0]) snprintf(output,capacity,error ? "ERROR storage=%d transport=%s\n" : "OK storage=%d disk=%s\n",
                             error,error ? shell_link_error() : cart_storage_name());
    return !error;
}

bool cart_storage_import_begin(const char *name)
{
    last_error=eject();
    if (!last_error) last_error=mount_fs();
    if (!last_error) last_error=a2_import_begin(&upload,&filesystem,name);
    if (!last_error) snprintf(upload_name,sizeof(upload_name),"/%s",name);
    return last_error==0;
}
bool cart_storage_import_write(const void *data,size_t size)
{
    last_error=a2_import_write(&upload,data,size);
    return last_error==0;
}
bool cart_storage_import_finish(void)
{
    last_error=a2_import_finish(&upload);
    if (!last_error) last_error=boot_image(upload_name,true,true);
    return last_error==0;
}
void cart_storage_import_abort(void) { (void)a2_import_abort(&upload); }
