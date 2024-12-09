/*

Perf trampoline instrumentation
===============================

This file contains instrumentation to allow to associate
calls to the CPython eval loop back to the names of the Python
functions and filename being executed.

Many native performance profilers like the Linux perf tools are
only available to 'see' the C stack when sampling from the profiled
process. This means that if we have the following python code:

    import time
    def foo(n):
        # Some CPU intensive code

    def bar(n):
        foo(n)

    def baz(n):
        bar(n)

    baz(10000000)

A performance profiler that is only able to see native frames will
produce the following backtrace when sampling from foo():

    _PyEval_EvalFrameDefault -----> Evaluation frame of foo()
    _PyEval_Vector
    _PyFunction_Vectorcall
    PyObject_Vectorcall
    call_function

    _PyEval_EvalFrameDefault ------> Evaluation frame of bar()
    _PyEval_EvalFrame
    _PyEval_Vector
    _PyFunction_Vectorcall
    PyObject_Vectorcall
    call_function

    _PyEval_EvalFrameDefault -------> Evaluation frame of baz()
    _PyEval_EvalFrame
    _PyEval_Vector
    _PyFunction_Vectorcall
    PyObject_Vectorcall
    call_function

    ...

    Py_RunMain

Because the profiler is only able to see the native frames and the native
function that runs the evaluation loop is the same (_PyEval_EvalFrameDefault)
then the profiler and any reporter generated by it will not be able to
associate the names of the Python functions and the filenames associated with
those calls, rendering the results useless in the Python world.

To fix this problem, we introduce the concept of a trampoline frame. A
trampoline frame is a piece of code that is unique per Python code object that
is executed before entering the CPython eval loop. This piece of code just
calls the original Python evaluation function (_PyEval_EvalFrameDefault) and
forwards all the arguments received. In this way, when a profiler samples
frames from the previous example it will see;

    _PyEval_EvalFrameDefault -----> Evaluation frame of foo()
    [Jit compiled code 3]
    _PyEval_Vector
    _PyFunction_Vectorcall
    PyObject_Vectorcall
    call_function

    _PyEval_EvalFrameDefault ------> Evaluation frame of bar()
    [Jit compiled code 2]
    _PyEval_EvalFrame
    _PyEval_Vector
    _PyFunction_Vectorcall
    PyObject_Vectorcall
    call_function

    _PyEval_EvalFrameDefault -------> Evaluation frame of baz()
    [Jit compiled code 1]
    _PyEval_EvalFrame
    _PyEval_Vector
    _PyFunction_Vectorcall
    PyObject_Vectorcall
    call_function

    ...

    Py_RunMain

When we generate every unique copy of the trampoline (what here we called "[Jit
compiled code N]") we write the relationship between the compiled code and the
Python function that is associated with it. Every profiler requires this
information in a different format. For example, the Linux "perf" profiler
requires a file in "/tmp/perf-PID.map" (name and location not configurable)
with the following format:

    <compiled code address> <compiled code size> <name of the compiled code>

If this file is available when "perf" generates reports, it will automatically
associate every trampoline with the Python function that it is associated with
allowing it to generate reports that include Python information. These reports
then can also be filtered in a way that *only* Python information appears.

Notice that for this to work, there must be a unique copied of the trampoline
per Python code object even if the code in the trampoline is the same. To
achieve this we have a assembly template in Objects/asm_trampiline.S that is
compiled into the Python executable/shared library. This template generates a
symbol that maps the start of the assembly code and another that marks the end
of the assembly code for the trampoline.  Then, every time we need a unique
trampoline for a Python code object, we copy the assembly code into a mmaped
area that has executable permissions and we return the start of that area as
our trampoline function.

Asking for a mmap-ed memory area for trampoline is very wasteful so we
allocate big arenas of memory in a single mmap call, we populate the entire
arena with copies of the trampoline (this allows us to now have to invalidate
the icache for the instructions in the page) and then we return the next
available chunk every time someone asks for a new trampoline. We keep a linked
list of arenas in case the current memory arena is exhausted and another one is
needed.

For the best results, Python should be compiled with
CFLAGS="-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer" as this allows
profilers to unwind using only the frame pointer and not on DWARF debug
information (note that as trampilines are dynamically generated there won't be
any DWARF information available for them).
*/

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


#if defined(__arm__) || defined(__arm64__) || defined(__aarch64__)
#define PY_HAVE_INVALIDATE_ICACHE

