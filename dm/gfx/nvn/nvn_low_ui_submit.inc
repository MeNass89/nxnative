// Include after up_mfx_scaler_for, before nvn_upscale_recipe.inc. Never calls a lazy texture resolver.
#include "nvn_ui_detached_resources_pooled.inc"
static LowUIPacket *g_lui_submit;
static BOOL lui_copy_ready(void){
    if(g_uid_copy_pipeline&&g_uid_copy_table)return YES;
    if(low_ui_armed())return NO;const char *path=getenv("DM_UI_SNAPSHOT_LIBRARY");if(!path||!*path)return NO;
    NSError *error=nil;id<MTLLibrary> lib=[g_dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
    id<MTLFunction> fn=[lib newFunctionWithName:@"ui_snapshot_copy"];if(!fn)return NO;
    id<MTLComputePipelineState> pipeline=[g_dev newComputePipelineStateWithFunction:fn error:&error];
    MTL4ArgumentTableDescriptor *d=[MTL4ArgumentTableDescriptor new];d.maxBufferBindCount=1;
    id<MTL4ArgumentTable> table=pipeline?[g_dev newArgumentTableWithDescriptor:d error:&error]:nil;
    if(!pipeline||!table)return NO;g_uid_copy_pipeline=pipeline;g_uid_copy_table=table;return YES;
}
static BOOL lui_reusable(LowUIPacket *p){ // g_lui_mu held; event value is actual GPU progress, not a CPU label.
    if(!p.epoch)return YES;if(!p.epochClosed||p.activePass)return NO;
    if(p.taken)return p.released&&p.readerEvent&&p.readerEvent.signaledValue>=p.readerValue;
    return p.nativeQualified&&p.nativeFence&&g_ev.signaledValue>=p.nativeFence;
}
static void low_ui_prepare(id<MTLTexture> targetR11,id<MTLTexture> trueLow1536){
    g_lui_submit=nil;
    if(!rec_on()||!g_cb)return;
    BOOL shape=targetR11&&trueLow1536&&targetR11.pixelFormat==MTLPixelFormatRG11B10Float&&targetR11.width==2048&&targetR11.height==1152&&targetR11.sampleCount==1&&
        trueLow1536.pixelFormat==MTLPixelFormatRGBA16Float&&trueLow1536.width==1536&&trueLow1536.height==864&&trueLow1536.sampleCount==1;
    if(!shape||!lui_copy_ready()){
        atomic_fetch_add(&g_lui_stats[LS_REFUSE],1);rec_cb_block(g_cb,^(id<MTL4CommandBuffer> cb){(void)cb;lui_switch(nil);});return;
    }
    BOOL grow=!low_ui_armed();LowUIPacket *p=nil;
    // Packet allocation is warmup only. Parent's ring/readers contract protects each reuse.
    for(unsigned n=0;n<LUI_RING;n++){
        unsigned i=(g_lui_next+n)%LUI_RING;
        pthread_mutex_lock(&g_lui_mu);LowUIPacket *q=g_lui_ring[i];BOOL free=q&&lui_reusable(q);pthread_mutex_unlock(&g_lui_mu);
        if(!q&&grow){
            q=[LowUIPacket new];q.arena=[g_dev newBufferWithLength:1u<<20 options:MTLResourceStorageModeShared];
            if(!q.arena)continue;res_add(q.arena);q.pool=[NSMutableDictionary dictionary];q.tablePool=[NSMutableDictionary dictionary];
            pthread_mutex_lock(&g_lui_mu);g_lui_ring[i]=q;pthread_mutex_unlock(&g_lui_mu);atomic_fetch_add(&g_lui_stats[LS_ALLOC],1);free=YES;
        }
        if(!free)continue;
        pthread_mutex_lock(&g_lui_mu);
        if(q.epoch)atomic_fetch_add(&g_lui_stats[LS_REUSE],1);
        q.epoch=++g_lui_epoch;q.frame=atomic_load(&g_cur_frame);q.target=targetR11;q.lowSource=trueLow1536;
        q.cursor=0;q.cache=[NSMutableDictionary dictionary];q.passes=[NSMutableArray array];q.sealedPasses=nil;q.sealedTableSets=nil;q.activePass=nil;q.ops=0;
        q.eligible=YES;q.sealed=NO;q.epochClosed=NO;q.taken=NO;q.released=NO;q.nativeQualified=NO;q.nativeFence=0;q.readerEvent=nil;q.readerValue=0;q.submitFinal=NO;
        p=q;g_lui_next=(i+1)%LUI_RING;pthread_mutex_unlock(&g_lui_mu);break;
    }
    if(!p)atomic_fetch_add(&g_lui_stats[LS_RING_MISS],1);else atomic_fetch_add(&g_lui_stats[LS_PREPARE],1);
    g_lui_submit=p;
    rec_cb_block(g_cb,^(id<MTL4CommandBuffer> cb){(void)cb;lui_switch(p);});
}
static void low_ui_note_draw(NvProg *pr,const p_draw *dw,const p_draw_idx *di,int cur_pull){
    LowUIPacket *p=g_lui_submit;if(!p||!rec_on())return;
    pthread_mutex_lock(&g_lui_mu);BOOL enabled=!p.submitFinal;pthread_mutex_unlock(&g_lui_mu);if(!enabled)return;
    if(R.rt.n!=1||!R.rt.color[0]||lui_root(tex_made(R.rt.color[0]))!=lui_root(p.target))return;
    BOOL ro=pr&&pr->sh[0]&&pr->sh[1]&&!pr->sh[2]&&!pr->sh[3]&&!pr->sh[4]&&!S.tf_on;
    for(int st=0;ro&&st<2;st++)for(int i=0;i<pr->sh[st]->rf.nb;i++)if(pr->sh[st]->rf.b[i].rw||pr->sh[st]->rf.b[i].atom)ro=NO;
    NSUInteger cursor=p.cursor;NSDictionary *plan=uid_draw_plan_pooled(pr,dw,di,cur_pull,p.arena,&cursor,p.cache,p.pool,!low_ui_armed());p.cursor=cursor;
    if(![plan[@"valid"] boolValue])lui_refuse_plan(p,plan[@"reason"],pr&&pr->sh[0]?pr->sh[0]->key:"",pr&&pr->sh[1]?pr->sh[1]->key:"",di!=NULL,di?di->itype:-1);
    atomic_fetch_add(&g_lui_stats[LS_POOL_CREATE],[plan[@"pool_created_textures"] count]);
    atomic_fetch_add(&g_lui_stats[LS_POOL_REUSE],[plan[@"pool_reused_textures"] unsignedLongLongValue]);
    atomic_fetch_add(&g_lui_stats[LS_BUFFER_BYTES],[plan[@"copied_buffer_bytes"] unsignedLongLongValue]);
    atomic_fetch_add(&g_lui_stats[LS_TEXTURE_COPIES],[plan[@"texture_copies"] count]);
    NSDictionary *m=@{@"vs":pr&&pr->sh[0]?@(pr->sh[0]->key):@"",@"fs":pr&&pr->sh[1]?@(pr->sh[1]->key):@"",@"read_only":@(ro),@"native_pass":@(g_stat_pass),@"private_plan":plan};
    rec_t *r=rec_put(RO_UI_META,1);r->a[0]=rec_obj(m);
}
static void low_ui_submit_final(id<MTLTexture> source){
    LowUIPacket *p=g_lui_submit;if(!p||!rec_on()||!g_cb||!source||lui_root(source)!=lui_root(p.target))return;
    pthread_mutex_lock(&g_lui_mu);BOOL first=!p.submitFinal;p.submitFinal=YES;pthread_mutex_unlock(&g_lui_mu);if(!first)return;
    rec_cb_block(g_cb,^(id<MTL4CommandBuffer> cb){(void)cb;lui_seal(p,source);});
}
