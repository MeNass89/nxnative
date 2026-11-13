// Private POOLED resource PLAN only. Caller guarantees all prior packet readers have finished before resetting cache/cursor. No command encoding, waits, CPU payload reads, or visible-state changes.
#include "nvn_ui_detached_bounds_pooled.h"
@interface DmUIDPooledResourceBuild : NSObject {
@public id<MTLBuffer> arena; NSUInteger at, copied; uint64_t textureBytes; BOOL textureBytesKnown;
    NSMutableDictionary *cache,*pending,*pool,*poolPending; BOOL allowGrow; NSUInteger poolReused; NSMutableArray *keep,*jobs,*textures,*made; NSString *reason;
}
@end
@implementation DmUIDPooledResourceBuild @end
typedef struct { uint64_t source,destination,bytes; } uidp_copy_job;
_Static_assert(sizeof(uidp_copy_job)==24,"UI snapshot GPU copy job ABI");
static int uidp_fail(DmUIDPooledResourceBuild *b,NSString *s){if(!b->reason)b->reason=s;return 0;}
static int uidp_span(id<MTLBuffer> b,uint64_t gpu,NSUInteger n){
    return b && gpu>=b.gpuAddress && gpu-b.gpuAddress<=b.length && n<=b.length-(gpu-b.gpuAddress);
}
static int uidp_alloc(DmUIDPooledResourceBuild *b,NSUInteger n,NSUInteger *off){
    if(b->at>NSUIntegerMax-15)return uidp_fail(b,@"arena alignment overflow");
    NSUInteger a=(b->at+15)&~(NSUInteger)15;
    if(a>b->arena.length||n>b->arena.length-a)return uidp_fail(b,@"private bounded arena exhausted");
    *off=a;b->at=a+n;return 1;
}
static const uidp_shader *uidp_proof(NvShader *sh,int stage){
    for(NSUInteger i=0;i<sizeof(uidp_shaders)/sizeof(uidp_shaders[0]);i++)
        if(sh->stage==stage&&uidp_shaders[i].stage==stage&&!strcmp(sh->key,uidp_shaders[i].key))return &uidp_shaders[i];
    return NULL;
}
static id<MTLBuffer> uidp_host_span(uint64_t gpu,NSUInteger n){
    if(g_cur_arena && uidp_span(g_cur_arena->buf,gpu,n))return g_cur_arena->buf;
    for(NvArena *a in g_sub_arenas)if(uidp_span(a->buf,gpu,n))return a->buf;
    for(NvArena *a in g_arenas)if(uidp_span(a->buf,gpu,n))return a->buf;
    for(int i=0;i<g_nubd;i++){id<MTLBuffer> b=g_ubd[i].buf;if(uidp_span(b,gpu,n))return b;}
    id<MTLBuffer> z=zero_bank();if(uidp_span(z,gpu,n))return z;
    return nil;
}
static id<MTLBuffer> uidp_bound_span(const nvn_rbind *r,int st,uint64_t gpu,NSUInteger n){
    if(r->reg>=16)return nil;
    const p_bind_buf *u=r->kind==NVN_RB_SBUFP?&S.ssbo[st][r->reg]:&S.ubo[st][r->reg];
    if(r->kind==NVN_RB_SBUFP && (!u->addr||u->size<n))return nil;
    void *raw=NULL;uint64_t off=0,actual=0;
    if(u->addr && u->size>=n && mtl_gpu_addr(u->addr,n,&raw,&off,&actual) && actual==gpu){
        id<MTLBuffer> b=(__bridge id<MTLBuffer>)raw;if(uidp_span(b,gpu,n))return b;
    }
    return r->kind==NVN_RB_CBUF?uidp_host_span(gpu,n):nil;
}
static uint64_t uidp_copy_buffer(DmUIDPooledResourceBuild *b,uint64_t source,NSUInteger n,id<MTLBuffer> owner){
    if(!source||!n||!owner||!uidp_span(owner,source,n)){uidp_fail(b,@"unproved effective buffer span");return 0;}
    NSString *key=[NSString stringWithFormat:@"uidp:b:%llu:%llx:%llu:%llx",(unsigned long long)g_stat_pass,(unsigned long long)source,(unsigned long long)n,(unsigned long long)b->arena.gpuAddress];
    NSDictionary *old=b->pending[key]?:b->cache[key];
    if(old){[b->keep addObjectsFromArray:old[@"keep"]];return [old[@"destination"] unsignedLongLongValue];}
    NSUInteger off;if(!uidp_alloc(b,n,&off))return 0;uint64_t dst=b->arena.gpuAddress+off;
    [b->jobs addObject:@[@(source),@(dst),@(n)]];b->copied+=n;
    NSDictionary *v=@{@"destination":@(dst),@"keep":@[owner]};b->pending[key]=v;[b->keep addObject:owner];return dst;
}
static id<MTLTexture> uidp_root(id<MTLTexture> t){id<MTLTexture> root=t;while(root.parentTexture)root=root.parentTexture;return root;}
static int uidp_overlap(uint64_t a,uint64_t n,uint64_t c,uint64_t m){
    if(!a||!c||!n||!m||a>UINT64_MAX-n||c>UINT64_MAX-m)return -1;
    return a<c+m&&c<a+n;
}
static uint64_t uidp_texture(DmUIDPooledResourceBuild *b,const nvn_rbind *r,int st,uint64_t old){
    id<MTLTexture> t=nil,c=nil;uint64_t mem=0,size=0,cmem=0,csize=0;
    os_unfair_lock_lock(&g_rmu);
    int tid=(int)(tex_slot_h(S.tex[st],S.img[st],r->reg)&0xfffff);
    // Never call a lazy resolver after draw_ checked its encoder: the AB's cached object must still exist.
    if(tid>0&&tid<g_ntreg&&g_treg[tid].ok&&g_treg[tid].view){
        id<MTLTexture> cached=(__bridge id<MTLTexture>)g_treg[tid].view;
        if(cached.gpuResourceID._impl==old){t=cached;mem=(uint64_t)(uintptr_t)g_treg[tid].t.mem;size=g_treg[tid].t.L.size;}
    }
    if(R.rt.n==1&&R.rt.color[0]){c=tex_made(R.rt.color[0]);cmem=(uint64_t)(uintptr_t)R.rt.color[0]->mem;csize=R.rt.color[0]->L.size;}
    os_unfair_lock_unlock(&g_rmu);
    if(!t||!c||t.gpuResourceID._impl!=old){uidp_fail(b,@"texture is stale, substituted, dummy, or differs from actual AB resourceID");return 0;}
    id<MTLTexture> root=uidp_root(t),cr=uidp_root(c);
    if(root==cr || (root.buffer&&root.buffer==cr.buffer) || (root.heap&&root.heap==cr.heap) || uidp_overlap(mem,size,cmem,csize)!=0){uidp_fail(b,@"texture/C backing alias or unknown range");return 0;}
    if(!t.width||!t.height||t.textureType!=MTLTextureType2D||t.sampleCount!=1||t.mipmapLevelCount!=1||t.arrayLength!=1||t.parentRelativeLevel||t.parentRelativeSlice){uidp_fail(b,@"unsupported texture shape, mip, or slice");return 0;}
    NSString *key=[NSString stringWithFormat:@"uidp:t:%llu:%llx:%llx",(unsigned long long)g_stat_pass,(unsigned long long)old,(unsigned long long)b->arena.gpuAddress];
    NSDictionary *saved=b->pending[key]?:b->cache[key];
    if(saved){[b->keep addObjectsFromArray:saved[@"keep"]];return [saved[@"destination"] unsignedLongLongValue];}
    // The per-frame cache holds usage ordinals; persistent pool entries contain destinations only.
    MTLTextureSwizzleChannels sw=t.swizzle;
    NSString *shape=[NSString stringWithFormat:@"uidp:shape:%lu:%lu:%lu:%d:%d:%d:%d",(unsigned long)t.pixelFormat,(unsigned long)t.width,(unsigned long)t.height,(int)sw.red,(int)sw.green,(int)sw.blue,(int)sw.alpha];
    NSString *usage=[@"uidp:usage:" stringByAppendingString:shape];
    id count=b->pending[usage]?:b->cache[usage];
    if(count && (![count isKindOfClass:NSNumber.class] || [count longLongValue]<0)){uidp_fail(b,@"invalid frame pool usage ordinal");return 0;}
    NSUInteger ordinal=count?[count unsignedIntegerValue]:0;
    id list=b->poolPending[shape]?:b->pool[shape];
    if(list && ![list isKindOfClass:NSArray.class]){uidp_fail(b,@"invalid texture pool shape list");return 0;}
    NSArray *slots=list?:@[];
    id<MTLTexture> copy=nil,view=nil;
    if(ordinal<slots.count){
        id entry=slots[ordinal];
        if(![entry isKindOfClass:NSDictionary.class]){uidp_fail(b,@"invalid texture pool slot");return 0;}
        id baseObject=entry[@"base"],viewObject=entry[@"view"];
        if(![baseObject conformsToProtocol:@protocol(MTLTexture)] || ![viewObject conformsToProtocol:@protocol(MTLTexture)]){uidp_fail(b,@"missing pooled texture objects");return 0;}
        copy=baseObject;view=viewObject;
        MTLTextureSwizzleChannels actual=view.swizzle;
        if(copy.textureType!=MTLTextureType2D || copy.storageMode!=MTLStorageModePrivate || copy.sampleCount!=1 || copy.mipmapLevelCount!=1 || copy.arrayLength!=1 || copy.parentTexture ||
           copy==root || copy==cr || (copy.buffer&&(copy.buffer==root.buffer||copy.buffer==cr.buffer)) || (copy.heap&&(copy.heap==root.heap||copy.heap==cr.heap)) ||
           copy.pixelFormat!=t.pixelFormat || copy.width!=t.width || copy.height!=t.height || !(copy.usage&MTLTextureUsageShaderRead) ||
           view.textureType!=MTLTextureType2D || view.pixelFormat!=t.pixelFormat || view.width!=t.width || view.height!=t.height || view.sampleCount!=1 || view.mipmapLevelCount!=1 || view.arrayLength!=1 ||
           view.parentRelativeLevel || view.parentRelativeSlice || uidp_root(view)!=copy ||
           actual.red!=sw.red || actual.green!=sw.green || actual.blue!=sw.blue || actual.alpha!=sw.alpha){uidp_fail(b,@"pooled texture does not match exact shape/view signature");return 0;}
        b->poolReused++;
    } else {
        if(ordinal!=slots.count || !b->allowGrow){uidp_fail(b,@"texture pool miss; growth disabled or usage gap");return 0;}
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:t.width height:t.height mipmapped:NO];
        d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
        copy=[g_dev newTextureWithDescriptor:d];
        if(!copy){uidp_fail(b,@"texture snapshot allocation failed");return 0;}res_add(copy);[b->made addObject:copy];
        BOOL identity=sw.red==MTLTextureSwizzleRed&&sw.green==MTLTextureSwizzleGreen&&sw.blue==MTLTextureSwizzleBlue&&sw.alpha==MTLTextureSwizzleAlpha;
        view=identity?copy:[copy newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0,1) slices:NSMakeRange(0,1) swizzle:sw];
        if(!view){uidp_fail(b,@"texture swizzle snapshot view failed");return 0;}
        NSMutableArray *grown=[slots mutableCopy];[grown addObject:@{@"base":copy,@"view":view}];b->poolPending[shape]=[grown copy];
    }
    // A destination may serve one content version per frame; same-pass dedup returned above.
    NSString *reserved=[NSString stringWithFormat:@"uidp:reserved:%llx",(unsigned long long)copy.gpuResourceID._impl];
    if(b->pending[reserved]||b->cache[reserved]||ordinal==NSUIntegerMax){uidp_fail(b,@"pooled destination already assigned in this frame");return 0;}
    b->pending[usage]=@(ordinal+1);b->pending[reserved]=@YES;
    NSArray *keep=@[t,copy,view];[b->keep addObjectsFromArray:keep];
    [b->textures addObject:@{@"source":t,@"destination":copy}];
    uint64_t bytes=0;int bc=bc_block_bytes(t.pixelFormat);NSUInteger px=mtl_pf_bytes(t.pixelFormat);
    if(bc)bytes=((t.width+3)/4)*((t.height+3)/4)*(uint64_t)bc;
    else if(px&&t.width<=UINT64_MAX/t.height&&t.width*t.height<=UINT64_MAX/px)bytes=t.width*t.height*px;
    else b->textureBytesKnown=NO;
    if(bytes>UINT64_MAX-b->textureBytes)b->textureBytesKnown=NO;else b->textureBytes+=bytes;
    b->pending[key]=@{@"destination":@(view.gpuResourceID._impl),@"keep":keep};return view.gpuResourceID._impl;
}
static id<MTLSamplerState> uidp_sampler(const nvn_rbind *r,int st,uint64_t old){
    id<MTLSamplerState> found=nil;os_unfair_lock_lock(&g_rmu);
    int sid=r->kind==NVN_RB_SAMPT?(r->reg<NTEX?(int)(S.tex[st][r->reg]>>20):0):(r->reg<128?(int)(S.samp[st][r->reg]>>20):0);
    if(sid>0&&sid<g_nsreg&&g_sreg_ok[sid]){
        if(r->bw&&g_sview_w[sid]&&g_srid_w[sid]==old)found=(__bridge id<MTLSamplerState>)g_sview_w[sid];
        else if(g_sview[sid]&&g_srid[sid]==old)found=(__bridge id<MTLSamplerState>)g_sview[sid];
    }
    os_unfair_lock_unlock(&g_rmu);
    if(!found&&old==dummy_samp_rid())found=dummy_samp();
    return found&&found.gpuResourceID._impl==old?found:nil;
}
static NSDictionary *uidp_argument(DmUIDPooledResourceBuild *b,NvShader *sh,int st){
    const uidp_shader *proof=uidp_proof(sh,st);
    NSUInteger n=sh->arg_len?:sh->ae.encodedLength;
    if(!proof||sh->rf.nb!=proof->nb||!n||n%8||!sh->ab_buf.contents||sh->ab_off>sh->ab_buf.length||n>sh->ab_buf.length-sh->ab_off||
       sh->ab_gpu!=sh->ab_buf.gpuAddress+sh->ab_off){uidp_fail(b,@"unknown shader or invalid synchronous native AB");return nil;}
    NSUInteger off;if(!uidp_alloc(b,n,&off))return nil;
    uint64_t *out=(void *)((uint8_t *)b->arena.contents+off);
    const uint64_t *old=(const void *)((const uint8_t *)sh->ab_buf.contents+sh->ab_off);
    memcpy(out,old,n);[b->keep addObject:sh];[b->keep addObject:sh->ab_buf];
    uint64_t seen=0;
    for(int i=0;i<proof->nb;i++){
        const nvn_rbind *r=&sh->rf.b[i];const uidp_bound *q=NULL;
        for(int j=0;j<proof->nb;j++)if(proof->b[j].aid==r->aid){q=&proof->b[j];break;}
        int sampler=q&&q->kind==NVN_RB_SAMPT&&(r->kind==NVN_RB_SAMP||r->kind==NVN_RB_SAMPT);
        if(!q||(!sampler&&r->kind!=q->kind)||r->aid>=n/8||r->aid>=64||(seen&(1ull<<r->aid))||r->rw||r->atom||r->n||
           ((q->reg>=0)&&r->reg!=q->reg)||r->bw!=q->bw){uidp_fail(b,@"reflection does not match audited scalar bindings");return nil;}
        seen|=1ull<<r->aid;
        uint64_t source=old[r->aid],dst=source;
        if(r->kind==NVN_RB_CBUF||r->kind==NVN_RB_SBUFP){
            id<MTLBuffer> owner=uidp_bound_span(r,st,source,q->bytes);
            dst=uidp_copy_buffer(b,source,q->bytes,owner);if(!dst)return nil;
        } else if(r->kind==NVN_RB_BORDER){
            id<MTLBuffer> owner=uidp_host_span(source,q->bytes);
            if(!owner){uidp_fail(b,@"unproved effective border span");return nil;}
            dst=uidp_copy_buffer(b,source,q->bytes,owner);if(!dst)return nil;
        } else if(r->kind==NVN_RB_C1){
            id<MTLBuffer> owner=sh->c1buf?:zero_bank();uint64_t expected=sh->c1buf?sh->c1_gpu+sh->c1off:zero_bank_gpu();
            if(source!=expected||!uidp_span(owner,source,q->bytes)){uidp_fail(b,@"private C1 span mismatch");return nil;}[b->keep addObject:owner];
        } else if(r->kind==NVN_RB_SUPPORT){
            id<MTLBuffer> owner=support_buf();if(source!=support_gpu()||!uidp_span(owner,source,q->bytes)){uidp_fail(b,@"private support span mismatch");return nil;}[b->keep addObject:owner];
        } else if(r->kind==NVN_RB_TEX){dst=uidp_texture(b,r,st,source);if(!dst)return nil;
        } else if(r->kind==NVN_RB_SAMP||r->kind==NVN_RB_SAMPT){
            id<MTLSamplerState> s=uidp_sampler(r,st,source);if(!s){uidp_fail(b,@"sampler does not match actual AB resourceID");return nil;}[b->keep addObject:s];
        } else {uidp_fail(b,@"unsupported binding kind");return nil;}
        out[r->aid]=dst;
    }
    return @{@"slot":@(sh->rf.abi),@"source":@(sh->ab_gpu),@"destination":@(b->arena.gpuAddress+off)};
}
static id<MTLBuffer> uidp_stream_zero(uint64_t gpu){
    id<MTLBuffer> zero=g_lui_vertex_zero;
    return zero && gpu==g_lui_vertex_zero_gpu && zero.gpuAddress==gpu && zero.length==64 ? zero : nil;
}
static NSDictionary *uid_draw_plan_pooled(NvProg *pr,const p_draw *dw,const p_draw_idx *di,int cur_pull,
                                  id<MTLBuffer> arena,NSUInteger *cursor,NSMutableDictionary *cache,NSMutableDictionary *pool,BOOL allowGrow){
    DmUIDPooledResourceBuild *b=[DmUIDPooledResourceBuild new];b->arena=arena;b->at=cursor?*cursor:0;b->cache=cache;b->pool=pool;b->allowGrow=allowGrow;
    b->pending=[NSMutableDictionary dictionary];b->poolPending=[NSMutableDictionary dictionary];b->keep=[NSMutableArray array];b->jobs=[NSMutableArray array];b->textures=[NSMutableArray array];b->made=[NSMutableArray array];b->textureBytesKnown=YES;
    NSMutableArray *vb=[NSMutableArray array];NSDictionary *va=nil,*fa=nil;id index=NSNull.null;
    if(!arena||arena.storageMode!=MTLStorageModeShared||!arena.contents||arena.length<(1u<<20)||arena.gpuAddress>UINT64_MAX-arena.length||!cursor||!cache||!pool||pool==cache){uidp_fail(b,@"invalid private Shared>=1MiB arena/cursor/cache/pool");goto done;}
    if(!pr||!pr->sh[0]||!pr->sh[1]||!uidp_proof(pr->sh[0],0)||!uidp_proof(pr->sh[1],1)||cur_pull||S.rast_discard||S.tf_on||atest_on()||lop_on()||
       pr->sh[2]||pr->sh[3]||pr->sh[4]||g_geo_program||g_geo_mesh.on||g_mtk.on||g_pso_lmap||g_pass.layered){uidp_fail(b,@"unsupported shader entry, pulling/TF/geometry/tile/alpha/logic path");goto done;}
    if((dw!=NULL)==(di!=NULL)|| (dw&&(dw->ind||dw->mdi_cnt)) || (di&&(di->ind||di->mdi_cnt))){uidp_fail(b,@"not exactly one direct draw");goto done;}
    {int prim=dw?dw->prim:di->prim;if((prim!=4&&prim!=5)||(dw&&(dw->first<0||dw->count<=0||dw->base_instance<0))||(di&&(di->count<=0||di->base_instance<0||di->base_vertex<0))){uidp_fail(b,@"unsupported primitive or draw parameters");goto done;}}
    for(int v=0;v<pr->sh[0]->rf.nv;v++){
        int a=pr->sh[0]->rf.v[v].nvn;if(a>=32||a>=S.nattr||!S.attr[a].fmt||vconv_wanted(&pr->sh[0]->rf,v,NULL,0)||
           (pr->sh[0]->rf.has_vin&&!vin_cpu()&&vin_ct(&pr->sh[0]->rf,v,S.attr[a].fmt,1)!=pr->sh[0]->rf.v[v].ct)){uidp_fail(b,@"missing or converted vertex attribute");goto done;}
    }
    [b->keep addObject:arena];[b->keep addObject:pr];
    va=uidp_argument(b,pr->sh[0],0);if(!va)goto done;
    fa=uidp_argument(b,pr->sh[1],1);if(!fa)goto done;
    {unsigned streams=vs_streams(&pr->sh[0]->rf);
    for(int s=0;s<16;s++)if(streams>>s&1){
        uint64_t n=S.vbuf[s].size,src=0,off=0;void *raw=NULL;NSUInteger slot=VB_SLOT(s);
        if(!S.vbuf[s].address||!n||n>(1u<<20)||g_vbuf_need[s]||!mtl_gpu_addr(S.vbuf[s].address,n,&raw,&off,&src)||slot>=31||!(g_vt_valid&(1u<<slot))||g_vt_addr[slot]!=src){uidp_fail(b,@"vertex span too large, absent, padded, or substituted");goto done;}
        uint64_t dst=uidp_copy_buffer(b,src,(NSUInteger)n,(__bridge id<MTLBuffer>)raw);if(!dst)goto done;
        [vb addObject:@{@"slot":@(slot),@"source":@(src),@"destination":@(dst),@"bytes":@(n)}];
    }}
    if(g_vt_valid&(1u<<30)){
        id<MTLBuffer> z=uidp_stream_zero(g_vt_addr[30]);if(!z){uidp_fail(b,@"unproved immutable vertex zero stream30");goto done;}[b->keep addObject:z];
        [vb addObject:@{@"slot":@30,@"source":@(g_vt_addr[30]),@"destination":@(g_vt_addr[30]),@"bytes":@0}];
    }
    if(di){
        if(di->itype!=1&&di->itype!=2){uidp_fail(b,@"8bit or unknown index encoding refused");goto done;}
        NSUInteger width=di->itype==1?2:4;uint64_t n=(uint64_t)di->count*width,src=0,off=0;void *raw=NULL;
        if(!di->ibuf||n>(1u<<20)||!mtl_gpu_addr(di->ibuf,n,&raw,&off,&src)){uidp_fail(b,@"unmapped or oversized direct index range");goto done;}
        uint64_t dst=uidp_copy_buffer(b,src,(NSUInteger)n,(__bridge id<MTLBuffer>)raw);if(!dst)goto done;
        index=@{@"source":@(src),@"destination":@(dst),@"type":@(di->itype==1?MTLIndexTypeUInt16:MTLIndexTypeUInt32),@"count":@(di->count),@"bytes":@(n)};
    }
    {NSUInteger off,n=b->jobs.count;if(n>NSUIntegerMax/sizeof(uidp_copy_job)||!uidp_alloc(b,n*sizeof(uidp_copy_job),&off))goto done;
    uidp_copy_job *j=(void *)((uint8_t *)arena.contents+off);for(NSUInteger i=0;i<n;i++){NSArray *a=b->jobs[i];j[i]=(uidp_copy_job){[a[0] unsignedLongLongValue],[a[1] unsignedLongLongValue],[a[2] unsignedLongLongValue]};}
    [pool addEntriesFromDictionary:b->poolPending];[cache addEntriesFromDictionary:b->pending];*cursor=b->at;
    return @{@"valid":@YES,@"reason":@"",@"buffer":arena,@"jobs_gpu":@(arena.gpuAddress+off),@"job_count":@(n),@"texture_copies":b->textures,@"vs_argument":va,@"fs_argument":fa,@"vertex_bindings":vb,@"index":index,@"keep":b->keep,@"owned_textures":@[],@"pool_created_textures":b->made,@"pool_reused_textures":@(b->poolReused),@"copied_buffer_bytes":@(b->copied),@"copied_texture_bytes":b->textureBytesKnown?(id)@(b->textureBytes):NSNull.null,@"native_pass":@(g_stat_pass)};}
done:
    for(id t in b->made)res_remove(t);
    return @{@"valid":@NO,@"reason":b->reason?:@"resource plan refused",@"native_pass":@(g_stat_pass)};
}
