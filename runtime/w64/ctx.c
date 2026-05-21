#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <windows.h>

#include "ctx.h"

#if 0
void ctx_dump(void *ctx) {
  CONTEXT *context = (CONTEXT*)ctx;
  int i;

  printf("ACDBx : %016I64x %016I64x %016I64x %016I64x\n",
         c->Rax, c->Rcx, c->Rdx, c->Rbx);
  printf("SBpSDi: %016I64x %016I64x %016I64x %016I64x\n",
         c->Rsp, c->Rbp, c->Rsi, c->Rdi);
  printf("r8-11 : %016I64x %016I64x %016I64x %016I64x\n",
         c->R8,  c->R9,  c->R10, c->R11);
  printf("r12-15: %016I64x %016I64x %016I64x %016I64x\n",
         c->R12, c->R13, c->R14, c->R15);

  for (i = 0; i < 16; i += 2)
    printf("x%02d-%02d: %016I64x.%016I64x %016I64x.%016I64x\n",
	   i, i+1,
           c->FloatSave.XmmRegisters[i].High,
           c->FloatSave.XmmRegisters[i].Low,
           c->FloatSave.XmmRegisters[i + 1].High,
           c->FloatSave.XmmRegisters[i + 1].Low);

  fflush (stdout);
}
#endif

void *ctx_function_at(void *ip) {
  ULONG64 ControlPC;
  ULONG64 ImageBase;
  ControlPC = (ULONG64)ip;
  PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(ControlPC, &ImageBase, NULL);
  return (void*)((ULONG64)entry->BeginAddress + ImageBase);
}

void *ctx_module_at(void *ip) {
  ULONG64 ControlPC;
  ULONG64 ImageBase;
  ControlPC = (ULONG64)ip;
  PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(ControlPC, &ImageBase, NULL);
  return (void*)ImageBase;
}

void *ctx_unwind(void *ctx) {
  PRUNTIME_FUNCTION entry;
  ULONG64 ControlPC;
  ULONG64 ImageBase;
  PVOID HandlerData;
  ULONG64 EstablisherFrame;
  CONTEXT *context = (CONTEXT*)ctx;
  ControlPC = context->Rip;
  entry = RtlLookupFunctionEntry(ControlPC, &ImageBase, NULL);
  if (entry == NULL) return 0;
  RtlVirtualUnwind(0, ImageBase, ControlPC, entry, context, &HandlerData, &EstablisherFrame, NULL);
  return (void*)((ULONG64)entry->BeginAddress + ImageBase);
}

void ctx_save(void *ctx) {
  CONTEXT *context = (CONTEXT*)ctx;
  context->ContextFlags = CONTEXT_ALL;
  RtlCaptureContext(context);
  //fprintf(stderr, "%p\n", ctx_ip(ctx));
  ctx_unwind(ctx);
  //fprintf(stderr, "%p\n", ctx_ip(ctx));
}

void ctx_load(void *ctx) {
  CONTEXT *context = (CONTEXT*)ctx;
  RtlRestoreContext(context, NULL);
}

void *ctx_ip(void *ctx) {
  return (void*)((CONTEXT*)ctx)->Rip;
}

void *ctx_sp(void *ctx) {
  return (void*)((CONTEXT*)ctx)->Rsp;
}

static int (*ctx_error_handler)(ctx_error_t *info);

