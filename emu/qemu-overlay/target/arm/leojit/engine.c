/*
 * leojit engine: block cache, execution loop, code invalidation, and the
 * verification mode against TCG.
 *
 * LEO_JIT=1       run guest code on leojit where it can (TCG elsewhere)
 * LEO_JIT=verify  run each leojit block, undo it, run the same insns on
 *                 TCG and compare registers and memory writes
 * LEO_JIT_STATS=1 log coverage every 10 s
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "internals.h"
#include "exec/cputlb.h"
#include "exec/target_page.h"
#include "accel/tcg/internal-common.h"
#include "exec/translation-block.h"
#include "accel/tcg/cpu-ops.h"
#include "hw/core/cpu.h"
#include "leojit.h"
#include "lir.h"
#include "leojit-backend.h"
#include <signal.h>
#include <dlfcn.h>
#ifdef __aarch64__
#include <ucontext.h>
#endif

#define HBITS       16
#define MAX_INSNS   64

static int leo_mode = -1;               /* 0 off, 1 on, 2 verify */
static QemuMutex leo_lock;
static LBlock *htab[1 << HBITS];
static GHashTable *pages;               /* ram page -> first LBlock (pnext) */
static LBlock *graveyard;               /* dead blocks, freed by the vCPU */
static uint64_t st_blocks, st_insns, st_fallback, st_translated, st_inval;
static uint64_t st_vok, st_vbad, st_vskip, st_vlost, st_compiled, st_nojit, st_interp;
static uint32_t st_lastpc, st_lastexit, st_ring[16], st_ringi;
static uint64_t st_irqexit;

/*
 * A host crash with leojit on: say where (our code or QEMU's, which
 * guest block), then let the default handler finish the process.
 */
static struct sigaction old_segv, old_bus, old_ill;

static LBlock *block_at(uintptr_t pc)
{
    for (int h = 0; h < (1 << HBITS); h++) {
        for (LBlock *b = htab[h]; b; b = b->hnext) {
            if (b->code && pc >= (uintptr_t)b->code && pc < (uintptr_t)b->code_end) {
                return b;
            }
        }
    }
    for (LBlock *b = graveyard; b; b = b->hnext) {
        if (b->code && pc >= (uintptr_t)b->code && pc < (uintptr_t)b->code_end) {
            return b;
        }
    }
    return NULL;
}

