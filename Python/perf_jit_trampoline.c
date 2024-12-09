#include "Python.h"
#include "pycore_ceval.h"         // _PyPerf_Callbacks
#include "pycore_frame.h"
#include "pycore_interp.h"


#ifdef PY_HAVE_PERF_TRAMPOLINE

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>             // mmap()
#include <sys/types.h>
#include <unistd.h>               // sysconf()
#include <sys/time.h>           // gettimeofday()
#include <sys/syscall.h>

// ----------------------------------
//         Perf jitdump API
// ----------------------------------

typedef struct {
    FILE* perf_map;
    PyThread_type_lock map_lock;
    void* mapped_buffer;
    size_t mapped_size;
    int code_id;
} PerfMapJitState;

static PerfMapJitState perf_jit_map_state;

/*
Usually the binary and libraries are mapped in separate region like below:

  address ->
   --+---------------------+--//--+---------------------+--
     | .text | .data | ... |      | .text | .data | ... |
   --+---------------------+--//--+---------------------+--
         myprog                      libc.so

So it'd be easy and straight-forward to find a mapped binary or library from an
address.

But for JIT code, the code arena only cares about the code section. But the
resulting DSOs (which is generated by perf inject -j) contain ELF headers and
unwind info too. Then it'd generate following address space with synthesized
MMAP events. Let's say it has a sample between address B and C.

                                               sample
                                                 |
  address ->                         A       B   v   C
  ---------------------------------------------------------------------------------------------------
  /tmp/jitted-PID-0.so   | (headers) | .text | unwind info |
  /tmp/jitted-PID-1.so           | (headers) | .text | unwind info |
  /tmp/jitted-PID-2.so                   | (headers) | .text | unwind info |
    ...
  ---------------------------------------------------------------------------------------------------

If it only maps the .text section, it'd find the jitted-PID-1.so but cannot see
the unwind info. If it maps both .text section and unwind sections, the sample
could be mapped to either jitted-PID-0.so or jitted-PID-1.so and it's confusing
which one is right. So to make perf happy we have non-overlapping ranges for each
DSO:

  address ->
  -------------------------------------------------------------------------------------------------------
  /tmp/jitted-PID-0.so   | (headers) | .text | unwind info |
  /tmp/jitted-PID-1.so                         | (headers) | .text | unwind info |
  /tmp/jitted-PID-2.so                                               | (headers) | .text | unwind info |
    ...
  -------------------------------------------------------------------------------------------------------

As the trampolines are constant, we add a constant padding but in general the padding needs to have the
size of the unwind info rounded to 16 bytes. In general, for our trampolines this is 0x50
 */

#define PERF_JIT_CODE_PADDING 0x100
#define trampoline_api _PyRuntime.ceval.perf.trampoline_api

typedef uint64_t uword;
typedef const char* CodeComments;

#define Pd "d"
#define MB (1024 * 1024)

#define EM_386      3
#define EM_X86_64   62
#define EM_ARM      40
#define EM_AARCH64  183
#define EM_RISCV    243

#define TARGET_ARCH_IA32   0
#define TARGET_ARCH_X64    0
#define TARGET_ARCH_ARM    0
#define TARGET_ARCH_ARM64  0
#define TARGET_ARCH_RISCV32 0
#define TARGET_ARCH_RISCV64 0

#define FLAG_generate_perf_jitdump 0
#define FLAG_write_protect_code 0
#define FLAG_write_protect_vm_isolate 0
#define FLAG_code_comments 0

#define UNREACHABLE()

static uword GetElfMachineArchitecture(void) {
#if TARGET_ARCH_IA32
    return EM_386;
#elif TARGET_ARCH_X64
    return EM_X86_64;
#elif TARGET_ARCH_ARM
    return EM_ARM;
#elif TARGET_ARCH_ARM64
    return EM_AARCH64;
#elif TARGET_ARCH_RISCV32 || TARGET_ARCH_RISCV64
    return EM_RISCV;
#else
    UNREACHABLE();
    return 0;
#endif
}

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t elf_mach_target;
    uint32_t reserved;
    uint32_t process_id;
    uint64_t time_stamp;
    uint64_t flags;
} Header;

 enum PerfEvent {
    PerfLoad = 0,
    PerfMove = 1,
    PerfDebugInfo = 2,
    PerfClose = 3,
    PerfUnwindingInfo = 4
};

struct BaseEvent {
    uint32_t event;
    uint32_t size;
    uint64_t time_stamp;
  };