static LONG WINAPI windows_exception_handler(EXCEPTION_POINTERS *ExceptionInfo) {
  ctx_error_t error;
  memset(&error, 0, sizeof(ctx_error_t));
  LONG r = EXCEPTION_EXECUTE_HANDLER; //by default abort execution
  error.ctx = ExceptionInfo->ContextRecord;
  //error.ip = (intptr_t)ExceptionInfo->ExceptionRecord->ExceptionAddress;
  switch(ExceptionInfo->ExceptionRecord->ExceptionCode) {
  //case EXCEPTION_IN_PAGE_ERROR:
  case EXCEPTION_ACCESS_VIOLATION:
    error.id = CTXE_ACCESS;
    error.text = "access violation";
    error.mem = (void*)ExceptionInfo->ExceptionRecord->ExceptionInformation[1];
    /* DEBUG (task #12 follow-on): when SYMTA_DUMP_SEGV is set, dump
     * the faulting instruction's address and full GPR state.  This
     * lets us correlate the segfault site to a C builtin and from
     * there to the slot that held the bad dyn. */
    if (getenv("SYMTA_DUMP_SEGV")) {
      CONTEXT *c = ExceptionInfo->ContextRecord;
      fprintf(stderr,
              "[SEGV] access %s @ %p  rip=%016llx\n"
              "[SEGV]   rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n"
              "[SEGV]   rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n"
              "[SEGV]    r8=%016llx  r9=%016llx r10=%016llx r11=%016llx\n"
              "[SEGV]   r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n"
              "[SEGV]   ImageBase=%p\n",
              (ExceptionInfo->ExceptionRecord->ExceptionInformation[0]
                ? "WRITE" : "READ"),
              error.mem, (unsigned long long)c->Rip,
              (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
              (unsigned long long)c->Rcx, (unsigned long long)c->Rdx,
              (unsigned long long)c->Rsi, (unsigned long long)c->Rdi,
              (unsigned long long)c->Rbp, (unsigned long long)c->Rsp,
              (unsigned long long)c->R8,  (unsigned long long)c->R9,
              (unsigned long long)c->R10, (unsigned long long)c->R11,
              (unsigned long long)c->R12, (unsigned long long)c->R13,
              (unsigned long long)c->R14, (unsigned long long)c->R15,
              (void*)GetModuleHandleA(NULL));
      fflush(stderr);
    }
    break;
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    error.id = CTXE_OTHER;
    error.text = "array bounds exceed";
    break;
  case EXCEPTION_BREAKPOINT:
    error.id = CTXE_OTHER;
    error.text = "breakpoint";
    break;
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
    error.id = CTXE_DIV_BY_ZERO;
    error.text = "division by zero";
    break;
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    error.id = CTXE_DIV_BY_ZERO_FPU;
    error.text = "division by zero (floating point)";
    break;
  case EXCEPTION_STACK_OVERFLOW:
    error.id = CTXE_STACK_OVERFLOW;
    error.text = "stack overflow";
    break;
  default:
    error.id = CTXE_OTHER;
    error.text = "unknown";
    break;
  }
  if (ctx_error_handler(&error) == CTXE_CONTINUE) {
    r = EXCEPTION_CONTINUE_EXECUTION; // retry execution, hoping that user has fixed error
  } else {
    r = EXCEPTION_EXECUTE_HANDLER; // abort execution
  }
  return r;
}

void ctx_set_error_handler(int (*error_handler)(ctx_error_t *info)) {
  if (!ctx_error_handler) {
    SetUnhandledExceptionFilter(windows_exception_handler);
  }
  ctx_error_handler = error_handler;
}

/* ============================================================
 * Hardware watchpoint (DR0) -- DEBUG (task #12).
 *
 * Configures the DR0 debug register to trap on writes to a
 * single 8-byte heap slot.  Used to pin down the SBC opcode or
 * C builtin that writes the poisoned 0x07 dyn into a list slot
 * under SYMTA_GEN0_SIZE=65536.
 *
 * Strategy:
 *   - First diagnostic run (without this watchpoint) reports
 *     the bad slot's gid via the [LISTSCAN] line in gc_types.h.
 *   - Compute slot address = api.heap0 + gid*8 + slot_idx*8.
 *     Pass it back via SYMTA_WATCH_ADDR=<hex> on the next run.
 *   - The vectored handler dumps RIP plus all GPRs at each
 *     write to the slot.  Cross-reference RIP against
 *     `objdump -d symta.exe` to find the source line.
 *
 * Honors `SYMTA_WATCH_VALUE=<hex>` to log only writes of a
 * specific value (e.g., 0x07).  Without it, every write fires.
 *
 * Single-step semantics: after the trap, the RF flag is set in
 * EFLAGS so the same instruction completes without re-trapping
 * on the next fetch.  Subsequent unrelated writes to other
 * addresses don't trip the handler at all -- DR0 only matches
 * its specific address.
 *
 * Limit: one DR slot is enough for this hunt.  If we need more
 * watchpoints, extend to DR1/DR2/DR3. */

static uintptr_t watch_addr_g = 0;
static uint64_t  watch_value_g = 0;   /* 0 = log all writes */
static int       watch_hits_g  = 0;

static LONG CALLBACK watchpoint_vectored_handler(PEXCEPTION_POINTERS info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
    return EXCEPTION_CONTINUE_SEARCH;

  CONTEXT *c = info->ContextRecord;

  /* DR6 bit B0 set means DR0 fired.  Clear it so the next trap
   * is detected. */
  if (!(c->Dr6 & 0x1)) {
    /* Not our watchpoint -- ignore. */
    return EXCEPTION_CONTINUE_SEARCH;
  }

  /* Read the current value at the watched address.  The CPU
   * traps AFTER the write completes, so *p reflects the value
   * we just wrote. */
  uint64_t v = *(uint64_t*)watch_addr_g;
  int log = !watch_value_g || v == watch_value_g;

  if (log) {
    watch_hits_g++;
    fprintf(stderr,
            "[WATCH#%d] addr=%016llx val=%016llx  rip=%016llx\n",
            watch_hits_g,
            (unsigned long long)watch_addr_g,
            (unsigned long long)v,
            (unsigned long long)c->Rip);
    fprintf(stderr,
            "[WATCH#%d]   rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n",
            watch_hits_g,
            (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
            (unsigned long long)c->Rcx, (unsigned long long)c->Rdx);
    fprintf(stderr,
            "[WATCH#%d]   rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            watch_hits_g,
            (unsigned long long)c->Rsi, (unsigned long long)c->Rdi,
            (unsigned long long)c->Rbp, (unsigned long long)c->Rsp);
    fprintf(stderr,
            "[WATCH#%d]    r8=%016llx  r9=%016llx r10=%016llx r11=%016llx\n",
            watch_hits_g,
            (unsigned long long)c->R8,  (unsigned long long)c->R9,
            (unsigned long long)c->R10, (unsigned long long)c->R11);
    fprintf(stderr,
            "[WATCH#%d]   r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
            watch_hits_g,
            (unsigned long long)c->R12, (unsigned long long)c->R13,
            (unsigned long long)c->R14, (unsigned long long)c->R15);
    fflush(stderr);
    if (watch_value_g && v == watch_value_g) {
      /* On match, abort so the post-mortem has a clean state. */
      fprintf(stderr, "[WATCH] hit target value -- aborting\n");
      fflush(stderr);
      abort();
    }
  }

  /* Set RF (Resume Flag) so the next instruction fetch ignores
   * this debug exception.  Clear DR6.B0 to re-arm. */
  c->EFlags |= 0x10000;
  c->Dr6 &= ~0x1;
  return EXCEPTION_CONTINUE_EXECUTION;
}

/* Arm DR0 from a helper thread that suspends the main thread.
 *
 * Windows refuses to commit DR0/DR7 changes from the same
 * thread that owns the registers -- GetThreadContext returns
 * a syscall-frame view, and a VEH that modifies CONTEXT.Dr*
 * is silently ignored when control returns to user code.
 * The only supported way to write the live debug registers
 * of a thread is from a different thread, with the target
 * thread suspended.
 *
 * Sequence:
 *   1. CreateThread for a one-shot helper.
 *   2. Helper SuspendThread's the main thread.
 *   3. Helper GetThreadContext + sets Dr0/Dr7 + SetThreadContext.
 *   4. Helper ResumeThread's main and exits.
 *   5. Main thread continues with DR0 live; writes to the
 *      watched address trigger EXCEPTION_SINGLE_STEP, caught
 *      by watchpoint_vectored_handler. */

typedef struct {
  HANDLE target_thread;
  void  *addr;
  HANDLE done;
  int    ok;
} arm_args_t;

static DWORD WINAPI arm_helper(LPVOID p) {
  arm_args_t *a = (arm_args_t*)p;
  a->ok = 0;
  if (SuspendThread(a->target_thread) == (DWORD)-1) goto done;
  CONTEXT ctx = {0};
  ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  if (!GetThreadContext(a->target_thread, &ctx)) goto resume;
  ctx.Dr0 = (DWORD64)a->addr;
  ctx.Dr7 = (1ULL<<1) | (1ULL<<16) | (1ULL<<19);
  ctx.Dr6 = 0;
  ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  if (SetThreadContext(a->target_thread, &ctx)) a->ok = 1;
resume:
  ResumeThread(a->target_thread);
done:
  SetEvent(a->done);
  return 0;
}

int ctx_set_write_watchpoint(void *addr, uint64_t target_value) {
  watch_addr_g  = (uintptr_t)addr;
  watch_value_g = target_value;
  watch_hits_g  = 0;

  AddVectoredExceptionHandler(1, watchpoint_vectored_handler);

  arm_args_t args;
  args.addr = addr;
  args.done = CreateEvent(NULL, FALSE, FALSE, NULL);
  args.ok   = 0;
  /* Duplicate the current pseudo-handle into a real handle the
   * helper can use to suspend us. */
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                       GetCurrentProcess(), &args.target_thread,
                       THREAD_ALL_ACCESS, FALSE, 0)) {
    if (args.done) CloseHandle(args.done);
    return 0;
  }

  HANDLE helper = CreateThread(NULL, 0, arm_helper, &args, 0, NULL);
  if (!helper) {
    CloseHandle(args.target_thread);
    CloseHandle(args.done);
    return 0;
  }
  WaitForSingleObject(args.done, INFINITE);
  CloseHandle(helper);
  CloseHandle(args.target_thread);
  CloseHandle(args.done);
  return args.ok;
}

