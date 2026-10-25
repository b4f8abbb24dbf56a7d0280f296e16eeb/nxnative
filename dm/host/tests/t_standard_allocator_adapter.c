// Synthetic original functions and real host-owned backing only: no SDK image,
// module offsets, instructions, ROM data or extracted game artifacts.
#define _POSIX_C_SOURCE 200809L
#include "hle_standard_allocator_adapter.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { C1,C2,CB1,CB2,CC1,CC2,I,IC,F,A,AA,FR,R,S,GS,TF,LA,TC,CL,W,D,H };
static unsigned calls[22], dumped;
static void *last_self, *last_pointer;
static size_t last_size, last_alignment;
static unsigned last_cache;
static unsigned char forwarded_pointer;
static void note(int n, const void *s) { calls[n]++; last_self=(void *)s; }
#define CTOR(n) static void f##n(void *s) { note(n,s); memset(s,0,56); }
CTOR(C1) CTOR(C2)
#define CTORB(n) static void f##n(void *s,void *b,size_t z) { note(n,s); last_pointer=b; last_size=z; ((unsigned char*)s)[0]=1; }
CTORB(CB1) CTORB(CB2) CTORB(I)
#define CTORC(n) static void f##n(void *s,void *b,size_t z,unsigned c) { note(n,s); last_pointer=b; last_size=z; last_cache=c; ((unsigned char*)s)[0]=1; }
CTORC(CC1) CTORC(CC2) CTORC(IC)
static void fF(void *s) { note(F,s); ((unsigned char*)s)[0]=0; }
static void *fA(void *s,size_t z) { note(A,s); last_size=z; return &forwarded_pointer; }
static void *fAA(void *s,size_t z,size_t a) { note(AA,s); last_size=z; last_alignment=a; return &forwarded_pointer; }
static int profile_big_align;
static size_t policy_override, last_provider_alignment;
static _Atomic unsigned policy_calls;
static int policy_override_active;
static size_t allocation_alignment(void *s,size_t z) {
    (void)s; policy_calls++;
    return policy_override_active ? policy_override : profile_big_align && z>=65536 ? 65536 : 16;
}
static void *profile_allocate(void *s,size_t z) {
    return profile_big_align && z>=65536 ? fAA(s,z,65536) : fA(s,z);
}
static void fFR(void *s,void *p) { note(FR,s); last_pointer=p; }
static void *fR(void *s,void *p,size_t z) { note(R,s); last_pointer=p; last_size=z; return &forwarded_pointer; }
static size_t fS(void *s,void *p,size_t z) { note(S,s); last_pointer=p; last_size=z; return 0x123456789ULL; }
static size_t fGS(const void *s,const void *p) { note(GS,s); last_pointer=(void*)p; return 0x234567891ULL; }
static size_t fTF(const void *s) { note(TF,s); return 0x345678912ULL; }
static size_t fLA(const void *s) { note(LA,s); return 0x456789123ULL; }
static void fTC(const void *s) { note(TC,s); }
static void fCL(const void *s) { note(CL,s); }
static int fW(const void *s,dm_sa_walk_fn cb,void *ctx) { note(W,s); return cb(&forwarded_pointer,17,ctx)+37; }
static void fD(const void *s) { note(D,s); }
static dm_sa_hash fH(const void *s) { note(H,s); return (dm_sa_hash){7,19,{1,2,3,4,5,6,7,8}}; }
static dm_sa_guest_fn fixture[22] = {
    (dm_sa_guest_fn)fC1,(dm_sa_guest_fn)fC2,(dm_sa_guest_fn)fCB1,(dm_sa_guest_fn)fCB2,
    (dm_sa_guest_fn)fCC1,(dm_sa_guest_fn)fCC2,(dm_sa_guest_fn)fI,(dm_sa_guest_fn)fIC,
    (dm_sa_guest_fn)fF,(dm_sa_guest_fn)fA,(dm_sa_guest_fn)fAA,(dm_sa_guest_fn)fFR,
    (dm_sa_guest_fn)fR,(dm_sa_guest_fn)fS,(dm_sa_guest_fn)fGS,(dm_sa_guest_fn)fTF,
    (dm_sa_guest_fn)fLA,(dm_sa_guest_fn)fTC,(dm_sa_guest_fn)fCL,(dm_sa_guest_fn)fW,
    (dm_sa_guest_fn)fD,(dm_sa_guest_fn)fH
};
#define HOOK(n,type) ((type)dm_sa_adapter_lookup(dm_sa_adapter_symbol_name(n)))
#define NEW(s) HOOK(C1,void(*)(void*))(s)
#define INIT(s,n,c) HOOK(IC,void(*)(void*,void*,size_t,unsigned))(s,NULL,n,c)
#define ALLOC(s,n) HOOK(A,void*(*)(void*,size_t))(s,n)
#define ALIGNED(s,n,a) HOOK(AA,void*(*)(void*,size_t,size_t))(s,n,a)
#define FREE(s,p) HOOK(FR,void(*)(void*,void*))(s,p)
#define REALLOC(s,p,n) HOOK(R,void*(*)(void*,void*,size_t))(s,p,n)
#define SHRINK(s,p,n) HOOK(S,size_t(*)(void*,void*,size_t))(s,p,n)
#define SIZE(s,p) HOOK(GS,size_t(*)(const void*,const void*))(s,p)
#define FINISH(s) HOOK(F,void(*)(void*))(s)
#define HASH(s) HOOK(H,dm_sa_hash(*)(const void*))(s)
#define WALK(s,c,x) HOOK(W,int(*)(const void*,dm_sa_walk_fn,void*))(s,c,x)
static size_t leased;
static unsigned acquisitions,releases;
static void begin(void *c) { (void)c; }
static void end(void *c) { (void)c; }
static dm_sa_result acquire(void *c,size_t n,size_t a,dm_sa_span *out) {
    (void)c; void *p=NULL; last_provider_alignment=a;
    if (n > 1024*1024-leased || posix_memalign(&p,a,n)) return DM_SA_NO_MEMORY;
    *out=(dm_sa_span){p,n,(uintptr_t)p}; leased+=n; acquisitions++; return DM_SA_OK;
}
static dm_sa_result release(void *c,const dm_sa_span *s) {
    (void)c; assert(s->token==(uintptr_t)s->base); assert(leased>=s->size);
    leased-=s->size; releases++; free(s->base); return DM_SA_OK;
}
static int available(void *c,const dm_sa_span *s,size_t o,size_t n) {
    (void)c; return o<=s->size && n<=s->size-o;
}
static dm_sa_result query(void *c,size_t a,dm_sa_provider_stats *out) {
    (void)c;(void)a; *out=(dm_sa_provider_stats){1024*1024-leased,1024*1024-leased}; return DM_SA_OK;
}
enum { FACTORY_NORMAL, FACTORY_OTHER, FACTORY_REENTER, FACTORY_BLOCK, FACTORY_FAIL };
static pthread_mutex_t factory_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t factory_changed=PTHREAD_COND_INITIALIZER;
static void *factory_target,*factory_other;
static int factory_mode,factory_entered,factory_continue;
static dm_sa_result make_config(void *c,void *self,size_t capacity,unsigned cache,dm_sa_config *out) {
    (void)c; assert(cache<=1);
    pthread_mutex_lock(&factory_mutex);
    int mode=self==factory_target?factory_mode:FACTORY_NORMAL;
    void *other=factory_other;
    if(mode==FACTORY_BLOCK) {
        factory_entered=1; pthread_cond_broadcast(&factory_changed);
        while(!factory_continue) pthread_cond_wait(&factory_changed,&factory_mutex);
    }
    pthread_mutex_unlock(&factory_mutex);
    if(mode==FACTORY_OTHER) assert(HASH(other).block_count==0);
    if(mode==FACTORY_REENTER) (void)ALLOC(self,16);
    if(mode==FACTORY_FAIL) return DM_SA_PROVIDER_FAILED;
    *out=(dm_sa_config){.provider={NULL,4096,begin,end,acquire,release,available,query},.capacity=capacity,.pool_size=4096};
    return DM_SA_OK;
}
static void diagnostic(void *c,const void *s,const dm_sa_stats *st) { (void)c;(void)s; assert(!st->closed); dumped++; }
static _Thread_local int fatal_in_thread;
static void fatal(void *c,const char *op,dm_sa_result r) {
    (void)c; assert(op); assert(r!=DM_SA_OK);
    if(fatal_in_thread) pthread_exit((void *)(uintptr_t)r);
    _exit(77);
}
static int simple_callback(void *p,size_t n,void *ctx) { assert(p); assert(n); (*(int*)ctx)++; return 1; }
static void expect_fatal(void (*action)(void *),void *context) {
    pid_t p=fork(); assert(p>=0);
    if (!p) { alarm(10); action(context); _exit(0); }
    int status; assert(waitpid(p,&status,0)==p); assert(WIFEXITED(status) && WEXITSTATUS(status)==77);
}
static void foreign_free(void *s) { FREE(s,&forwarded_pointer); }
static void stale_allocate(void *s) { (void)ALLOC(s,16); }
static void bad_capacity(void *s) { NEW(s); INIT(s,4097,0); }
static void zero_capacity(void *s) { NEW(s); INIT(s,0,0); }
static void double_initialize(void *s) { INIT(s,8192,0); }
static void nonnull_transition(void *s) { HOOK(I,void(*)(void*,void*,size_t))(s,&forwarded_pointer,4096); }

