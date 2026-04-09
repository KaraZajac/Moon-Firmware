#include "elf_file.h"
#include "elf_file_i.h"
#include "elf_file_xip.h"

#include <storage/storage.h>
#include <elf.h>
#include <furi_hal_flash.h>
#include <toolbox/crc32_calc.h>
#include "elf_api_interface.h"
#include "../api_hashtable/api_hashtable.h"

#define TAG "Elf"

#define ELF_NAME_BUFFER_LEN        32
#define SECTION_OFFSET(e, n)       ((e)->section_table + (n) * sizeof(Elf32_Shdr))
#define IS_FLAGS_SET(v, m)         (((v) & (m)) == (m))
#define RESOLVER_THREAD_YIELD_STEP 30
#define FAST_RELOCATION_VERSION    1

// #define ELF_DEBUG_LOG 1

#ifndef ELF_DEBUG_LOG
#undef FURI_LOG_D
#define FURI_LOG_D(...)
#endif

#define ELF_INVALID_ADDRESS 0xFFFFFFFF

#define TRAMPOLINE_CODE_SIZE 6

/**
ldr r12, [pc, #2]
bx r12
*/
const uint8_t trampoline_code_little_endian[TRAMPOLINE_CODE_SIZE] =
    {0xdf, 0xf8, 0x02, 0xc0, 0x60, 0x47};

typedef struct {
    uint8_t code[TRAMPOLINE_CODE_SIZE];
    uint32_t addr;
} FURI_PACKED JMPTrampoline;

/**************************************************************************************************/
/********************************************* Caches *********************************************/
/**************************************************************************************************/

static bool address_cache_get(AddressCache_t cache, int symEntry, Elf32_Addr* symAddr) {
    Elf32_Addr* addr = AddressCache_get(cache, symEntry);
    if(addr) {
        *symAddr = *addr;
        return true;
    } else {
        return false;
    }
}

static void address_cache_put(AddressCache_t cache, int symEntry, Elf32_Addr symAddr) {
    AddressCache_set_at(cache, symEntry, symAddr);
}

/**************************************************************************************************/
/********************************************** ELF ***********************************************/
/**************************************************************************************************/

static void elf_file_maybe_release_fd(ELFFile* elf) {
    if(elf->fd) {
        storage_file_free(elf->fd);
        elf->fd = NULL;
    }
}

static ELFSection* elf_file_get_section(ELFFile* elf, const char* name) {
    return ELFSectionDict_get(elf->sections, name);
}

static ELFSection* elf_file_get_or_put_section(ELFFile* elf, const char* name) {
    ELFSection* section_p = elf_file_get_section(elf, name);
    if(!section_p) {
        ELFSectionDict_set_at(
            elf->sections,
            strdup(name),
            (ELFSection){
                .data = NULL,
                .exec_addr = 0,
                .sec_idx = 0,
                .size = 0,
                .sh_flags = 0,
                .file_offset = 0,
                .file_align = 0,
                .xip = false,
                .rel_count = 0,
                .rel_offset = 0,
                .fast_rel = NULL,
            });
        section_p = elf_file_get_section(elf, name);
    }

    return section_p;
}

static bool elf_read_string_from_offset(ELFFile* elf, off_t offset, FuriString* name) {
    bool result = false;

    off_t old = storage_file_tell(elf->fd);

    do {
        if(!storage_file_seek(elf->fd, offset, true)) break;

        char buffer[ELF_NAME_BUFFER_LEN + 1];
        buffer[ELF_NAME_BUFFER_LEN] = 0;

        while(true) {
            size_t read = storage_file_read(elf->fd, buffer, ELF_NAME_BUFFER_LEN);
            furi_string_cat(name, buffer);
            if(strlen(buffer) < ELF_NAME_BUFFER_LEN) {
                result = true;
                break;
            }

            if(storage_file_get_error(elf->fd) != FSE_OK || read == 0) break;
        }

    } while(false);
    storage_file_seek(elf->fd, old, true);

    return result;
}

static bool elf_read_section_name(ELFFile* elf, off_t offset, FuriString* name) {
    return elf_read_string_from_offset(elf, elf->section_table_strings + offset, name);
}

static bool elf_read_symbol_name(ELFFile* elf, off_t offset, FuriString* name) {
    return elf_read_string_from_offset(elf, elf->symbol_table_strings + offset, name);
}

static bool elf_read_section_header(ELFFile* elf, size_t section_idx, Elf32_Shdr* section_header) {
    off_t offset = SECTION_OFFSET(elf, section_idx);
    return storage_file_seek(elf->fd, offset, true) &&
           storage_file_read(elf->fd, section_header, sizeof(Elf32_Shdr)) == sizeof(Elf32_Shdr);
}

static bool elf_read_section(
    ELFFile* elf,
    size_t section_idx,
    Elf32_Shdr* section_header,
    FuriString* name) {
    if(!elf_read_section_header(elf, section_idx, section_header)) {
        return false;
    }

    if(section_header->sh_name && !elf_read_section_name(elf, section_header->sh_name, name)) {
        return false;
    }

    return true;
}

static bool elf_read_symbol(ELFFile* elf, int n, Elf32_Sym* sym, FuriString* name) {
    bool success = false;
    off_t old = storage_file_tell(elf->fd);
    off_t pos = elf->symbol_table + n * sizeof(Elf32_Sym);
    if(storage_file_seek(elf->fd, pos, true) &&
       storage_file_read(elf->fd, sym, sizeof(Elf32_Sym)) == sizeof(Elf32_Sym)) {
        if(sym->st_name)
            success = elf_read_symbol_name(elf, sym->st_name, name);
        else {
            Elf32_Shdr shdr;
            success = elf_read_section(elf, sym->st_shndx, &shdr, name);
        }
    }
    storage_file_seek(elf->fd, old, true);
    return success;
}

static ELFSection* elf_section_of(ELFFile* elf, int index) {
    ELFSectionDict_it_t it;
    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it); ELFSectionDict_next(it)) {
        ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
        if(itref->value.sec_idx == index) {
            return &itref->value;
        }
    }

    return NULL;
}

static Elf32_Addr elf_address_of(ELFFile* elf, Elf32_Sym* sym, const char* sName) {
    if(sym->st_shndx == SHN_UNDEF) {
        Elf32_Addr addr = 0;
        uint32_t hash = elf_symbolname_hash(sName);
        if(elf->api_interface->resolver_callback(elf->api_interface, hash, &addr)) {
            return addr;
        }
    } else {
        ELFSection* symSec = elf_section_of(elf, sym->st_shndx);
        if(symSec) {
            return (symSec->exec_addr) + sym->st_value;
        }
    }
    FURI_LOG_D(TAG, "  Can not find address for symbol %s", sName);
    return ELF_INVALID_ADDRESS;
}

__attribute__((unused)) static const char* elf_reloc_type_to_str(int symt) {
#define STRCASE(name) \
    case name:        \
        return #name;
    switch(symt) {
        STRCASE(R_ARM_NONE)
        STRCASE(R_ARM_TARGET1)
        STRCASE(R_ARM_ABS32)
        STRCASE(R_ARM_REL32)
        STRCASE(R_ARM_THM_PC22)
        STRCASE(R_ARM_THM_JUMP24)
    default:
        return "R_<unknow>";
    }
#undef STRCASE
}

static JMPTrampoline* elf_create_trampoline(Elf32_Addr addr) {
    JMPTrampoline* trampoline = malloc(sizeof(JMPTrampoline));
    memcpy(trampoline->code, trampoline_code_little_endian, TRAMPOLINE_CODE_SIZE);
    trampoline->addr = addr;
    return trampoline;
}

/**
 * @param patchAddr  RAM address to read/write instruction bytes
 * @param relAddr    Runtime (exec) address for PC-relative offset calculation
 */
static void elf_relocate_jmp_call(
    ELFFile* elf,
    Elf32_Addr patchAddr,
    Elf32_Addr relAddr,
    int type,
    Elf32_Addr symAddr) {
    int offset, hi, lo, s, j1, j2, i1, i2, imm10, imm11;
    int to_thumb, is_call, blx_bit = 1 << 12;

    /* Get initial offset — read from RAM staging buffer */
    hi = ((uint16_t*)patchAddr)[0];
    lo = ((uint16_t*)patchAddr)[1];
    s = (hi >> 10) & 1;
    j1 = (lo >> 13) & 1;
    j2 = (lo >> 11) & 1;
    i1 = (j1 ^ s) ^ 1;
    i2 = (j2 ^ s) ^ 1;
    imm10 = hi & 0x3ff;
    imm11 = lo & 0x7ff;
    offset = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1);
    if(offset & 0x01000000) offset -= 0x02000000;

    to_thumb = symAddr & 1;
    is_call = (type == R_ARM_THM_PC22);

    /* Store offset */
    int offset_copy = offset;

    /* Compute final offset — PC-relative from runtime address */
    offset += symAddr - relAddr;
    if(!to_thumb && is_call) {
        blx_bit = 0; /* bl -> blx */
        offset = (offset + 3) & -4; /* Compute offset from aligned PC */
    }

    /* Check that relocation is possible
    * offset must not be out of range
    * if target is to be entered in arm mode:
        - bit 1 must not set
        - instruction must be a call (bl) or a jump to PLT */
    if(!to_thumb || offset >= 0x1000000 || offset < -0x1000000) {
        if(to_thumb || (symAddr & 2) || (!is_call)) {
            FURI_LOG_D(
                TAG,
                "can't relocate value at %lx, %s, doing trampoline",
                relAddr,
                elf_reloc_type_to_str(type));

            Elf32_Addr addr;
            if(!address_cache_get(elf->trampoline_cache, symAddr, &addr)) {
                addr = (Elf32_Addr)elf_create_trampoline(symAddr);
                address_cache_put(elf->trampoline_cache, symAddr, addr);
            }

            offset = offset_copy;
            offset += (int)addr - relAddr;
            if(!to_thumb && is_call) {
                blx_bit = 0; /* bl -> blx */
                offset = (offset + 3) & -4; /* Compute offset from aligned PC */
            }
        }
    }

    /* Compute and store final offset — write to RAM staging buffer */
    s = (offset >> 24) & 1;
    i1 = (offset >> 23) & 1;
    i2 = (offset >> 22) & 1;
    j1 = s ^ (i1 ^ 1);
    j2 = s ^ (i2 ^ 1);
    imm10 = (offset >> 12) & 0x3ff;
    imm11 = (offset >> 1) & 0x7ff;
    (*(uint16_t*)patchAddr) = (uint16_t)((hi & 0xf800) | (s << 10) | imm10);
    (*(uint16_t*)(patchAddr + 2)) =
        (uint16_t)((lo & 0xc000) | (j1 << 13) | blx_bit | (j2 << 11) | imm11);
}