#if defined(__clang__) || defined(__GNUC__)
extern void __clear_cache(void *, void*);
#endif

static void invalidate_icache(char* begin, char*end) {
#if defined(__clang__) || defined(__GNUC__)
    return __clear_cache(begin, end);
#else
    return;
#endif
}
#endif

/* The function pointer is passed as last argument. The other three arguments
 * are passed in the same order as the function requires. This results in
 * shorter, more efficient ASM code for trampoline.
 */
typedef PyObject *(*py_evaluator)(PyThreadState *, _PyInterpreterFrame *,
                                  int throwflag);
typedef PyObject *(*py_trampoline)(PyThreadState *, _PyInterpreterFrame *, int,
                                   py_evaluator);

extern void *_Py_trampoline_func_start;  // Start of the template of the
                                         // assembly trampoline
extern void *
    _Py_trampoline_func_end;  // End of the template of the assembly trampoline

struct code_arena_st {
    char *start_addr;    // Start of the memory arena
    char *current_addr;  // Address of the current trampoline within the arena
    size_t size;         // Size of the memory arena
    size_t size_left;    // Remaining size of the memory arena
    size_t code_size;    // Size of the code of every trampoline in the arena
    struct code_arena_st
        *prev;  // Pointer to the arena  or NULL if this is the first arena.
};

typedef struct code_arena_st code_arena_t;
typedef struct trampoline_api_st trampoline_api_t;

enum perf_trampoline_type {
    PERF_TRAMPOLINE_UNSET = 0,
    PERF_TRAMPOLINE_TYPE_MAP = 1,
    PERF_TRAMPOLINE_TYPE_JITDUMP = 2,
};

#define perf_status _PyRuntime.ceval.perf.status
#define extra_code_index _PyRuntime.ceval.perf.extra_code_index
#define perf_code_arena _PyRuntime.ceval.perf.code_arena
#define trampoline_api _PyRuntime.ceval.perf.trampoline_api
#define perf_map_file _PyRuntime.ceval.perf.map_file
#define persist_after_fork _PyRuntime.ceval.perf.persist_after_fork
#define perf_trampoline_type _PyRuntime.ceval.perf.perf_trampoline_type

static void
perf_map_write_entry(void *state, const void *code_addr,
                         unsigned int code_size, PyCodeObject *co)
{
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
    PyUnstable_WritePerfMapEntry(code_addr, code_size, perf_map_entry);
    PyMem_RawFree(perf_map_entry);
}

static void*
perf_map_init_state(void)
{
    PyUnstable_PerfMapState_Init();
    trampoline_api.code_padding = 0;
    perf_trampoline_type = PERF_TRAMPOLINE_TYPE_MAP;
    return NULL;
}

static int
perf_map_free_state(void *state)
{
    PyUnstable_PerfMapState_Fini();
    return 0;
}

_PyPerf_Callbacks _Py_perfmap_callbacks = {
    &perf_map_init_state,
    &perf_map_write_entry,
    &perf_map_free_state,
};


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

// TRAMPOLINE MANAGEMENT API

static int
new_code_arena(void)
{
    // non-trivial programs typically need 64 to 256 kiB.
    size_t mem_size = 4096 * 16;
    assert(mem_size % sysconf(_SC_PAGESIZE) == 0);
    char *memory =
        mmap(NULL,  // address
             mem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
             -1,  // fd (not used here)
             0);  // offset (not used here)
    if (memory == MAP_FAILED) {
        PyErr_SetFromErrno(PyExc_OSError);
        PyErr_FormatUnraisable("Failed to create new mmap for perf trampoline");
        perf_status = PERF_STATUS_FAILED;
        return -1;
    }
    void *start = &_Py_trampoline_func_start;
    void *end = &_Py_trampoline_func_end;
    size_t code_size = end - start;
    size_t chunk_size = round_up(code_size + trampoline_api.code_padding, 16);
    // TODO: Check the effect of alignment of the code chunks. Initial investigation
    // showed that this has no effect on performance in x86-64 or aarch64 and the current
    // version has the advantage that the unwinder in GDB can unwind across JIT-ed code.
    //
    // We should check the values in the future and see if there is a
    // measurable performance improvement by rounding trampolines up to 32-bit
    // or 64-bit alignment.

    size_t n_copies = mem_size / chunk_size;
    for (size_t i = 0; i < n_copies; i++) {
        memcpy(memory + i * chunk_size, start, code_size * sizeof(char));
    }
    // Some systems may prevent us from creating executable code on the fly.
    int res = mprotect(memory, mem_size, PROT_READ | PROT_EXEC);
    if (res == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        munmap(memory, mem_size);
        PyErr_FormatUnraisable("Failed to set mmap for perf trampoline to "
                               "PROT_READ | PROT_EXEC");
        return -1;
    }

#ifdef PY_HAVE_INVALIDATE_ICACHE
    // Before the JIT can run a block of code that has been emitted it must invalidate
    // the instruction cache on some platforms like arm and aarch64.
    invalidate_icache(memory, memory + mem_size);
#endif

    code_arena_t *new_arena = PyMem_RawCalloc(1, sizeof(code_arena_t));
    if (new_arena == NULL) {
        PyErr_NoMemory();
        munmap(memory, mem_size);
        PyErr_FormatUnraisable("Failed to allocate new code arena struct for perf trampoline");
        return -1;
    }

    new_arena->start_addr = memory;
    new_arena->current_addr = memory;
    new_arena->size = mem_size;
    new_arena->size_left = mem_size;
    new_arena->code_size = code_size;
    new_arena->prev = perf_code_arena;
    perf_code_arena = new_arena;
    return 0;
}

