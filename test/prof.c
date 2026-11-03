/* Poor-man's function profiler for on-target hotspot ranking.
 *
 * Compiled into the speex library when CMake option SPEEX_PROF=ON, which
 * also adds -finstrument-functions to the library sources. GCC then calls
 * __cyg_profile_func_enter/exit around every (non-inlined) function,
 * including static ones, so multi-return functions are handled without
 * any source edits.
 *
 * At process exit a "PROF <addr> <nanoseconds> <calls>" line is printed
 * per function to stderr; symbolize on the host with
 *   nm <binary> | grep <addr>
 *
 * Inclusive time (nested calls count in the parent too) — read rankings
 * accordingly. Single-threaded use only.
 */
#include <stdio.h>
#include <time.h>

#define MAX_ENTRIES 512
#define MAX_DEPTH 96

typedef struct {
   void *fn;
   unsigned long long ns;
   unsigned long cnt;
} prof_entry_t;

/* Runtime gate: the harness can stop/start attribution between top-level
 * library calls (e.g. bench_e2e keeps the one untimed encode of its dec
 * phase out of the data). Flip it ONLY between library calls. */
int speex_prof_enabled = 1;

static prof_entry_t entries[MAX_ENTRIES];
static int n_entries = 0;
static struct { void *fn; unsigned long long t0; } prof_stack[MAX_DEPTH];
static int prof_depth = 0;

__attribute__((no_instrument_function))
static unsigned long long prof_now(void)
{
#if defined(__riscv)
   /* rdtime: user-mode CSR read, a few cycles — clock_gettime would be a
    * syscall (~2us on this board) and swamp the data. Ranking uses raw
    * ticks; the tick rate is constant within a run. */
   unsigned long v;
   __asm__ volatile("rdtime %0" : "=r"(v));
   return (unsigned long long)v;
#else
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
#endif
}

__attribute__((no_instrument_function))
void __cyg_profile_func_enter(void *fn, void *callsite)
{
   (void)callsite;
   if (!speex_prof_enabled)
      return;
   if (prof_depth < MAX_DEPTH)
   {
      prof_stack[prof_depth].fn = fn;
      prof_stack[prof_depth].t0 = prof_now();
   }
   prof_depth++;
}

__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void *fn, void *callsite)
{
   unsigned long long t = prof_now();
   int i;
   (void)callsite;
   if (!speex_prof_enabled)
      return;
   prof_depth--;
   if (prof_depth >= MAX_DEPTH)
      return;
   if (prof_stack[prof_depth].fn != fn)
      return; /* unbalanced (should not happen single-threaded) */
   for (i = 0; i < n_entries; i++)
      if (entries[i].fn == fn)
         break;
   if (i == n_entries && n_entries < MAX_ENTRIES)
   {
      entries[n_entries].fn = fn;
      entries[n_entries].ns = 0;
      entries[n_entries].cnt = 0;
      n_entries++;
   }
   if (i < n_entries)
   {
      entries[i].ns += t - prof_stack[prof_depth].t0;
      entries[i].cnt++;
   }
}

__attribute__((destructor))
__attribute__((no_instrument_function))
static void prof_report(void)
{
   int i;
   for (i = 0; i < n_entries; i++)
      fprintf(stderr, "PROF %p %llu %lu\n", entries[i].fn,
              (unsigned long long)entries[i].ns, (unsigned long)entries[i].cnt);
}
