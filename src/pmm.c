// Post memory manager (PMM) calls
//
// Copyright (C) 2009  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "util.h" // checksum
#include "config.h" // BUILD_BIOS_ADDR
#include "memmap.h" // struct e820entry
#include "farptr.h" // GET_FARVAR
#include "biosvar.h" // GET_BDA
#include "optionroms.h" // OPTION_ROM_ALIGN

// Information on a reserved area.
struct allocinfo_s {
    struct allocinfo_s *next, **pprev;
    void *data, *dataend, *allocend;
};

// Information on a tracked memory allocation.
struct allocdetail_s {
    struct allocinfo_s detailinfo;
    struct allocinfo_s datainfo;
    u32 handle;
};

// The various memory zones.
struct zone_s {
    struct allocinfo_s *info;
};

struct zone_s ZoneLow, ZoneHigh, ZoneFSeg, ZoneTmpLow, ZoneTmpHigh;

static struct zone_s *Zones[] = {
    &ZoneTmpLow, &ZoneLow, &ZoneFSeg, &ZoneTmpHigh, &ZoneHigh
};


/****************************************************************
 * low-level memory reservations
 ****************************************************************/

// Find and reserve space from a given zone
static void *
allocSpace(struct zone_s *zone, u32 size, u32 align, struct allocinfo_s *fill)
{
    struct allocinfo_s *info;
    for (info = zone->info; info; info = info->next) {
        void *dataend = info->dataend;
        void *allocend = info->allocend;
        void *newallocend = (void*)ALIGN_DOWN((u32)allocend - size, align);
        if (newallocend >= dataend && newallocend <= allocend) {
            // Found space - now reserve it.
            struct allocinfo_s **pprev = info->pprev;
            if (!fill)
                fill = newallocend;
            fill->next = info;
            fill->pprev = pprev;
            fill->data = newallocend;
            fill->dataend = newallocend + size;
            fill->allocend = allocend;

            info->allocend = newallocend;
            info->pprev = &fill->next;
            *pprev = fill;
            return newallocend;
        }
    }
    return NULL;
}

// Release space allocated with allocSpace()
static void
freeSpace(struct allocinfo_s *info)
{
    struct allocinfo_s *next = info->next;
    struct allocinfo_s **pprev = info->pprev;
    *pprev = next;
    if (next) {
        if (next->allocend == info->data)
            next->allocend = info->allocend;
        next->pprev = pprev;
    }
}

// Add new memory to a zone
static void
addSpace(struct zone_s *zone, void *start, void *end)
{
    // Find position to add space
    struct allocinfo_s **pprev = &zone->info, *info;
    for (;;) {
        info = *pprev;
        if (!info || info->data < start)
            break;
        pprev = &info->next;
    }

    // Add space using temporary allocation info.
    struct allocdetail_s tempdetail;
    tempdetail.datainfo.next = info;
    tempdetail.datainfo.pprev = pprev;
    tempdetail.datainfo.data = tempdetail.datainfo.dataend = start;
    tempdetail.datainfo.allocend = end;
    *pprev = &tempdetail.datainfo;
    if (info)
        info->pprev = &tempdetail.datainfo.next;

    // Allocate final allocation info.
    struct allocdetail_s *detail = allocSpace(
        &ZoneTmpHigh, sizeof(*detail), MALLOC_MIN_ALIGN, NULL);
    if (!detail) {
        detail = allocSpace(&ZoneTmpLow, sizeof(*detail)
                            , MALLOC_MIN_ALIGN, NULL);
        if (!detail) {
            *tempdetail.datainfo.pprev = tempdetail.datainfo.next;
            if (tempdetail.datainfo.next)
                tempdetail.datainfo.next->pprev = tempdetail.datainfo.pprev;
            warn_noalloc();
            return;
        }
    }

    // Replace temp alloc space with final alloc space
    memcpy(&detail->datainfo, &tempdetail.datainfo, sizeof(detail->datainfo));
    detail->handle = PMM_DEFAULT_HANDLE;

    *tempdetail.datainfo.pprev = &detail->datainfo;
    if (tempdetail.datainfo.next)
        tempdetail.datainfo.next->pprev = &detail->datainfo.next;
}

