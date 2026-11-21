/* A QEMU TCG plugin: counts executed instructions per address between two
 * marker functions, and writes the counts to a file. (QEMU 8.x plugin API,
 * declared here by hand.) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t qemu_plugin_id_t;
struct qemu_plugin_tb;
struct qemu_plugin_insn;
typedef struct qemu_info_t qemu_info_t;
enum qemu_plugin_op { QEMU_PLUGIN_INLINE_ADD_U64 };
enum qemu_plugin_cb_flags { QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_CB_R_REGS, QEMU_PLUGIN_CB_RW_REGS };
typedef void (*tb_trans_cb_t)(qemu_plugin_id_t id, struct qemu_plugin_tb* tb);
typedef void (*vcpu_udata_cb_t)(unsigned int vcpu_index, void* userdata);
typedef void (*udata_cb_t)(qemu_plugin_id_t id, void* userdata);
extern void qemu_plugin_register_vcpu_tb_trans_cb(qemu_plugin_id_t id, tb_trans_cb_t cb);
extern size_t qemu_plugin_tb_n_insns(const struct qemu_plugin_tb* tb);
extern uint64_t qemu_plugin_tb_vaddr(const struct qemu_plugin_tb* tb);
extern struct qemu_plugin_insn* qemu_plugin_tb_get_insn(const struct qemu_plugin_tb* tb, size_t idx);
extern uint64_t qemu_plugin_insn_vaddr(const struct qemu_plugin_insn* insn);
extern void qemu_plugin_register_vcpu_insn_exec_inline(struct qemu_plugin_insn* insn, enum qemu_plugin_op op, void* ptr, uint64_t imm);
extern void qemu_plugin_register_vcpu_tb_exec_cb(struct qemu_plugin_tb* tb, vcpu_udata_cb_t cb, enum qemu_plugin_cb_flags flags, void* userdata);
extern void qemu_plugin_register_atexit_cb(qemu_plugin_id_t id, udata_cb_t cb, void* userdata);

__attribute__((visibility("default"))) int qemu_plugin_version = 1;

#define SPAN 0x40000u
static uint64_t counts[SPAN / 2];
static uint64_t startAt = ~0ull, stopAt = ~0ull;
static const char* outPath = "qprof.out";
static int active;

static void dump(void) {
  FILE* f = fopen(outPath, "w");
  if(!f) return;
  for(uint32_t i = 0; i < SPAN / 2; i++)
    if(counts[i]) fprintf(f, "%x %llu\n", i * 2, (unsigned long long)counts[i]);
  fclose(f);
}
static void onStart(unsigned int vcpu, void* u) {
  (void)vcpu; (void)u;
  memset(counts, 0, sizeof counts);
  active = 1;
}
static void onStop(unsigned int vcpu, void* u) {
  (void)vcpu; (void)u;
  if(active) dump();
  active = 0;
}
static void onTrans(qemu_plugin_id_t id, struct qemu_plugin_tb* tb) {
  (void)id;
  const size_t n = qemu_plugin_tb_n_insns(tb);
  const uint64_t at = qemu_plugin_tb_vaddr(tb) & ~1ull;
  if(at == startAt) qemu_plugin_register_vcpu_tb_exec_cb(tb, onStart, QEMU_PLUGIN_CB_NO_REGS, NULL);
  if(at == stopAt) qemu_plugin_register_vcpu_tb_exec_cb(tb, onStop, QEMU_PLUGIN_CB_NO_REGS, NULL);
  for(size_t i = 0; i < n; i++) {
    struct qemu_plugin_insn* insn = qemu_plugin_tb_get_insn(tb, i);
    const uint64_t a = qemu_plugin_insn_vaddr(insn) & ~1ull;
    if(a < SPAN) qemu_plugin_register_vcpu_insn_exec_inline(insn, QEMU_PLUGIN_INLINE_ADD_U64, &counts[a / 2], 1);
  }
}
static void onExit(qemu_plugin_id_t id, void* u) {
  (void)id; (void)u;
  if(active) dump();
}

__attribute__((visibility("default"))) int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t* info, int argc, char** argv) {
  (void)info;
  for(int i = 0; i < argc; i++) {
    if(!strncmp(argv[i], "start=", 6)) startAt = strtoull(argv[i] + 6, NULL, 16) & ~1ull;
    else if(!strncmp(argv[i], "stop=", 5)) stopAt = strtoull(argv[i] + 5, NULL, 16) & ~1ull;
    else if(!strncmp(argv[i], "out=", 4)) outPath = strdup(argv[i] + 4);
  }
  if(startAt == ~0ull) active = 1;
  qemu_plugin_register_vcpu_tb_trans_cb(id, onTrans);
  qemu_plugin_register_atexit_cb(id, onExit, NULL);
  return 0;
}