typedef struct {
    struct BaseEvent base;
    uint32_t process_id;
    uint32_t thread_id;
    uint64_t vma;
    uint64_t code_address;
    uint64_t code_size;
    uint64_t code_id;
} CodeLoadEvent;

typedef struct {
    struct BaseEvent base;
    uint64_t unwind_data_size;
    uint64_t eh_frame_hdr_size;
    uint64_t mapped_size;
} CodeUnwindingInfoEvent;

static const intptr_t nanoseconds_per_second = 1000000000;

// Dwarf encoding constants

static const uint8_t DwarfUData4 = 0x03;
static const uint8_t DwarfSData4 = 0x0b;
static const uint8_t DwarfPcRel = 0x10;
static const uint8_t DwarfDataRel = 0x30;
// static uint8_t DwarfOmit = 0xff;
typedef struct {
    unsigned char version;
    unsigned char eh_frame_ptr_enc;
    unsigned char fde_count_enc;
    unsigned char table_enc;
    int32_t eh_frame_ptr;
    int32_t eh_fde_count;
    int32_t from;
    int32_t to;
} EhFrameHeader;

static int64_t get_current_monotonic_ticks(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        UNREACHABLE();
        return 0;
    }
    // Convert to nanoseconds.
    int64_t result = ts.tv_sec;
    result *= nanoseconds_per_second;
    result += ts.tv_nsec;
    return result;
}

static int64_t get_current_time_microseconds(void) {
  // gettimeofday has microsecond resolution.
  struct timeval tv;
  if (gettimeofday(&tv, NULL) < 0) {
    UNREACHABLE();
    return 0;
  }
  return ((int64_t)(tv.tv_sec) * 1000000) + tv.tv_usec;
}


static size_t round_up(int64_t value, int64_t multiple) {
    if (multiple == 0) {
        // Avoid division by zero
        return value;
    }

    int64_t remainder = value % multiple;
    if (remainder == 0) {
        // Value is already a multiple of 'multiple'
        return value;
    }

    // Calculate the difference to the next multiple
    int64_t difference = multiple - remainder;

    // Add the difference to the value
    int64_t rounded_up_value = value + difference;

    return rounded_up_value;
}


static void perf_map_jit_write_fully(const void* buffer, size_t size) {
    FILE* out_file = perf_jit_map_state.perf_map;
    const char* ptr = (const char*)(buffer);
    while (size > 0) {
        const size_t written = fwrite(ptr, 1, size, out_file);
        if (written == 0) {
            UNREACHABLE();
            break;
        }
        size -= written;
        ptr += written;
    }
}

static void perf_map_jit_write_header(int pid, FILE* out_file) {
    Header header;
    header.magic = 0x4A695444;
    header.version = 1;
    header.size = sizeof(Header);
    header.elf_mach_target = GetElfMachineArchitecture();
    header.process_id = pid;
    header.time_stamp = get_current_time_microseconds();
    header.flags = 0;
    perf_map_jit_write_fully(&header, sizeof(header));
}

