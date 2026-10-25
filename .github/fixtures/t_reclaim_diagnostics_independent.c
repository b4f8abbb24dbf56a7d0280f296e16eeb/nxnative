// Independent telemetry review over real runtime/adapter/Vamm/SVC. Only host
// metadata allocation is injected to seed counters; no owner callback changes.
#define main original_runtime_fixture_main
#include "t_memory_runtime.c"
#undef main
#define SH(s,p,n) S(13,size_t (*)(void *,void *,size_t))(s,p,n)
#define RR(s,p,n) S(12,void *(*)(void *,void *,size_t))(s,p,n)
#define DUMP(s) S(20,void (*)(const void *))(s)
static dm_sa_result (*original_config)(void *,void *,size_t,unsigned,dm_sa_config *);
static dm_sa_page_diagnostics *tail;
static dm_sa_allocator *captured_core;
static int capture_core;
static dm_sa_result (*original_query)(void *,size_t,dm_sa_provider_stats *);
static _Atomic unsigned diagnostic_queries;
static _Atomic int reject_diagnostic_query;
static dm_sa_result query_spy(void *ctx,size_t alignment,dm_sa_provider_stats *out) {
    atomic_fetch_add(&diagnostic_queries,1);
    return atomic_load(&reject_diagnostic_query)?DM_SA_PROVIDER_FAILED:original_query(ctx,alignment,out);
}
static _Atomic unsigned core_frees;
static void *meta_alloc(void *ctx,size_t size) {
    (void)ctx; void *p=malloc(size);
    if(p && capture_core) { assert(size>=sizeof *tail); capture_core=0; captured_core=p; tail=(void *)((unsigned char *)p+size-sizeof *tail); }
    return p;
}
static void meta_free(void *ctx,void *p) { (void)ctx; if(p==captured_core) atomic_fetch_add(&core_frees,1); free(p); }
static dm_sa_result telemetry_config(void *ctx,void *self,size_t size,unsigned cache,dm_sa_config *out) {
    dm_sa_result r=original_config(ctx,self,size,cache,out);
    if(r==DM_SA_OK) {
        if(!original_query) original_query=out->provider.query; else assert(original_query==out->provider.query);
        out->provider.query=query_spy;
        out->metadata_alloc=meta_alloc; out->metadata_free=meta_free; capture_core=1;
    }
    return r;
}
static pthread_mutex_t obs_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t obs_changed=PTHREAD_COND_INITIALIZER;
static dm_sa_adapter_event history[512], blocked_copy;
static unsigned history_count;
static int block_kind=-1, is_blocked, may_continue;
static dm_sa_stats dump_stats;
static void dump_observer(void *ctx,const void *self,const dm_sa_stats *s) { assert(ctx && self && s); dump_stats=*s; }
static void event_observer(void *ctx,const dm_sa_adapter_event *e) {
    assert(ctx && e && e->pages_valid);
    if(e->stats_result==DM_SA_OK) assert(!memcmp(&e->pages,&e->stats.pages,sizeof e->pages));
    pthread_mutex_lock(&obs_mutex); assert(history_count<512); history[history_count++]=*e;
    if((int)e->kind==block_kind) {
        block_kind=-1; blocked_copy=*e; is_blocked=1; pthread_cond_broadcast(&obs_changed);
        while(!may_continue) pthread_cond_wait(&obs_changed,&obs_mutex);
        assert(!memcmp(e,&blocked_copy,sizeof *e)); // core and self identity may already be replaced
    }
    pthread_mutex_unlock(&obs_mutex);
}
static unsigned count_kind(dm_sa_adapter_event_kind kind) {
    pthread_mutex_lock(&obs_mutex); unsigned n=0; for(unsigned i=0;i<history_count;++i) n+=history[i].kind==kind; pthread_mutex_unlock(&obs_mutex); return n;
}
static dm_sa_adapter_event last_event(void) { pthread_mutex_lock(&obs_mutex); assert(history_count); dm_sa_adapter_event e=history[history_count-1]; pthread_mutex_unlock(&obs_mutex); return e; }
static void seed_saturation(void *self) {
    INIT(self); assert(!capture_core && tail && !tail->sequence);
    // Reachable in principle after UINT64_MAX skipped cleanup observations, but
    // injected here without billions of calls or any production mutation hook.
    tail->sequence=UINT64_MAX; tail->reclaim_borrowed_runs=UINT64_MAX;
    unsigned before=count_kind(DM_SA_EVENT_PAGE_ACTIVITY);
    void *p=ALLOC(self,SMALL); assert(p && count_kind(DM_SA_EVENT_PAGE_ACTIVITY)==before+1);
    assert(last_event().pages.sequence==UINT64_MAX && !last_event().pages.reclaim_successes);
    before=count_kind(DM_SA_EVENT_PAGE_ACTIVITY);
    assert(SH(self,p,32)==32 && alias_mapped()==PAGE);
    fprintf(stderr,"saturated first reclaim: before=%u after=%u actual_successes=%llu\n",before,count_kind(DM_SA_EVENT_PAGE_ACTIVITY),(unsigned long long)tail->reclaim_successes);
    assert(count_kind(DM_SA_EVENT_PAGE_ACTIVITY)==before+1); // fails original a0f1feec
    assert(last_event().pages.reclaim_successes==1);
    before=count_kind(DM_SA_EVENT_PAGE_ACTIVITY);
    assert(RR(self,p,SMALL)==p);
    assert(count_kind(DM_SA_EVENT_PAGE_ACTIVITY)==before+1 && last_event().pages.recommit_successes==1);
    before=count_kind(DM_SA_EVENT_PAGE_ACTIVITY);
    for(unsigned n=0;n<8;++n) { assert(SH(self,p,32)==32); assert(RR(self,p,SMALL)==p); }
    assert(count_kind(DM_SA_EVENT_PAGE_ACTIVITY)==before); // no periodic flood at saturation
    DUMP(self); assert(dump_stats.pages.sequence==UINT64_MAX && dump_stats.pages.reclaim_successes==9 && dump_stats.pages.recommit_successes==9);
    assert(dump_stats.committed_bytes==SMALL);
    FINISH(self); assert(last_event().kind==DM_SA_EVENT_FINALIZED && last_event().pages.sequence==UINT64_MAX && last_event().stats_result==DM_SA_CLOSED);
}
static void first_success_outside_sample(void *self) {
    INIT(self); void *p=ALLOC(self,SMALL); assert(p); memset(p,0x73,SMALL);
    // Four changed skipped observations consume the ordinary first4 schedule.
    for(unsigned n=0;n<4;++n) { ++tail->sequence; ++tail->reclaim_borrowed_runs; assert(!RR(self,NULL,0)); }
    unsigned before=count_kind(DM_SA_EVENT_PAGE_ACTIVITY);
    assert(SH(self,p,32)==32 && count_kind(DM_SA_EVENT_PAGE_ACTIVITY)==before+1);
    assert(last_event().page_event_count==5 && last_event().pages.reclaim_successes==1);
    before=count_kind(DM_SA_EVENT_PAGE_ACTIVITY);
    assert(RR(self,p,3*PAGE)==p && count_kind(DM_SA_EVENT_PAGE_ACTIVITY)==before+1);
    dm_sa_adapter_event e=last_event();
    assert(e.page_event_count==6 && e.pages.recommit_successful_range_bytes==3*PAGE);
    assert(e.stats_result==DM_SA_OK && e.stats.committed_bytes==3*PAGE && e.stats.backing_bytes==SMALL);
    assert(e.pages.recommit_successful_range_bytes != e.stats.committed_bytes-PAGE); // coverage is not delta
    for(unsigned n=0;n<32;++n) assert(((unsigned char *)p)[n]==0x73);
    for(unsigned n=PAGE;n<3*PAGE;++n) assert(!((unsigned char *)p)[n]);
    FINISH(self);
}
static void diagnostic_failure_keeps_success(void *self) {
    INIT(self); unsigned char *p=ALLOC(self,SMALL); assert(p); memset(p,0x39,SMALL);
    unsigned queries=atomic_load(&diagnostic_queries); dm_sa_page_diagnostics copy;
    assert(dm_sa_get_page_diagnostics(captured_core,&copy)==DM_SA_OK && !copy.sequence);
    assert(atomic_load(&diagnostic_queries)==queries); // O(1) snapshot never calls provider
    atomic_store(&reject_diagnostic_query,1);
    assert(SH(self,p,32)==32 && alias_mapped()==PAGE);
    dm_sa_adapter_event e=last_event(); assert(e.kind==DM_SA_EVENT_PAGE_ACTIVITY && e.result==DM_SA_OK && e.stats_result==DM_SA_PROVIDER_FAILED);
    assert(e.pages_valid && e.pages.reclaim_successes==1 && !e.pages.page_failures);
    for(unsigned n=0;n<32;++n) assert(p[n]==0x39);
    atomic_store(&reject_diagnostic_query,0);
    assert(RR(self,p,3*PAGE)==p && last_event().stats_result==DM_SA_OK);
    queries=atomic_load(&diagnostic_queries); FINISH(self);
    assert(last_event().kind==DM_SA_EVENT_FINALIZED && last_event().stats_result==DM_SA_CLOSED);
    assert(atomic_load(&diagnostic_queries)==queries); // no stats/provider access after destruction
}
typedef struct { void *self,*pointer; int finish; } worker_args;
static void *delayed_worker(void *arg) { worker_args *w=arg; if(w->finish) FINISH(w->self); else assert(SH(w->self,w->pointer,32)==32); return NULL; }
static void copied_event_lifetime(void *self,int finalize_event) {
    INIT(self); void *p=ALLOC(self,SMALL); assert(p); memset(p,0x56,SMALL);
    pthread_mutex_lock(&obs_mutex); block_kind=finalize_event?DM_SA_EVENT_FINALIZED:DM_SA_EVENT_PAGE_ACTIVITY; is_blocked=may_continue=0; pthread_mutex_unlock(&obs_mutex);
    worker_args args={self,p,finalize_event}; pthread_t worker; assert(!pthread_create(&worker,NULL,delayed_worker,&args));
    pthread_mutex_lock(&obs_mutex); while(!is_blocked) pthread_cond_wait(&obs_changed,&obs_mutex); uint64_t generation=blocked_copy.generation; pthread_mutex_unlock(&obs_mutex);
    if(!finalize_event) FINISH(self);
    CONSTRUCT(self); INIT(self); assert(last_event().generation!=generation && !last_event().pages.sequence);
    unsigned char *next=ALLOC(self,32); assert(next); memset(next,0x28,32);
    pthread_mutex_lock(&obs_mutex); may_continue=1; pthread_cond_broadcast(&obs_changed); pthread_mutex_unlock(&obs_mutex); assert(!pthread_join(worker,NULL));
    for(unsigned n=0;n<32;++n) assert(next[n]==0x28);
    FREE(self,next); FINISH(self);
}

