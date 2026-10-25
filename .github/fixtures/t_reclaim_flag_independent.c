// Independent startup flag review. Synthetic ELF originals only; production
// authorization, hook transaction, runtime, owner and SVC are linked unchanged.
#define MEMORY_STARTUP_MAIN independent_unused_startup_main
#include "t_memory_runtime_startup.c"
#undef MEMORY_STARTUP_MAIN
#include "../hle_memory_experiment.h"

static unsigned review_creates, review_pages, review_destroys, review_installs;
static int review_fail_create, review_fail_config;
static dm_sa_result review_create(dm_memory_runtime **out, size_t n, const dm_memory_runtime_contract *c) {
    ++review_creates; assert(n == 65536 && c && c->domain == DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA && c->placement == DM_VAMM_PLACEMENT_MODERN);
    if (review_fail_create) return DM_SA_NO_MEMORY;
    dm_sa_result r = dm_memory_runtime_create_with_contract(out, n, c);
    if (r == DM_SA_OK) startup_runtime = *out;
    return r;
}
static dm_sa_result review_create_pages(dm_memory_runtime **out, size_t n, const dm_memory_runtime_contract *c) {
    ++review_creates; ++review_pages;
    assert(n == 65536 && c && c->domain == DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA && c->placement == DM_VAMM_PLACEMENT_MODERN);
    if (review_fail_create) return DM_SA_NO_MEMORY;
    dm_sa_result r = dm_memory_runtime_create_page_capable_with_contract(out, n, c);
    if (r == DM_SA_OK) startup_runtime = *out;
    return r;
}
static dm_sa_result review_configs(dm_memory_runtime *r, dm_vamm_adapter_config *v, dm_sa_adapter_config *a) {
    if (review_fail_config) return DM_SA_INVALID;
    return dm_memory_runtime_configs(r, v, a);
}
static dm_sa_result review_destroy(dm_memory_runtime *r) { ++review_destroys; return dm_memory_runtime_destroy(r); }
int loader_install_memory_hooks(const dm_sa_adapter_config *a, const dm_vamm_adapter_config *v) {
    ++review_installs; assert(!a->proofs && !v->proofs && a->reject_managed_walk && a->dump && a->event && a->fatal && v->fatal);
    assert(dm_memory_experiment_authorized(a->experimental_startup, a->context));
    assert(dm_memory_experiment_authorized(v->experimental_startup, v->context));
    startup_allocator = *a; startup_vamm = *v;
    return dm_memory_hooks_install(g_mod, (size_t)g_nmod, a, v, startup_resolve);
}
int loader_install_vamm_hooks(const dm_vamm_adapter_config *v) {
    (void)v; assert(!"SDK-only must reject reclaim before installation"); return DM_MEMORY_HOOKS_INVALID;
}
#ifdef MEMORY_EXPERIMENT_NO_ALIGNED_LEASES
#undef DM_MEMORY_RUNTIME_ALIGNED_LEASES
#endif
#define dm_memory_runtime_create_with_contract review_create
#define dm_memory_runtime_create_page_capable_with_contract review_create_pages
#define dm_memory_runtime_configs review_configs
#define dm_memory_runtime_destroy review_destroy
#include "../hle_memory_experiment.c"
#undef dm_memory_runtime_create_with_contract
#undef dm_memory_runtime_create_page_capable_with_contract
#undef dm_memory_runtime_configs
#undef dm_memory_runtime_destroy