static void elf_relocate_mov(Elf32_Addr patchAddr, int type, Elf32_Addr symAddr) {
    uint16_t upper_insn = ((uint16_t*)patchAddr)[0];
    uint16_t lower_insn = ((uint16_t*)patchAddr)[1];

    /* MOV*<C> <Rd>,#<imm16>
     *
     * i = upper[10]
     * imm4 = upper[3:0]
     * imm3 = lower[14:12]
     * imm8 = lower[7:0]
     *
     * imm16 = imm4:i:imm3:imm8
     */
    uint32_t i = (upper_insn >> 10) & 1; /* upper[10] */
    uint32_t imm4 = upper_insn & 0x000F; /* upper[3:0] */
    uint32_t imm3 = (lower_insn >> 12) & 0x7; /* lower[14:12] */
    uint32_t imm8 = lower_insn & 0x00FF; /* lower[7:0] */

    int32_t addend = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8; /* imm16 */

    uint32_t addr = (symAddr + addend);
    if(type == R_ARM_THM_MOVT_ABS) {
        addr >>= 16; /* upper 16 bits */
    } else {
        addr &= 0x0000FFFF; /* lower 16 bits */
    }

    /* Re-encode — write to RAM staging buffer */
    ((uint16_t*)patchAddr)[0] = (upper_insn & 0xFBF0) | (((addr >> 11) & 1) << 10) /* i */
                                | ((addr >> 12) & 0x000F); /* imm4 */
    ((uint16_t*)patchAddr)[1] = (lower_insn & 0x8F00) | (((addr >> 8) & 0x7) << 12) /* imm3 */
                                | (addr & 0x00FF); /* imm8 */
}

/**
 * @param patchAddr  RAM address where instruction bytes are read/written
 * @param relAddr    Runtime address for PC-relative relocations (flash for XIP sections)
 */
static bool elf_relocate_symbol(
    ELFFile* elf,
    Elf32_Addr patchAddr,
    Elf32_Addr relAddr,
    int type,
    Elf32_Addr symAddr) {
    switch(type) {
    case R_ARM_TARGET1:
    case R_ARM_ABS32:
        *((uint32_t*)patchAddr) += symAddr;
        FURI_LOG_D(
            TAG, "  R_ARM_ABS32 relocated is 0x%08X", (unsigned int)*((uint32_t*)patchAddr));
        break;
    case R_ARM_REL32:
        *((uint32_t*)patchAddr) += symAddr - relAddr;
        FURI_LOG_D(
            TAG, "  R_ARM_REL32 relocated is 0x%08X", (unsigned int)*((uint32_t*)patchAddr));
        break;
    case R_ARM_THM_PC22:
    case R_ARM_CALL:
    case R_ARM_THM_JUMP24:
        elf_relocate_jmp_call(elf, patchAddr, relAddr, type, symAddr);
        FURI_LOG_D(
            TAG,
            "  R_ARM_THM_CALL/JMP relocated is 0x%08X",
            (unsigned int)*((uint32_t*)patchAddr));
        break;
    case R_ARM_THM_MOVW_ABS_NC:
    case R_ARM_THM_MOVT_ABS:
        elf_relocate_mov(patchAddr, type, symAddr);
        FURI_LOG_D(
            TAG,
            "  R_ARM_THM_MOVW_ABS_NC/MOVT_ABS relocated is 0x%08X",
            (unsigned int)*((uint32_t*)patchAddr));
        break;
    default:
        FURI_LOG_E(TAG, "  Undefined relocation %d", type);
        return false;
    }
    return true;
}

static bool elf_relocate(ELFFile* elf, ELFSection* s) {
    if(s->data) {
        Elf32_Rel rel;
        size_t relEntries = s->rel_count;
        size_t relCount;
        (void)storage_file_seek(elf->fd, s->rel_offset, true);
        FURI_LOG_D(TAG, " Offset   Info     Type             Name");

        int relocate_result = true;
        FuriString* symbol_name;
        symbol_name = furi_string_alloc();

        for(relCount = 0; relCount < relEntries; relCount++) {
            if(relCount % RESOLVER_THREAD_YIELD_STEP == 0) {
                FURI_LOG_D(TAG, "  reloc YIELD");
                furi_delay_tick(1);
            }

            if(storage_file_read(elf->fd, &rel, sizeof(Elf32_Rel)) != sizeof(Elf32_Rel)) {
                FURI_LOG_E(TAG, "  reloc read fail");
                furi_string_free(symbol_name);
                return false;
            }

            Elf32_Addr symAddr;

            int symEntry = ELF32_R_SYM(rel.r_info);
            int relType = ELF32_R_TYPE(rel.r_info);
            Elf32_Addr patchAddr = ((Elf32_Addr)s->data) + rel.r_offset;
            Elf32_Addr relAddr = (s->exec_addr) + rel.r_offset;

            if(!address_cache_get(elf->relocation_cache, symEntry, &symAddr)) {
                Elf32_Sym sym;
                furi_string_reset(symbol_name);
                if(!elf_read_symbol(elf, symEntry, &sym, symbol_name)) {
                    FURI_LOG_E(TAG, "  symbol read fail");
                    furi_string_free(symbol_name);
                    return false;
                }

                FURI_LOG_D(
                    TAG,
                    " %08X %08X %-16s %s",
                    (unsigned int)rel.r_offset,
                    (unsigned int)rel.r_info,
                    elf_reloc_type_to_str(relType),
                    furi_string_get_cstr(symbol_name));

                symAddr = elf_address_of(elf, &sym, furi_string_get_cstr(symbol_name));
                address_cache_put(elf->relocation_cache, symEntry, symAddr);
            }

            if(symAddr != ELF_INVALID_ADDRESS) {
                FURI_LOG_D(
                    TAG,
                    "  symAddr=%08X relAddr=%08X",
                    (unsigned int)symAddr,
                    (unsigned int)relAddr);
                if(!elf_relocate_symbol(elf, patchAddr, relAddr, relType, symAddr)) {
                    relocate_result = false;
                }
            } else {
                FURI_LOG_E(TAG, "  No symbol address of %s", furi_string_get_cstr(symbol_name));
                relocate_result = false;
            }
        }
        furi_string_free(symbol_name);

        return relocate_result;
    } else {
        FURI_LOG_D(TAG, "Section not loaded");
    }

    return false;
}

/**************************************************************************************************/
/************************************ Internal FAP interfaces *************************************/
/**************************************************************************************************/
typedef enum {
    SectionTypeUnused = 1 << 0,
    SectionTypeData = 1 << 1,
    SectionTypeRelData = 1 << 2,
    SectionTypeSymTab = 1 << 3,
    SectionTypeStrTab = 1 << 4,
    SectionTypeDebugLink = 1 << 5,
    SectionTypeFastRelData = 1 << 6,
} SectionType;

static bool elf_load_debug_link(ELFFile* elf, Elf32_Shdr* section_header) {
    elf->debug_link_info.debug_link_size = section_header->sh_size;
    elf->debug_link_info.debug_link = malloc(section_header->sh_size);

    return storage_file_seek(elf->fd, section_header->sh_offset, true) &&
           storage_file_read(elf->fd, elf->debug_link_info.debug_link, section_header->sh_size) ==
               section_header->sh_size;
}

static bool str_prefix(const char* str, const char* prefix) {
    return strncmp(prefix, str, strlen(prefix)) == 0;
}

typedef enum {
    ELFLoadSectionResultSuccess,
    ELFLoadSectionResultNoMemory,
    ELFLoadSectionResultError,
} ELFLoadSectionResult;

typedef struct {
    SectionType type;
    ELFLoadSectionResult result;
} SectionTypeInfo;

/** Save section metadata without allocating RAM or reading data.
 *  Actual loading is deferred to elf_materialize_section().
 */
static ELFLoadSectionResult
    elf_save_section_metadata(ELFSection* section, Elf32_Shdr* section_header) {
    section->size = section_header->sh_size;
    section->file_offset = section_header->sh_offset;
    section->file_align = section_header->sh_addralign;
    section->data = NULL;

    if(section_header->sh_type == SHT_NOBITS) {
        /* BSS: allocate zeroed RAM immediately (cheap, always needed) */
        if(section_header->sh_size > 0) {
            section->data = aligned_malloc(section_header->sh_size, section_header->sh_addralign);
        }
    }

    return ELFLoadSectionResultSuccess;
}