static void* perf_map_jit_init(void) {
    char filename[100];
    int pid = getpid();
    snprintf(filename, sizeof(filename) - 1, "/tmp/jit-%d.dump", pid);
    const int fd = open(filename, O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (fd == -1) {
        return NULL;
    }

    const long page_size = sysconf(_SC_PAGESIZE);  // NOLINT(runtime/int)
    if (page_size == -1) {
        close(fd);
        return NULL;
    }

    // The perf jit interface forces us to map the first page of the file
    // to signal that we are using the interface.
    perf_jit_map_state.mapped_buffer = mmap(NULL, page_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    if (perf_jit_map_state.mapped_buffer == NULL) {
        close(fd);
        return NULL;
    }
    perf_jit_map_state.mapped_size = page_size;
    perf_jit_map_state.perf_map = fdopen(fd, "w+");
    if (perf_jit_map_state.perf_map == NULL) {
        close(fd);
        return NULL;
    }
    setvbuf(perf_jit_map_state.perf_map, NULL, _IOFBF, 2 * MB);
    perf_map_jit_write_header(pid, perf_jit_map_state.perf_map);

    perf_jit_map_state.map_lock = PyThread_allocate_lock();
    if (perf_jit_map_state.map_lock == NULL) {
        fclose(perf_jit_map_state.perf_map);
        return NULL;
    }
    perf_jit_map_state.code_id = 0;

    trampoline_api.code_padding = PERF_JIT_CODE_PADDING;
    return &perf_jit_map_state;
}

/* DWARF definitions. */

#define DWRF_CIE_VERSION 1

enum {
    DWRF_CFA_nop = 0x0,
    DWRF_CFA_offset_extended = 0x5,
    DWRF_CFA_def_cfa = 0xc,
    DWRF_CFA_def_cfa_offset = 0xe,
    DWRF_CFA_offset_extended_sf = 0x11,
    DWRF_CFA_advance_loc = 0x40,
    DWRF_CFA_offset = 0x80
};

enum
  {
    DWRF_EH_PE_absptr = 0x00,
    DWRF_EH_PE_omit = 0xff,

    /* FDE data encoding.  */
    DWRF_EH_PE_uleb128 = 0x01,
    DWRF_EH_PE_udata2 = 0x02,
    DWRF_EH_PE_udata4 = 0x03,
    DWRF_EH_PE_udata8 = 0x04,
    DWRF_EH_PE_sleb128 = 0x09,
    DWRF_EH_PE_sdata2 = 0x0a,
    DWRF_EH_PE_sdata4 = 0x0b,
    DWRF_EH_PE_sdata8 = 0x0c,
    DWRF_EH_PE_signed = 0x08,

    /* FDE flags.  */
    DWRF_EH_PE_pcrel = 0x10,
    DWRF_EH_PE_textrel = 0x20,
    DWRF_EH_PE_datarel = 0x30,
    DWRF_EH_PE_funcrel = 0x40,
    DWRF_EH_PE_aligned = 0x50,

    DWRF_EH_PE_indirect = 0x80
  };

enum { DWRF_TAG_compile_unit = 0x11 };

enum { DWRF_children_no = 0, DWRF_children_yes = 1 };

enum { DWRF_AT_name = 0x03, DWRF_AT_stmt_list = 0x10, DWRF_AT_low_pc = 0x11, DWRF_AT_high_pc = 0x12 };

enum { DWRF_FORM_addr = 0x01, DWRF_FORM_data4 = 0x06, DWRF_FORM_string = 0x08 };

enum { DWRF_LNS_extended_op = 0, DWRF_LNS_copy = 1, DWRF_LNS_advance_pc = 2, DWRF_LNS_advance_line = 3 };

enum { DWRF_LNE_end_sequence = 1, DWRF_LNE_set_address = 2 };

enum {
#ifdef __x86_64__
    /* Yes, the order is strange, but correct. */
    DWRF_REG_AX,
    DWRF_REG_DX,
    DWRF_REG_CX,
    DWRF_REG_BX,
    DWRF_REG_SI,
    DWRF_REG_DI,
    DWRF_REG_BP,
    DWRF_REG_SP,
    DWRF_REG_8,
    DWRF_REG_9,
    DWRF_REG_10,
    DWRF_REG_11,
    DWRF_REG_12,
    DWRF_REG_13,
    DWRF_REG_14,
    DWRF_REG_15,
    DWRF_REG_RA,
#elif defined(__aarch64__) && defined(__AARCH64EL__) && !defined(__ILP32__)
    DWRF_REG_SP = 31,
    DWRF_REG_RA = 30,
#else
#    error "Unsupported target architecture"
#endif
};

typedef struct ELFObjectContext
{
    uint8_t* p; /* Pointer to next address in obj.space. */
    uint8_t* startp; /* Pointer to start address in obj.space. */
    uint8_t* eh_frame_p; /* Pointer to start address in obj.space. */
    uint32_t code_size; /* Size of machine code. */
} ELFObjectContext;

/* Append a null-terminated string. */
static uint32_t
elfctx_append_string(ELFObjectContext* ctx, const char* str)
{
    uint8_t* p = ctx->p;
    uint32_t ofs = (uint32_t)(p - ctx->startp);
    do {
        *p++ = (uint8_t)*str;
    } while (*str++);
    ctx->p = p;
    return ofs;
}

/* Append a SLEB128 value. */
static void
elfctx_append_sleb128(ELFObjectContext* ctx, int32_t v)
{
    uint8_t* p = ctx->p;
    for (; (uint32_t)(v + 0x40) >= 0x80; v >>= 7) {
        *p++ = (uint8_t)((v & 0x7f) | 0x80);
    }
    *p++ = (uint8_t)(v & 0x7f);
    ctx->p = p;
}

/* Append a ULEB128 to buffer. */
static void
elfctx_append_uleb128(ELFObjectContext* ctx, uint32_t v)
{
    uint8_t* p = ctx->p;
    for (; v >= 0x80; v >>= 7) {
        *p++ = (char)((v & 0x7f) | 0x80);
    }
    *p++ = (char)v;
    ctx->p = p;
}

/* Shortcuts to generate DWARF structures. */
#define DWRF_U8(x) (*p++ = (x))
#define DWRF_I8(x) (*(int8_t*)p = (x), p++)
#define DWRF_U16(x) (*(uint16_t*)p = (x), p += 2)
#define DWRF_U32(x) (*(uint32_t*)p = (x), p += 4)
#define DWRF_ADDR(x) (*(uintptr_t*)p = (x), p += sizeof(uintptr_t))
#define DWRF_UV(x) (ctx->p = p, elfctx_append_uleb128(ctx, (x)), p = ctx->p)
#define DWRF_SV(x) (ctx->p = p, elfctx_append_sleb128(ctx, (x)), p = ctx->p)
#define DWRF_STR(str) (ctx->p = p, elfctx_append_string(ctx, (str)), p = ctx->p)
#define DWRF_ALIGNNOP(s)                                                                                \
    while ((uintptr_t)p & ((s)-1)) {                                                                    \
        *p++ = DWRF_CFA_nop;                                                                            \
    }
#define DWRF_SECTION(name, stmt)                                                                        \
    {                                                                                                   \
        uint32_t* szp_##name = (uint32_t*)p;                                                            \
        p += 4;                                                                                         \
        stmt;                                                                                           \
        *szp_##name = (uint32_t)((p - (uint8_t*)szp_##name) - 4);                                       \
    }

/* Initialize .eh_frame section. */
static void
elf_init_ehframe(ELFObjectContext* ctx)
{
    uint8_t* p = ctx->p;
    uint8_t* framep = p;

    /* Emit DWARF EH CIE. */
    DWRF_SECTION(CIE, DWRF_U32(0); /* Offset to CIE itself. */
                 DWRF_U8(DWRF_CIE_VERSION);
                 DWRF_STR("zR"); /* Augmentation. */
                 DWRF_UV(1); /* Code alignment factor. */
                 DWRF_SV(-(int64_t)sizeof(uintptr_t)); /* Data alignment factor. */
                 DWRF_U8(DWRF_REG_RA); /* Return address register. */
                 DWRF_UV(1);
                 DWRF_U8(DWRF_EH_PE_pcrel | DWRF_EH_PE_sdata4); /* Augmentation data. */
                 DWRF_U8(DWRF_CFA_def_cfa); DWRF_UV(DWRF_REG_SP); DWRF_UV(sizeof(uintptr_t));
                 DWRF_U8(DWRF_CFA_offset|DWRF_REG_RA); DWRF_UV(1);
                 DWRF_ALIGNNOP(sizeof(uintptr_t));
    )

    ctx->eh_frame_p = p;

    /* Emit DWARF EH FDE. */
    DWRF_SECTION(FDE, DWRF_U32((uint32_t)(p - framep)); /* Offset to CIE. */
                 DWRF_U32(-0x30); /* Machine code offset relative to .text. */
                 DWRF_U32(ctx->code_size); /* Machine code length. */
                 DWRF_U8(0); /* Augmentation data. */
    /* Registers saved in CFRAME. */
#ifdef __x86_64__
                 DWRF_U8(DWRF_CFA_advance_loc | 4);
                 DWRF_U8(DWRF_CFA_def_cfa_offset); DWRF_UV(16);
                 DWRF_U8(DWRF_CFA_advance_loc | 6);
                 DWRF_U8(DWRF_CFA_def_cfa_offset); DWRF_UV(8);
    /* Extra registers saved for JIT-compiled code. */
#elif defined(__aarch64__) && defined(__AARCH64EL__) && !defined(__ILP32__)
                 DWRF_U8(DWRF_CFA_advance_loc | 1);
                 DWRF_U8(DWRF_CFA_def_cfa_offset); DWRF_UV(16);
                 DWRF_U8(DWRF_CFA_offset | 29); DWRF_UV(2);
                 DWRF_U8(DWRF_CFA_offset | 30); DWRF_UV(1);
                 DWRF_U8(DWRF_CFA_advance_loc | 3);
                 DWRF_U8(DWRF_CFA_offset | -(64 - 29));
                 DWRF_U8(DWRF_CFA_offset | -(64 - 30));
                 DWRF_U8(DWRF_CFA_def_cfa_offset);
                 DWRF_UV(0);
#else
#    error "Unsupported target architecture"
#endif
                 DWRF_ALIGNNOP(sizeof(uintptr_t));)

    ctx->p = p;
}

static void perf_map_jit_write_entry(void *state, const void *code_addr,
                         unsigned int code_size, PyCodeObject *co)
{

    if (perf_jit_map_state.perf_map == NULL) {
        void* ret = perf_map_jit_init();
        if(ret == NULL){
            return;
        }
    }

    const char *entry = "";
    if (co->co_qualname != NULL) {
        entry = PyUnicode_AsUTF8(co->co_qualname);
    }
    const char *filename = "";
    if (co->co_filename != NULL) {
        filename = PyUnicode_AsUTF8(co->co_filename);
    }


    size_t perf_map_entry_size = snprintf(NULL, 0, "py::%s:%s", entry, filename) + 1;
    char* perf_map_entry = (char*) PyMem_RawMalloc(perf_map_entry_size);
    if (perf_map_entry == NULL) {
        return;
    }
    snprintf(perf_map_entry, perf_map_entry_size, "py::%s:%s", entry, filename);

    const size_t name_length = strlen(perf_map_entry);
    uword base = (uword)code_addr;
    uword size = code_size;

    // Write the code unwinding info event.

    // Create unwinding information (eh frame)
    ELFObjectContext ctx;
    char buffer[1024];
    ctx.code_size = code_size;
    ctx.startp = ctx.p = (uint8_t*)buffer;
    elf_init_ehframe(&ctx);
    int eh_frame_size = ctx.p - ctx.startp;

    // Populate the unwind info event for perf
    CodeUnwindingInfoEvent ev2;
    ev2.base.event = PerfUnwindingInfo;
    ev2.base.time_stamp = get_current_monotonic_ticks();
    ev2.unwind_data_size = sizeof(EhFrameHeader) + eh_frame_size;
    // Ensure we have enough space between DSOs when perf maps them
    assert(ev2.unwind_data_size <= PERF_JIT_CODE_PADDING);
    ev2.eh_frame_hdr_size = sizeof(EhFrameHeader);
    ev2.mapped_size = round_up(ev2.unwind_data_size, 16);
    int content_size = sizeof(ev2) + sizeof(EhFrameHeader) + eh_frame_size;
    int padding_size = round_up(content_size, 8) - content_size;
    ev2.base.size = content_size + padding_size;
    perf_map_jit_write_fully(&ev2, sizeof(ev2));


    // Populate the eh Frame header
    EhFrameHeader f;
    f.version = 1;
    f.eh_frame_ptr_enc = DwarfSData4 | DwarfPcRel;
    f.fde_count_enc = DwarfUData4;
    f.table_enc = DwarfSData4 | DwarfDataRel;
    f.eh_frame_ptr = -(eh_frame_size + 4 * sizeof(unsigned char));
    f.eh_fde_count = 1;
    f.from = -(round_up(code_size, 8) + eh_frame_size);
    int cie_size = ctx.eh_frame_p - ctx.startp;
    f.to = -(eh_frame_size - cie_size);

    perf_map_jit_write_fully(ctx.startp, eh_frame_size);
    perf_map_jit_write_fully(&f, sizeof(f));

    char padding_bytes[] = "\0\0\0\0\0\0\0\0";
    perf_map_jit_write_fully(&padding_bytes, padding_size);

    // Write the code load event.
    CodeLoadEvent ev;
    ev.base.event = PerfLoad;
    ev.base.size = sizeof(ev) + (name_length+1) + size;
    ev.base.time_stamp = get_current_monotonic_ticks();
    ev.process_id = getpid();
    ev.thread_id = syscall(SYS_gettid);
    ev.vma = base;
    ev.code_address = base;
    ev.code_size = size;
    perf_jit_map_state.code_id += 1;
    ev.code_id = perf_jit_map_state.code_id;

    perf_map_jit_write_fully(&ev, sizeof(ev));
    perf_map_jit_write_fully(perf_map_entry, name_length+1);
    perf_map_jit_write_fully((void*)(base), size);
    return;
}

static int perf_map_jit_fini(void* state) {
    if (perf_jit_map_state.perf_map != NULL) {
        // close the file
        PyThread_acquire_lock(perf_jit_map_state.map_lock, 1);
        fclose(perf_jit_map_state.perf_map);
        PyThread_release_lock(perf_jit_map_state.map_lock);

        // clean up the lock and state
        PyThread_free_lock(perf_jit_map_state.map_lock);
        perf_jit_map_state.perf_map = NULL;
    }
    if (perf_jit_map_state.mapped_buffer != NULL) {
        munmap(perf_jit_map_state.mapped_buffer, perf_jit_map_state.mapped_size);
    }
    trampoline_api.state = NULL;
    return 0;
}

_PyPerf_Callbacks _Py_perfmap_jit_callbacks = {
    &perf_map_jit_init,
    &perf_map_jit_write_entry,
    &perf_map_jit_fini,
};

#endif
