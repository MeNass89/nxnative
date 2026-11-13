// Private diagnostic: actual pipeline descriptor, never inferred from mutable S.
static os_unfair_lock g_uis_pso_lock=OS_UNFAIR_LOCK_INIT;
static NSMapTable *g_uis_descriptors,*g_uis_pipelines;
static id<MTLRenderPipelineState> pso_build_ex(MTLRenderPipelineDescriptor *d,NSError **e,int compile) {
    id<MTLRenderPipelineState> ps=pso_build_ex_original(d,e,compile);
    if(ps && getenv("DM_LOW_ROUTE_ARM") && d.colorAttachments[0].pixelFormat==MTLPixelFormatRG11B10Float && d.vertexFunction && !d.depthAttachmentPixelFormat && !d.stencilAttachmentPixelFormat) {
        os_unfair_lock_lock(&g_uis_pso_lock);
        if(!g_uis_descriptors)g_uis_descriptors=[NSMapTable strongToStrongObjectsMapTable];
        [g_uis_descriptors setObject:[d copy] forKey:ps];os_unfair_lock_unlock(&g_uis_pso_lock);
    }
    return ps;
}
static id<MTLRenderPipelineState> uis_float_pipeline(id<MTLRenderPipelineState> original) {
    os_unfair_lock_lock(&g_uis_pso_lock);
    id<MTLRenderPipelineState> cached=[g_uis_pipelines objectForKey:original];
    MTLRenderPipelineDescriptor *d=[[g_uis_descriptors objectForKey:original]copy];os_unfair_lock_unlock(&g_uis_pso_lock);
    if(cached)return cached;
    if(!d || d.rasterSampleCount!=1 || d.alphaToCoverageEnabled || d.depthAttachmentPixelFormat || d.stencilAttachmentPixelFormat)return nil;
    for(int i=1;i<8;i++)if(d.colorAttachments[i].pixelFormat)return nil;
    d.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA16Float;
    d.colorAttachments[0].writeMask&=MTLColorWriteMaskRed|MTLColorWriteMaskGreen|MTLColorWriteMaskBlue;
    NSError *error=nil;id<MTLRenderPipelineState> ps=pso_build_ex_original(d,&error,1);
    if(!ps){MLOG("UI final PSO refused: %s",error.description.UTF8String);return nil;}
    os_unfair_lock_lock(&g_uis_pso_lock);
    if(!g_uis_pipelines)g_uis_pipelines=[NSMapTable strongToStrongObjectsMapTable];
    [g_uis_pipelines setObject:ps forKey:original];os_unfair_lock_unlock(&g_uis_pso_lock);return ps;
}

static id<MTLRenderPipelineState> uis_float_pipeline_cached(id<MTLRenderPipelineState> original){os_unfair_lock_lock(&g_uis_pso_lock);id ps=[g_uis_pipelines objectForKey:original];os_unfair_lock_unlock(&g_uis_pso_lock);return ps;}
static void uis_float_pipeline_warm(id<MTLRenderPipelineState> original){
    static NSMapTable *warming;os_unfair_lock_lock(&g_uis_pso_lock);
    if([g_uis_pipelines objectForKey:original]||[warming objectForKey:original]){os_unfair_lock_unlock(&g_uis_pso_lock);return;}
    if(!warming)warming=[NSMapTable strongToStrongObjectsMapTable];[warming setObject:@YES forKey:original];os_unfair_lock_unlock(&g_uis_pso_lock);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool{(void)uis_float_pipeline(original);}});
}