/** Allocate RAM and read section data from the ELF file.
 *  Called after XIP setup so we know which sections need RAM.
 */
static ELFLoadSectionResult
    elf_materialize_section(ELFFile* elf, ELFSection* section) {
    if(section->size == 0 || section->data != NULL) {
        /* Already materialized (BSS) or empty */
        return ELFLoadSectionResultSuccess;
    }

    size_t safe_size = section->size + 1024;

    furi_kernel_lock();

    if(memmgr_heap_get_max_free_block() < safe_size) {
        furi_kernel_unlock();
        FURI_LOG_E(TAG, "Not enough memory to load section data (%lu bytes)", section->size);
        return ELFLoadSectionResultNoMemory;
    }

    section->data = aligned_malloc(section->size, section->file_align);

    furi_kernel_unlock();

    if(!storage_file_seek(elf->fd, section->file_offset, true)) {
        FURI_LOG_E(TAG, "    seek fail");
        aligned_free(section->data);
        section->data = NULL;
        return ELFLoadSectionResultError;
    }

    /* Read section data — handle sections > 64KB via chunked reads
     * (storage_file_read returns uint16_t, max 65535 bytes per call) */
    size_t total_read = 0;
    while(total_read < section->size) {
        size_t chunk = section->size - total_read;
        if(chunk > 0xFFFF) chunk = 0xFFFF;
        uint16_t got =
            storage_file_read(elf->fd, (uint8_t*)section->data + total_read, chunk);
        if(got == 0) {
            FURI_LOG_E(TAG, "    read fail at %zu/%lu", total_read, section->size);
            aligned_free(section->data);
            section->data = NULL;
            return ELFLoadSectionResultError;
        }
        total_read += got;
    }

    FURI_LOG_D(TAG, "Materialized %lu bytes at 0x%p", section->size, section->data);
    return ELFLoadSectionResultSuccess;
}

static SectionTypeInfo elf_preload_section(
    ELFFile* elf,
    size_t section_idx,
    Elf32_Shdr* section_header,
    FuriString* name_string) {
    const char* name = furi_string_get_cstr(name_string);
    SectionTypeInfo info;

#ifdef ELF_DEBUG_LOG
    // log section name, type and flags
    FuriString* flags_string = furi_string_alloc();
    if(section_header->sh_flags & SHF_WRITE) furi_string_cat(flags_string, "W");
    if(section_header->sh_flags & SHF_ALLOC) furi_string_cat(flags_string, "A");
    if(section_header->sh_flags & SHF_EXECINSTR) furi_string_cat(flags_string, "X");
    if(section_header->sh_flags & SHF_MERGE) furi_string_cat(flags_string, "M");
    if(section_header->sh_flags & SHF_STRINGS) furi_string_cat(flags_string, "S");
    if(section_header->sh_flags & SHF_INFO_LINK) furi_string_cat(flags_string, "I");
    if(section_header->sh_flags & SHF_LINK_ORDER) furi_string_cat(flags_string, "L");
    if(section_header->sh_flags & SHF_OS_NONCONFORMING) furi_string_cat(flags_string, "O");
    if(section_header->sh_flags & SHF_GROUP) furi_string_cat(flags_string, "G");
    if(section_header->sh_flags & SHF_TLS) furi_string_cat(flags_string, "T");
    if(section_header->sh_flags & SHF_COMPRESSED) furi_string_cat(flags_string, "T");
    if(section_header->sh_flags & SHF_MASKOS) furi_string_cat(flags_string, "o");
    if(section_header->sh_flags & SHF_MASKPROC) furi_string_cat(flags_string, "p");
    if(section_header->sh_flags & SHF_ORDERED) furi_string_cat(flags_string, "R");
    if(section_header->sh_flags & SHF_EXCLUDE) furi_string_cat(flags_string, "E");

    FURI_LOG_I(
        TAG,
        "Section %s: type: %ld, flags: %s",
        name,
        section_header->sh_type,
        furi_string_get_cstr(flags_string));
    furi_string_free(flags_string);
#endif

    // ignore .ARM and .rel.ARM sections
    // TODO FL-3525: how to do it not by name?
    // .ARM: type 0x70000001, flags SHF_ALLOC | SHF_LINK_ORDER
    // .rel.ARM: type 0x9, flags SHT_REL
    if(str_prefix(name, ".ARM.") || str_prefix(name, ".rel.ARM.") ||
       str_prefix(name, ".fast.rel.ARM.")) {
        FURI_LOG_D(TAG, "Ignoring ARM section");

        info.type = SectionTypeUnused;
        info.result = ELFLoadSectionResultSuccess;
        return info;
    }

    // Load allocable section
    if(section_header->sh_flags & SHF_ALLOC) {
        ELFSection* section_p = elf_file_get_or_put_section(elf, name);
        section_p->sec_idx = section_idx;
        section_p->sh_flags = section_header->sh_flags;

        if(section_header->sh_type == SHT_PREINIT_ARRAY) {
            furi_assert(elf->preinit_array == NULL);
            elf->preinit_array = section_p;
        } else if(section_header->sh_type == SHT_INIT_ARRAY) {
            furi_assert(elf->init_array == NULL);
            elf->init_array = section_p;
        } else if(section_header->sh_type == SHT_FINI_ARRAY) {
            furi_assert(elf->fini_array == NULL);
            elf->fini_array = section_p;
        }

        info.type = SectionTypeData;
        info.result = elf_save_section_metadata(section_p, section_header);

        if(info.result != ELFLoadSectionResultSuccess) {
            FURI_LOG_E(TAG, "Error saving metadata for section '%s'", name);
        }

        return info;
    }

    // Load link info section
    if(section_header->sh_flags & SHF_INFO_LINK) {
        info.type = SectionTypeRelData;

        if(str_prefix(name, ".rel")) {
            name = name + strlen(".rel");
            ELFSection* section_p = elf_file_get_or_put_section(elf, name);
            section_p->rel_count = section_header->sh_size / sizeof(Elf32_Rel);
            section_p->rel_offset = section_header->sh_offset;
            info.result = ELFLoadSectionResultSuccess;
        } else {
            FURI_LOG_E(TAG, "Unknown link info section '%s'", name);
            info.result = ELFLoadSectionResultError;
        }

        return info;
    }

    // Load fast rel section
    if(str_prefix(name, ".fast.rel")) {
        name = name + strlen(".fast.rel");
        ELFSection* section_p = elf_file_get_or_put_section(elf, name);
        section_p->fast_rel = malloc(sizeof(ELFSection));

        info.type = SectionTypeFastRelData;
        /* Fast rel data is small metadata — save and materialize immediately */
        info.result = elf_save_section_metadata(section_p->fast_rel, section_header);
        if(info.result == ELFLoadSectionResultSuccess) {
            info.result = elf_materialize_section(elf, section_p->fast_rel);
        }

        if(info.result != ELFLoadSectionResultSuccess) {
            FURI_LOG_E(TAG, "Error loading section '%s'", name);
        } else {
            FURI_LOG_D(TAG, "Loaded fast rel section for '%s'", name);
        }

        return info;
    }

    // Load symbol table
    if(strcmp(name, ".symtab") == 0) {
        FURI_LOG_D(TAG, "Found .symtab section");
        elf->symbol_table = section_header->sh_offset;
        elf->symbol_count = section_header->sh_size / sizeof(Elf32_Sym);

        info.type = SectionTypeSymTab;
        info.result = ELFLoadSectionResultSuccess;
        return info;
    }

    // Load string table
    if(strcmp(name, ".strtab") == 0) {
        FURI_LOG_D(TAG, "Found .strtab section");
        elf->symbol_table_strings = section_header->sh_offset;

        info.type = SectionTypeStrTab;
        info.result = ELFLoadSectionResultSuccess;
        return info;
    }

    // Load debug link section
    if(strcmp(name, ".gnu_debuglink") == 0) {
        FURI_LOG_D(TAG, "Found .gnu_debuglink section");
        info.type = SectionTypeDebugLink;

        if(elf_load_debug_link(elf, section_header)) {
            info.result = ELFLoadSectionResultSuccess;
            return info;
        } else {
            info.result = ELFLoadSectionResultError;
            return info;
        }
    }

    info.type = SectionTypeUnused;
    info.result = ELFLoadSectionResultSuccess;
    return info;
}

static Elf32_Addr elf_address_of_by_hash(ELFFile* elf, uint32_t hash) {
    Elf32_Addr addr = 0;
    if(elf->api_interface->resolver_callback(elf->api_interface, hash, &addr)) {
        return addr;
    }
    return ELF_INVALID_ADDRESS;
}

static bool elf_file_find_string_by_hash(ELFFile* elf, uint32_t hash, FuriString* out) {
    bool result = false;

    FuriString* symbol_name = furi_string_alloc();
    Elf32_Sym sym;
    for(size_t i = 0; i < elf->symbol_count; i++) {
        furi_string_reset(symbol_name);
        if(elf_read_symbol(elf, i, &sym, symbol_name)) {
            if(elf_symbolname_hash(furi_string_get_cstr(symbol_name)) == hash) {
                furi_string_set(out, symbol_name);
                result = true;
                break;
            }
        }
    }
    furi_string_free(symbol_name);

    return result;
}