// Search all zones for an allocation obtained from allocSpace()
static struct allocinfo_s *
findAlloc(void *data)
{
    int i;
    for (i=0; i<ARRAY_SIZE(Zones); i++) {
        struct zone_s *zone = Zones[i];
        struct allocinfo_s *info;
        for (info = zone->info; info; info = info->next)
            if (info->data == data)
                return info;
    }
    return NULL;
}

// Return the last sentinal node of a zone
static struct allocinfo_s *
findLast(struct zone_s *zone)
{
    struct allocinfo_s *info = zone->info;
    if (!info)
        return NULL;
    for (;;) {
        struct allocinfo_s *next = info->next;
        if (!next)
            return info;
        info = next;
    }
}


/****************************************************************
 * 0xc0000-0xf0000 management
 ****************************************************************/

static u32 RomEnd = BUILD_ROM_START;
static struct allocinfo_s *RomBase;

#define OPROM_HEADER_RESERVE 16

// Return the maximum memory position option roms may use.
u32
rom_get_max(void)
{
    return ALIGN_DOWN((u32)RomBase->allocend - OPROM_HEADER_RESERVE
                      , OPTION_ROM_ALIGN);
}

// Return the end of the last deployed option rom.
u32
rom_get_last(void)
{
    return RomEnd;
}

// Request space for an optionrom in 0xc0000-0xf0000 area.
struct rom_header *
rom_reserve(u32 size)
{
    u32 newend = ALIGN(RomEnd + size, OPTION_ROM_ALIGN) + OPROM_HEADER_RESERVE;
    if (newend > (u32)RomBase->allocend)
        return NULL;
    if (newend < (u32)zonelow_base + OPROM_HEADER_RESERVE)
        newend = (u32)zonelow_base + OPROM_HEADER_RESERVE;
    RomBase->data = RomBase->dataend = (void*)newend;
    return (void*)RomEnd;
}

// Confirm space as in use by an optionrom.
int
rom_confirm(u32 size)
{
    void *new = rom_reserve(size);
    if (!new) {
        warn_noalloc();
        return -1;
    }
    RomEnd = ALIGN(RomEnd + size, OPTION_ROM_ALIGN);
    return 0;
}


/****************************************************************
 * Setup
 ****************************************************************/

// Space for bios tables built an run-time.
char BiosTableSpace[CONFIG_MAX_BIOSTABLE] __aligned(MALLOC_MIN_ALIGN) VARFSEG;

void
malloc_preinit(void)
{
    ASSERT32FLAT();
    dprintf(3, "malloc preinit\n");

    // Don't declare any memory between 0xa0000 and 0x100000
    add_e820(BUILD_LOWRAM_END, BUILD_BIOS_ADDR-BUILD_LOWRAM_END, E820_HOLE);

    // Mark known areas as reserved.
    add_e820(BUILD_BIOS_ADDR, BUILD_BIOS_SIZE, E820_RESERVED);

    // Populate temp high ram
    u32 highram = 0;
    int i;
    for (i=e820_count-1; i>=0; i--) {
        struct e820entry *en = &e820_list[i];
        u64 end = en->start + en->size;
        if (end < 1024*1024)
            break;
        if (en->type != E820_RAM || end > 0xffffffff)
            continue;
        u32 s = en->start, e = end;
        if (!highram) {
            u32 newe = ALIGN_DOWN(e - CONFIG_MAX_HIGHTABLE, MALLOC_MIN_ALIGN);
            if (newe <= e && newe >= s) {
                highram = newe;
                e = newe;
            }
        }
        addSpace(&ZoneTmpHigh, (void*)s, (void*)e);
    }

    // Populate regions
    addSpace(&ZoneTmpLow, (void*)BUILD_STACK_ADDR, (void*)BUILD_EBDA_MINIMUM);
    if (highram) {
        addSpace(&ZoneHigh, (void*)highram
                 , (void*)highram + CONFIG_MAX_HIGHTABLE);
        add_e820(highram, CONFIG_MAX_HIGHTABLE, E820_RESERVED);
    }
}

