// SPDX-License-Identifier: MIT
#define main storage_foundation_test_main
#include "storage_test.c"
#undef main
#include "a2_disk_overlay.h"

static a2_storage storage;
static a2_disk_image base;
static a2_disk_overlay overlay;
static const uint8_t hash[32] = {0x12,0x34,0x56,0x78};
static uint8_t track[4096], output[4096];

static void reopen(const a2_storage_backend *backend) {
    memset(&storage, 0, sizeof(storage));
    memset(&base, 0, sizeof(base));
    memset(&overlay, 0, sizeof(overlay));
    CHECK(!a2_storage_init(&storage, backend));
    CHECK(!a2_storage_mount(&storage));
    CHECK(!a2_disk_image_open(&storage, &base, "/TEST.dsk", false));
    CHECK(!a2_disk_overlay_open(&overlay, &base, "TEST.dsk", hash));
}
static void shut(void) {
    CHECK(!a2_disk_overlay_close(&overlay));
    CHECK(!a2_disk_image_close(&base));
    CHECK(!a2_storage_unmount(&storage));
}
static void check_base(void) {
    CHECK(!a2_disk_image_read_at(&base, 0, output, sizeof(output)));
    for (unsigned i = 0; i < sizeof(output); ++i) CHECK(!output[i]);
}
int main(void) {
    puts("Testing immutable disk base with committed sector overlays");
    nor_flash nor = {.data = malloc(A2_STORAGE_FLASH_BYTES), .program_budget = -1};
    CHECK(nor.data); memset(nor.data, 0xff, A2_STORAGE_FLASH_BYTES);
    a2_storage_backend backend = {.context=&nor,.read=nor_read,.program=nor_program,.erase=nor_erase,.sync=nor_sync};
    CHECK(!a2_storage_init(&storage,&backend));
    CHECK(!a2_storage_format(&storage)); CHECK(!a2_storage_mount(&storage));
    CHECK(!a2_disk_image_create(&storage,"/TEST.dsk"));
    CHECK(!a2_disk_image_create(&storage,"/PLAIN.dsk"));
    CHECK(!a2_disk_image_open(&storage,&base,"/TEST.dsk",false));
    CHECK(a2_disk_overlay_open(&overlay,&base,"../TEST.dsk",hash)==LFS_ERR_INVAL);
    CHECK(a2_disk_overlay_open(&overlay,&base,"/TEST.dsk",hash)==LFS_ERR_INVAL);
    CHECK(!a2_disk_overlay_open(&overlay,&base,"TEST.dsk",hash));
    CHECK(overlay.journal_bytes==64 && !overlay.recovered_tail);
    CHECK(a2_storage_unmount(&storage)==LFS_ERR_INVAL);

    memset(track,0,sizeof(track)); memset(track+5*256,0xa5,256); track[4095]=0x7f;
    unsigned before=nor.programs;
    CHECK(!a2_disk_overlay_write_track(&overlay,0,track));
    unsigned overlay_pages=nor.programs-before;
    CHECK(overlay.last_changed_sectors==2 && overlay.journal_bytes==64+3*268);
    CHECK(!a2_disk_overlay_read_track(&overlay,0,output) && !memcmp(track,output,sizeof(track)));
    before=nor.programs;
    CHECK(!a2_disk_overlay_write_track(&overlay,0,track));
    CHECK(overlay.last_changed_sectors==0 && nor.programs==before);
    a2_disk_image plain={0};
    CHECK(!a2_disk_image_open(&storage,&plain,"/PLAIN.dsk",true));
    before=nor.programs;
    CHECK(!a2_disk_image_write_at(&plain,0,track,sizeof(track)));
    CHECK(!a2_disk_image_sync(&plain));
    unsigned plain_pages=nor.programs-before;
    CHECK(!a2_disk_image_close(&plain));
    CHECK(overlay_pages*10 < plain_pages);
    check_base(); shut(); reopen(&backend);
    CHECK(!a2_disk_overlay_read_track(&overlay,0,output) && !memcmp(track,output,sizeof(track)));

    // A different full base identity gets its own empty journal, never old saves.
    a2_disk_overlay other={0}; uint8_t other_hash[32]; memcpy(other_hash,hash,32); other_hash[31]=1;
    CHECK(!a2_disk_overlay_open(&other,&base,"TEST.dsk",other_hash));
    CHECK(strcmp(other.path,overlay.path));
    CHECK(!a2_disk_overlay_read_track(&other,0,output));
    for(unsigned i=0;i<sizeof(output);++i)CHECK(!output[i]);
    CHECK(!a2_disk_overlay_close(&other));

    // A physically committed incomplete tail cannot publish a partial track.
    char path[A2_OVERLAY_PATH_BYTES]; strcpy(path,overlay.path);
    uint32_t orphan_offset=overlay.latest[5];
    CHECK(!a2_disk_overlay_close(&overlay));
    lfs_file_t file; uint8_t cache[256]; struct lfs_file_config cfg={.buffer=cache};
    CHECK(!lfs_file_opencfg(&storage.lfs,&file,path,LFS_O_RDWR|LFS_O_APPEND,&cfg));
    uint8_t orphan[A2_OVERLAY_RECORD_BYTES];
    CHECK(lfs_file_seek(&storage.lfs,&file,(lfs_soff_t)orphan_offset,LFS_SEEK_SET)==(lfs_soff_t)orphan_offset);
    CHECK(lfs_file_read(&storage.lfs,&file,orphan,sizeof(orphan))==(lfs_ssize_t)sizeof(orphan));
    CHECK(lfs_file_write(&storage.lfs,&file,orphan,sizeof(orphan))==(lfs_ssize_t)sizeof(orphan));
    const uint8_t torn[]={0x41,0x32,0x53,0x31,0x01};
    CHECK(lfs_file_write(&storage.lfs,&file,torn,sizeof(torn))==sizeof(torn));
    CHECK(!lfs_file_sync(&storage.lfs,&file));CHECK(!lfs_file_close(&storage.lfs,&file));
    CHECK(!a2_disk_overlay_open(&overlay,&base,"TEST.dsk",hash));
    CHECK(overlay.recovered_tail);
    CHECK(!a2_disk_overlay_read_track(&overlay,0,output) && !memcmp(track,output,sizeof(track)));
    track[0]=0x42;
    CHECK(!a2_disk_overlay_write_track(&overlay,0,track));
    CHECK(!overlay.recovered_tail);

    // Failed append leaves RAM indices unchanged; close must not retry sync.
    uint32_t index[A2_OVERLAY_SECTORS]; memcpy(index,overlay.latest,sizeof(index));
    uint32_t old_size=overlay.journal_bytes;
    uint8_t previous[4096]; memcpy(previous,track,sizeof(previous));
    memset(track,0x6d,sizeof(track)); nor.program_budget=1;
    CHECK(a2_disk_overlay_write_track(&overlay,0,track)==LFS_ERR_IO);
    CHECK(!memcmp(index,overlay.latest,sizeof(index)) && overlay.journal_bytes==old_size);
    before=nor.programs;
    CHECK(a2_disk_overlay_close(&overlay)==LFS_ERR_IO && nor.programs==before);
    nor.program_budget=-1; reopen(&backend);
    CHECK(!a2_disk_overlay_read_track(&overlay,0,output) && !memcmp(previous,output,sizeof(previous)));

    // A failed scratch rewrite leaves the old committed journal recoverable.
    nor.program_budget=1;
    CHECK(a2_disk_overlay_compact(&overlay)==LFS_ERR_IO);
    before=nor.programs;
    CHECK(a2_disk_overlay_close(&overlay)==LFS_ERR_IO && nor.programs==before);
    nor.program_budget=-1; reopen(&backend);
    CHECK(!a2_disk_overlay_read_track(&overlay,0,output) && !memcmp(previous,output,sizeof(previous)));
    CHECK(!a2_disk_overlay_compact(&overlay));

    memset(track,0,sizeof(track));memset(track+15*256,0xa7,256);
    CHECK(!a2_disk_overlay_write_track(&overlay,34,track));

    // Repeated saves automatically compact at the bounded log limit.
    bool compacted=false;
    for(unsigned round=1;round<=70;++round){
        memset(track,(int)round,sizeof(track));old_size=overlay.journal_bytes;
        CHECK(!a2_disk_overlay_write_track(&overlay,0,track));
        CHECK(overlay.journal_bytes<=A2_OVERLAY_LIMIT);
        if(overlay.journal_bytes<old_size)compacted=true;
    }
    CHECK(compacted);shut();reopen(&backend);
    CHECK(!a2_disk_overlay_read_track(&overlay,0,output) && !memcmp(track,output,sizeof(track)));
    CHECK(!a2_disk_overlay_read_track(&overlay,34,output));
    for(unsigned i=0;i<sizeof(output);++i)CHECK(output[i]==(i>=15*256?0xa7:0));
    check_base();
    nor.program_budget=0;
    CHECK(a2_disk_overlay_reset_journal(&overlay)==LFS_ERR_IO);
    before=nor.programs;
    CHECK(a2_disk_overlay_close(&overlay)==LFS_ERR_IO && nor.programs==before);
    nor.program_budget=-1;reopen(&backend);
    CHECK(!a2_disk_overlay_read_track(&overlay,0,output) && !memcmp(track,output,sizeof(track)));
    CHECK(!a2_disk_overlay_reset_journal(&overlay));
    CHECK(overlay.journal_bytes==64);
    CHECK(!a2_disk_overlay_read_track(&overlay,0,output));
    for(unsigned i=0;i<sizeof(output);++i)CHECK(!output[i]);

    // Complete corruption must fail open, with no writes or silent fallback.
    memset(track,0x32,sizeof(track));CHECK(!a2_disk_overlay_write_track(&overlay,0,track));
    uint32_t damaged=overlay.latest[0]+8;
    strcpy(path,overlay.path);CHECK(!a2_disk_overlay_close(&overlay));
    CHECK(!lfs_file_opencfg(&storage.lfs,&file,path,LFS_O_RDWR,&cfg));
    CHECK(lfs_file_seek(&storage.lfs,&file,(lfs_soff_t)damaged,LFS_SEEK_SET)==(lfs_soff_t)damaged);
    uint8_t bad=0x11;CHECK(lfs_file_write(&storage.lfs,&file,&bad,1)==1);
    CHECK(!lfs_file_close(&storage.lfs,&file));before=nor.programs;
    CHECK(a2_disk_overlay_open(&overlay,&base,"TEST.dsk",hash)==LFS_ERR_CORRUPT);
    CHECK(nor.programs==before);check_base();
    CHECK(!a2_disk_image_close(&base));CHECK(!a2_storage_unmount(&storage));
    CHECK(!nor.violations);
    printf("PASS: sector/track commits, identities, remount, incomplete tail, CRC failure, append/compaction/reset IO faults; 2-sector save %u overlay pages vs %u base-COW pages; object=%zu bytes\n",overlay_pages,plain_pages,sizeof(overlay));
    free(nor.data);return 0;
}