static void where(const char *what, uintptr_t pc)
{
    Dl_info di;

    if (leo_backend_owns(pc)) {
        LBlock *b = block_at(pc);
        if (b) {
            fprintf(stderr, "  %s %#lx: JIT block %08x+%#lx (%s%s, %u insns)\n", what,
                    (unsigned long)pc, (uint32_t)b->pc,
                    (unsigned long)(pc - (uintptr_t)b->code),
                    FIELD_EX32(b->flags, TBFLAG_AM32, THUMB) ? "T32" : "A32",
                    b->dead ? ", dead" : "", b->ninsn);
        } else {
            fprintf(stderr, "  %s %#lx: JIT buffer (trampoline or reused)\n", what,
                    (unsigned long)pc);
        }
    } else if (dladdr((void *)pc, &di) && di.dli_fbase) {
        fprintf(stderr, "  %s %#lx: %s+%#lx %s\n", what, (unsigned long)pc,
                di.dli_fname ? strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1
                                                         : di.dli_fname : "?",
                (unsigned long)(pc - (uintptr_t)di.dli_fbase),
                di.dli_sname ? di.dli_sname : "");
    } else {
        fprintf(stderr, "  %s %#lx\n", what, (unsigned long)pc);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *uc_)
{
    struct sigaction *old = sig == SIGSEGV ? &old_segv : sig == SIGBUS ? &old_bus : &old_ill;
    CPUState *cs = current_cpu;

    fprintf(stderr, "leojit: CRASH signal %d code %d at address %p\n",
            sig, si->si_code, si->si_addr);
#ifdef __aarch64__
    {
        ucontext_t *uc = uc_;
        mcontext_t *m = &uc->uc_mcontext;
        uintptr_t fp = m->regs[29], sp = m->sp;

        where("pc", m->pc);
        where("lr", m->regs[30]);
        if (getenv("LEO_JIT_CRASHDUMP")) {
            LBlock *b = block_at(m->regs[30]);
            if (!b) {
                b = block_at(m->pc);
            }
            if (b) {
                fprintf(stderr, "  code %p:", b->code);
                for (uint32_t *p = b->code; p < (uint32_t *)b->code_end; p++) {
                    fprintf(stderr, " %08x", *p);
                }
                fprintf(stderr, "\n  ir:");
                for (uint32_t i = 0; i < b->nops; i++) {
                    const LOp *o = &b->ops[i];
                    fprintf(stderr, " [%u %u %u %u %u %x]", o->op, o->d, o->a, o->b, o->c, o->imm);
                }
                fprintf(stderr, "\n");
            }
        }
        for (int i = 0; i < 31; i += 4) {
            fprintf(stderr, "  x%-2d %016llx %016llx %016llx %016llx\n", i,
                    (unsigned long long)m->regs[i],
                    (unsigned long long)(i + 1 < 31 ? m->regs[i + 1] : sp),
                    (unsigned long long)(i + 2 < 31 ? m->regs[i + 2] : 0),
                    (unsigned long long)(i + 3 < 31 ? m->regs[i + 3] : 0));
        }
        /* frame records of the C code around (x29 may hold a guest value) */
        for (int i = 0; i < 12 && fp > sp && fp - sp < (1 << 20) && !(fp & 7); i++) {
            uintptr_t *fr = (uintptr_t *)fp;
            where("frame", fr[1]);
            if (fr[0] <= fp) {
                break;
            }
            fp = fr[0];
        }
    }
#endif
    if (cs) {
        CPUARMState *env = cpu_env(cs);
        fprintf(stderr, "  guest pc %08x cpsr %08x thumb %d, last block %08x, last exit %08x\n",
                env->regs[15], cpsr_read(env), env->thumb, st_lastpc, st_lastexit);
        for (int i = 0; i < 16; i += 4) {
            fprintf(stderr, "  r%-2d %08x %08x %08x %08x\n", i, env->regs[i],
                    env->regs[i + 1], env->regs[i + 2], env->regs[i + 3]);
        }
    }
    fprintf(stderr, "  recent:");
    for (int i = 0; i < 16; i++) {
        fprintf(stderr, " %08x", st_ring[(st_ringi + i) & 15]);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    sigaction(sig, old, NULL);          /* re-fault into the previous handler */
}

static void install_crash_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    sigaction(SIGILL, &sa, &old_ill);
}

static void leo_init(void)
{
    const char *e = getenv("LEO_JIT");

    leo_mode = !e ? 0 : !strcmp(e, "verify") ? 2 : !strcmp(e, "jitcheck") ? 3 :
               atoi(e) ? 1 : 0;
    if (leo_mode) {
        qemu_mutex_init(&leo_lock);
        pages = g_hash_table_new(g_direct_hash, g_direct_equal);
        info_report("leojit: %s", leo_mode == 2 ? "verifying against TCG" : "on");
        install_crash_handler();
    }
}

bool leo_jit_enabled(void)
{
    if (unlikely(leo_mode < 0)) {
        leo_init();
    }
    return leo_mode > 0;
}

static unsigned hash_pc(vaddr pc, uint32_t flags)
{
    uint32_t h = (uint32_t)pc ^ (flags * 0x9e3779b9u);
    return (h ^ (h >> HBITS)) & ((1 << HBITS) - 1);
}

static int backend = -1;                /* host code generator available */

LJmp leo_jc[1 << LEO_JC_BITS] QEMU_ALIGNED(32);

void leo_jc_clear(void)
{
    for (int i = 0; i < (1 << LEO_JC_BITS); i++) {
        qatomic_set(&leo_jc[i].key, 0);
        qatomic_set(&leo_jc[i].code, NULL);
    }
}

/* QEMU's own virtual-PC jump cache was dropped: drop ours the same way */
void leo_jit_flush_jc(void)
{
    if (leo_mode > 0) {
        leo_jc_clear();
    }
}

void leo_jit_flush_jc_page(vaddr page)
{
    unsigned i0 = (page >> 1) & ((1 << LEO_JC_BITS) - 1) & ~2047u;

    if (leo_mode <= 0) {
        return;
    }
    for (unsigned i = 0; i < 2048; i++) {
        LJmp *j = &leo_jc[(i0 + i) & ((1 << LEO_JC_BITS) - 1)];
        qatomic_set(&j->key, 0);
        qatomic_set(&j->code, NULL);
    }
}

static void jc_insert(LBlock *b)
{
    static int nojc = -1;
    if (nojc < 0) {
        nojc = getenv("LEO_JIT_NOJC") != NULL;
    }
    if (nojc) {
        return;
    }
    uint32_t key = (uint32_t)b->pc | FIELD_EX32(b->flags, TBFLAG_AM32, THUMB);
    LJmp *j = &leo_jc[(key >> 1) & ((1 << LEO_JC_BITS) - 1)];

    if (j->key == key && j->code == b->code) {
        return;
    }
    qatomic_set(&j->key, 0);
    smp_wmb();
    j->flags = b->flags & LEO_FLAGS_MASK;
    j->flags2 = b->cs_base;
    qatomic_set(&j->code, b->code);
    smp_mb();
    /*
     * Another thread may have killed b (and cleared the cache) since the
     * lookup: publish only a live block, then check again.
     */
    if (qatomic_read(&b->dead)) {
        qatomic_set(&j->code, NULL);
        return;
    }
    qatomic_set(&j->key, key);
    smp_mb();
    if (qatomic_read(&b->dead)) {
        qatomic_set(&j->key, 0);
        qatomic_set(&j->code, NULL);
    }
}

static void free_block(LBlock *b)
{
    g_free(b->ops);
    g_free(b->exits);
    if (b->incoming) {
        g_array_free(b->incoming, true);
    }
    g_free(b);
}

/* undo every jump patched into b's code (leo_lock held) */
static void unlink_incoming(LBlock *b)
{
    if (b->incoming) {
        for (guint i = 0; i < b->incoming->len; i++) {
            LIncoming *in = &g_array_index(b->incoming, LIncoming, i);
            leo_backend_patch(in->site, NULL, in->orig);
        }
        g_array_set_size(b->incoming, 0);
    }
}

/* unlink a block (leo_lock held); freed later by the vCPU thread */
static void kill_block(LBlock *b)
{
    LBlock **pp = &htab[hash_pc(b->pc, b->flags)];

    while (*pp && *pp != b) {
        pp = &(*pp)->hnext;
    }
    if (*pp) {
        *pp = b->hnext;
    }
    qatomic_set(&b->dead, true);
    smp_mb();
    unlink_incoming(b);
    if (b->code) {
        leo_jc_clear();
    }
    b->hnext = graveyard;
    graveyard = b;
    st_inval++;
}

/*
 * A write hit guest RAM [start, last]: forget every block on those pages.
 * Returns true when leojit had code there (the caller may then drop the
 * page's write protection if TCG has none either).
 */
bool leo_jit_invalidate(ram_addr_t start, ram_addr_t last)
{
    bool had = false;

    if (leo_mode <= 0) {
        return false;
    }
    qemu_mutex_lock(&leo_lock);
    for (ram_addr_t p = start & TARGET_PAGE_MASK; p <= last; p += TARGET_PAGE_SIZE) {
        gpointer key = (gpointer)(uintptr_t)(p >> TARGET_PAGE_BITS);
        LBlock *b = g_hash_table_lookup(pages, key);
        if (!b && !g_hash_table_contains(pages, key)) {
            continue;
        }
        had = true;
        g_hash_table_remove(pages, key);
        while (b) {
            LBlock *n = b->pnext;
            kill_block(b);
            b = n;
        }
    }
    qemu_mutex_unlock(&leo_lock);
    return had;
}

/* QEMU dropped all its code (e.g. a snapshot was loaded): drop ours too */
void leo_jit_flush(void)
{
    if (leo_mode <= 0) {
        return;
    }
    qemu_mutex_lock(&leo_lock);
    for (int h = 0; h < (1 << HBITS); h++) {
        while (htab[h]) {
            kill_block(htab[h]);
        }
    }
    g_hash_table_remove_all(pages);
    leo_jc_clear();
    qemu_mutex_unlock(&leo_lock);
}

static LBlock *lookup(CPUARMState *env, vaddr pc, uint64_t cs_base,
                      uint32_t flags)
{
    LBlock *b;

    qemu_mutex_lock(&leo_lock);
    for (b = htab[hash_pc(pc, flags)]; b; b = b->hnext) {
        if (b->pc == pc && b->flags == flags && b->cs_base == cs_base) {
            break;
        }
    }
    qemu_mutex_unlock(&leo_lock);
    if (b) {
        /* the virtual page may map elsewhere now */
        if (get_page_addr_code(env, pc) != (tb_page_addr_t)b->page0) {
            return NULL;
        }
    }
    return b;
}

static void insert(LBlock *b)
{
    gpointer key = (gpointer)(uintptr_t)(b->page0 >> TARGET_PAGE_BITS);
    unsigned h = hash_pc(b->pc, b->flags);

    qemu_mutex_lock(&leo_lock);
    b->hnext = htab[h];
    htab[h] = b;
    b->pnext = g_hash_table_lookup(pages, key);
    g_hash_table_insert(pages, key, b);
    qemu_mutex_unlock(&leo_lock);
    /* stores to this page must now take the slow path that invalidates */
    tlb_protect_code(b->page0 & TARGET_PAGE_MASK);
}

static void free_graveyard(void)
{
    LBlock *g;

    qemu_mutex_lock(&leo_lock);
    g = graveyard;
    graveyard = NULL;
    qemu_mutex_unlock(&leo_lock);
    while (g) {
        LBlock *n = g->hnext;
        free_block(g);
        g = n;
    }
}

static void stats(void)
{
    static int64_t last;
    int64_t now = get_clock_realtime();

    if (!getenv("LEO_JIT_STATS")) {
        return;
    }
    if (!last) {
        last = now;
    } else if (now - last > 10 * NANOSECONDS_PER_SECOND) {
        last = now;
        info_report("leojit: %" PRIu64 " blocks (%" PRIu64 " insns) run, %" PRIu64
                    " TCG fallbacks, %" PRIu64 " translated, %" PRIu64 " invalidated"
                    "; verify ok %" PRIu64 " bad %" PRIu64 " skipped %" PRIu64
                    " lost %" PRIu64,
                    st_blocks, st_insns, st_fallback, st_translated, st_inval,
                    st_vok, st_vbad, st_vskip, st_vlost);
        info_report("leojit: compiled %" PRIu64 ", not compilable %" PRIu64
                    ", interpreted runs %" PRIu64, st_compiled, st_nojit, st_interp);
        st_interp = 0;
        info_report("leojit: last block %08x, last exit record pc %08x, %" PRIu64
                    " exits with no record; recent: %08x %08x %08x %08x %08x %08x %08x %08x"
                    " %08x %08x %08x %08x %08x %08x %08x %08x", st_lastpc, st_lastexit,
                    st_irqexit, st_ring[(st_ringi + 0) & 15], st_ring[(st_ringi + 1) & 15],
                    st_ring[(st_ringi + 2) & 15], st_ring[(st_ringi + 3) & 15],
                    st_ring[(st_ringi + 4) & 15], st_ring[(st_ringi + 5) & 15],
                    st_ring[(st_ringi + 6) & 15], st_ring[(st_ringi + 7) & 15],
                    st_ring[(st_ringi + 8) & 15], st_ring[(st_ringi + 9) & 15],
                    st_ring[(st_ringi + 10) & 15], st_ring[(st_ringi + 11) & 15],
                    st_ring[(st_ringi + 12) & 15], st_ring[(st_ringi + 13) & 15],
                    st_ring[(st_ringi + 14) & 15], st_ring[(st_ringi + 15) & 15]);
        st_irqexit = 0;
        st_blocks = st_insns = st_fallback = st_translated = st_inval = 0;
    }
}

/* ---------------- verification against TCG ---------------- */

typedef struct VState {
    uint32_t r[16], nf, zf, cf, vf, thumb, condexec;
    uint64_t excl_addr, excl_val;
} VState;

static struct {
    bool on;
    vaddr pc;
    bool thumb;
    uint32_t ninsn;
    VState ours;
    int nw;
    LeoWrite w[LEO_WLOG_MAX];
    uint8_t code[256];
    int ncode;
} vp;
static CPUARMState vsave;

static void vcapture(CPUARMState *env, VState *v)
{
    memcpy(v->r, env->regs, sizeof(v->r));
    v->nf = env->NF;
    v->zf = env->ZF;
    v->cf = env->CF;
    v->vf = env->VF;
    v->thumb = env->thumb;
    v->condexec = env->condexec_bits;
    v->excl_addr = env->exclusive_addr;
    v->excl_val = env->exclusive_val;
}

static void vreport(CPUARMState *env, const VState *t, const char *why)
{
    GString *g = g_string_new("");

    g_string_append_printf(g, "leojit: MISMATCH %s at %s block %08x (%u insns):",
                           why, vp.thumb ? "T32" : "A32", (uint32_t)vp.pc, vp.ninsn);
    for (int i = 0; i < vp.ncode; i += vp.thumb ? 2 : 4) {
        g_string_append_printf(g, vp.thumb ? " %04x" : " %08x",
                               vp.thumb ? lduw_le_p(vp.code + i) : ldl_le_p(vp.code + i));
    }
    for (int i = 0; i < 16; i++) {
        if (vp.ours.r[i] != t->r[i]) {
            g_string_append_printf(g, "\n   r%d ours %08x tcg %08x", i, vp.ours.r[i], t->r[i]);
        }
    }
    g_string_append_printf(g, "\n   nzcv ours %d%d%d%d tcg %d%d%d%d thumb %u/%u cond %x/%x",
                           vp.ours.nf >> 31, vp.ours.zf == 0, vp.ours.cf, vp.ours.vf >> 31,
                           t->nf >> 31, t->zf == 0, t->cf, t->vf >> 31,
                           vp.ours.thumb, t->thumb, vp.ours.condexec, t->condexec);
    error_report("%s", g->str);
    g_string_free(g, true);
}

/* called by cpu_exec right after the TB that replayed a verified block */
void leo_jit_after_tb(CPUState *cs)
{
    CPUARMState *env = cpu_env(cs);
    VState t;
    const char *why = NULL;

    if (!vp.on) {
        return;
    }
    vp.on = false;
    vcapture(env, &t);
    if (t.r[15] == (uint32_t)vp.pc && !memcmp(t.r, vsave.regs, sizeof(t.r)) &&
        t.nf == vsave.NF && t.zf == vsave.ZF && t.cf == vsave.CF && t.vf == vsave.VF) {
        st_vlost++;                     /* the TB exited before running (IRQ) */
        return;
    }
    if (memcmp(vp.ours.r, t.r, sizeof(t.r))) {
        why = "registers";
    } else if ((vp.ours.nf ^ t.nf) >> 31 || (vp.ours.zf == 0) != (t.zf == 0) ||
               vp.ours.cf != t.cf || (vp.ours.vf ^ t.vf) >> 31) {
        why = "flags";
    } else if (vp.ours.thumb != t.thumb || vp.ours.condexec != t.condexec) {
        why = "thumb/IT state";
    } else if (vp.ours.excl_addr != t.excl_addr ||
               (vp.ours.excl_addr != -1 && vp.ours.excl_val != t.excl_val)) {
        why = "exclusive monitor";
    } else {
        for (int i = 0; i < vp.nw && !why; i++) {
            if (leo_mem_ld(env, vp.w[i].addr, vp.w[i].oi) != vp.w[i].new) {
                /* a later write in the block may cover an earlier one */
                bool later = false;
                uint32_t a0 = vp.w[i].addr;
                uint32_t a1 = a0 + (1 << (get_memop(vp.w[i].oi) & MO_SIZE));
                for (int j = i + 1; j < vp.nw; j++) {
                    uint32_t b0 = vp.w[j].addr;
                    uint32_t b1 = b0 + (1 << (get_memop(vp.w[j].oi) & MO_SIZE));
                    later |= b0 < a1 && a0 < b1;
                }
                if (!later) {
                    static char buf[96];
                    snprintf(buf, sizeof(buf), "memory [%08x]/%d ours %08x now %08x was %08x",
                             a0, (int)(a1 - a0), vp.w[i].new,
                             leo_mem_ld(env, vp.w[i].addr, vp.w[i].oi), vp.w[i].old);
                    why = buf;
                }
            }
        }
    }
    if (why) {
        static int reported;
        st_vbad++;
        if (reported++ < 60) {
            vreport(env, &t, why);
        }
    } else {
        st_vok++;
    }
}

/* run one block, undo it and have TCG replay the same instructions */
static bool verify_one(CPUState *cs, TCGTBCPUState *s, LBlock *b)
{
    CPUARMState *env = cpu_env(cs);

    static uint32_t tick;

    if (b->impure || (b->nverified >= 8 && (tick++ & 63))) {
        if (b->impure) {
            st_vskip++;
        }
        leo_interp(env, b);
        return true;
    }
    b->nverified++;
    memcpy(&vsave, env, sizeof(vsave));
    leo_nwlog = 0;
    leo_mmio_seen = false;
    leo_verify_log = true;
    leo_interp(env, b);                 /* a fault longjmps out from here */
    leo_verify_log = false;
    if (leo_mmio_seen) {
        st_vskip++;
        return true;                    /* device side effects: keep ours */
    }
    vcapture(env, &vp.ours);
    vp.nw = leo_nwlog;
    memcpy(vp.w, leo_wlog, sizeof(LeoWrite) * leo_nwlog);
    vp.pc = b->pc;
    vp.thumb = env->thumb;
    vp.thumb = vsave.thumb;
    vp.ninsn = b->ninsn;
    {
        void *host = NULL;
        get_page_addr_code_hostp(env, b->pc, &host);
        vp.ncode = 0;
        if (host) {
            vp.ncode = MIN(256, (int)(TARGET_PAGE_SIZE - (b->pc & ~TARGET_PAGE_MASK)));
            vp.ncode = MIN(vp.ncode, (int)b->ninsn * 4);
            memcpy(vp.code, host, vp.ncode);
        }
    }
    for (int i = leo_nwlog - 1; i >= 0; i--) {
        leo_mem_st(env, leo_wlog[i].addr, leo_wlog[i].old, leo_wlog[i].oi);
    }
    memcpy(env, &vsave, sizeof(vsave));
    vp.on = true;
    s->cflags = (s->cflags & ~CF_COUNT_MASK) | b->ninsn | CF_NO_GOTO_TB | CF_NO_GOTO_PTR;
    return false;
}

/* the code buffer is full: drop all generated code (blocks stay) */
static void flush_code(void)
{
    qemu_mutex_lock(&leo_lock);
    for (int h = 0; h < (1 << HBITS); h++) {
        for (LBlock *b = htab[h]; b; b = b->hnext) {
            b->code = NULL;
            b->nojit = false;
            g_free(b->exits);
            b->exits = NULL;
            b->nexits = 0;
            if (b->incoming) {
                g_array_set_size(b->incoming, 0);
            }
        }
    }
    leo_jc_clear();
    leo_backend_reset();
    qemu_mutex_unlock(&leo_lock);
}

/* a chainable exit was taken: patch it to jump to the next block directly */
static void try_chain(CPUState *cs, CPUARMState *env, LExitRec *x)
{
    LBlock *from = x->from, *t;
    TCGTBCPUState s;

    static int nochain = -1;
    if (nochain < 0) {
        nochain = getenv("LEO_JIT_NOCHAIN") != NULL;
    }
    /* after CPSR writes and the like, QEMU must look at pending IRQs first */
    if (nochain || from->impure || !x->chain || from->dead || env->regs[15] != x->pc) {
        return;
    }
    s = cs->cc->tcg_ops->get_tb_cpu_state(cs);
    if (s.flags != from->flags || s.cs_base != from->cs_base ||
        (s.pc ^ from->pc) & TARGET_PAGE_MASK) {
        return;
    }
    t = lookup(env, s.pc, s.cs_base, s.flags);
    if (!t || !t->code || t->page0 != from->page0) {
        return;
    }
    qemu_mutex_lock(&leo_lock);
    if (!t->dead && !from->dead && t->code) {
        LIncoming in = { x->site, x->orig };
        if (!t->incoming) {
            t->incoming = g_array_new(false, false, sizeof(LIncoming));
        }
        g_array_append_val(t->incoming, in);
        leo_backend_patch(x->site, t->code, x->orig);
    }
    qemu_mutex_unlock(&leo_lock);
}

/*
 * LEO_JIT=jitcheck: run each block on the interpreter (checked against
 * TCG), undo it, run the generated code alone and compare.
 */
static bool jitcheck_one(CPUState *cs, LBlock *b)
{
    CPUARMState *env = cpu_env(cs);
    VState ref, got;
    static int reported;

    if (backend < 0) {
        backend = leo_backend_init(env);
    }
    if (!backend || b->impure) {
        leo_interp(env, b);
        return true;
    }
    if (!b->code && !b->nojit) {
        if (leo_backend_full()) {
            flush_code();
        }
        b->code = leo_backend_compile(b);
        b->code_end = leo_backend_ptr();
        b->nojit = !b->code;
    }
    if (!b->code) {
        leo_interp(env, b);
        return true;
    }
    memcpy(&vsave, env, sizeof(vsave));
    leo_nwlog = 0;
    leo_mmio_seen = false;
    leo_verify_log = true;
    leo_interp(env, b);
    leo_verify_log = false;
    if (leo_mmio_seen) {
        return true;
    }
    vcapture(env, &ref);
    int nw = leo_nwlog;
    LeoWrite w[LEO_WLOG_MAX];
    memcpy(w, leo_wlog, sizeof(LeoWrite) * nw);
    for (int i = nw - 1; i >= 0; i--) {
        leo_mem_st(env, w[i].addr, w[i].old, w[i].oi);
    }
    memcpy(env, &vsave, sizeof(vsave));
    leo_backend_run(env, b->code);
    vcapture(env, &got);
    const char *why = NULL;
    if (got.r[15] == (uint32_t)b->pc && !memcmp(got.r, vsave.regs, sizeof(got.r))) {
        return true;                    /* exit request at entry: nothing ran */
    }
    if (memcmp(ref.r, got.r, sizeof(ref.r))) {
        why = "registers";
    } else if ((ref.nf ^ got.nf) >> 31 || (ref.zf == 0) != (got.zf == 0) ||
               ref.cf != got.cf || (ref.vf ^ got.vf) >> 31) {
        why = "flags";
    } else if (ref.thumb != got.thumb || ref.condexec != got.condexec) {
        why = "thumb/IT";
    }
    for (int i = 0; i < nw && !why; i++) {
        bool later = false;
        for (int j = i + 1; j < nw; j++) {
            later |= w[j].addr == w[i].addr;
        }
        if (!later && leo_mem_ld(env, w[i].addr, w[i].oi) != w[i].new) {
            static char buf[96];
            snprintf(buf, sizeof(buf), "memory [%08x] interp %08x jit %08x was %08x",
                     w[i].addr, w[i].new, leo_mem_ld(env, w[i].addr, w[i].oi), w[i].old);
            why = buf;
        }
    }
    if (why && reported++ < 40) {
        GString *g = g_string_new("");
        g_string_append_printf(g, "leojit: JIT MISMATCH %s at %s block %08x (%u insns, %u ops)",
                               why, vsave.thumb ? "T32" : "A32", (uint32_t)b->pc,
                               b->ninsn, b->nops);
        for (int i = 0; i < 16; i++) {
            if (ref.r[i] != got.r[i]) {
                g_string_append_printf(g, "\n   r%d interp %08x jit %08x", i, ref.r[i], got.r[i]);
            }
        }
        g_string_append_printf(g, "\n   nzcv interp %d%d%d%d jit %d%d%d%d\n   code:",
                               ref.nf >> 31, ref.zf == 0, ref.cf, ref.vf >> 31,
                               got.nf >> 31, got.zf == 0, got.cf, got.vf >> 31);
        for (uint32_t *p = b->code; p < (uint32_t *)b->code + 160 && p; p++) {
            g_string_append_printf(g, "%08x", *p);
        }
        g_string_append(g, "\n   ir:");
        for (uint32_t i = 0; i < b->nops; i++) {
            const LOp *o = &b->ops[i];
            g_string_append_printf(g, " [%u %u %u %u %u %x]", o->op, o->d, o->a, o->b, o->c, o->imm);
        }
        error_report("%s", g->str);
        g_string_free(g, true);
    }
    if (why) {
        st_vbad++;
    } else {
        st_vok++;
    }
    return true;
}

/*
 * Run guest code from the current state until something needs the main
 * loop.  Returns false when nothing ran: TCG must execute the next TB.
 */
bool leo_jit_exec(CPUState *cs, TCGTBCPUState *first)
{
    CPUARMState *env = cpu_env(cs);
    bool ran = false;

    if (graveyard) {
        free_graveyard();
    }
    stats();
    if (leo_mode == 3) {
        LBlock *b = lookup(env, first->pc, first->cs_base, first->flags);
        if (!b) {
            b = leo_translate(env, first->pc, first->cs_base, first->flags, MAX_INSNS);
            if (!b) {
                st_fallback++;
                return false;
            }
            insert(b);
        }
        st_blocks++;
        return jitcheck_one(cs, b);
    }
    if (leo_mode == 2) {
        if (vp.on || leo_verify_log) {
            st_vlost++;                 /* an exception cut the last one short */
            vp.on = false;
            leo_verify_log = false;
        }
        LBlock *b = lookup(env, first->pc, first->cs_base, first->flags);
        if (!b) {
            b = leo_translate(env, first->pc, first->cs_base, first->flags, MAX_INSNS);
            if (!b) {
                st_fallback++;
                return false;
            }
            st_translated++;
            insert(b);
        }
        st_blocks++;
        st_insns += b->ninsn;
        return verify_one(cs, first, b);
    }
    for (int n = 0; n < 4096; n++) {
        TCGTBCPUState s = n ? cs->cc->tcg_ops->get_tb_cpu_state(cs) : *first;
        LBlock *b = lookup(env, s.pc, s.cs_base, s.flags);

        if (!b) {
            b = leo_translate(env, s.pc, s.cs_base, s.flags, MAX_INSNS);
            if (!b) {
                st_fallback++;
                break;
            }
            st_translated++;
            insert(b);
        }
        if (backend < 0) {
            backend = !getenv("LEO_JIT_INTERP") && leo_backend_init(env);
        }
        if (backend && !b->code && !b->nojit) {
            if (leo_backend_full()) {
                flush_code();
            }
            b->code = leo_backend_compile(b);
            b->code_end = leo_backend_ptr();
            b->nojit = !b->code;
            if (b->code) {
                st_compiled++;
            } else {
                st_nojit++;
            }
        }
        st_lastpc = b->pc;
        st_ring[st_ringi++ & 15] = b->pc;
        if (b->code) {
            jc_insert(b);
            LExitRec *x = (LExitRec *)leo_backend_run(env, b->code);
            if (!x) {
                st_irqexit++;
            }
            if (x) {
                st_lastexit = x->pc;
                try_chain(cs, env, x);
            }
        } else {
            leo_interp(env, b);
            st_interp++;
        }
        st_blocks++;
        st_insns += b->ninsn;
        ran = true;
        /*
         * As TCG ends a TB after CPSR writes: IRQs may just have been
         * unmasked, and an IRQ already pending raises no new exit request.
         */
        if (b->impure || (int32_t)qatomic_read(&cs->neg.icount_decr.u32) < 0) {
            break;
        }
    }
    return ran;
}