void
csm_malloc_preinit(u32 low_pmm, u32 low_pmm_size, u32 hi_pmm, u32 hi_pmm_size)
{
    ASSERT32FLAT();

    if (hi_pmm_size > CONFIG_MAX_HIGHTABLE) {
        void *hi_pmm_end = (void *)hi_pmm + hi_pmm_size;
        addSpace(&ZoneTmpHigh, (void *)hi_pmm, hi_pmm_end - CONFIG_MAX_HIGHTABLE);
        addSpace(&ZoneHigh, hi_pmm_end - CONFIG_MAX_HIGHTABLE, hi_pmm_end);
    } else {
        addSpace(&ZoneTmpHigh, (void *)hi_pmm, (void *)hi_pmm + hi_pmm_size);
    }
    addSpace(&ZoneTmpLow, (void *)low_pmm, (void *)low_pmm + low_pmm_size);
}

u32 LegacyRamSize VARFSEG;

// Calculate the maximum ramsize (less than 4gig) from e820 map.
static void
calcRamSize(void)
{
    u32 rs = 0;
    int i;
    for (i=e820_count-1; i>=0; i--) {
        struct e820entry *en = &e820_list[i];
        u64 end = en->start + en->size;
        u32 type = en->type;
        if (end <= 0xffffffff && (type == E820_ACPI || type == E820_RAM)) {
            rs = end;
            break;
        }
    }
    LegacyRamSize = rs >= 1024*1024 ? rs : 1024*1024;
}

// Update pointers after code relocation.
void
malloc_init(void)
{
    ASSERT32FLAT();
    dprintf(3, "malloc init\n");

    if (CONFIG_RELOCATE_INIT) {
        // Fixup malloc pointers after relocation
        int i;
        for (i=0; i<ARRAY_SIZE(Zones); i++) {
            struct zone_s *zone = Zones[i];
            if (zone->info)
                zone->info->pprev = &zone->info;
        }
    }

    // Initialize low-memory region
    extern u8 varlow_start[], varlow_end[], final_varlow_start[];
    memmove(final_varlow_start, varlow_start, varlow_end - varlow_start);
    addSpace(&ZoneLow, zonelow_base + OPROM_HEADER_RESERVE, final_varlow_start);
    RomBase = findLast(&ZoneLow);

    // Add space available in f-segment to ZoneFSeg
    addSpace(&ZoneFSeg, BiosTableSpace, &BiosTableSpace[CONFIG_MAX_BIOSTABLE]);
    extern u8 code32init_end[];
    if ((u32)code32init_end > BUILD_BIOS_ADDR) {
        memset((void*)BUILD_BIOS_ADDR, 0, (u32)code32init_end - BUILD_BIOS_ADDR);
        addSpace(&ZoneFSeg, (void*)BUILD_BIOS_ADDR, code32init_end);
    }

    calcRamSize();
}

void
malloc_prepboot(void)
{
    ASSERT32FLAT();
    dprintf(3, "malloc finalize\n");

    // Place an optionrom signature around used low mem area.
    u32 base = rom_get_max();
    struct rom_header *dummyrom = (void*)base;
    dummyrom->signature = OPTION_ROM_SIGNATURE;
    int size = (BUILD_BIOS_ADDR - base) / 512;
    dummyrom->size = (size > 255) ? 255 : size;
    memset((void*)RomEnd, 0, base-RomEnd);

    // Clear unused f-seg ram.
    struct allocinfo_s *info = findLast(&ZoneFSeg);
    memset(info->dataend, 0, info->allocend - info->dataend);
    dprintf(1, "Space available for UMB: %x-%x, %x-%x\n"
            , RomEnd, base, (u32)info->dataend, (u32)info->allocend);

    // Give back unused high ram.
    info = findLast(&ZoneHigh);
    if (info) {
        u32 giveback = ALIGN_DOWN(info->allocend - info->dataend, PAGE_SIZE);
        add_e820((u32)info->dataend, giveback, E820_RAM);
        dprintf(1, "Returned %d bytes of ZoneHigh\n", giveback);
    }

    calcRamSize();
}