static bool elf_relocate_fast(ELFFile* elf, ELFSection* s) {
    UNUSED(elf);
    const uint8_t* start = s->fast_rel->data;
    const uint8_t version = *start;
    bool no_errors = true;

    if(version != FAST_RELOCATION_VERSION) {
        FURI_LOG_E(TAG, "Unsupported fast relocation version %d", version);
        return false;
    }
    start += 1;

    const uint32_t records_count = *((uint32_t*)start);
    start += 4;
    FURI_LOG_D(TAG, "Fast relocation records count: %ld", records_count);

    for(uint32_t i = 0; i < records_count; i++) {
        bool is_section = (*start & (0x1 << 7)) ? true : false;
        uint8_t type = *start & 0x7F;
        start += 1;
        uint32_t hash_or_section_index = *((uint32_t*)start);
        start += 4;

        uint32_t section_value = ELF_INVALID_ADDRESS;
        if(is_section) {
            section_value = *((uint32_t*)start);
            start += 4;
        }

        const uint32_t offsets_count = *((uint32_t*)start);
        start += 4;

        FURI_LOG_D(
            TAG,
            "Fast relocation record %ld: is_section=%d, type=%d, hash_or_section_index=%lX, offsets_count=%ld",
            i,
            is_section,
            type,
            hash_or_section_index,
            offsets_count);

        Elf32_Addr address = 0;
        if(is_section) {
            ELFSection* symSec = elf_section_of(elf, hash_or_section_index);
            if(symSec) {
                address = (symSec->exec_addr) + section_value;
            }
        } else {
            address = elf_address_of_by_hash(elf, hash_or_section_index);
        }

        if(address == ELF_INVALID_ADDRESS) {
            FuriString* symbol_name = furi_string_alloc();
            if(elf_file_find_string_by_hash(elf, hash_or_section_index, symbol_name)) {
                FURI_LOG_E(
                    TAG,
                    "Failed to resolve address for symbol %s (hash %lX)",
                    furi_string_get_cstr(symbol_name),
                    hash_or_section_index);
            } else {
                FURI_LOG_E(
                    TAG,
                    "Failed to resolve address for hash %lX (string not found)",
                    hash_or_section_index);
            }
            furi_string_free(symbol_name);

            no_errors = false;
            start += 3 * offsets_count;
        } else {
            for(uint32_t j = 0; j < offsets_count; j++) {
                uint32_t offset = *((uint32_t*)start) & 0x00FFFFFF;
                start += 3;
                Elf32_Addr patchAddr = ((Elf32_Addr)s->data) + offset;
                Elf32_Addr relAddr = (s->exec_addr) + offset;
                elf_relocate_symbol(elf, patchAddr, relAddr, type, address);
            }
        }
    }

    aligned_free(s->fast_rel->data);
    free(s->fast_rel);
    s->fast_rel = NULL;

    return no_errors;
}

static bool elf_relocate_section(ELFFile* elf, ELFSection* section) {
    if(section->fast_rel) {
        FURI_LOG_D(TAG, "Fast relocating section");
        return elf_relocate_fast(elf, section);
    } else if(section->rel_count) {
        FURI_LOG_D(TAG, "Relocating section");
        return elf_relocate(elf, section);
    } else {
        FURI_LOG_D(TAG, "No relocation index"); /* Not an error */
    }
    return true;
}

static void elf_file_call_section_list(ELFSection* section, bool reverse_order) {
    if(section && section->size) {
        const uint32_t* start = section->data;
        const uint32_t* end = section->data + section->size;

        if(reverse_order) {
            while(end > start) {
                end--;
                ((void (*)(void))(*end))();
            }
        } else {
            while(start < end) {
                ((void (*)(void))(*start))();
                start++;
            }
        }
    }
}

/**************************************************************************************************/
/********************************************* Public *********************************************/
/**************************************************************************************************/

ELFFile* elf_file_alloc(Storage* storage, const ElfApiInterface* api_interface) {
    ELFFile* elf = malloc(sizeof(ELFFile));
    elf->fd = storage_file_alloc(storage);
    elf->api_interface = api_interface;
    elf->xip_disabled = false;
    ELFSectionDict_init(elf->sections);
    AddressCache_init(elf->trampoline_cache);
    elf->init_array_called = false;
    memset(&elf->xip_region, 0, sizeof(XipRegion));
    return elf;
}

void elf_file_disable_xip(ELFFile* elf) {
    furi_check(elf);
    elf->xip_disabled = true;
}

void elf_file_free(ELFFile* elf) {
    /* Release XIP flash region so the next app can use it */
    xip_region_release(&elf->xip_region);

    // furi_check(!elf->init_array_called);
    if(elf->init_array_called) {
        FURI_LOG_W(TAG, "Init array was called, but fini array wasn't");
        elf_file_call_section_list(elf->fini_array, true);
    }

    // free sections data
    {
        ELFSectionDict_it_t it;
        for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
            ELFSectionDict_next(it)) {
            const ELFSectionDict_itref_t* itref = ELFSectionDict_cref(it);
            /* XIP sections: data points to flash after commit — do not free */
            if(!itref->value.xip) {
                aligned_free(itref->value.data);
            }
            if(itref->value.fast_rel) {
                aligned_free(itref->value.fast_rel->data);
                free(itref->value.fast_rel);
            }
            free((void*)itref->key);
        }

        ELFSectionDict_clear(elf->sections);
    }

    // free trampoline data
    {
        AddressCache_it_t it;
        for(AddressCache_it(it, elf->trampoline_cache); !AddressCache_end_p(it);
            AddressCache_next(it)) {
            const AddressCache_itref_t* itref = AddressCache_cref(it);
            free((void*)itref->value);
        }

        AddressCache_clear(elf->trampoline_cache);
    }

    if(elf->debug_link_info.debug_link) {
        free(elf->debug_link_info.debug_link);
    }

    elf_file_maybe_release_fd(elf);
    free(elf);
}

bool elf_file_open(ELFFile* elf, const char* path) {
    Elf32_Ehdr h;
    Elf32_Shdr sH;

    if(!storage_file_open(elf->fd, path, FSAM_READ, FSOM_OPEN_EXISTING) ||
       !storage_file_seek(elf->fd, 0, true) ||
       storage_file_read(elf->fd, &h, sizeof(h)) != sizeof(h) ||
       !storage_file_seek(elf->fd, h.e_shoff + h.e_shstrndx * sizeof(sH), true) ||
       storage_file_read(elf->fd, &sH, sizeof(Elf32_Shdr)) != sizeof(Elf32_Shdr)) {
        return false;
    }

    elf->entry = h.e_entry;
    elf->sections_count = h.e_shnum;
    elf->section_table = h.e_shoff;
    elf->section_table_strings = sH.sh_offset;
    return true;
}

static bool elf_section_is_xip_eligible(const ELFSection* section) {
    /* XIP-eligible: allocated, not writable, has size, and is not BSS (has file data) */
    if(!(section->sh_flags & SHF_ALLOC)) return false;
    if(section->sh_flags & SHF_WRITE) return false;
    if(section->size == 0) return false;
    if(section->file_offset == 0) return false; /* BSS or empty */
    return true;
}