/* ============================================================
 * Page-level write trap via VirtualProtect (fallback when DR0
 * isn't honored by the kernel session).
 *
 * Marks the 4KB page containing `addr` PAGE_READONLY.  Every
 * write to ANY address on that page raises an access violation.
 * The handler:
 *   - if the faulting address matches our watched slot, logs
 *     RIP and registers (and the value once we read it),
 *   - flips the page back to PAGE_READWRITE, sets the trap flag
 *     so the next instruction single-steps,
 *   - on the SINGLE_STEP, re-protects the page.
 *
 * This is much slower than DR0 (every page write traps) but
 * it works without elevated privileges. */

static void  *page_watch_addr_g = 0;   /* exact slot we care about */
static void  *page_watch_base_g = 0;   /* page-aligned base */
static int    page_watch_active_g = 0; /* page currently protected? */
static int    page_step_pending_g = 0; /* single-step armed for re-protect */

static int page_protect(void *base, DWORD prot) {
  DWORD old;
  return VirtualProtect(base, 4096, prot, &old) ? 1 : 0;
}

static LONG CALLBACK page_watch_handler(PEXCEPTION_POINTERS info) {
  EXCEPTION_RECORD *er = info->ExceptionRecord;
  CONTEXT *c = info->ContextRecord;

  if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && page_step_pending_g) {
    /* The instruction after the trap has completed.  Read the
     * slot value AFTER the write (this is what would be GC'd). */
    uint64_t post = *(volatile uint64_t*)page_watch_addr_g;
    fprintf(stderr, "[PAGE#%d]   post-write value = %016llx",
            watch_hits_g, (unsigned long long)post);
    if (watch_value_g && post == watch_value_g) {
      fprintf(stderr, "  <-- MATCH TARGET, aborting\n");
      fflush(stderr);
      abort();
    }
    fprintf(stderr, "\n");
    fflush(stderr);

    page_step_pending_g = 0;
    if (!page_protect(page_watch_base_g, PAGE_READONLY)) {
      fprintf(stderr, "[PAGE] re-protect failed\n"); fflush(stderr);
    }
    page_watch_active_g = 1;
    c->EFlags &= ~0x100;  /* clear TF */
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  if (er->NumberParameters < 2 || er->ExceptionInformation[0] != 1)
    return EXCEPTION_CONTINUE_SEARCH;  /* not a write */
  void *fault = (void*)er->ExceptionInformation[1];
  /* Match by page, then narrow to the slot. */
  if (((uintptr_t)fault & ~(uintptr_t)4095) !=
      (uintptr_t)page_watch_base_g)
    return EXCEPTION_CONTINUE_SEARCH;

  int is_slot = (fault == page_watch_addr_g);
  if (is_slot) {
    watch_hits_g++;
    fprintf(stderr,
            "[PAGE#%d] hit slot=%p rip=%016llx\n"
            "[PAGE#%d]   rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n"
            "[PAGE#%d]   rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n"
            "[PAGE#%d]    r8=%016llx  r9=%016llx r10=%016llx r11=%016llx\n"
            "[PAGE#%d]   r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
            watch_hits_g, fault, (unsigned long long)c->Rip,
            watch_hits_g,
            (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
            (unsigned long long)c->Rcx, (unsigned long long)c->Rdx,
            watch_hits_g,
            (unsigned long long)c->Rsi, (unsigned long long)c->Rdi,
            (unsigned long long)c->Rbp, (unsigned long long)c->Rsp,
            watch_hits_g,
            (unsigned long long)c->R8,  (unsigned long long)c->R9,
            (unsigned long long)c->R10, (unsigned long long)c->R11,
            watch_hits_g,
            (unsigned long long)c->R12, (unsigned long long)c->R13,
            (unsigned long long)c->R14, (unsigned long long)c->R15);
    fflush(stderr);
  }
  /* Allow the write to complete by relaxing protection, then
   * arm TF so we re-protect on the next instruction. */
  if (!page_protect(page_watch_base_g, PAGE_READWRITE)) {
    fprintf(stderr, "[PAGE] unprotect failed\n"); fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  page_watch_active_g = 0;
  page_step_pending_g = 1;
  c->EFlags |= 0x100;  /* TF: single-step after this instruction */
  return EXCEPTION_CONTINUE_EXECUTION;
}

int ctx_set_page_watchpoint(void *addr) {
  page_watch_addr_g  = addr;
  page_watch_base_g  = (void*)((uintptr_t)addr & ~(uintptr_t)4095);
  watch_addr_g       = (uintptr_t)addr;  /* shared logging */
  watch_hits_g       = 0;

  AddVectoredExceptionHandler(1, page_watch_handler);
  if (!page_protect(page_watch_base_g, PAGE_READONLY)) {
    fprintf(stderr, "[PAGE] initial protect failed err=%lu\n",
            (unsigned long)GetLastError());
    fflush(stderr);
    return 0;
  }
  page_watch_active_g = 1;
  return 1;
}

void ctx_set_watch_target_value(uint64_t v) {
  watch_value_g = v;
}