/****************************************************************
 * tracked memory allocations
 ****************************************************************/

// Allocate memory from the given zone and track it as a PMM allocation
void * __malloc
pmm_malloc(struct zone_s *zone, u32 handle, u32 size, u32 align)
{
    ASSERT32FLAT();
    if (!size)
        return NULL;

    // Find and reserve space for bookkeeping.
    struct allocdetail_s *detail = allocSpace(
        &ZoneTmpHigh, sizeof(*detail), MALLOC_MIN_ALIGN, NULL);
    if (!detail) {
        detail = allocSpace(&ZoneTmpLow, sizeof(*detail)
                            , MALLOC_MIN_ALIGN, NULL);
        if (!detail)
            return NULL;
    }

    // Find and reserve space for main allocation
    void *data = allocSpace(zone, size, align, &detail->datainfo);
    if (!data) {
        freeSpace(&detail->detailinfo);
        return NULL;
    }

    dprintf(8, "pmm_malloc zone=%p handle=%x size=%d align=%x"
            " ret=%p (detail=%p)\n"
            , zone, handle, size, align
            , data, detail);
    detail->handle = handle;

    return data;
}

// Free a data block allocated with pmm_malloc
int
pmm_free(void *data)
{
    ASSERT32FLAT();
    struct allocinfo_s *info = findAlloc(data);
    if (!info || data == (void*)info || data == info->dataend)
        return -1;
    struct allocdetail_s *detail = container_of(
        info, struct allocdetail_s, datainfo);
    dprintf(8, "pmm_free %p (detail=%p)\n", data, detail);
    freeSpace(info);
    freeSpace(&detail->detailinfo);
    return 0;
}

// Find the amount of free space in a given zone.
static u32
pmm_getspace(struct zone_s *zone)
{
    // XXX - doesn't account for ZoneLow being able to grow.
    // XXX - results not reliable when CONFIG_THREAD_OPTIONROMS
    u32 maxspace = 0;
    struct allocinfo_s *info;
    for (info = zone->info; info; info = info->next) {
        u32 space = info->allocend - info->dataend;
        if (space > maxspace)
            maxspace = space;
    }

    if (zone != &ZoneTmpHigh && zone != &ZoneTmpLow)
        return maxspace;
    // Account for space needed for PMM tracking.
    u32 reserve = ALIGN(sizeof(struct allocdetail_s), MALLOC_MIN_ALIGN);
    if (maxspace <= reserve)
        return 0;
    return maxspace - reserve;
}

// Find the data block allocated with pmm_malloc with a given handle.
static void *
pmm_find(u32 handle)
{
    int i;
    for (i=0; i<ARRAY_SIZE(Zones); i++) {
        struct zone_s *zone = Zones[i];
        struct allocinfo_s *info;
        for (info = zone->info; info; info = info->next) {
            if (info->data != (void*)info)
                continue;
            struct allocdetail_s *detail = container_of(
                info, struct allocdetail_s, detailinfo);
            if (detail->handle == handle)
                return detail->datainfo.data;
        }
    }
    return NULL;
}


/****************************************************************
 * pmm interface
 ****************************************************************/

struct pmmheader {
    u32 signature;
    u8 version;
    u8 length;
    u8 checksum;
    struct segoff_s entry;
    u8 reserved[5];
} PACKED;

extern struct pmmheader PMMHEADER;

#define PMM_SIGNATURE 0x4d4d5024 // $PMM