static void elf_setup_xip(ELFFile* elf) {
    /* Plugins must not use XIP — they share the flash region with the main app */
    if(elf->xip_disabled) {
        memset(&elf->xip_region, 0, sizeof(XipRegion));
        FURI_LOG_D(TAG, "XIP disabled for this ELF (plugin mode)");
        return;
    }

    /* Check if all remaining sections fit in RAM — if so, skip XIP entirely.
     * RAM execution is faster and avoids flash wear.
     *
     * Only count sections that still need allocation (data == NULL); BSS is
     * pre-allocated earlier.  Use a generous margin because sections are
     * allocated sequentially: each allocation adds allocator overhead (~32B),
     * alignment padding, and elf_materialize_section adds a 1KB safety check
     * per section.  With 5+ sections this easily exceeds 4KB. */
    size_t remaining_alloc_size = 0;
    ELFSectionDict_it_t ram_it;
    for(ELFSectionDict_it(ram_it, elf->sections); !ELFSectionDict_end_p(ram_it);
        ELFSectionDict_next(ram_it)) {
        ELFSectionDict_itref_t* itref = ELFSectionDict_ref(ram_it);
        if((itref->value.sh_flags & SHF_ALLOC) && itref->value.data == NULL) {
            remaining_alloc_size += itref->value.size;
        }
    }

    furi_kernel_lock();
    size_t max_block = memmgr_heap_get_max_free_block();
    furi_kernel_unlock();

    size_t ram_needed = remaining_alloc_size + 32768; /* 32KB margin for fragmentation */

    if(ram_needed <= max_block) {
        FURI_LOG_I(
            TAG,
            "App fits in RAM (%zu bytes, %zu available) — skipping XIP",
            remaining_alloc_size,
            max_block);
        /* Don't initialize XIP — all sections will be loaded to RAM */
        memset(&elf->xip_region, 0, sizeof(XipRegion));
        return;
    }

    xip_region_init(&elf->xip_region);
    if(!elf->xip_region.active) return;

    /* Check if cached XIP data is still valid for this app */
    if(xip_cache_validate(
           &elf->xip_region,
           elf->fd,
           elf->api_interface->api_version_major,
           elf->api_interface->api_version_minor)) {
        /* Cache hit — restore section layout from the cached header */
        const XipCacheHeader* header = xip_cache_get_header(&elf->xip_region);
        ELFSectionDict_it_t it;

        for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
            ELFSectionDict_next(it)) {
            ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
            ELFSection* sec = &itref->value;

            if(!elf_section_is_xip_eligible(sec)) {
                sec->xip = false;
                continue;
            }

            /* Find this section in the cache header */
            bool found = false;
            for(uint32_t i = 0; i < header->section_count; i++) {
                if(strncmp(itref->key, header->sections[i].name, 15) == 0 &&
                   sec->size == header->sections[i].size) {
                    sec->exec_addr = elf->xip_region.base_addr + header->sections[i].flash_offset;
                    sec->data = (void*)sec->exec_addr;
                    sec->xip = true;
                    found = true;
                    FURI_LOG_I(
                        TAG,
                        "Cache restore: '%s' (%lu bytes) at 0x%08lX",
                        itref->key,
                        sec->size,
                        sec->exec_addr);
                    break;
                }
            }

            if(!found) {
                /* Section not in cache — invalidate and do full load */
                FURI_LOG_W(TAG, "Cache miss: section '%s' not found in cache", itref->key);
                elf->xip_region.cache_valid = false;
                break;
            }
        }

        if(elf->xip_region.cache_valid) {
            FURI_LOG_I(TAG, "XIP cache hit — skipping flash erase/write");
            return;
        }
        /* Cache invalidated during restore — clear any partially restored
         * sections so the fresh allocation path doesn't see stale flash
         * pointers in sec->data (which would cause elf_materialize_section
         * to skip loading, leaving the section pointing at old data). */
        FURI_LOG_I(TAG, "XIP cache invalidated, doing fresh load");
        ELFSectionDict_it_t reset_it;
        for(ELFSectionDict_it(reset_it, elf->sections); !ELFSectionDict_end_p(reset_it);
            ELFSectionDict_next(reset_it)) {
            ELFSectionDict_itref_t* ritref = ELFSectionDict_ref(reset_it);
            if(ritref->value.xip) {
                ritref->value.data = NULL;
                ritref->value.exec_addr = 0;
                ritref->value.xip = false;
            }
        }
    }

    /* Reset bump allocator for fresh allocation */
    elf->xip_region.next_free = elf->xip_region.data_start;
    elf->xip_region.cache_valid = false;

    /* Calculate total XIP size needed (with 8-byte alignment per section) */
    size_t xip_total = 0;
    ELFSectionDict_it_t it;
    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
        ELFSectionDict_next(it)) {
        ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
        if(elf_section_is_xip_eligible(&itref->value)) {
            size_t aligned_size = (itref->value.size + 7) & ~(size_t)7;
            xip_total += aligned_size;
        }
    }

    if(xip_total == 0) {
        FURI_LOG_D(TAG, "No XIP-eligible sections found");
        xip_region_release(&elf->xip_region);
        return;
    }

    /* Check if everything fits (account for header reservation) */
    if(xip_total > (XIP_REGION_MAX_SIZE - XIP_CACHE_HEADER_SIZE)) {
        FURI_LOG_W(
            TAG,
            "XIP sections too large (%zu bytes), falling back to RAM",
            xip_total);
        xip_region_release(&elf->xip_region);
        return;
    }

    /* Allocate flash addresses for each XIP-eligible section */
    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
        ELFSectionDict_next(it)) {
        ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
        ELFSection* sec = &itref->value;

        if(elf_section_is_xip_eligible(sec)) {
            uint32_t flash_addr = xip_region_alloc(&elf->xip_region, sec->size, 8);
            if(flash_addr == 0) {
                FURI_LOG_E(TAG, "XIP alloc failed for '%s', falling back to RAM", itref->key);
                xip_region_release(&elf->xip_region);
                ELFSectionDict_it_t it2;
                for(ELFSectionDict_it(it2, elf->sections); !ELFSectionDict_end_p(it2);
                    ELFSectionDict_next(it2)) {
                    ELFSectionDict_itref_t* itref2 = ELFSectionDict_ref(it2);
                    itref2->value.xip = false;
                }
                return;
            }
            sec->exec_addr = flash_addr;
            sec->xip = true;
            FURI_LOG_I(
                TAG,
                "Section '%s' (%lu bytes) -> XIP 0x%08lX",
                itref->key,
                sec->size,
                flash_addr);
        } else {
            sec->exec_addr = (Elf32_Addr)sec->data;
            sec->xip = false;
        }
    }

    FURI_LOG_I(TAG, "XIP setup: %zu bytes allocated in flash", xip_region_used(&elf->xip_region));
}

ElfLoadSectionTableResult elf_file_load_section_table(ELFFile* elf) {
    SectionType loaded_sections = 0;
    FuriString* name = furi_string_alloc();
    ElfLoadSectionTableResult result = ElfLoadSectionTableResultSuccess;

    FURI_LOG_D(TAG, "Scan ELF indexs...");

    for(size_t section_idx = 1; section_idx < elf->sections_count; section_idx++) {
        Elf32_Shdr section_header;

        furi_string_reset(name);
        if(!elf_read_section(elf, section_idx, &section_header, name)) {
            loaded_sections = 0;
            break;
        }

        FURI_LOG_D(
            TAG, "Preloading data for section #%d %s", section_idx, furi_string_get_cstr(name));
        SectionTypeInfo section_type_info =
            elf_preload_section(elf, section_idx, &section_header, name);
        loaded_sections |= section_type_info.type;

        if(section_type_info.result != ELFLoadSectionResultSuccess) {
            if(section_type_info.result == ELFLoadSectionResultNoMemory) {
                FURI_LOG_E(TAG, "Not enough memory");
                result = ElfLoadSectionTableResultNoMemory;
            } else if(section_type_info.result == ELFLoadSectionResultError) {
                FURI_LOG_E(TAG, "Error loading section");
                result = ElfLoadSectionTableResultError;
            }

            loaded_sections = 0;
            break;
        }
    }

    furi_string_free(name);

    if(result != ElfLoadSectionTableResultSuccess) {
        return result;
    } else {
        bool sections_valid =
            IS_FLAGS_SET(loaded_sections, SectionTypeSymTab | SectionTypeStrTab) |
            IS_FLAGS_SET(loaded_sections, SectionTypeFastRelData);
        if(sections_valid) {
            /* Attempt XIP setup — assigns flash addresses to read-only sections.
             * Section data has NOT been loaded yet (deferred), so XIP setup uses
             * only metadata (sh_flags, size) to determine eligibility. */
            elf_setup_xip(elf);

            /* Now materialize non-XIP sections into RAM.
             * XIP sections stay deferred — they'll be loaded one at a time
             * during elf_file_load_sections() to avoid peak RAM usage. */
            ELFSectionDict_it_t it;
            for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
                ELFSectionDict_next(it)) {
                ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
                ELFSection* sec = &itref->value;

                if(!sec->xip) {
                    /* RAM section: set exec_addr and load data now */
                    ELFLoadSectionResult res = elf_materialize_section(elf, sec);
                    if(res == ELFLoadSectionResultNoMemory) {
                        return ElfLoadSectionTableResultNoMemory;
                    } else if(res != ELFLoadSectionResultSuccess) {
                        return ElfLoadSectionTableResultError;
                    }
                    sec->exec_addr = (Elf32_Addr)sec->data;
                }
            }

            return ElfLoadSectionTableResultSuccess;
        } else {
            FURI_LOG_E(TAG, "No valid sections found");
            return ElfLoadSectionTableResultError;
        }
    }
}

ElfProcessSectionResult elf_process_section(
    ELFFile* elf,
    const char* name,
    ElfProcessSection* process_section,
    void* context) {
    ElfProcessSectionResult result = ElfProcessSectionResultNotFound;
    FuriString* section_name = furi_string_alloc();
    Elf32_Shdr section_header;

    // find section
    for(size_t section_idx = 1; section_idx < elf->sections_count; section_idx++) {
        furi_string_reset(section_name);
        if(!elf_read_section(elf, section_idx, &section_header, section_name)) {
            break;
        }

        if(furi_string_cmp(section_name, name) == 0) {
            result = ElfProcessSectionResultCannotProcess;
            break;
        }
    }

    if(result != ElfProcessSectionResultNotFound) { //-V547
        if(process_section(elf->fd, section_header.sh_offset, section_header.sh_size, context)) {
            result = ElfProcessSectionResultSuccess;
        } else {
            result = ElfProcessSectionResultCannotProcess; //-V1048
        }
    }

    furi_string_free(section_name);

    return result;
}

/** Pre-parsed fast relocation record for per-page streaming */
typedef struct {
    uint8_t type;
    Elf32_Addr address; /**< Resolved target address */
    uint32_t offsets_count;
    const uint8_t* offsets_data; /**< Pointer into fast_rel blob (3 bytes per offset) */
} FastRelRecord;

/** Stream an XIP section to flash page-by-page, applying relocations per-page.
 *  Only needs ~PAGE_SIZE + relocation entries in RAM at once, not the full section.
 */
