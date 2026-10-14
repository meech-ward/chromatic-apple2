// SPDX-License-Identifier: MIT
// Host proof with externally supplied ROM/disk inputs; none are distributed.
// Define A2_OVERLAY_BASELINE to measure the same SAVE through direct file COW.
#define main storage_foundation_test_main
#include "storage_test.c"
#undef main
#include "a2_import.h"
#include "a2_disk_overlay.h"
#include "apple2.h"
#include "disk2.h"

static apple2_t machine;
static disk2_t disk;
static a2_storage storage;
static a2_disk_image image;
static uint8_t rom[16384], slot[256], base_hash[32];
static unsigned stored_tracks;
#ifndef A2_OVERLAY_BASELINE
static a2_disk_overlay overlay;
static unsigned changed_sectors;
#endif
static bool track_read(void *unused, unsigned track, uint8_t data[4096]) {
    (void)unused;
#ifdef A2_OVERLAY_BASELINE
    return track < 35 && !a2_disk_image_read_at(&image,track*4096,data,4096);
#else
    return !a2_disk_overlay_read_track(&overlay,track,data);
#endif
}
static bool track_write(void *unused, unsigned track, const uint8_t data[4096]) {
    (void)unused;
#ifdef A2_OVERLAY_BASELINE
    if(track>=35 || a2_disk_image_write_at(&image,track*4096,data,4096)
            || a2_disk_image_sync(&image))return false;
#else
    if(a2_disk_overlay_write_track(&overlay,track,data))return false;
    changed_sectors+=overlay.last_changed_sectors;
#endif
    stored_tracks++;
    return true;
}
static void type_program(const char *text) {
    for(unsigned i=0;text[i];++i){
        for(unsigned waits=0;machine.key_ready && waits<50;++waits)apple2_run(&machine,100000);
        CHECK(apple2_key(&machine,text[i]));apple2_run(&machine,30000);
    }
    apple2_run(&machine,2000000);
}
static void boot(void) {
    CHECK(disk2_init(&disk,slot,track_read,track_write,NULL));
    CHECK(apple2_init(&machine,rom,sizeof(rom)));apple2_attach_disk(&machine,&disk);
    for(unsigned i=0;i<12;++i)apple2_run(&machine,2000000);
    CHECK(!disk.error);
}
static void open_disk(void) {
#ifdef A2_OVERLAY_BASELINE
    CHECK(!a2_disk_image_open(&storage,&image,"/Workshop.dsk",true));
#else
    CHECK(!a2_disk_image_open(&storage,&image,"/Workshop.dsk",false));
    CHECK(!a2_disk_overlay_open(&overlay,&image,"Workshop.dsk",base_hash));
#endif
}
static void close_disk(void) {
#ifndef A2_OVERLAY_BASELINE
    CHECK(!a2_disk_overlay_close(&overlay));
#endif
    CHECK(!a2_disk_image_close(&image));CHECK(!a2_storage_unmount(&storage));
}
int main(int argc,char **argv) {
    CHECK(argc==5 && strlen(argv[4])==64);
    for(unsigned i=0;i<32;++i){unsigned n;CHECK(sscanf(argv[4]+i*2,"%2x",&n)==1);base_hash[i]=(uint8_t)n;}
    FILE *source=fopen(argv[1],"rb");CHECK(source);
    CHECK(fread(rom,1,sizeof(rom),source)==sizeof(rom));fclose(source);
    source=fopen(argv[2],"rb");CHECK(source);
    CHECK(fread(slot,1,sizeof(slot),source)==sizeof(slot));fclose(source);
    nor_flash flash={.data=malloc(A2_STORAGE_FLASH_BYTES),.program_budget=-1};CHECK(flash.data);
    memset(flash.data,0xff,A2_STORAGE_FLASH_BYTES);
    a2_storage_backend backend={.context=&flash,.read=nor_read,.program=nor_program,.erase=nor_erase,.sync=nor_sync};
    CHECK(!a2_storage_init(&storage,&backend));CHECK(!a2_storage_format(&storage));CHECK(!a2_storage_mount(&storage));
    source=fopen(argv[3],"rb");CHECK(source);
    a2_import upload={0};CHECK(!a2_import_begin(&upload,&storage,"Workshop.dsk"));
    uint8_t chunk[1024];size_t count;
    while((count=fread(chunk,1,sizeof(chunk),source)))CHECK(!a2_import_write(&upload,chunk,count));
    CHECK(!ferror(source));fclose(source);CHECK(!a2_import_finish(&upload));
    open_disk();boot();
    unsigned before=flash.programs,erases=flash.erases;
    type_program("NEW\r10 PRINT \"OVERLAY STORAGE PASS\"\rSAVE OVERLAY PROOF\r");
    apple2_run(&machine,6000000);CHECK(disk2_flush(&disk));
    CHECK(!disk.error && stored_tracks && disk.flush_count);
    unsigned save_pages=flash.programs-before,save_erases=flash.erases-erases;
#ifndef A2_OVERLAY_BASELINE
    source=fopen(argv[3],"rb");CHECK(source);
    uint32_t offset=0;uint8_t base_bytes[1024];
    while((count=fread(chunk,1,sizeof(chunk),source))){
        CHECK(!a2_disk_image_read_at(&image,offset,base_bytes,count));
        CHECK(!memcmp(chunk,base_bytes,count));offset+=(uint32_t)count;
    }
    CHECK(!ferror(source) && offset==A2_DISK_IMAGE_BYTES);fclose(source);
#endif
    close_disk();
    memset(&storage,0,sizeof(storage));memset(&image,0,sizeof(image));
#ifndef A2_OVERLAY_BASELINE
    memset(&overlay,0,sizeof(overlay));
#endif
    CHECK(!a2_storage_init(&storage,&backend));CHECK(!a2_storage_mount(&storage));
    open_disk();boot();type_program("NEW\rLOAD OVERLAY PROOF\rRUN\r");
    bool found=false;
    for(unsigned row=0;row<24;++row){char line[41];apple2_text_line(&machine,row,line);if(strstr(line,"OVERLAY STORAGE PASS"))found=true;}
    CHECK(found && !disk.error && !flash.violations);close_disk();
#ifdef A2_OVERLAY_BASELINE
    printf("PASS real DOS SAVE/reconstruct/LOAD/RUN baseline: %u tracks; %u SAVE page programs; %u SAVE erases\n",stored_tracks,save_pages,save_erases);
#else
    printf("PASS real DOS SAVE/reconstruct/LOAD/RUN overlay: %u tracks; %u changed sectors; %u SAVE page programs; %u SAVE erases\n",stored_tracks,changed_sectors,save_pages,save_erases);
#endif
    free(flash.data);return 0;
}
