/*
 * qprof - a QEMU TCG plugin: between two marker functions, counts for each
 * address the instructions executed there, and plays the two caches of the
 * calculator's processor (a Cortex-M7: 16 KB for code, 2 ways; 16 KB for
 * data, 4 ways; lines of 32 bytes) to count the misses. What is in flash
 * (the code, the constants, the game) and what is in RAM are told apart: on
 * the calculator a miss in flash costs a read of the external flash chip,
 * far more than an instruction.
 *
 *   -plugin qprof.so,start=ADDR,stop=ADDR,out=FILE
 *
 * FILE: "address instructions code-misses flash-data-misses ram-data-misses"
 * for each address with something to report. Data misses are counted
 * against the instruction that reads or writes.
 * (QEMU 8.x plugin API, declared here by hand.)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t qemu_plugin_id_t;
struct qemu_plugin_tb;
struct qemu_plugin_insn;
typedef struct qemu_info_t qemu_info_t;
typedef uint32_t qemu_plugin_meminfo_t;
enum qemu_plugin_op { QEMU_PLUGIN_INLINE_ADD_U64 };
enum qemu_plugin_cb_flags { QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_CB_R_REGS, QEMU_PLUGIN_CB_RW_REGS };
enum qemu_plugin_mem_rw { QEMU_PLUGIN_MEM_R = 1, QEMU_PLUGIN_MEM_W, QEMU_PLUGIN_MEM_RW };
typedef void (*tb_trans_cb_t)(qemu_plugin_id_t id, struct qemu_plugin_tb* tb);
typedef void (*vcpu_udata_cb_t)(unsigned int vcpu_index, void* userdata);
typedef void (*vcpu_mem_cb_t)(unsigned int vcpu_index, qemu_plugin_meminfo_t info, uint64_t vaddr, void* userdata);
typedef void (*udata_cb_t)(qemu_plugin_id_t id, void* userdata);
extern void qemu_plugin_register_vcpu_tb_trans_cb(qemu_plugin_id_t id, tb_trans_cb_t cb);
extern size_t qemu_plugin_tb_n_insns(const struct qemu_plugin_tb* tb);
extern uint64_t qemu_plugin_tb_vaddr(const struct qemu_plugin_tb* tb);
extern struct qemu_plugin_insn* qemu_plugin_tb_get_insn(const struct qemu_plugin_tb* tb, size_t idx);
extern uint64_t qemu_plugin_insn_vaddr(const struct qemu_plugin_insn* insn);
extern size_t qemu_plugin_insn_size(const struct qemu_plugin_insn* insn);
extern void qemu_plugin_register_vcpu_insn_exec_inline(struct qemu_plugin_insn* insn, enum qemu_plugin_op op, void* ptr, uint64_t imm);
extern void qemu_plugin_register_vcpu_tb_exec_cb(struct qemu_plugin_tb* tb, vcpu_udata_cb_t cb, enum qemu_plugin_cb_flags flags, void* userdata);
extern void qemu_plugin_register_vcpu_mem_cb(struct qemu_plugin_insn* insn, vcpu_mem_cb_t cb, enum qemu_plugin_cb_flags flags, enum qemu_plugin_mem_rw rw, void* userdata);
extern void qemu_plugin_register_atexit_cb(qemu_plugin_id_t id, udata_cb_t cb, void* userdata);

__attribute__((visibility("default"))) int qemu_plugin_version = 1;

#define SPAN 0x40000u   /* code is below this address */
static uint64_t counts[SPAN / 2], codeMiss[SPAN / 2], flashMiss[SPAN / 2], ramMiss[SPAN / 2];
static uint64_t startAt = ~0ull, stopAt = ~0ull;
static const char* outPath = "qprof.out";
static int active;

/* a cache: sets of ways, each a line number, the least lately used last */
#define LINE 5
typedef struct Cache {
  uint32_t sets, ways;
  uint32_t line[256][4];
} Cache;
static Cache icache = {256, 2, {{0}}}, dcache = {128, 4, {{0}}};
static void cacheReset(Cache* c) { memset(c->line, 0xff, sizeof c->line); }
static int cacheTouch(Cache* c, uint32_t addr) {   /* 1: a miss */
  const uint32_t l = addr >> LINE;
  uint32_t* s = c->line[l % c->sets];
  for(uint32_t w = 0; w < c->ways; w++) {
    if(s[w] == l) {
      for(; w; w--) s[w] = s[w - 1];
      s[0] = l;
      return 0;
    }
  }
  for(uint32_t w = c->ways - 1; w; w--) s[w] = s[w - 1];
  s[0] = l;
  return 1;
}

