static BOOL low_ui_known_read(const rec_t *r,id<MTLTexture> target);
// Product UI prefix packets. Include immediately before rec_exec, after rec_t/g_rx_cb/g_rx_enc.
// No diagnostic readbacks, frame files, waits or native table writes from replay.
static id<MTLComputePipelineState> g_uid_copy_pipeline;
static id<MTL4ArgumentTable> g_uid_copy_table;
enum { LUI_RING=6,LUI_TABLES=128,LUI_MAX_OPS=8192,LUI_MAX_PASSES=16 };
enum { LS_ALLOC,LS_PREPARE,LS_RING_MISS,LS_REUSE,LS_REFUSE,LS_TAKE,LS_RENDER,LS_RELEASE,LS_POOL_CREATE,LS_POOL_REUSE,LS_BUFFER_BYTES,LS_TEXTURE_COPIES,LS_TABLE_OVERFLOW,LS_PSO_MISS,LS_ABANDON,LS_TABLE_ALLOC,LS_TABLE_REUSE,LS_TABLE_RUNTIME_ALLOC,LS_TABLE_MISS,LS_COUNT };
static _Atomic uint64_t g_lui_stats[LS_COUNT];
@interface LowUIPass : NSObject
@property MTL4RenderPassDescriptor *descriptor;
@property NSMutableData *records;
@property NSArray *initial;
@property NSMutableArray *keep;
@property NSUInteger draws,notes;
@property BOOL invalid;
@end
@implementation LowUIPass @end
@interface LowUIPacket : NSObject
@property id<MTLBuffer> arena;
@property NSMutableDictionary *cache,*pool,*tablePool;
@property NSUInteger cursor,ops;
@property uint64_t frame,epoch,nativeFence;
@property id<MTLTexture> target,lowSource;
@property NSMutableArray *passes;
@property NSArray *sealedPasses,*sealedTableSets;
@property LowUIPass *activePass;
@property BOOL eligible,sealed,epochClosed,taken,released,nativeQualified,submitFinal;
@property id<MTLSharedEvent> readerEvent;
@property uint64_t readerValue;
@end
@implementation LowUIPacket @end
@interface LowUILease : NSObject
@property LowUIPacket *packet;
@property uint64_t epoch,frame;
@property id<MTLTexture> source,targetRoot;
@property NSArray *passes;
@property NSDictionary *pipelines;
@property NSArray *tableSets;
@property NSUInteger renderCount;
@end
@implementation LowUILease @end
static LowUIPacket *g_lui_ring[LUI_RING],*g_lui_encoded;
static pthread_mutex_t g_lui_mu=PTHREAD_MUTEX_INITIALIZER;
static unsigned g_lui_next;
static uint64_t g_lui_epoch;
typedef struct { uint64_t table,addr[31],texture,sampler;uint32_t mask;uint8_t tv,sv,bad; } LowUITable;
static LowUITable g_lui_tables[LUI_TABLES];static id g_lui_table_owners[LUI_TABLES];static unsigned g_lui_ntables;
static int low_ui_armed(void){static _Atomic int armed;if(atomic_load(&armed))return 1;const char*p=getenv("DM_LOW_ROUTE_ARM");int ready=p&&*p&&!access(p,F_OK);if(ready)atomic_store(&armed,1);return ready;}
static id<MTLTexture> lui_root(id<MTLTexture> t){id<MTLTexture> root=t;while(root.parentTexture)root=root.parentTexture;return root;}
// Diagnostic-only, bounded first eligible->false transitions. No heap work here.
enum { LUI_REJECT_MAX=16 };
typedef struct { const char *file,*reason;unsigned line;uint64_t count,frame,epoch,op,ops,passes,draws,notes;BOOL active,sealed,submitFinal;
    char planReason[192],vs[17],fs[17];BOOL indexed;int32_t indexType;
    uint64_t copySource,copyDestination,copySourceLength,copyDestinationLength,copySourceOffset,copyDestinationOffset,copyBytes,arena,arenaLength,worldRoot,lowRoot,worldStorage,lowStorage;
    BOOL sourceIsBuffer,destinationIsBuffer,destinationIsArena,worldBuffer,worldHeap,lowBuffer,lowHeap;
} LowUIReject;
static LowUIReject g_lui_reject[LUI_REJECT_MAX];static unsigned g_lui_reject_n;static uint64_t g_lui_reject_overflow;
// g_lui_mu held. Returns a newly encountered callsite, for optional logging AFTER unlock.
static int lui_reject_locked(LowUIPacket*p,const char*file,unsigned line,const char*reason,uint64_t op,LowUIPass*pass){
    unsigned i=0;for(;i<g_lui_reject_n;i++)if(g_lui_reject[i].line==line && g_lui_reject[i].file==file)break;
    if(i==LUI_REJECT_MAX){g_lui_reject_overflow++;return -1;}
    BOOL first=i==g_lui_reject_n;if(first){g_lui_reject_n++;LowUIPass*a=pass?:p.activePass;
        g_lui_reject[i]=(LowUIReject){.file=file,.reason=reason,.line=line,.frame=p.frame,.epoch=p.epoch,.op=op,.ops=p.ops,.passes=p.passes.count,.draws=a.draws,.notes=a.notes,.active=a!=nil,.sealed=p.sealed,.submitFinal=p.submitFinal};}
    g_lui_reject[i].count++;return first?(int)i:-1;
}
static void lui_copy_context_locked(int index,LowUIPacket*p,const rec_t*record){
    if(index<0 || !record || record->op!=RO_C_COPY_BB)return;LowUIReject*d=&g_lui_reject[index];
    id source=rec_id(record->a[0]),destination=rec_id(record->a[1]);
    d->copySource=(uint64_t)(uintptr_t)(__bridge void*)source;d->copyDestination=(uint64_t)(uintptr_t)(__bridge void*)destination;
    d->sourceIsBuffer=[source conformsToProtocol:@protocol(MTLBuffer)];d->destinationIsBuffer=[destination conformsToProtocol:@protocol(MTLBuffer)];
    if(d->sourceIsBuffer)d->copySourceLength=[(id<MTLBuffer>)source length];if(d->destinationIsBuffer)d->copyDestinationLength=[(id<MTLBuffer>)destination length];
    d->copySourceOffset=record->a[2];d->copyDestinationOffset=record->a[3];d->copyBytes=record->a[4];d->arena=(uint64_t)(uintptr_t)(__bridge void*)p.arena;d->arenaLength=p.arena.length;d->destinationIsArena=destination==p.arena;
    id<MTLTexture>world=lui_root(p.target),low=lui_root(p.lowSource);d->worldRoot=(uint64_t)(uintptr_t)(__bridge void*)world;d->lowRoot=(uint64_t)(uintptr_t)(__bridge void*)low;
    d->worldStorage=world.storageMode;d->lowStorage=low.storageMode;d->worldBuffer=world.buffer!=nil;d->worldHeap=world.heap!=nil;d->lowBuffer=low.buffer!=nil;d->lowHeap=low.heap!=nil;
}
static void lui_log_first(int index){
    if(index<0 || !getenv("DM_LOW_ROUTE_FIRST_REFUSAL"))return;
    // The first-context fields never change after publication; count is excluded.
    LowUIReject*r=&g_lui_reject[index];
    MLOG("LOW_UI_FIRST_REFUSAL %s:%u reason=%s frame=%llu epoch=%llu op=%llu phase=%s passes=%llu draws=%llu notes=%llu ops=%llu sealed=%d submitFinal=%d",r->file,r->line,r->reason,(unsigned long long)r->frame,(unsigned long long)r->epoch,(unsigned long long)r->op,r->active?"render":"between passes",(unsigned long long)r->passes,(unsigned long long)r->draws,(unsigned long long)r->notes,(unsigned long long)r->ops,r->sealed,r->submitFinal);
    if(r->planReason[0])MLOG("LOW_UI_FIRST_PLAN reason=%s VS=%s FS=%s indexed=%d itype=%d frame=%llu",r->planReason,r->vs,r->fs,r->indexed,r->indexType,(unsigned long long)r->frame);
    if(r->op==RO_C_COPY_BB)MLOG("LOW_UI_FIRST_BB src=%llx len=%llu off=%llu dst=%llx len=%llu off=%llu bytes=%llu arena=%llx arenaLen=%llu dstArena=%d C=%llx storage=%llu buffer=%d heap=%d low=%llx storage=%llu buffer=%d heap=%d",(unsigned long long)r->copySource,(unsigned long long)r->copySourceLength,(unsigned long long)r->copySourceOffset,(unsigned long long)r->copyDestination,(unsigned long long)r->copyDestinationLength,(unsigned long long)r->copyDestinationOffset,(unsigned long long)r->copyBytes,(unsigned long long)r->arena,(unsigned long long)r->arenaLength,r->destinationIsArena,(unsigned long long)r->worldRoot,(unsigned long long)r->worldStorage,r->worldBuffer,r->worldHeap,(unsigned long long)r->lowRoot,(unsigned long long)r->lowStorage,r->lowBuffer,r->lowHeap);
}
static void lui_refuse_at(LowUIPacket*p,const char*file,unsigned line,const char*reason,uint64_t op,LowUIPass*pass){
    int first=-1;pthread_mutex_lock(&g_lui_mu);if(p.eligible){first=lui_reject_locked(p,file,line,reason,op,pass);p.eligible=NO;atomic_fetch_add(&g_lui_stats[LS_REFUSE],1);}pthread_mutex_unlock(&g_lui_mu);lui_log_first(first);
}
static void lui_refuse_record_at(LowUIPacket*p,const char*file,unsigned line,const char*reason,const rec_t*record){
    int first=-1;pthread_mutex_lock(&g_lui_mu);if(p.eligible){first=lui_reject_locked(p,file,line,reason,record->op,nil);lui_copy_context_locked(first,p,record);p.eligible=NO;atomic_fetch_add(&g_lui_stats[LS_REFUSE],1);}pthread_mutex_unlock(&g_lui_mu);lui_log_first(first);
}
static void lui_refuse_plan_at(LowUIPacket*p,const char*file,unsigned line,NSString*reason,const char*vs,const char*fs,BOOL indexed,int32_t indexType){
    int first=-1;pthread_mutex_lock(&g_lui_mu);if(p.eligible){first=lui_reject_locked(p,file,line,"submit detached plan invalid",UINT64_MAX,nil);
        if(first>=0){LowUIReject*d=&g_lui_reject[first];snprintf(d->planReason,sizeof d->planReason,"%s",reason.UTF8String?:"missing reason");snprintf(d->vs,sizeof d->vs,"%.16s",vs?:"");snprintf(d->fs,sizeof d->fs,"%.16s",fs?:"");d->indexed=indexed;d->indexType=indexType;}
        p.eligible=NO;atomic_fetch_add(&g_lui_stats[LS_REFUSE],1);}pthread_mutex_unlock(&g_lui_mu);lui_log_first(first);
}
#define lui_refuse_record(p,reason,record) lui_refuse_record_at(p,__FILE__,__LINE__,reason,record)
#define lui_refuse_plan(p,reason,vs,fs,indexed,indexType) lui_refuse_plan_at(p,__FILE__,__LINE__,reason,vs,fs,indexed,indexType)
#define lui_refuse(p,reason,op) lui_refuse_at(p,__FILE__,__LINE__,reason,op,nil)
#define lui_refuse_pass(p,pass,reason,op) lui_refuse_at(p,__FILE__,__LINE__,reason,op,pass)