static void
free_code_arenas(void)
{
    code_arena_t *cur = perf_code_arena;
    code_arena_t *prev;
    perf_code_arena = NULL;  // invalid static pointer
    while (cur) {
        munmap(cur->start_addr, cur->size);
        prev = cur->prev;
        PyMem_RawFree(cur);
        cur = prev;
    }
}

static inline py_trampoline
code_arena_new_code(code_arena_t *code_arena)
{
    py_trampoline trampoline = (py_trampoline)code_arena->current_addr;
    size_t total_code_size = round_up(code_arena->code_size + trampoline_api.code_padding, 16);
    code_arena->size_left -= total_code_size;
    code_arena->current_addr += total_code_size;
    return trampoline;
}

static inline py_trampoline
compile_trampoline(void)
{
    size_t total_code_size = round_up(perf_code_arena->code_size + trampoline_api.code_padding, 16);
    if ((perf_code_arena == NULL) ||
        (perf_code_arena->size_left <= total_code_size)) {
        if (new_code_arena() < 0) {
            return NULL;
        }
    }
    assert(perf_code_arena->size_left <= perf_code_arena->size);
    return code_arena_new_code(perf_code_arena);
}

static PyObject *
py_trampoline_evaluator(PyThreadState *ts, _PyInterpreterFrame *frame,
                        int throw)
{
    if (perf_status == PERF_STATUS_FAILED ||
        perf_status == PERF_STATUS_NO_INIT) {
        goto default_eval;
    }
    PyCodeObject *co = _PyFrame_GetCode(frame);
    py_trampoline f = NULL;
    assert(extra_code_index != -1);
    int ret = _PyCode_GetExtra((PyObject *)co, extra_code_index, (void **)&f);
    if (ret != 0 || f == NULL) {
        // This is the first time we see this code object so we need
        // to compile a trampoline for it.
        py_trampoline new_trampoline = compile_trampoline();
        if (new_trampoline == NULL) {
            goto default_eval;
        }
        trampoline_api.write_state(trampoline_api.state, new_trampoline,
                                   perf_code_arena->code_size, co);
        _PyCode_SetExtra((PyObject *)co, extra_code_index,
                         (void *)new_trampoline);
        f = new_trampoline;
    }
    assert(f != NULL);
    return f(ts, frame, throw, _PyEval_EvalFrameDefault);
default_eval:
    // Something failed, fall back to the default evaluator.
    return _PyEval_EvalFrameDefault(ts, frame, throw);
}
#endif  // PY_HAVE_PERF_TRAMPOLINE

int PyUnstable_PerfTrampoline_CompileCode(PyCodeObject *co)
{
#ifdef PY_HAVE_PERF_TRAMPOLINE
    py_trampoline f = NULL;
    assert(extra_code_index != -1);
    int ret = _PyCode_GetExtra((PyObject *)co, extra_code_index, (void **)&f);
    if (ret != 0 || f == NULL) {
        py_trampoline new_trampoline = compile_trampoline();
        if (new_trampoline == NULL) {
            return 0;
        }
        trampoline_api.write_state(trampoline_api.state, new_trampoline,
                                   perf_code_arena->code_size, co);
        return _PyCode_SetExtra((PyObject *)co, extra_code_index,
                         (void *)new_trampoline);
    }
#endif // PY_HAVE_PERF_TRAMPOLINE
    return 0;
}