#if CONFIG_PMM
struct pmmheader PMMHEADER __aligned(16) VARFSEG = {
    .signature = PMM_SIGNATURE,
    .version = 0x01,
    .length = sizeof(PMMHEADER),
};
#endif

#define PMM_FUNCTION_NOT_SUPPORTED 0xffffffff

// PMM - allocate
static u32
handle_pmm00(u16 *args)
{
    u32 length = *(u32*)&args[1], handle = *(u32*)&args[3];
    u16 flags = args[5];
    dprintf(3, "pmm00: length=%x handle=%x flags=%x\n"
            , length, handle, flags);
    struct zone_s *lowzone = &ZoneTmpLow, *highzone = &ZoneTmpHigh;
    if (flags & 8) {
        // Permanent memory request.
        lowzone = &ZoneLow;
        highzone = &ZoneHigh;
    }
    if (!length) {
        // Memory size request
        switch (flags & 3) {
        default:
        case 0:
            return 0;
        case 1:
            return pmm_getspace(lowzone);
        case 2:
            return pmm_getspace(highzone);
        case 3: {
            u32 spacelow = pmm_getspace(lowzone);
            u32 spacehigh = pmm_getspace(highzone);
            if (spacelow > spacehigh)
                return spacelow;
            return spacehigh;
        }
        }
    }
    u32 size = length * 16;
    if ((s32)size <= 0)
        return 0;
    u32 align = MALLOC_MIN_ALIGN;
    if (flags & 4) {
        align = 1<<__ffs(size);
        if (align < MALLOC_MIN_ALIGN)
            align = MALLOC_MIN_ALIGN;
    }
    switch (flags & 3) {
    default:
    case 0:
        return 0;
    case 1:
        return (u32)pmm_malloc(lowzone, handle, size, align);
    case 2:
        return (u32)pmm_malloc(highzone, handle, size, align);
    case 3: {
        void *data = pmm_malloc(lowzone, handle, size, align);
        if (data)
            return (u32)data;
        return (u32)pmm_malloc(highzone, handle, size, align);
    }
    }
}

// PMM - find
static u32
handle_pmm01(u16 *args)
{
    u32 handle = *(u32*)&args[1];
    dprintf(3, "pmm01: handle=%x\n", handle);
    if (handle == PMM_DEFAULT_HANDLE)
        return 0;
    return (u32)pmm_find(handle);
}

// PMM - deallocate
static u32
handle_pmm02(u16 *args)
{
    u32 buffer = *(u32*)&args[1];
    dprintf(3, "pmm02: buffer=%x\n", buffer);
    int ret = pmm_free((void*)buffer);
    if (ret)
        // Error
        return 1;
    return 0;
}

static u32
handle_pmmXX(u16 *args)
{
    return PMM_FUNCTION_NOT_SUPPORTED;
}

u32 VISIBLE32INIT
handle_pmm(u16 *args)
{
    ASSERT32FLAT();
    if (! CONFIG_PMM)
        return PMM_FUNCTION_NOT_SUPPORTED;

    u16 arg1 = args[0];
    dprintf(DEBUG_HDL_pmm, "pmm call arg1=%x\n", arg1);

    u32 ret;
    switch (arg1) {
    case 0x00: ret = handle_pmm00(args); break;
    case 0x01: ret = handle_pmm01(args); break;
    case 0x02: ret = handle_pmm02(args); break;
    default:   ret = handle_pmmXX(args); break;
    }

    return ret;
}

void
pmm_init(void)
{
    if (! CONFIG_PMM)
        return;

    dprintf(3, "init PMM\n");

    PMMHEADER.entry = FUNC16(entry_pmm);
    PMMHEADER.checksum -= checksum(&PMMHEADER, sizeof(PMMHEADER));
}

void
pmm_prepboot(void)
{
    if (! CONFIG_PMM)
        return;

    dprintf(3, "finalize PMM\n");

    PMMHEADER.signature = 0;
    PMMHEADER.entry.segoff = 0;
}