#ifdef DM_REVIEW_STALE_SAMPLER
// Only the adapter translation unit renames the accessor to this wrapper. The
// real accessor still copies under the core mutex; this pause is AFTER its copy.
static _Thread_local int pause_sampler;
static pthread_mutex_t sampler_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sampler_cond=PTHREAD_COND_INITIALIZER;
static int sampler_paused,sampler_continue;
dm_sa_result review_get_page_diagnostics(dm_sa_allocator *a,dm_sa_page_diagnostics *out) {
    dm_sa_result r=dm_sa_get_page_diagnostics(a,out);
    if(pause_sampler) {
        pause_sampler=0; pthread_mutex_lock(&sampler_mutex); sampler_paused=1; pthread_cond_broadcast(&sampler_cond);
        while(!sampler_continue) pthread_cond_wait(&sampler_cond,&sampler_mutex);
        pthread_mutex_unlock(&sampler_mutex);
    }
    return r;
}
static void *stale_worker(void *self) { pause_sampler=1; assert(!RR(self,NULL,0)); return NULL; }
static void stale_sampler_observation(void *self) {
    INIT(self); void *p=ALLOC(self,SMALL); assert(p);
    tail->sequence=1; tail->reclaim_borrowed_runs=1;
    unsigned before=count_kind(DM_SA_EVENT_PAGE_ACTIVITY); pthread_t worker;
    assert(!pthread_create(&worker,NULL,stale_worker,self));
    pthread_mutex_lock(&sampler_mutex); while(!sampler_paused) pthread_cond_wait(&sampler_cond,&sampler_mutex); pthread_mutex_unlock(&sampler_mutex);
    assert(SH(self,p,32)==32 && count_kind(DM_SA_EVENT_PAGE_ACTIVITY)==before+1);
    assert(last_event().pages.sequence==2 && last_event().page_event_count==1);
    pthread_mutex_lock(&sampler_mutex); sampler_continue=1; pthread_cond_broadcast(&sampler_cond); pthread_mutex_unlock(&sampler_mutex);
    assert(!pthread_join(worker,NULL)); assert(count_kind(DM_SA_EVENT_PAGE_ACTIVITY)==before+1);
    assert(RR(self,p,SMALL)==p && last_event().pages.sequence==3 && last_event().page_event_count==2);
    FINISH(self);
}
#endif
static void owner_destroyed_during_detached_finalize(void *self,dm_memory_runtime *runtime) {
    INIT(self); assert(ALLOC(self,SMALL));
    pthread_mutex_lock(&obs_mutex); block_kind=DM_SA_EVENT_FINALIZED; is_blocked=may_continue=0; pthread_mutex_unlock(&obs_mutex);
    worker_args args={self,NULL,1}; pthread_t worker; assert(!pthread_create(&worker,NULL,delayed_worker,&args));
    pthread_mutex_lock(&obs_mutex); while(!is_blocked) pthread_cond_wait(&obs_changed,&obs_mutex); pthread_mutex_unlock(&obs_mutex);
    assert(!alias_mapped());
    assert(dm_sa_adapter_shutdown()==DM_SA_OK && !dm_vamm_adapter_shutdown());
    assert(dm_memory_runtime_destroy(runtime)==DM_SA_OK); // both core and owner now freed
    pthread_mutex_lock(&obs_mutex); may_continue=1; pthread_cond_broadcast(&obs_changed); pthread_mutex_unlock(&obs_mutex);
    assert(!pthread_join(worker,NULL)); // no later core/provider/owner access
}
int main(void) {
    alarm(60); dm_memory_runtime *r=NULL; dm_memory_runtime_contract c={DM_VAMM_DOMAIN_ALIAS_MINUS_EXTRA,DM_VAMM_PLACEMENT_MODERN};
    assert(dm_memory_runtime_create_page_capable_with_contract(&r,128,&c)==DM_SA_OK);
    dm_sa_adapter_config a; dm_vamm_adapter_config v; assert(dm_memory_runtime_configs(r,&v,&a)==DM_SA_OK);
    dm_vamm_symbol vs[DM_VAMM_SYMBOL_COUNT]; dm_sa_adapter_symbol as[DM_SA_ADAPTER_SYMBOL_COUNT]; census(&v,&a,vs,as);
    original_config=a.make_config; a.make_config=telemetry_config; a.page_events=1; a.event=event_observer; a.dump=dump_observer;
    assert(!dm_vamm_adapter_install(&v) && dm_sa_adapter_install(&a)==DM_SA_OK);
    unsigned char manager[64]={0}, self[DM_SA_ADAPTER_OBJECT_SIZE]={0}; V(8,void (*)(void *))(manager); V(0,void (*)(void))(); CONSTRUCT(self);
    seed_saturation(self); first_success_outside_sample(self); diagnostic_failure_keeps_success(self); copied_event_lifetime(self,0); copied_event_lifetime(self,1);
#ifdef DM_REVIEW_STALE_SAMPLER
    stale_sampler_observation(self);
#endif
    owner_destroyed_during_detached_finalize(self,r);
    assert(!alias_mapped() && atomic_load(&core_frees)==
#ifdef DM_REVIEW_STALE_SAMPLER
        9
#else
        8
#endif
    );
    puts("independent page diagnostics: saturation first-success, sampling, coherent range/snapshot and detached core/owner lifetime copies passed"); return 0;
}