static bool elf_xip_stream_section(ELFFile* elf, ELFSection* sec) {
    const size_t PAGE_SIZE = 4096;
    const size_t OVERLAP = 8; /* Extra bytes on each side for boundary-crossing relocations */

    uint32_t stream_start = furi_get_tick();

    /* ---- Try fast relocation path first (no standard rels needed) ---- */
    FastRelRecord* fast_records = NULL;
    uint32_t fast_records_count = 0;
    bool has_fast_rel = false;

    if(sec->fast_rel && sec->fast_rel->data && sec->fast_rel->size > 0) {
        const uint8_t* fr_ptr = sec->fast_rel->data;
        const uint8_t fr_version = *fr_ptr;
        fr_ptr += 1;

        if(fr_version == FAST_RELOCATION_VERSION) {
            fast_records_count = *((uint32_t*)fr_ptr);
            fr_ptr += 4;

            fast_records = malloc(fast_records_count * sizeof(FastRelRecord));
            if(fast_records) {
                bool fr_ok = true;
                for(uint32_t i = 0; i < fast_records_count; i++) {
                    bool is_section = (*fr_ptr & (0x1 << 7)) ? true : false;
                    fast_records[i].type = *fr_ptr & 0x7F;
                    fr_ptr += 1;
                    uint32_t hash_or_section_index = *((uint32_t*)fr_ptr);
                    fr_ptr += 4;

                    uint32_t section_value = ELF_INVALID_ADDRESS;
                    if(is_section) {
                        section_value = *((uint32_t*)fr_ptr);
                        fr_ptr += 4;
                    }

                    fast_records[i].offsets_count = *((uint32_t*)fr_ptr);
                    fr_ptr += 4;
                    fast_records[i].offsets_data = fr_ptr;

                    if(is_section) {
                        ELFSection* symSec = elf_section_of(elf, hash_or_section_index);
                        fast_records[i].address =
                            symSec ? (symSec->exec_addr) + section_value : ELF_INVALID_ADDRESS;
                    } else {
                        fast_records[i].address =
                            elf_address_of_by_hash(elf, hash_or_section_index);
                    }

                    if(fast_records[i].address == ELF_INVALID_ADDRESS) {
                        FURI_LOG_E(TAG, "XIP stream: unresolved fast rel record %lu (hash %lX)", i, hash_or_section_index);
                        fr_ok = false;
                    }

                    fr_ptr += 3 * fast_records[i].offsets_count;
                }

                if(fr_ok) {
                    has_fast_rel = true;
                    FURI_LOG_I(TAG, "XIP stream: using fast relocation path (%lu records, %lums)", fast_records_count, furi_get_tick() - stream_start);
                } else {
                    free(fast_records);
                    fast_records = NULL;
                    fast_records_count = 0;
                }
            }
        }
    }

    /* ---- Standard relocation path (only if fast_rel unavailable) ---- */
    Elf32_Rel* rels = NULL;
    size_t rel_count = has_fast_rel ? 0 : sec->rel_count;

    if(!has_fast_rel && rel_count > 0) {
        FURI_LOG_I(TAG, "XIP stream: phase 1 — reading %zu standard relocations (heap free=%zu)", rel_count, memmgr_get_free_heap());
        size_t rel_bytes_needed = rel_count * sizeof(Elf32_Rel);
        rels = malloc(rel_bytes_needed);
        if(!rels) {
            FURI_LOG_E(TAG, "XIP stream: can't alloc rel table (%zu bytes, free=%zu)", rel_bytes_needed, memmgr_get_free_heap());
            return false;
        }
        if(!storage_file_seek(elf->fd, sec->rel_offset, true)) {
            FURI_LOG_E(TAG, "XIP stream: can't seek rel table");
            free(rels);
            return false;
        }
        size_t rel_read = 0;
        while(rel_read < rel_bytes_needed) {
            size_t chunk = rel_bytes_needed - rel_read;
            if(chunk > 0xFFFF) chunk = 0xFFFF;
            uint16_t got = storage_file_read(elf->fd, (uint8_t*)rels + rel_read, chunk);
            if(got == 0) {
                FURI_LOG_E(TAG, "XIP stream: rel table read failed at %zu/%zu", rel_read, rel_bytes_needed);
                free(rels);
                return false;
            }
            rel_read += got;
        }
    }

    /* Symbol resolution — only needed for the standard relocation path.
     * Fast relocation records resolve via hash lookup (no file I/O). */
    bool resolve_ok = true;
    if(has_fast_rel) {
        FURI_LOG_I(TAG, "XIP stream: skipping symbol resolution (fast_rel handles it)");
        goto skip_symbol_resolution;
    }

    FURI_LOG_I(TAG, "XIP stream: phase 2 — resolving symbols (%lums so far)", furi_get_tick() - stream_start);

    /* Collect unique, uncached symbol indices */
    size_t unique_cap = 256;
    size_t unique_count = 0;
    int* unique_syms = malloc(unique_cap * sizeof(int));
    if(!unique_syms) {
        FURI_LOG_E(TAG, "XIP stream: can't alloc unique sym list");
        if(rels) free(rels);
        return false;
    }

    for(size_t i = 0; i < rel_count; i++) {
        int symEntry = ELF32_R_SYM(rels[i].r_info);
        Elf32_Addr symAddr;
        if(address_cache_get(elf->relocation_cache, symEntry, &symAddr)) continue;

        /* Check if already in our unique list */
        bool found = false;
        for(size_t j = 0; j < unique_count; j++) {
            if(unique_syms[j] == symEntry) { found = true; break; }
        }
        if(found) continue;

        /* Grow if needed */
        if(unique_count >= unique_cap) {
            unique_cap *= 2;
            int* tmp = realloc(unique_syms, unique_cap * sizeof(int));
            if(!tmp) {
                FURI_LOG_E(TAG, "XIP stream: can't grow unique sym list");
                free(unique_syms);
                if(rels) free(rels);
                return false;
            }
            unique_syms = tmp;
        }
        unique_syms[unique_count++] = symEntry;
    }

    FURI_LOG_I(TAG, "XIP stream: %zu unique symbols to resolve from %zu relocations", unique_count, rel_count);

    /* Free rels temporarily — symbol resolution doesn't need them and they
     * consume 54 KB of the ~65 KB available heap.  Re-read after resolution. */
    size_t rel_bytes = rel_count * sizeof(Elf32_Rel);
    if(rels) { free(rels); rels = NULL; }

    /* Sort unique indices so we can scan the symbol table sequentially */
    for(size_t i = 1; i < unique_count; i++) {
        int key = unique_syms[i];
        size_t j = i;
        while(j > 0 && unique_syms[j - 1] > key) {
            unique_syms[j] = unique_syms[j - 1];
            j--;
        }
        unique_syms[j] = key;
    }

    /* Bulk-resolve by reading the symbol table in chunks.
     * Heap-allocated — loader stack is only 2 KB. */
    #define SYM_CHUNK_COUNT 64 /* 64 × 16 = 1024 bytes per chunk */
    Elf32_Sym* sym_buf = malloc(SYM_CHUNK_COUNT * sizeof(Elf32_Sym));
    if(!sym_buf) {
        FURI_LOG_E(TAG, "XIP stream: can't alloc symbol chunk buffer");
        free(unique_syms);
        return false;
    }
    FuriString* sym_name = furi_string_alloc();
    resolve_ok = true;
    size_t resolved = 0;
    size_t ui = 0; /* cursor into sorted unique_syms */

    FURI_LOG_I(TAG, "XIP stream: bulk-reading symbol table (count=%zu, heap free=%zu)", elf->symbol_count, memmgr_get_free_heap());

    for(size_t chunk_base = 0; chunk_base < elf->symbol_count && ui < unique_count; chunk_base += SYM_CHUNK_COUNT) {
        size_t chunk_end = chunk_base + SYM_CHUNK_COUNT;
        if(chunk_end > elf->symbol_count) chunk_end = elf->symbol_count;
        size_t chunk_len = chunk_end - chunk_base;

        /* Skip chunk if no unique indices fall in this range */
        if((size_t)unique_syms[ui] >= chunk_end) continue;

        /* Read this chunk from the symbol table */
        off_t chunk_off = elf->symbol_table + chunk_base * sizeof(Elf32_Sym);
        if(!storage_file_seek(elf->fd, chunk_off, true) ||
           storage_file_read(elf->fd, sym_buf, chunk_len * sizeof(Elf32_Sym)) !=
               (uint16_t)(chunk_len * sizeof(Elf32_Sym))) {
            FURI_LOG_E(TAG, "XIP stream: symbol table chunk read failed at %zu", chunk_base);
            resolve_ok = false;
            break;
        }

        /* Process all unique symbols that fall in this chunk */
        while(ui < unique_count && (size_t)unique_syms[ui] < chunk_end) {
            int symEntry = unique_syms[ui];
            Elf32_Sym* sym = &sym_buf[symEntry - chunk_base];
            Elf32_Addr symAddr;

            if(sym->st_shndx == SHN_UNDEF) {
                /* External API symbol — need name for hash lookup */
                furi_string_reset(sym_name);
                off_t name_off = elf->symbol_table_strings + sym->st_name;
                if(storage_file_seek(elf->fd, name_off, true)) {
                    char nbuf[ELF_NAME_BUFFER_LEN + 1];
                    nbuf[ELF_NAME_BUFFER_LEN] = 0;
                    while(true) {
                        uint16_t nr = storage_file_read(elf->fd, nbuf, ELF_NAME_BUFFER_LEN);
                        furi_string_cat(sym_name, nbuf);
                        if(strlen(nbuf) < ELF_NAME_BUFFER_LEN) break;
                        if(nr == 0) break;
                    }
                }
                uint32_t hash = elf_symbolname_hash(furi_string_get_cstr(sym_name));
                if(!elf->api_interface->resolver_callback(elf->api_interface, hash, &symAddr)) {
                    symAddr = ELF_INVALID_ADDRESS;
                    FURI_LOG_E(TAG, "XIP stream: unresolved symbol %s", furi_string_get_cstr(sym_name));
                    resolve_ok = false;
                }
            } else {
                /* Section-relative symbol — resolve from section table, no name needed */
                ELFSection* symSec = elf_section_of(elf, sym->st_shndx);
                symAddr = symSec ? (symSec->exec_addr) + sym->st_value : ELF_INVALID_ADDRESS;
                if(symAddr == ELF_INVALID_ADDRESS) {
                    FURI_LOG_E(TAG, "XIP stream: unresolved section symbol (shndx=%d)", sym->st_shndx);
                    resolve_ok = false;
                }
            }

            address_cache_put(elf->relocation_cache, symEntry, symAddr);
            resolved++;
            ui++;
        }

        /* Yield + progress after each chunk */
        furi_delay_tick(1);
        FURI_LOG_I(TAG, "XIP stream: resolved %zu/%zu symbols (%lums)", resolved, unique_count, furi_get_tick() - stream_start);
    }

    furi_string_free(sym_name);
    free(sym_buf);
    free(unique_syms);

    FURI_LOG_I(TAG, "XIP stream: phase 2 done — %zu symbols (%lums so far)", resolved, furi_get_tick() - stream_start);

    /* Re-read relocation table — freed earlier to make room for symbol resolution */
    if(resolve_ok && rel_count > 0) {
        rels = malloc(rel_bytes);
        if(!rels) {
            FURI_LOG_E(TAG, "XIP stream: can't re-alloc rel table");
            return false;
        }
        if(!storage_file_seek(elf->fd, sec->rel_offset, true)) {
            FURI_LOG_E(TAG, "XIP stream: can't seek rel table for re-read");
            free(rels);
            return false;
        }
        size_t rel_read = 0;
        while(rel_read < rel_bytes) {
            size_t chunk = rel_bytes - rel_read;
            if(chunk > 0xFFFF) chunk = 0xFFFF;
            uint16_t got = storage_file_read(elf->fd, (uint8_t*)rels + rel_read, chunk);
            if(got == 0) {
                FURI_LOG_E(TAG, "XIP stream: rel table re-read failed");
                free(rels);
                return false;
            }
            rel_read += got;
        }
        FURI_LOG_I(TAG, "XIP stream: re-read %zu relocations (%lums so far)", rel_count, furi_get_tick() - stream_start);
    }

    if(!has_fast_rel && !resolve_ok) {
        if(rels) free(rels);
        return false;
    }

skip_symbol_resolution:
    /* At this point we have either fast_records OR rels (or both) for the page loop */

    /* Allocate page buffer with overlap on each side for boundary-crossing relocations.
     * Layout: [OVERLAP prefix][PAGE_SIZE data][OVERLAP suffix]
     * The prefix/suffix hold raw bytes from adjacent pages so that relocations
     * spanning page boundaries can read/write the full instruction. */
    uint8_t* buf = aligned_malloc(PAGE_SIZE + 2 * OVERLAP, 8);
    if(!buf) {
        FURI_LOG_E(TAG, "XIP stream: can't alloc page buffer");
        if(rels) free(rels);
        if(fast_records) free(fast_records);
        return false;
    }

    bool success = true;
    size_t total_pages = (sec->size + PAGE_SIZE - 1) / PAGE_SIZE;
    FURI_LOG_I(TAG, "XIP stream: phase 4 — writing %zu pages to flash (%lums so far)", total_pages, furi_get_tick() - stream_start);

    for(size_t page_start = 0; page_start < sec->size; page_start += PAGE_SIZE) {
        size_t page_end = page_start + PAGE_SIZE;
        if(page_end > sec->size) page_end = sec->size;
        size_t page_len = page_end - page_start;

        /* Clear entire buffer including overlap areas */
        FURI_LOG_D(TAG, "XIP stream: reading page %zu/%zu from SD", page_start / PAGE_SIZE + 1, total_pages);
        memset(buf, 0, PAGE_SIZE + 2 * OVERLAP);

        /* Read the page data into the middle of the buffer (after prefix area).
         * Also read prefix bytes (before this page) and suffix bytes (after). */

        /* Prefix: up to OVERLAP bytes from before this page */
        size_t prefix_avail = (page_start < OVERLAP) ? page_start : OVERLAP;
        if(prefix_avail > 0) {
            if(!storage_file_seek(elf->fd, sec->file_offset + page_start - prefix_avail, true) ||
               storage_file_read(elf->fd, buf + (OVERLAP - prefix_avail), prefix_avail) !=
                   (uint16_t)prefix_avail) {
                FURI_LOG_E(TAG, "XIP stream: prefix read failed at %zu", page_start);
                success = false;
                break;
            }
        }

        /* Page data */
        if(!storage_file_seek(elf->fd, sec->file_offset + page_start, true) ||
           storage_file_read(elf->fd, buf + OVERLAP, page_len) != (uint16_t)page_len) {
            FURI_LOG_E(TAG, "XIP stream: page read failed at %zu", page_start);
            success = false;
            break;
        }

        /* Suffix: up to OVERLAP bytes from after this page */
        if(page_end < sec->size) {
            size_t suffix_avail = sec->size - page_end;
            if(suffix_avail > OVERLAP) suffix_avail = OVERLAP;
            if(!storage_file_seek(elf->fd, sec->file_offset + page_end, true) ||
               storage_file_read(elf->fd, buf + OVERLAP + page_len, suffix_avail) !=
                   (uint16_t)suffix_avail) {
                FURI_LOG_E(TAG, "XIP stream: suffix read failed at %zu", page_end);
                success = false;
                break;
            }
        }

        /* Apply relocations for this page.
         * Fast relocations take priority (same as elf_relocate_section):
         * if fast_rel exists, it REPLACES standard rels, not supplements them. */
        if(fast_records_count > 0) {
            /* Fast relocation path — replaces standard rels (same as elf_relocate_section) */
            for(uint32_t fri = 0; fri < fast_records_count; fri++) {
                if(fast_records[fri].address == ELF_INVALID_ADDRESS) continue;
                const uint8_t* off_ptr = fast_records[fri].offsets_data;
                for(uint32_t j = 0; j < fast_records[fri].offsets_count; j++) {
                    uint32_t r_off = *((uint32_t*)off_ptr) & 0x00FFFFFF;
                    off_ptr += 3;
                    if((r_off + 4) <= (uint32_t)page_start || r_off >= (uint32_t)page_end)
                        continue;
                    int32_t buf_offset = (int32_t)r_off - (int32_t)page_start;
                    Elf32_Addr patchAddr = (Elf32_Addr)(buf + OVERLAP + buf_offset);
                    Elf32_Addr relAddr = sec->exec_addr + r_off;
                    elf_relocate_symbol(
                        elf,
                        patchAddr,
                        relAddr,
                        fast_records[fri].type,
                        fast_records[fri].address);
                }
            }
        } else {
            /* Standard relocation path */
            for(size_t i = 0; i < rel_count; i++) {
                uint32_t r_off = rels[i].r_offset;
                if((r_off + 4) <= (uint32_t)page_start || r_off >= (uint32_t)page_end) continue;
                int symEntry = ELF32_R_SYM(rels[i].r_info);
                int relType = ELF32_R_TYPE(rels[i].r_info);
                Elf32_Addr symAddr;
                address_cache_get(elf->relocation_cache, symEntry, &symAddr);
                if(symAddr != ELF_INVALID_ADDRESS) {
                    int32_t buf_offset = (int32_t)r_off - (int32_t)page_start;
                    Elf32_Addr patchAddr = (Elf32_Addr)(buf + OVERLAP + buf_offset);
                    Elf32_Addr relAddr = sec->exec_addr + r_off;
                    elf_relocate_symbol(elf, patchAddr, relAddr, relType, symAddr);
                }
            }
        }

        /* Write only the page data (skip prefix overlap) to flash */
        FURI_LOG_D(TAG, "XIP stream: writing page %zu/%zu", page_start / PAGE_SIZE + 1, total_pages);
        if(!xip_region_commit(
               &elf->xip_region,
               sec->exec_addr + page_start,
               buf + OVERLAP,
               page_len)) {
            FURI_LOG_E(TAG, "XIP stream: commit failed at offset %zu", page_start);
            success = false;
            break;
        }
        FURI_LOG_D(TAG, "XIP stream: page %zu/%zu committed", page_start / PAGE_SIZE + 1, total_pages);

        /* Yield after every page to let BLE stack and watchdog breathe. */
        furi_delay_tick(1);
    }

    aligned_free(buf);
    if(rels) free(rels);
    if(fast_records) free(fast_records);

    /* Free fast_rel data — already applied per-page during streaming */
    if(sec->fast_rel) {
        if(sec->fast_rel->data) aligned_free(sec->fast_rel->data);
        free(sec->fast_rel);
        sec->fast_rel = NULL;
    }

    if(success) {
        sec->data = (void*)sec->exec_addr;
        FURI_LOG_I(
            TAG,
            "XIP streamed %lu bytes to flash at 0x%08lX (heap: free=%zu max_block=%zu)",
            sec->size,
            sec->exec_addr,
            memmgr_get_free_heap(),
            memmgr_heap_get_max_free_block());
    } else {
        FURI_LOG_E(
            TAG,
            "XIP stream FAILED (heap: free=%zu max_block=%zu)",
            memmgr_get_free_heap(),
            memmgr_heap_get_max_free_block());
    }

    return success;
}