static LowUITable *lui_table(uint64_t object,BOOL make){
    for(unsigned i=0;i<g_lui_ntables;i++)if(g_lui_tables[i].table==object)return &g_lui_tables[i];
    if(!make||!object)return NULL;
    if(g_lui_ntables==LUI_TABLES){atomic_fetch_add(&g_lui_stats[LS_TABLE_OVERFLOW],1);return NULL;}
    unsigned i=g_lui_ntables++;g_lui_table_owners[i]=rec_id(object);g_lui_tables[i].table=object;return &g_lui_tables[i];
}
static void lui_shadow(const rec_t *r){
    if(r->op!=RO_T_ADDR&&r->op!=RO_T_TEX&&r->op!=RO_T_SAMP)return;
    LowUITable *t=lui_table(r->a[0],YES);if(!t)return;
    uint64_t slot=r->a[2];if(r->op==RO_T_ADDR){if(slot>=31){t->bad=1;return;}t->addr[slot]=r->a[1];t->mask|=1u<<slot;}
    else if(slot){t->bad=1;}
    else if(r->op==RO_T_TEX){t->texture=r->a[1];t->tv=1;}else{t->sampler=r->a[1];t->sv=1;}
}
static NSArray *lui_initial(void){
    NSMutableArray *result=[NSMutableArray array];
    for(unsigned i=0;i<g_lui_ntables;i++){LowUITable *t=&g_lui_tables[i];
        for(unsigned slot=0;slot<31;slot++)if(t->mask>>slot&1){rec_t r={0};r.op=RO_T_ADDR;r.a[0]=t->table;r.a[1]=t->addr[slot];r.a[2]=slot;[result addObject:[NSData dataWithBytes:&r length:sizeof r]];}
        if(t->tv){rec_t r={0};r.op=RO_T_TEX;r.a[0]=t->table;r.a[1]=t->texture;[result addObject:[NSData dataWithBytes:&r length:sizeof r]];}
        if(t->sv){rec_t r={0};r.op=RO_T_SAMP;r.a[0]=t->table;r.a[1]=t->sampler;[result addObject:[NSData dataWithBytes:&r length:sizeof r]];}
    }return result;
}
static void lui_barrier(id<MTL4ComputeCommandEncoder> e,int before){
    if(before)[e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice|MTL4VisibilityOptionResourceAlias];
    else[e barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice|MTL4VisibilityOptionResourceAlias];
}
#include "nvn_low_ui_helpers.inc"
#include "nvn_low_ui_context.inc"
static BOOL lui_descriptor(MTL4RenderPassDescriptor *d){
    id<MTLTexture> t=d.colorAttachments[0].texture;
    if(!t||t.pixelFormat!=MTLPixelFormatRG11B10Float||t.width!=2048||t.height!=1152||t.sampleCount!=1||d.depthAttachment.texture||d.stencilAttachment.texture||
       d.colorAttachments[0].level||d.colorAttachments[0].slice||d.colorAttachments[0].depthPlane||d.renderTargetArrayLength>1)return NO;
    for(int i=1;i<8;i++)if(d.colorAttachments[i].texture)return NO;
    return d.colorAttachments[0].loadAction==MTLLoadActionLoad||d.colorAttachments[0].loadAction==MTLLoadActionClear;
}
static NSDictionary *lui_cached_pipelines(NSArray *passes){
    NSMutableDictionary *out=[NSMutableDictionary dictionary];
    for(NSDictionary *pass in passes){NSData *data=pass[@"records"];if(data.length%sizeof(rec_t))return nil;const rec_t *r=data.bytes;
        for(NSUInteger i=0;i<data.length/sizeof(rec_t);i++)if(r[i].op==RO_R_PSO){id original=rec_id(r[i].a[0]);id ps=uis_float_pipeline_cached(original);if(!ps){atomic_fetch_add(&g_lui_stats[LS_PSO_MISS],1);return nil;}out[@(r[i].a[0])]=ps;}
    }return out;
}
static void low_ui_before(const rec_t *r){
    lui_shadow(r);LowUIPacket *p=g_lui_encoded;if(!p||p.sealed)return;
    if(!p.activePass && (r->op==RO_C_DISPATCH||r->op==RO_C_DISPATCH_IND||r->op==RO_C_COPY_BB||r->op==RO_C_COPY_BT||r->op==RO_C_COPY_BT_OPT||r->op==RO_C_COPY_TB||r->op==RO_C_COPY_TB_OPT||r->op==RO_C_COPY_TT||r->op==RO_C_FILL)&&!low_ui_known_read(r,p.target)){lui_refuse_record(p,"outside render compute/copy/fill",r);}
    if(r->op==RO_RE_BEGIN){
        MTL4RenderPassDescriptor *d=rec_id(r->a[0]);id<MTLTexture> t=d.colorAttachments[0].texture;
        if(lui_root(t)!=lui_root(p.target))return;
        if(p.activePass||p.passes.count>=LUI_MAX_PASSES){lui_refuse(p,"overlapping pass or pass limit",r->op);return;}
        LowUIPass *a=[LowUIPass new];a.descriptor=[d copy];a.records=[NSMutableData data];a.keep=[NSMutableArray array];a.initial=lui_initial();
        a.invalid=!lui_descriptor(d);p.activePass=a;if(a.invalid)lui_refuse(p,"unsupported render descriptor",r->op);return;
    }
    LowUIPass *a=p.activePass;if(!a||r->op==RO_END)return;
    if(!lui_opcode(r->op)||p.ops>=LUI_MAX_OPS){a.invalid=YES;lui_refuse(p,"unsupported opcode or operation limit",r->op);return;}
    if(r->op==RO_R_TABLE){LowUITable *t=lui_table(r->a[0],NO);if(!t||t->bad){a.invalid=YES;lui_refuse(p,"missing or invalid native table",r->op);}}
    if(r->op==RO_R_AMP&&r->a[0]>1){a.invalid=YES;lui_refuse(p,"unsupported amplification",r->op);}
    if(r->op==RO_R_STORE_C&&(r->a[1]||r->a[0]!=MTLStoreActionStore)){a.invalid=YES;lui_refuse(p,"unsupported store action",r->op);}
    if(r->op==RO_R_PSO&&!low_ui_armed())uis_float_pipeline_warm(rec_id(r->a[0]));
    if(r->op==RO_UI_META){NSDictionary *m=rec_id(r->a[0]);a.notes++;if(![m[@"read_only"] boolValue]||![m[@"private_plan"][@"valid"] boolValue]){a.invalid=YES;lui_refuse(p,"draw metadata not read-only or plan invalid",r->op);}}
    if(r->op>=RO_R_DRAW&&r->op<=RO_R_DRAW_IDX_IND)a.draws++;
    rec_t x=*r;x.nobj=0;if(x.op==RO_R_VIS){x.a[0]=MTLVisibilityResultModeDisabled;x.a[1]=0;}
    for(unsigned i=0;i<r->nobj;i++)if(r->a[i])[a.keep addObject:rec_id(r->a[i])];
    if(r->op==RO_R_TABLE||r->op==RO_T_ADDR||r->op==RO_T_TEX||r->op==RO_T_SAMP)if(r->a[0])[a.keep addObject:rec_id(r->a[0])];
    [a.records appendBytes:&x length:sizeof x];p.ops++;
}
static void low_ui_after(const rec_t *r){
    if(r->op==RO_SIGNAL&&rec_id(r->a[0])==g_ev){
        pthread_mutex_lock(&g_lui_mu);
        for(unsigned i=0;i<LUI_RING;i++){LowUIPacket *p=g_lui_ring[i];if(!p)continue;
            if(p==g_lui_encoded&&p.sealed){p.nativeFence=r->a[1];p.nativeQualified=YES;}
            else if(p.epochClosed&&!p.nativeQualified){p.nativeFence=r->a[1];p.nativeQualified=YES;}
        }pthread_mutex_unlock(&g_lui_mu);
    }
    LowUIPacket *p=g_lui_encoded;LowUIPass *a=p.activePass;
    if(r->op!=RO_END||!a)return;p.activePass=nil;
    if(a.invalid||a.notes!=a.draws||(a.draws&&lui_validate(a.records,a.initial))){lui_refuse_pass(p,a,"render invalid or draw/plan coverage/validation",r->op);return;}
    if(!a.draws&&a.descriptor.colorAttachments[0].loadAction!=MTLLoadActionClear)return;
    if(a.draws){if(!g_uid_copy_pipeline||!g_uid_copy_table){lui_refuse_pass(p,a,"snapshot copy pipeline unavailable",r->op);return;}lui_encode_snapshots(a.records,(id<MTL4CommandBuffer>)g_rx_cb);}
    [p.passes addObject:@{@"records":[a.records copy],@"initial":a.initial,@"descriptor":a.descriptor,@"keep":[a.keep copy],@"draws":@(a.draws)}];
}
static void lui_switch(LowUIPacket *next){
    pthread_mutex_lock(&g_lui_mu);LowUIPacket *old=g_lui_encoded;
    int first=-1;if(old){old.epochClosed=YES;if(!old.sealed||old.activePass){if(old.eligible)first=lui_reject_locked(old,__FILE__,__LINE__,"abandoned before seal or pass end",UINT64_MAX,nil);old.eligible=NO;atomic_fetch_add(&g_lui_stats[LS_ABANDON],1);}}
    g_lui_encoded=next;pthread_mutex_unlock(&g_lui_mu);lui_log_first(first);
}
static void lui_seal(LowUIPacket *p,id<MTLTexture> source){
    if(g_lui_encoded!=p||p.sealed)return;
    if(p.activePass||lui_root(source)!=lui_root(p.target))lui_refuse(p,"seal active pass or source root mismatch",UINT64_MAX);
    NSArray *frozen=[p.passes copy];NSDictionary *pipelines=lui_cached_pipelines(frozen);
    NSArray *tableSets=lui_packet_table_sets(p,frozen,!low_ui_armed());
    int first=-1;pthread_mutex_lock(&g_lui_mu);p.sealedPasses=frozen;p.sealedTableSets=tableSets;p.sealed=YES;if((!pipelines||!tableSets)&&p.eligible){first=lui_reject_locked(p,__FILE__,__LINE__,!pipelines?"seal missing float pipelines":"seal missing private table sets",UINT64_MAX,nil);p.eligible=NO;atomic_fetch_add(&g_lui_stats[LS_REFUSE],1);}pthread_mutex_unlock(&g_lui_mu);lui_log_first(first);
}
static id low_ui_take(uint64_t frame,id<MTLTexture> expectedWorldRoot){
    if(!low_ui_armed())return nil;LowUIPacket *p=nil;NSArray *passes=nil,*tableSets=nil;uint64_t epoch=0;
    pthread_mutex_lock(&g_lui_mu);
    for(unsigned i=0;i<LUI_RING;i++){LowUIPacket *q=g_lui_ring[i];if(q&&q.frame==frame&&q.sealed&&q.eligible&&!q.taken&&!q.epochClosed&&
        lui_root(q.target)==lui_root(expectedWorldRoot)&&q.lowSource.pixelFormat==MTLPixelFormatRGBA16Float&&q.lowSource.width==1536&&q.lowSource.height==864){p=q;passes=q.sealedPasses;tableSets=q.sealedTableSets;epoch=q.epoch;break;}}
    pthread_mutex_unlock(&g_lui_mu);if(!p)return nil;
    NSDictionary *pipelines=lui_cached_pipelines(passes);if(!pipelines||tableSets.count!=2)return nil;
    pthread_mutex_lock(&g_lui_mu);if(p.epoch!=epoch||p.taken||p.epochClosed||!p.eligible){pthread_mutex_unlock(&g_lui_mu);return nil;}p.taken=YES;pthread_mutex_unlock(&g_lui_mu);
    LowUILease *l=[LowUILease new];l.packet=p;l.epoch=epoch;l.frame=frame;l.source=p.lowSource;l.targetRoot=lui_root(p.target);l.passes=passes;l.pipelines=pipelines;l.tableSets=tableSets;l.renderCount=0;
    atomic_fetch_add(&g_lui_stats[LS_TAKE],1);return l;
}
static id<MTLTexture> low_ui_source(id packet){
    if(![packet isKindOfClass:LowUILease.class])return nil;LowUILease *l=packet;
    pthread_mutex_lock(&g_lui_mu);BOOL valid=l.packet.epoch==l.epoch&&l.packet.taken&&!l.packet.released&&!l.packet.epochClosed;pthread_mutex_unlock(&g_lui_mu);
    return valid?l.source:nil;
}
static NSArray *low_ui_render(id packet,id<MTL4CommandBuffer> cb,id<MTLTexture> output){
    if(![packet isKindOfClass:LowUILease.class]||!cb||!output||!output.width||!output.height||output.sampleCount!=1||
       (output.pixelFormat!=MTLPixelFormatRGBA16Float&&output.pixelFormat!=MTLPixelFormatRG11B10Float))return nil;
    LowUILease *l=packet;pthread_mutex_lock(&g_lui_mu);BOOL valid=l.packet.epoch==l.epoch&&l.packet.taken&&!l.packet.released&&l.renderCount<2;
    NSUInteger render=l.renderCount;if(valid)l.renderCount++;pthread_mutex_unlock(&g_lui_mu);if(!valid)return nil;
    if(l.tableSets.count!=2||[l.tableSets[render] count]!=l.passes.count)return nil;
    NSMutableArray *prepared=[NSMutableArray array],*owners=[NSMutableArray arrayWithObject:l];
    // Preflight EVERY pass and its already-pooled tables before the first pixel draw.
    NSUInteger ordinal=0,tableCount=0;
    for(NSDictionary *pass in l.passes){NSData *data=pass[@"records"];MTL4RenderPassDescriptor *d=pass[@"descriptor"];if(!lui_descriptor(d)||data.length%sizeof(rec_t))return nil;
        const rec_t *r=data.bytes;for(NSUInteger i=0;i<data.length/sizeof(rec_t);i++){if(!lui_opcode(r[i].op))return nil;if(r[i].op==RO_R_PSO&&output.pixelFormat==MTLPixelFormatRGBA16Float&&!l.pipelines[@(r[i].a[0])])return nil;}
        if([pass[@"draws"] unsignedIntegerValue]&&lui_validate(data,pass[@"initial"]))return nil;
        NSDictionary *tables=l.tableSets[render][ordinal++];if(![tables isKindOfClass:NSDictionary.class])return nil;
        for(NSUInteger i=0;i<data.length/sizeof(rec_t);i++)if(r[i].op==RO_R_TABLE&&!tables[@(r[i].a[0])])return nil;
        tableCount+=tables.count;[prepared addObject:tables];
    }
    for(NSUInteger i=0;i<l.passes.count;i++){NSDictionary *pass=l.passes[i];NSArray *tables=lui_replay_on(cb,pass[@"records"],pass[@"initial"],pass[@"descriptor"],output,prepared[i],l.pipelines);if(!tables)return nil;[owners addObjectsFromArray:tables];}
    [owners addObjectsFromArray:l.pipelines.allValues];atomic_fetch_add(&g_lui_stats[LS_TABLE_REUSE],tableCount);atomic_fetch_add(&g_lui_stats[LS_RENDER],1);return owners;
}
static void low_ui_release(id packet,id<MTLSharedEvent> event,uint64_t value){
    if(![packet isKindOfClass:LowUILease.class]||!event||!value)return;LowUILease *l=packet;
    pthread_mutex_lock(&g_lui_mu);if(l.packet.epoch==l.epoch&&l.packet.taken&&!l.packet.released){l.packet.readerEvent=event;l.packet.readerValue=value;l.packet.released=YES;atomic_fetch_add(&g_lui_stats[LS_RELEASE],1);}pthread_mutex_unlock(&g_lui_mu);
}
static NSDictionary *low_ui_stats(void){
    const char *names[LS_COUNT]={"packet_allocations","prepared","ring_misses","packet_reuses","refused","taken","renders","releases","pool_created","pool_reused","copied_buffer_bytes","texture_copies","table_overflows","float_pso_misses","abandoned","private_table_allocations","private_table_reuses","private_table_runtime_allocations","private_table_pool_misses"};NSMutableDictionary *out=[NSMutableDictionary dictionary];
    for(int i=0;i<LS_COUNT;i++)out[@(names[i])]=@(atomic_load(&g_lui_stats[i]));
    NSMutableArray*reasons=[NSMutableArray array];pthread_mutex_lock(&g_lui_mu);
    for(unsigned i=0;i<g_lui_reject_n;i++){LowUIReject*r=&g_lui_reject[i];[reasons addObject:@{@"file":@(r->file),@"line":@(r->line),@"reason":@(r->reason),@"count":@(r->count),@"first_frame":@(r->frame),@"first_epoch":@(r->epoch),@"first_op":@(r->op),@"first_phase":r->active?@"render":@"between passes",@"first_ops":@(r->ops),@"first_passes":@(r->passes),@"first_draws":@(r->draws),@"first_notes":@(r->notes),@"first_sealed":@(r->sealed),@"first_submit_final":@(r->submitFinal),@"plan_reason":@(r->planReason),@"vs":@(r->vs),@"fs":@(r->fs),@"indexed":@(r->indexed),@"index_type":@(r->indexType),
        @"copy_source":@(r->copySource),@"copy_destination":@(r->copyDestination),@"copy_source_is_buffer":@(r->sourceIsBuffer),@"copy_destination_is_buffer":@(r->destinationIsBuffer),@"copy_source_length":@(r->copySourceLength),@"copy_destination_length":@(r->copyDestinationLength),@"copy_source_offset":@(r->copySourceOffset),@"copy_destination_offset":@(r->copyDestinationOffset),@"copy_bytes":@(r->copyBytes),@"arena":@(r->arena),@"arena_length":@(r->arenaLength),@"destination_is_arena":@(r->destinationIsArena),@"world_root":@(r->worldRoot),@"low_root":@(r->lowRoot),@"world_storage":@(r->worldStorage),@"low_storage":@(r->lowStorage),@"world_buffer_backed":@(r->worldBuffer),@"world_heap_backed":@(r->worldHeap),@"low_buffer_backed":@(r->lowBuffer),@"low_heap_backed":@(r->lowHeap)}];}
    out[@"first_refusals"]=reasons;out[@"first_refusal_overflow"]=@(g_lui_reject_overflow);pthread_mutex_unlock(&g_lui_mu);return out;
}