int
_PyIsPerfTrampolineActive(void)
{
#ifdef PY_HAVE_PERF_TRAMPOLINE
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->interp->eval_frame == py_trampoline_evaluator;
#endif
    return 0;
}

void
_PyPerfTrampoline_GetCallbacks(_PyPerf_Callbacks *callbacks)
{
    if (callbacks == NULL) {
        return;
    }
#ifdef PY_HAVE_PERF_TRAMPOLINE
    callbacks->init_state = trampoline_api.init_state;
    callbacks->write_state = trampoline_api.write_state;
    callbacks->free_state = trampoline_api.free_state;
#endif
    return;
}

int
_PyPerfTrampoline_SetCallbacks(_PyPerf_Callbacks *callbacks)
{
    if (callbacks == NULL) {
        return -1;
    }
#ifdef PY_HAVE_PERF_TRAMPOLINE
    if (trampoline_api.state) {
        _PyPerfTrampoline_Fini();
    }
    trampoline_api.init_state = callbacks->init_state;
    trampoline_api.write_state = callbacks->write_state;
    trampoline_api.free_state = callbacks->free_state;
    trampoline_api.state = NULL;
#endif
    return 0;
}

int
_PyPerfTrampoline_Init(int activate)
{
#ifdef PY_HAVE_PERF_TRAMPOLINE
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate->interp->eval_frame &&
        tstate->interp->eval_frame != py_trampoline_evaluator) {
        PyErr_SetString(PyExc_RuntimeError,
                        "Trampoline cannot be initialized as a custom eval "
                        "frame is already present");
        return -1;
    }
    if (!activate) {
        _PyInterpreterState_SetEvalFrameFunc(tstate->interp, NULL);
        perf_status = PERF_STATUS_NO_INIT;
    }
    else {
        _PyInterpreterState_SetEvalFrameFunc(tstate->interp, py_trampoline_evaluator);
        if (new_code_arena() < 0) {
            return -1;
        }
        extra_code_index = _PyEval_RequestCodeExtraIndex(NULL);
        if (extra_code_index == -1) {
            return -1;
        }
        if (trampoline_api.state == NULL && trampoline_api.init_state != NULL) {
            trampoline_api.state = trampoline_api.init_state();
        }
        perf_status = PERF_STATUS_OK;
    }
#endif
    return 0;
}

int
_PyPerfTrampoline_Fini(void)
{
#ifdef PY_HAVE_PERF_TRAMPOLINE
    if (perf_status != PERF_STATUS_OK) {
        return 0;
    }
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate->interp->eval_frame == py_trampoline_evaluator) {
        _PyInterpreterState_SetEvalFrameFunc(tstate->interp, NULL);
    }
    if (perf_status == PERF_STATUS_OK) {
        trampoline_api.free_state(trampoline_api.state);
        perf_trampoline_type = PERF_TRAMPOLINE_UNSET;
    }
    extra_code_index = -1;
    perf_status = PERF_STATUS_NO_INIT;
#endif
    return 0;
}

void _PyPerfTrampoline_FreeArenas(void) {
#ifdef PY_HAVE_PERF_TRAMPOLINE
    free_code_arenas();
#endif
    return;
}

int
PyUnstable_PerfTrampoline_SetPersistAfterFork(int enable){
#ifdef PY_HAVE_PERF_TRAMPOLINE
    persist_after_fork = enable;
    return persist_after_fork;
#endif
    return 0;
}

PyStatus
_PyPerfTrampoline_AfterFork_Child(void)
{
#ifdef PY_HAVE_PERF_TRAMPOLINE
    if (persist_after_fork) {
        if (perf_trampoline_type != PERF_TRAMPOLINE_TYPE_MAP) {
            return PyStatus_Error("Failed to copy perf map file as perf trampoline type is not type map.");
        }
        _PyPerfTrampoline_Fini();
        char filename[256];
        pid_t parent_pid = getppid();
        snprintf(filename, sizeof(filename), "/tmp/perf-%d.map", parent_pid);
        if (PyUnstable_CopyPerfMapFile(filename) != 0) {
            return PyStatus_Error("Failed to copy perf map file.");
        }
    } else {
        // Restart trampoline in file in child.
        int was_active = _PyIsPerfTrampolineActive();
        _PyPerfTrampoline_Fini();
        if (was_active) {
            _PyPerfTrampoline_Init(1);
        }
    }
#endif
    return PyStatus_Ok();
}