ELFFileLoadStatus elf_file_load_sections(ELFFile* elf) {
    furi_check(elf->fd != NULL);
    ELFFileLoadStatus status = ELFFileLoadStatusSuccess;
    ELFSectionDict_it_t it;

    AddressCache_init(elf->relocation_cache);

    FURI_LOG_I(
        TAG,
        "Heap before load_sections: free=%zu max_block=%zu",
        memmgr_get_free_heap(),
        memmgr_heap_get_max_free_block());

    /* Phase 1a: Relocate non-XIP (RAM) sections — their data is already loaded */
    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it); ELFSectionDict_next(it)) {
        ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
        if(itref->value.xip) continue; /* XIP sections handled in phase 1b */

        FURI_LOG_D(TAG, "Relocating RAM section '%s'", itref->key);
        if(!elf_relocate_section(elf, &itref->value)) {
            FURI_LOG_E(TAG, "Error relocating section '%s'", itref->key);
            status = ELFFileLoadStatusMissingImports;
        }
    }

    /* Phase 1b + 2: XIP section processing.
     * If cache is valid, skip entirely (data already in flash from previous launch).
     * Otherwise: erase flash, stream/stage sections, write cache header. */
    if(status == ELFFileLoadStatusSuccess && elf->xip_region.active &&
       elf->xip_region.cache_valid) {
        /* Cache hit — XIP sections already in flash, nothing to do */
        FURI_LOG_I(TAG, "XIP cache hit: skipping erase/write, sections already in flash");
    } else if(status == ELFFileLoadStatusSuccess && elf->xip_region.active) {
        /* Batch Core2 locking: one SHCI notification for the entire erase+write
         * sequence instead of per-page cycling. Prevents BLE semaphore timeout
         * crashes and dramatically speeds up the flash operations. */
        furi_hal_flash_batch_begin();

        /* Erase all XIP flash pages upfront */
        if(!xip_region_erase(&elf->xip_region)) {
            FURI_LOG_E(TAG, "XIP flash erase failed");
            furi_hal_flash_batch_end();
            status = ELFFileLoadStatusUnspecifiedError;
        }

        if(status == ELFFileLoadStatusSuccess) {
            size_t xip_committed = 0;
            for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
                ELFSectionDict_next(it)) {
                ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
                ELFSection* sec = &itref->value;
                if(!sec->xip || sec->size == 0) continue;

                /* Try to stage the full section in RAM first (fastest).
                 * If not enough RAM, fall back to page-by-page streaming. */
                ELFLoadSectionResult mat_res = elf_materialize_section(elf, sec);

                if(mat_res == ELFLoadSectionResultSuccess) {
                    /* Full staging: relocate, commit, free */
                    FURI_LOG_D(TAG, "Relocating XIP section '%s' (staged)", itref->key);
                    if(!elf_relocate_section(elf, sec)) {
                        FURI_LOG_E(TAG, "Error relocating section '%s'", itref->key);
                        aligned_free(sec->data);
                        sec->data = NULL;
                        status = ELFFileLoadStatusMissingImports;
                        break;
                    }
                    if(!xip_region_commit(
                           &elf->xip_region, sec->exec_addr, sec->data, sec->size)) {
                        FURI_LOG_E(TAG, "XIP commit failed for section '%s'", itref->key);
                        aligned_free(sec->data);
                        sec->data = NULL;
                        status = ELFFileLoadStatusUnspecifiedError;
                        break;
                    }
                    aligned_free(sec->data);
                    sec->data = (void*)sec->exec_addr;
                } else if(mat_res == ELFLoadSectionResultNoMemory) {
                    /* Section too large for RAM — stream page-by-page */
                    FURI_LOG_I(
                        TAG,
                        "Section '%s' (%lu bytes) too large for RAM, streaming to flash",
                        itref->key,
                        sec->size);
                    if(!elf_xip_stream_section(elf, sec)) {
                        FURI_LOG_E(TAG, "XIP stream failed for section '%s'", itref->key);
                        status = ELFFileLoadStatusUnspecifiedError;
                        break;
                    }
                } else {
                    FURI_LOG_E(TAG, "Failed to load XIP section '%s'", itref->key);
                    status = ELFFileLoadStatusUnspecifiedError;
                    break;
                }

                xip_committed += sec->size;

                FURI_LOG_D(
                    TAG,
                    "Section '%s' committed to flash at 0x%08lX",
                    itref->key,
                    sec->exec_addr);
            }

            /* Flush instruction and data caches after writing code to flash */
            furi_hal_flash_flush_cache();

            FURI_LOG_I(TAG, "XIP: %zu bytes committed to flash", xip_committed);
            FURI_LOG_I(
                TAG,
                "Heap after XIP commit: free=%zu max_block=%zu",
                memmgr_get_free_heap(),
                memmgr_heap_get_max_free_block());

            /* Write cache header so next launch of the same app can skip erase/write */
            if(status == ELFFileLoadStatusSuccess) {
                XipCacheHeader cache_hdr;
                memset(&cache_hdr, 0, sizeof(cache_hdr));
                cache_hdr.magic = XIP_CACHE_MAGIC;
                cache_hdr.file_size = (uint32_t)storage_file_size(elf->fd);
                cache_hdr.file_crc32 = crc32_calc_file(elf->fd, NULL, NULL);
                cache_hdr.api_version =
                    ((uint32_t)elf->api_interface->api_version_major << 16) |
                    elf->api_interface->api_version_minor;

                uint32_t sec_idx = 0;
                for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
                    ELFSectionDict_next(it)) {
                    ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
                    if(itref->value.xip && itref->value.size > 0 &&
                       sec_idx < XIP_CACHE_MAX_SECTIONS) {
                        cache_hdr.sections[sec_idx].flash_offset =
                            itref->value.exec_addr - elf->xip_region.base_addr;
                        cache_hdr.sections[sec_idx].size = itref->value.size;
                        strncpy(
                            cache_hdr.sections[sec_idx].name, itref->key, 15);
                        cache_hdr.sections[sec_idx].name[15] = '\0';
                        sec_idx++;
                    }
                }
                cache_hdr.section_count = sec_idx;

                if(!xip_cache_commit_header(&elf->xip_region, &cache_hdr)) {
                    FURI_LOG_E(TAG, "XIP cache header write failed — app will re-flash on next launch");
                }
            }
        }

        /* End batch — resume BLE operations */
        furi_hal_flash_batch_end();
    }

    /* Phase 3: Fix up entry point */
    if(status == ELFFileLoadStatusSuccess) {
        ELFSection* text_section = elf_file_get_section(elf, ".text");

        if(text_section == NULL) {
            FURI_LOG_E(TAG, "No .text section found");
            status = ELFFileLoadStatusUnspecifiedError;
        } else {
            elf->entry += (uint32_t)text_section->exec_addr;
        }
    }

    FURI_LOG_D(TAG, "Relocation cache size: %u", AddressCache_size(elf->relocation_cache));
    FURI_LOG_D(TAG, "Trampoline cache size: %u", AddressCache_size(elf->trampoline_cache));
    AddressCache_clear(elf->relocation_cache);

    {
        size_t total_size = 0;
        size_t ram_size = 0;
        for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it);
            ELFSectionDict_next(it)) {
            ELFSectionDict_itref_t* itref = ELFSectionDict_ref(it);
            total_size += itref->value.size;
            if(!itref->value.xip) {
                ram_size += itref->value.size;
            }
        }
        FURI_LOG_I(TAG, "Total size of loaded sections: %zu", total_size);
        if(elf->xip_region.active) {
            FURI_LOG_I(TAG, "RAM usage: %zu bytes (XIP saved %zu bytes)", ram_size, total_size - ram_size);
        }
    }

    elf_file_maybe_release_fd(elf);
    return status;
}