static void forwarding(void) {
    unsigned char s[56]={0}; void *b=&forwarded_pointer;
    HOOK(C1,void(*)(void*))(s); HOOK(C2,void(*)(void*))(s);
    HOOK(CB1,void(*)(void*,void*,size_t))(s,b,91); assert(last_pointer==b&&last_size==91);
    HOOK(CB2,void(*)(void*,void*,size_t))(s,b,92);
    HOOK(CC1,void(*)(void*,void*,size_t,unsigned))(s,b,93,0xa5); assert(last_cache==0xa5);
    HOOK(CC2,void(*)(void*,void*,size_t,unsigned))(s,b,94,0xa7);
    HOOK(I,void(*)(void*,void*,size_t))(s,b,95);
    HOOK(IC,void(*)(void*,void*,size_t,unsigned))(s,b,96,0xfe); assert(last_cache==0xfe);
    assert(!dm_sa_adapter_is_managed(s));
    assert(ALLOC(s,13)==b); assert(last_size==13);
    assert(ALIGNED(s,14,65536)==b); assert(last_alignment==65536);
    FREE(s,b); assert(last_pointer==b);
    assert(REALLOC(s,b,17)==b); assert(last_size==17);
    assert(SHRINK(s,b,9)==0x123456789ULL);
    assert(SIZE(s,b)==0x234567891ULL);
    assert(HOOK(TF,size_t(*)(const void*))(s)==0x345678912ULL);
    assert(HOOK(LA,size_t(*)(const void*))(s)==0x456789123ULL);
    HOOK(TC,void(*)(const void*))(s); HOOK(CL,void(*)(const void*))(s);
    int cb=0; assert(WALK(s,simple_callback,&cb)==38 && cb==1);
    HOOK(D,void(*)(const void*))(s);
    dm_sa_hash h=HASH(s); assert(h.block_count==7&&h.allocated_bytes==19&&h.digest[7]==8);
    FINISH(s); assert(!s[0]); assert(last_self==s);
    for(int n=0;n<22;n++) assert(calls[n]==1);
    profile_big_align=1;
    assert(ALLOC(s,65536)==b && calls[AA]==2 && last_alignment==65536);
    assert(ALLOC(s,65535)==b && calls[A]==2);
    profile_big_align=0;
}
typedef struct { void *self,*other; unsigned count; int stop; } walk_context;
static int reentrant_callback(void *p,size_t n,void *ctx) {
    walk_context *c=ctx; c->count++;
    assert(SIZE(c->self,p)==n);
    if(c->other && c->other!=p) { FREE(c->self,c->other); c->other=NULL; }
    return c->stop?0:1;
}
static int finalize_callback(void *p,size_t n,void *ctx) {
    (void)p;(void)n; FINISH(ctx); return 1;
}
static int replace_callback(void *p,size_t n,void *ctx) {
    (void)p;(void)n; walk_context *c=ctx; c->count++;
    FINISH(c->self); NEW(c->self); INIT(c->self,16384,0);
    assert(ALLOC(c->self,32));
    return 1;
}
static void *worker(void *self) {
    for(unsigned n=1;n<=500;n++) {
        size_t z=1+(n*37)%1024; void *p=ALLOC(self,z); assert(p);
        memset(p,(unsigned char)n,z); assert(SIZE(self,p)>=z);
        void *q=REALLOC(self,p,z+32); assert(q);
        for(size_t b=0;b<z;b++) assert(((unsigned char*)q)[b]==(unsigned char)n);
        FREE(self,q);
    }
    return NULL;
}
static void managed(void) {
    struct { unsigned char before[8],self[56],after[8]; } object;
    memset(&object,0xa7,sizeof object); void *s=object.self;
    expect_fatal(bad_capacity,s); expect_fatal(zero_capacity,s);
    NEW(s); INIT(s,UINT64_C(62)<<30,3);
    assert(dm_sa_adapter_is_managed(s)); assert(object.self[0]==1 && object.self[1]==1);
    assert(!leased && !acquisitions); // capacity is not an eagerly mapped arena
    expect_fatal(double_initialize,s); expect_fatal(nonnull_transition,s);
    unsigned original_calls[22]; memcpy(original_calls,calls,sizeof calls);
    profile_big_align=1;
    void *large=ALLOC(s,65536);
    assert(large && !((uintptr_t)large&65535) && last_provider_alignment==65536);
    FREE(s,large);
    void *bridged=NULL;
    assert(dm_sa_adapter_allocate_if_managed(s,65536,65536,&bridged));
    assert(bridged && !((uintptr_t)bridged&65535)); FREE(s,bridged);
    unsigned before_policy=policy_calls;
    large=ALIGNED(s,65536,4096);
    assert(large && last_provider_alignment==4096 && policy_calls==before_policy); FREE(s,large);
    // Policy applies only to managed size-only dispatch. Malformed and
    // overflowing alignment must fail without calling any original allocator.
    policy_override_active=1; policy_override=3; assert(!ALLOC(s,64));
    policy_override=SIZE_MAX; assert(!ALLOC(s,64));
    policy_override=(size_t)1<<63; assert(!ALLOC(s,64));
    policy_override=0; assert(!ALLOC(s,64)); policy_override_active=0;
    bridged=s; assert(dm_sa_adapter_allocate_if_managed(s,64,0,&bridged) && !bridged);
    bridged=s; assert(dm_sa_adapter_allocate_if_managed(s,64,3,&bridged) && !bridged);
    bridged=s; assert(dm_sa_adapter_allocate_if_managed(s,64,(size_t)1<<63,&bridged) && !bridged);
    profile_big_align=0;
    void *p=ALLOC(s,31), *q=ALIGNED(s,65,65536);
    assert(p&&q&& !((uintptr_t)p&15)&&!((uintptr_t)q&65535));
    assert(SIZE(s,p)==32&&SIZE(s,q)==80);
    memset(p,0x5a,31);
    p=REALLOC(s,p,8192); assert(p); for(int n=0;n<31;n++) assert(((unsigned char*)p)[n]==0x5a);
    assert(SHRINK(s,p,33)==48);
    void *shrink_zero=ALLOC(s,32); assert(shrink_zero);
    assert(SHRINK(s,shrink_zero,0)==16&&SIZE(s,shrink_zero)==16); FREE(s,shrink_zero);
    assert(!ALLOC(s,SIZE_MAX)); assert(!ALIGNED(s,16,3)); assert(!ALIGNED(s,16,0));
    void *small=ALIGNED(s,1,1); assert(small&&!((uintptr_t)small&15)); FREE(s,small);
    size_t before_zero=leased;
    assert(!ALLOC(s,0) && !ALIGNED(s,0,65536) && !REALLOC(s,NULL,0));
    assert(SIZE(s,NULL)==0 && SHRINK(s,NULL,0)==0 && leased==before_zero);
    void *null_realloc=REALLOC(s,NULL,17); assert(null_realloc); assert(!REALLOC(s,null_realloc,0));
    assert(!REALLOC(s,p,SIZE_MAX)&&SIZE(s,p)==48);
    dm_sa_hash h=HASH(s); assert(h.block_count==2&&h.allocated_bytes==128);
    dm_sa_hash h2=HASH(s); assert(!memcmp(&h,&h2,sizeof h));
    assert(HOOK(TF,size_t(*)(const void*))(s)<=1024*1024);
    assert(HOOK(LA,size_t(*)(const void*))(s)<=1024*1024);
    HOOK(TC,void(*)(const void*))(s); HOOK(CL,void(*)(const void*))(s);
    HOOK(D,void(*)(const void*))(s); assert(dumped==1);
    assert(WALK(s,NULL,NULL)==22);
    walk_context c={s,NULL,0,1}; assert(WALK(s,reentrant_callback,&c)==0&&c.count==1);
    c=(walk_context){s,NULL,0,0}; assert(WALK(s,reentrant_callback,&c)==0&&c.count==2);
    // Free the later address from the first callback; it must not be passed as
    // an already-known stale entry on the next snapshot iteration.
    c=(walk_context){s,(uintptr_t)p>(uintptr_t)q?p:q,0,0};
    assert(WALK(s,reentrant_callback,&c)==0&&c.count==1);
    if((uintptr_t)p>(uintptr_t)q) p=NULL; else q=NULL;
    expect_fatal(foreign_free,s);
    FREE(s,p); FREE(s,q); FREE(s,NULL);
    assert(WALK(s,reentrant_callback,&c)==2);
    pthread_t threads[4];
    for(unsigned n=0;n<4;n++) assert(!pthread_create(&threads[n],NULL,worker,s));
    for(unsigned n=0;n<4;n++) assert(!pthread_join(threads[n],NULL));
    assert(HASH(s).block_count==0);
    assert(!memcmp(calls,original_calls,sizeof calls));
    FINISH(s); assert(!leased&&releases==acquisitions); assert(object.self[0]==0);
    assert(dm_sa_adapter_is_managed(s)); expect_fatal(stale_allocate,s);
    for(unsigned n=0;n<8;n++) assert(object.before[n]==0xa7&&object.after[n]==0xa7);
    INIT(s,16384,0); assert(ALLOC(s,16)); assert(WALK(s,finalize_callback,s)==0); assert(!leased);
    INIT(s,16384,0); assert(ALLOC(s,32)); assert(ALLOC(s,32));
    c=(walk_context){s,NULL,0,0};
    assert(WALK(s,replace_callback,&c)==0 && c.count==1);
    assert(HASH(s).block_count==1); FINISH(s);
    // A fresh provided-buffer Initialize is the sole intentional ownership
    // transition. It forwards the unchanged pointer/extent, not a host block.
    HOOK(I,void(*)(void*,void*,size_t))(s,&forwarded_pointer,4096);
    assert(!dm_sa_adapter_is_managed(s)); assert(last_pointer==&forwarded_pointer);
    FINISH(s);
    // Every constructor alias with null buffer takes exactly the same owner.
    for(int n=CB1;n<=CC2;n++) {
        if(n<=CB2) HOOK(n,void(*)(void*,void*,size_t))(s,NULL,16384);
        else HOOK(n,void(*)(void*,void*,size_t,unsigned))(s,NULL,16384,1);
        assert(dm_sa_adapter_is_managed(s)); p=ALLOC(s,16); assert(p); FREE(s,p); FINISH(s);
    }
    NEW(s); assert(!dm_sa_adapter_is_managed(s)); // retire finalized tombstone
}
static void factory_reentry(void *s) {
    factory_target=s; factory_mode=FACTORY_REENTER; INIT(s,16384,0);
}
static void initialize_instance(void *s) { INIT(s,16384,0); }
static void finalize_instance(void *s) { FINISH(s); }
static void construct_instance(void *s) { NEW(s); }
typedef struct { void (*action)(void *); void *self; } thread_action;
static void *fatal_worker(void *arg) {
    thread_action *a=arg; fatal_in_thread=1; a->action(a->self); return NULL;
}
static void expect_thread_failure(void (*action)(void *),void *self,dm_sa_result expected) {
    thread_action a={action,self}; pthread_t t; void *result=NULL;
    assert(!pthread_create(&t,NULL,fatal_worker,&a)); assert(!pthread_join(t,&result));
    assert((uintptr_t)result==(uintptr_t)expected);
}
static void *initialize_worker(void *self) { initialize_instance(self); return NULL; }
static void initialization_lifecycle(void) {
    // A bounded alarm turns the pre-fix global-registry deadlock into a test
    // failure instead of leaving the suite hung indefinitely.
    alarm(20);
    unsigned char first[56],second[56]; NEW(first); NEW(second); INIT(first,16384,0);
    factory_target=second; factory_other=first; factory_mode=FACTORY_OTHER;
    INIT(second,16384,0); FINISH(second);
    expect_fatal(factory_reentry,second);
    factory_mode=FACTORY_BLOCK; factory_entered=0; factory_continue=0;
    pthread_t pending; assert(!pthread_create(&pending,NULL,initialize_worker,second));
    pthread_mutex_lock(&factory_mutex);
    while(!factory_entered) pthread_cond_wait(&factory_changed,&factory_mutex);
    pthread_mutex_unlock(&factory_mutex);
    assert(dm_sa_adapter_is_managed(second));
    expect_thread_failure(initialize_instance,second,DM_SA_BUSY);
    expect_thread_failure(finalize_instance,second,DM_SA_BUSY);
    expect_thread_failure(construct_instance,second,DM_SA_BUSY);
    expect_thread_failure(stale_allocate,second,DM_SA_BUSY);
    assert(HASH(first).block_count==0); // unrelated instances remain available
    pthread_mutex_lock(&factory_mutex); factory_continue=1;
    pthread_cond_broadcast(&factory_changed); pthread_mutex_unlock(&factory_mutex);
    assert(!pthread_join(pending,NULL)); assert(second[0]==1); FINISH(second);
    // Failure on a tombstone keeps it owned; failure on a fresh address removes
    // only its provisional record. Both support an explicit successful retry.
    factory_mode=FACTORY_FAIL;
    expect_thread_failure(initialize_instance,second,DM_SA_PROVIDER_FAILED);
    assert(dm_sa_adapter_is_managed(second) && !second[0]);
    factory_mode=FACTORY_NORMAL; INIT(second,16384,0); FINISH(second); NEW(second);
    factory_mode=FACTORY_FAIL;
    expect_thread_failure(initialize_instance,second,DM_SA_PROVIDER_FAILED);
    assert(!dm_sa_adapter_is_managed(second) && !second[0]);
    factory_mode=FACTORY_NORMAL; INIT(second,16384,0); FINISH(second); NEW(second);
    FINISH(first); NEW(first); factory_target=NULL; factory_other=NULL;
    alarm(0);
}
int main(void) {
    dm_sa_adapter_symbol symbols[22];
    for(size_t n=0;n<22;n++) { symbols[n]=(dm_sa_adapter_symbol){dm_sa_adapter_symbol_name(n),fixture[n]}; assert(!dm_sa_adapter_lookup(symbols[n].name)); }
    assert(!dm_sa_adapter_symbol_name(22));
    dm_sa_adapter_config c={.proofs=DM_SA_PROOF_ALL,.symbols=symbols,.symbol_count=22,
        .installed_bindings=DM_SA_ADAPTER_ALL_BINDINGS,.forward_allocate=profile_allocate,.allocation_alignment=allocation_alignment,.make_config=make_config,.dump=diagnostic,.fatal=fatal};
    for(unsigned n=0;n<5;n++) { c.proofs=DM_SA_PROOF_ALL^(1u<<n); assert(dm_sa_adapter_install(&c)==DM_SA_INVALID); }
    c.proofs=DM_SA_PROOF_ALL;
    for(unsigned n=0;n<22;n++) {
        c.installed_bindings=DM_SA_ADAPTER_ALL_BINDINGS^(UINT64_C(1)<<n);
        assert(dm_sa_adapter_install(&c)==DM_SA_INVALID); assert(!dm_sa_adapter_lookup(symbols[A].name));
    }
    c.installed_bindings=DM_SA_ADAPTER_ALL_BINDINGS;
    c.allocation_alignment=NULL; assert(dm_sa_adapter_install(&c)==DM_SA_INVALID); c.allocation_alignment=allocation_alignment;
    c.symbol_count=21; assert(dm_sa_adapter_install(&c)==DM_SA_INVALID); c.symbol_count=22;
    dm_sa_adapter_symbol old=symbols[0]; symbols[0]=symbols[1]; assert(dm_sa_adapter_install(&c)==DM_SA_INVALID); symbols[0]=old;
    symbols[0].name="_ZN2nn3mem17StandardAllocatorD1Ev"; assert(dm_sa_adapter_install(&c)==DM_SA_INVALID); symbols[0]=old;
    symbols[0].function=NULL; assert(dm_sa_adapter_install(&c)==DM_SA_INVALID); symbols[0]=old;
    assert(dm_sa_adapter_install(&c)==DM_SA_OK); assert(dm_sa_adapter_install(&c)==DM_SA_BUSY);
    for(size_t n=0;n<22;n++) assert(dm_sa_adapter_lookup(symbols[n].name));
    assert(!dm_sa_adapter_lookup("_ZN2nn3mem17StandardAllocatorD1Ev")); assert(!dm_sa_adapter_lookup(NULL));
    forwarding(); assert(!policy_calls); managed(); initialization_lifecycle();
    puts("StandardAllocator adapter: complete synthetic binding/forwarding/ownership/ABI contracts passed");
}