static void dump(void) {
  FILE* f = fopen(outPath, "w");
  if(!f) return;
  for(uint32_t i = 0; i < SPAN / 2; i++)
    if(counts[i] || codeMiss[i] || flashMiss[i] || ramMiss[i])
      fprintf(f, "%x %llu %llu %llu %llu\n", i * 2, (unsigned long long)counts[i], (unsigned long long)codeMiss[i],
              (unsigned long long)flashMiss[i], (unsigned long long)ramMiss[i]);
  fclose(f);
}
static void onStart(unsigned int vcpu, void* u) {
  (void)vcpu;
  (void)u;
  memset(counts, 0, sizeof counts);
  memset(codeMiss, 0, sizeof codeMiss);
  memset(flashMiss, 0, sizeof flashMiss);
  memset(ramMiss, 0, sizeof ramMiss);
  active = 1;
}
static void onStop(unsigned int vcpu, void* u) {
  (void)vcpu;
  (void)u;
  if(active) dump();
  active = 0;
}
/* a block of code runs: its lines go through the code cache */
static void onBlock(unsigned int vcpu, void* u) {
  (void)vcpu;
  const uint64_t span = (uint64_t)(uintptr_t)u;   /* start, and length in the top bits */
  const uint32_t from = (uint32_t)(span & 0xffffff), to = from + (uint32_t)(span >> 24);
  for(uint32_t a = from & ~31u; a < to; a += 32)
    if(cacheTouch(&icache, a) && a < SPAN) codeMiss[(a < from ? from : a) / 2]++;
}
static void onMem(unsigned int vcpu, qemu_plugin_meminfo_t info, uint64_t vaddr, void* u) {
  (void)vcpu;
  (void)info;
  const uint32_t a = (uint32_t)vaddr, pc = (uint32_t)(uintptr_t)u;
  if(a >= 0xe0000000u) return;   /* the processor's own registers */
  if(!cacheTouch(&dcache, a) || pc >= SPAN) return;
  if(a < 0x20000000u || (a >= 0x60000000u && a < 0x70000000u)) flashMiss[pc / 2]++;
  else ramMiss[pc / 2]++;
}
static void onTrans(qemu_plugin_id_t id, struct qemu_plugin_tb* tb) {
  (void)id;
  const size_t n = qemu_plugin_tb_n_insns(tb);
  const uint64_t at = qemu_plugin_tb_vaddr(tb) & ~1ull;
  if(at == startAt) qemu_plugin_register_vcpu_tb_exec_cb(tb, onStart, QEMU_PLUGIN_CB_NO_REGS, NULL);
  if(at == stopAt) qemu_plugin_register_vcpu_tb_exec_cb(tb, onStop, QEMU_PLUGIN_CB_NO_REGS, NULL);
  uint64_t end = at;
  for(size_t i = 0; i < n; i++) {
    struct qemu_plugin_insn* insn = qemu_plugin_tb_get_insn(tb, i);
    const uint64_t a = qemu_plugin_insn_vaddr(insn) & ~1ull;
    end = a + qemu_plugin_insn_size(insn);
    if(a < SPAN) qemu_plugin_register_vcpu_insn_exec_inline(insn, QEMU_PLUGIN_INLINE_ADD_U64, &counts[a / 2], 1);
    qemu_plugin_register_vcpu_mem_cb(insn, onMem, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_RW, (void*)(uintptr_t)a);
  }
  if(at < 0x1000000) qemu_plugin_register_vcpu_tb_exec_cb(tb, onBlock, QEMU_PLUGIN_CB_NO_REGS, (void*)(uintptr_t)(at | (end - at) << 24));
}
static void onExit(qemu_plugin_id_t id, void* u) {
  (void)id;
  (void)u;
  if(active) dump();
}

__attribute__((visibility("default"))) int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t* info, int argc, char** argv) {
  (void)info;
  for(int i = 0; i < argc; i++) {
    if(!strncmp(argv[i], "start=", 6)) startAt = strtoull(argv[i] + 6, NULL, 16) & ~1ull;
    else if(!strncmp(argv[i], "stop=", 5)) stopAt = strtoull(argv[i] + 5, NULL, 16) & ~1ull;
    else if(!strncmp(argv[i], "out=", 4)) outPath = strdup(argv[i] + 4);
  }
  cacheReset(&icache);
  cacheReset(&dcache);
  if(startAt == ~0ull) active = 1;
  qemu_plugin_register_vcpu_tb_trans_cb(id, onTrans);
  qemu_plugin_register_atexit_cb(id, onExit, NULL);
  return 0;
}