static void review_identities(void) {
    static const char *id[] = {"7222e13ecf6adb3249b5dd5c57fa40b102245fce000000000000000000000000", "df7a3960c74c3a4803f826c5871532523aa9cf2a000000000000000000000000"};
    for (unsigned k = 0; k < 2; ++k) { g_mod[k] = startup_modules[k]; memcpy(g_mod[k].build_id,id[k],32); g_mod[k].build_id[32]=0; strcpy(g_mod[k].full_build_id,id[k]); }
    g_nmod = 2;
}
static void review_cycle(int pages) {
    assert(dm_memory_hooks_active() && dm_memory_hooks_enter_guest());
    assert(!dm_memory_experiment_authorized(startup_allocator.experimental_startup, startup_runtime));
    // Startup policy is captured once; later environment changes are not a live switch.
    assert(!setenv("DM_MEMORY_RECLAIM", pages ? "0" : "1", 1));
    unsigned char manager[64] = {0}, self[DM_SA_ADAPTER_OBJECT_SIZE] = {0};
    V(8,void (*)(void *))(manager); V(10,void (*)(void *))(manager);
    dm_sa_config probe;
    assert(startup_allocator.make_config(startup_allocator.context,self,SMALL,0,&probe) == DM_SA_OK);
    assert(!!probe.provider.pages.granule == pages && !!probe.provider.pages.query_run == pages && !!probe.provider.pages.change == pages && !!probe.provider.pages.budget == pages);
    startup_allocator.release_config(startup_allocator.context,self,&probe);
    CONSTRUCT(self); S(7,void (*)(void *,void *,size_t,unsigned))(self,NULL,(size_t)12 << 30,1);
    unsigned char *p = ALLOC(self,SMALL - 32), *edge = ALLOC(self,32);
    assert(p && edge == p + SMALL - 32 && alias_mapped() == SMALL);
    memset(p,0x6a,SMALL - 32); memset(edge,0x7c,32);
    assert(S(13,size_t (*)(void *,void *,size_t))(self,p,32) == 32);
    size_t shrunk = alias_mapped(); assert(shrunk == (pages ? 2*PAGE : SMALL));
    S(18,void (*)(const void *))(self); assert(alias_mapped() == shrunk);
    S(20,void (*)(const void *))(self); // Real logger must report authoritative commitment, not request-span totals.
    assert(S(12,void *(*)(void *,void *,size_t))(self,p,SMALL - 32) == p && alias_mapped() == SMALL);
    for(size_t i=0;i<32;++i) assert(p[i] == 0x6a && edge[i] == 0x7c);
    for(size_t i=PAGE;i<SMALL-PAGE;++i) assert(p[i] == (pages ? 0 : 0x6a));
    FREE(self,p); FREE(self,edge); FINISH(self); assert(!alias_mapped());
    assert(!atomic_load(&forwards) && dm_memory_runtime_destroy(startup_runtime) == DM_SA_BUSY);
    fprintf(stderr,"TRACE: page_capability=%d allocated=%u shrunk=%zu regrown=%u final=0 forwards=0\n",pages,SMALL,shrunk,SMALL);
}
typedef struct { const char *flag, *request; int fault, expected; } review_case;
static void review_run(const review_case *c) {
    if (c->flag) assert(!setenv("DM_MEMORY_RECLAIM",c->flag,1)); else assert(!unsetenv("DM_MEMORY_RECLAIM"));
    review_identities();
    if(c->fault != 9) assert(dm_memory_hooks_open_startup());
    if(c->fault == 1) g_mod[0].full_build_id[63]='1';
    if(c->fault == 2) { module_t m=g_mod[0]; g_mod[0]=g_mod[1]; g_mod[1]=m; }
    if(c->fault == 3) g_nmod=1;
    if(c->fault == 4) g_nmod=3;
    if(c->fault == 5) g_mod[1].nro=1;
    if(c->fault == 6) g_mod[1].loaded=0;
    if(c->fault == 7) g_mod[0].loads=1;
    if(c->fault == 8) g_mod[1].build_id[0]='1';
    if(c->fault == 10) assert(dm_memory_hooks_enter_guest());
    if(c->fault == 11) resolver_failure=0;
    if(c->fault == 12) startup_images[1]->rela[STARTUP_SYMBOLS-1].r_info=999;
    if(c->fault == 13) startup_images[1]->slots[STARTUP_SYMBOLS-1]^=8;
    if(c->fault == 14) review_fail_create=1;
    if(c->fault == 15) review_fail_config=1;
    uint64_t before[2][STARTUP_SYMBOLS+1];
    for(unsigned k=0;k<2;++k) memcpy(before[k],startup_images[k]->slots,sizeof before[k]);
    const char *why=(void *)1;
    int r=dm_memory_experiment_startup(c->request,&why);
    assert(r==c->expected);
    if(r==DM_MEMORY_EXPERIMENT_ACTIVE) {
        assert(!why && review_creates==1 && review_installs==1 && !review_destroys);
        int pages=c->flag && !strcmp(c->flag,"1"); assert(review_pages==(unsigned)pages);
        review_cycle(pages);
    } else {
        assert((r==DM_MEMORY_EXPERIMENT_OFF && !why) || (r==DM_MEMORY_EXPERIMENT_FAILED && why && *why));
        assert(!dm_memory_hooks_active() && !alias_mapped() && !atomic_load(&constructors));
        for(unsigned k=0;k<2;++k) assert(!memcmp(before[k],startup_images[k]->slots,sizeof before[k]));
        for(unsigned i=0;i<STARTUP_SYMBOLS;++i) assert(!dm_memory_hooks_lookup(startup_name(i)) && !dm_sa_adapter_lookup(startup_name(i)) && !dm_vamm_adapter_lookup(startup_name(i)));
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY && defined(DM_MEMORY_RUNTIME_ALIGNED_LEASES)
        if(c->fault >= 9 && c->fault <= 13) assert(review_creates==1 && review_installs==1 && review_destroys==1);
        else if(c->fault == 14) assert(review_creates==1 && !review_installs && !review_destroys);
        else if(c->fault == 15) assert(review_creates==1 && !review_installs && review_destroys==1);
        else assert(!review_creates && !review_installs && !review_destroys);
#else
        assert(!review_creates && !review_installs && !review_destroys);
#endif
    }
    why=NULL; assert(dm_memory_experiment_startup("0",&why)==DM_MEMORY_EXPERIMENT_FAILED && why);
}
static unsigned review_cases;
static void review_child(const review_case *c, char *out, size_t cap) {
    int fds[2]; assert(!pipe(fds)); pid_t pid=fork(); assert(pid>=0);
    if(!pid) { close(fds[0]); assert(dup2(fds[1],STDERR_FILENO)==STDERR_FILENO); close(fds[1]); review_run(c); _exit(0); }
    close(fds[1]); size_t n=0;
    for(;;) { assert(n<cap-1); ssize_t got=read(fds[0],out+n,cap-1-n); if(got<0 && errno==EINTR) continue; assert(got>=0); if(!got) break; n+=(size_t)got; }
    out[n]=0; close(fds[0]); int status; assert(waitpid(pid,&status,0)==pid);
    if(!WIFEXITED(status) || WEXITSTATUS(status)) { fprintf(stderr,"CASE flag=%s request=%s fault=%d\n%s",c->flag?c->flag:"<absent>",c->request?c->request:"<absent>",c->fault,out); }
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
#ifndef DM_RECLAIM_REVIEW_BASELINE
    if(c->expected == DM_MEMORY_EXPERIMENT_ACTIVE) {
        int pages=c->flag && !strcmp(c->flag,"1"); char marker[256];
        snprintf(marker,sizeof marker,"experimental memory policy contract=%s mode=full reclaim=%s",DM_MEMORY_EXPERIMENT_NAME,pages?"on":"off");
        const char *policy=strstr(out,marker); assert(policy && !strstr(policy+1,marker));
        const char *dump=strstr(out,"experimental shared memory Dump self="); assert(dump);
        snprintf(marker,sizeof marker,"reserved=%u committed=%zu",SMALL,pages?2*(size_t)PAGE:(size_t)SMALL);
        assert(strstr(dump,marker));
        const char *snapshot=strstr(out,"experimental shared memory snapshot self="); assert(snapshot);
        assert(strstr(snapshot,"reserved=0 committed=0"));
    } else assert(!strstr(out,"experimental memory policy contract="));
#endif
    ++review_cases;
}
int main(void) {
    struct rlimit limit={0,0}; assert(!setrlimit(RLIMIT_CORE,&limit)); startup_images_create();
    char log[32768], trace[512]={0};
#if defined(DM_EXPERIMENTAL_SHARED_MEMORY) && DM_EXPERIMENTAL_SHARED_MEMORY && defined(DM_MEMORY_RUNTIME_ALIGNED_LEASES)
    const int active=DM_MEMORY_EXPERIMENT_ACTIVE;
#else
    const int active=DM_MEMORY_EXPERIMENT_FAILED;
#endif
    const char *off[]={NULL,"0"};
    for(unsigned f=0;f<2;++f) {
        review_case c={off[f],DM_MEMORY_EXPERIMENT_NAME,0,active}; review_child(&c,log,sizeof log);
        if(active==DM_MEMORY_EXPERIMENT_ACTIVE) { const char *s=strstr(log,"TRACE:"); assert(s); if(!f) strcpy(trace,s); else assert(!strcmp(trace,s)); }
        const char *inactive[]={NULL,"","0"};
        for(unsigned i=0;i<3;++i) { c=(review_case){off[f],inactive[i],0,DM_MEMORY_EXPERIMENT_OFF}; review_child(&c,log,sizeof log); }
        for(int fault=1;fault<=15;++fault) { c=(review_case){off[f],DM_MEMORY_EXPERIMENT_NAME,fault,DM_MEMORY_EXPERIMENT_FAILED}; review_child(&c,log,sizeof log); }
    }
#ifndef DM_RECLAIM_REVIEW_BASELINE
    const char *requests[]={NULL,"","0","1","unknown",DM_SDK_MEMORY_EXPERIMENT_NAME,DM_MEMORY_EXPERIMENT_NAME};
    const char *bad[]={"","00","01","10","2","-1","+1"," 1","1 ","0 ","1\n","true","false","yes","\xef\xbc\x91"};
    for(unsigned b=0;b<sizeof bad/sizeof *bad;++b) for(unsigned q=0;q<sizeof requests/sizeof *requests;++q) {
        review_case c={bad[b],requests[q],0,DM_MEMORY_EXPERIMENT_FAILED}; review_child(&c,log,sizeof log);
    }
    for(unsigned q=0;q<sizeof requests/sizeof *requests;++q) {
        review_case c={"1",requests[q],0,q==6?active:DM_MEMORY_EXPERIMENT_FAILED}; review_child(&c,log,sizeof log);
    }
    for(int fault=1;fault<=15;++fault) { review_case c={"1",DM_MEMORY_EXPERIMENT_NAME,fault,DM_MEMORY_EXPERIMENT_FAILED}; review_child(&c,log,sizeof log); }
#endif
    for(unsigned k=0;k<2;++k) assert(!munmap(startup_images[k],startup_image_bytes));
    if(*trace) fputs(trace,stdout);
    printf("independent reclaim flag: strict selection, original ownership gates, transactional rollback and off differential passed (%u cases)\n",review_cases);
    return 0;
}