void elf_file_call_init(ELFFile* elf) {
    furi_check(!elf->init_array_called);
    elf_file_call_section_list(elf->preinit_array, false);
    elf_file_call_section_list(elf->init_array, false);
    elf->init_array_called = true;
}

bool elf_file_is_init_complete(ELFFile* elf) {
    return elf->init_array_called;
}

void* elf_file_get_entry_point(ELFFile* elf) {
    furi_check(elf->init_array_called);
    return (void*)elf->entry;
}

void elf_file_call_fini(ELFFile* elf) {
    furi_check(elf->init_array_called);
    elf_file_call_section_list(elf->fini_array, true);
    elf->init_array_called = false;
}

const ElfApiInterface* elf_file_get_api_interface(ELFFile* elf_file) {
    return elf_file->api_interface;
}

void elf_file_init_debug_info(ELFFile* elf, ELFDebugInfo* debug_info) {
    // set entry
    debug_info->entry = elf->entry;

    // copy debug info
    memcpy(&debug_info->debug_link_info, &elf->debug_link_info, sizeof(ELFDebugLinkInfo));

    // init mmap
    debug_info->mmap_entry_count = ELFSectionDict_size(elf->sections);
    debug_info->mmap_entries = malloc(sizeof(ELFMemoryMapEntry) * debug_info->mmap_entry_count);
    uint32_t mmap_entry_idx = 0;

    ELFSectionDict_it_t it;
    for(ELFSectionDict_it(it, elf->sections); !ELFSectionDict_end_p(it); ELFSectionDict_next(it)) {
        const ELFSectionDict_itref_t* itref = ELFSectionDict_cref(it);

        const void* data_ptr = itref->value.data;
        if(data_ptr) {
            ELFMemoryMapEntry* entry = &debug_info->mmap_entries[mmap_entry_idx];
            entry->address = (uint32_t)data_ptr;
            entry->name = itref->key;
            mmap_entry_idx++;
        }
    }
}

void elf_file_clear_debug_info(ELFDebugInfo* debug_info) {
    // clear debug info
    memset(&debug_info->debug_link_info, 0, sizeof(ELFDebugLinkInfo));

    // clear mmap
    if(debug_info->mmap_entries) {
        free(debug_info->mmap_entries);
        debug_info->mmap_entries = NULL;
    }

    debug_info->mmap_entry_count = 0;
}
