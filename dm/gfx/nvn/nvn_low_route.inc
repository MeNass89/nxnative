// Private full-route candidate. Legacy snapshots remain the fallback.
typedef struct {
    id<MTLTexture> world,depth,motion,generated,nativeFinal,generatedFinal;
    id<MTL4CommandAllocator> allocator;id<MTL4CommandBuffer> commands;
    uint64_t generationReader,uiReader,frame;BOOL active,sdr;
} low_route_slot;
static low_route_slot g_low_slots[SNAP_MAX];
static id<MTLFXFrameInterpolator> g_low_interpolator;
static id<MTLFXSpatialScaler> g_low_native_scaler,g_low_generated_scaler;
static id<MTL4CommandQueue> g_low_ui_queue;
static id<MTLSharedEvent> g_low_ui_done;
static uint64_t g_low_ui_sequence,g_low_previous_frame=UINT64_MAX,g_low_previous_ns;
static id<MTLTexture> g_low_previous;
static nvn_camera_input g_low_camera;
static NSUInteger g_low_width,g_low_height;
static unsigned g_low_age;static BOOL g_low_sdr;
static _Atomic uint64_t g_low_sdr_accepted,g_low_sdr_fallback;
static NSDictionary*g_low_armed_stats;static uint64_t g_low_armed_frame=UINT64_MAX;
static NSArray *g_low_capture_buffers;static BOOL g_low_captured;
static _Atomic uint64_t g_low_frames,g_low_fallbacks,g_low_ui_failures,g_low_rebuilds;
static int low_route_wanted(void){const char *s=getenv("DM_LOW_ROUTE_ARM");return s&&*s;}
static int low_route_armed(void){return low_ui_armed();}
static uint64_t low_route_ui_reader(unsigned slot){return slot<SNAP_MAX?g_low_slots[slot].uiReader:0;}
static id<MTLSharedEvent> low_route_ui_event(void){return g_low_ui_done;}
static void low_route_drain(void){
    uint64_t value=g_low_ui_sequence;id<MTLSharedEvent> event=g_low_ui_done;
    if(value&&event&&![event waitUntilSignaledValue:value timeoutMS:10000])MDIE("low route UI did not drain");
}
static id<MTLTexture> low_route_texture(NSUInteger w,NSUInteger h,MTLPixelFormat pf,MTLTextureUsage usage){
    MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];d.storageMode=MTLStorageModePrivate;d.usage=usage;
    id<MTLTexture>t=[g_dev newTextureWithDescriptor:d];if(t)res_add(t);return t;
}
static id<MTLFXSpatialScaler> low_route_scaler(NSUInteger w,NSUInteger h,BOOL sdr){
    MTLFXSpatialScalerDescriptor*d=[MTLFXSpatialScalerDescriptor new];d.inputWidth=1536;d.inputHeight=864;d.outputWidth=w;d.outputHeight=h;d.colorTextureFormat=d.outputTextureFormat=MTLPixelFormatRGBA16Float;d.colorProcessingMode=sdr?MTLFXSpatialScalerColorProcessingModeLinear:MTLFXSpatialScalerColorProcessingModeHDR;return[d newSpatialScalerWithDevice:g_dev];
}
// Called only from the producer thread. Rebuilds are warmup/resize, never hidden per-frame work.
static int low_route_prepare(void){
    if(!low_route_wanted()||!temporal_async_on()||!temporal_async_init()||!temporal_motion_pipeline())return 0;
    CAMetalLayer *layer=(__bridge CAMetalLayer*)front_metal_layer();CGSize size=layer.drawableSize;
    NSUInteger w=(NSUInteger)size.width,h=(NSUInteger)size.height;BOOL sdr=!front_hdr_enabled();
    if(sdr && ((layer.pixelFormat!=MTLPixelFormatBGRA8Unorm && layer.pixelFormat!=MTLPixelFormatRGBA8Unorm) || !low_sdr_present_pipeline(MTLPixelFormatBGRA8Unorm) || !low_sdr_present_pipeline(MTLPixelFormatRGBA8Unorm)))return 0;
    if(!w||!h||w>8192||h>8192||w*9!=h*16)return 0;
    pthread_mutex_lock(&g_temporal_async_mu);
    if(g_low_interpolator&&w==g_low_width&&h==g_low_height&&sdr==g_low_sdr){pthread_mutex_unlock(&g_temporal_async_mu);return 1;}
    pthread_mutex_lock(&g_pq_mu);BOOL pending=NO;
    for(unsigned i=0;i<SNAP_MAX;i++)pending|=g_low_slots[i].active && atomic_load(&g_snap[i].ticket)>g_pq_done;
    pthread_mutex_unlock(&g_pq_mu);if(pending){pthread_mutex_unlock(&g_temporal_async_mu);return 0;}
    // No allocation can be retired or command allocator reset while a real reader remains.
    for(unsigned i=0;i<SNAP_MAX;i++)if(g_temporal_async_done.signaledValue<g_low_slots[i].generationReader || (g_low_ui_done&&g_low_ui_done.signaledValue<g_low_slots[i].uiReader) || g_temporal_display_done.signaledValue<atomic_load(&g_temporal_snap[i].display_reader)){
        pthread_mutex_unlock(&g_temporal_async_mu);return 0;
    }
    NSError *error=nil;MTLFXFrameInterpolatorDescriptor*fd=[MTLFXFrameInterpolatorDescriptor new];fd.inputWidth=fd.outputWidth=1536;fd.inputHeight=fd.outputHeight=864;fd.colorTextureFormat=fd.outputTextureFormat=MTLPixelFormatRGBA16Float;fd.depthTextureFormat=MTLPixelFormatDepth32Float;fd.motionTextureFormat=MTLPixelFormatRG16Float;
    id<MTLFXFrameInterpolator> fi=[fd newFrameInterpolatorWithDevice:g_dev];id<MTLFXSpatialScaler> ns=low_route_scaler(w,h,sdr),gs=low_route_scaler(w,h,sdr);
    if(!fi||!ns||!gs){pthread_mutex_unlock(&g_temporal_async_mu);return 0;}
    low_route_slot made[SNAP_MAX]={0};BOOL valid=YES;
    for(unsigned i=0;i<SNAP_MAX;i++){
        low_route_slot*s=&made[i];s->sdr=sdr;s->world=low_route_texture(1536,864,MTLPixelFormatRGBA16Float,fi.colorTextureUsage|ns.colorTextureUsage|MTLTextureUsageShaderRead);
        s->depth=low_route_texture(1536,864,MTLPixelFormatDepth32Float,fi.depthTextureUsage|MTLTextureUsageShaderRead);
        s->motion=low_route_texture(1536,864,MTLPixelFormatRG16Float,fi.motionTextureUsage|MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite);
        s->generated=low_route_texture(1536,864,MTLPixelFormatRGBA16Float,fi.outputTextureUsage|gs.colorTextureUsage|MTLTextureUsageShaderRead);
        s->nativeFinal=low_route_texture(w,h,MTLPixelFormatRGBA16Float,ns.outputTextureUsage|MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead);
        s->generatedFinal=low_route_texture(w,h,MTLPixelFormatRGBA16Float,gs.outputTextureUsage|MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead);
        s->allocator=[g_dev newCommandAllocator];s->commands=[g_dev newCommandBuffer];
        valid&=s->world&&s->depth&&s->motion&&s->generated&&s->nativeFinal&&s->generatedFinal&&s->allocator&&s->commands;
    }
    if(!g_low_ui_queue){g_low_ui_queue=[g_dev newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new]error:&error];if(g_low_ui_queue)[g_low_ui_queue addResidencySet:g_res];}
    if(!g_low_ui_done)g_low_ui_done=[g_dev newSharedEvent];valid&=g_low_ui_queue&&g_low_ui_done;
    if(!valid){for(unsigned i=0;i<SNAP_MAX;i++)for(id t in @[made[i].world?:NSNull.null,made[i].depth?:NSNull.null,made[i].motion?:NSNull.null,made[i].generated?:NSNull.null,made[i].nativeFinal?:NSNull.null,made[i].generatedFinal?:NSNull.null])if(t!=NSNull.null)res_remove(t);pthread_mutex_unlock(&g_temporal_async_mu);return 0;}
    for(unsigned i=0;i<SNAP_MAX;i++){
        low_route_slot *s=&g_low_slots[i];for(id t in @[s->world?:NSNull.null,s->depth?:NSNull.null,s->motion?:NSNull.null,s->generated?:NSNull.null,s->nativeFinal?:NSNull.null,s->generatedFinal?:NSNull.null])if(t!=NSNull.null)res_remove(t);
        *s=made[i];
    }
    if(getenv("DM_LOW_ROUTE_CAPTURE")&&!g_low_captured){
        NSUInteger pitch=(w*8+255)&~255;id<MTLBuffer>a=[g_dev newBufferWithLength:pitch*h options:MTLResourceStorageModeShared],b=[g_dev newBufferWithLength:pitch*h options:MTLResourceStorageModeShared];
        if(a&&b){if(g_low_capture_buffers)for(id old in g_low_capture_buffers)res_remove(old);res_add(a);res_add(b);g_low_capture_buffers=@[a,b];}
    }
    g_low_sdr=sdr;g_low_interpolator=fi;g_low_native_scaler=ns;g_low_generated_scaler=gs;g_low_width=w;g_low_height=h;g_low_previous=nil;g_low_previous_frame=UINT64_MAX;g_low_age=0;atomic_fetch_add(&g_low_rebuilds,1);
    pthread_mutex_unlock(&g_temporal_async_mu);res_commit();return 1;
}
static void low_route_disable(unsigned slot){if(slot<SNAP_MAX){pthread_mutex_lock(&g_temporal_async_mu);g_low_slots[slot].active=NO;pthread_mutex_unlock(&g_temporal_async_mu);}atomic_fetch_add(&g_low_fallbacks,1);if(!front_hdr_enabled())atomic_fetch_add(&g_low_sdr_fallback,1);}
// Under g_temporal_async_mu in the present thread. Original serials stay valid fallback objects.
static int low_route_display(unsigned slot,uint64_t serial,id<MTLTexture>*texture,id<MTLSharedEvent>*event,uint64_t*value){
    if(slot>=SNAP_MAX||!g_low_slots[slot].active)return 0;
    low_route_slot*s=&g_low_slots[slot];BOOL generated=serial==g_temporal_snap[slot].serial;
    if(!generated&&serial!=g_snap[slot].serial)return 0;
    *texture=generated?s->generatedFinal:s->nativeFinal;*event=g_low_ui_done;*value=s->uiReader;return *texture&&*event&&*value?(s->sdr?2:1):0;
}
static int low_route_encode(id<MTL4CommandBuffer> producerCB,unsigned slot,uint64_t frame,uint64_t now,id<MTLTexture>expectedTarget,id<MTLTexture>depth,nvn_camera_input camera,id<MTLSharedEvent>producer,uint64_t producerValue,uint64_t*generatedSerial){
    if(g_low_sdr==front_hdr_enabled()||!expectedTarget||!low_route_armed()||slot>=SNAP_MAX||!producer||!producerValue||!g_low_interpolator||depth.width!=1536||depth.height!=864||depth.pixelFormat!=MTLPixelFormatDepth32Float||depth.sampleCount!=1)return 0;
    id packet=low_ui_take(frame,expectedTarget);if(!packet)return 0;
    if(!g_low_armed_stats){g_low_armed_stats=low_ui_stats();g_low_armed_frame=frame;}
    id<MTLTexture> source=low_ui_source(packet);
    if(!source||source.width!=1536||source.height!=864||source.pixelFormat!=MTLPixelFormatRGBA16Float){low_ui_release(packet,producer,producerValue);return 0;}
    pthread_mutex_lock(&g_temporal_async_mu);low_route_slot*s=&g_low_slots[slot];
    if(g_low_ui_done.signaledValue<s->uiReader){pthread_mutex_unlock(&g_temporal_async_mu);low_ui_release(packet,producer,producerValue);return 0;}
    // Slot-copy waits captured before this function guard world, FI, UI and display reuse.
    id<MTL4ComputeCommandEncoder> copy=[producerCB computeCommandEncoder];
    [copy barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
    for(int i=0;i<2;i++){id<MTLTexture>a=i?depth:source,b=i?s->depth:s->world;[copy copyFromTexture:a sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(a.width,a.height,1) toTexture:b destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];}
    [copy barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];[copy endEncoding];
    BOOL matrices=camera.has_current_transform&&g_low_camera.has_current_transform;BOOL continuous=matrices?nvn_camera_previous_matches(&camera,&g_low_camera):frame==g_low_previous_frame+1;
    BOOL reset=!g_low_previous||g_low_previous==s->world||frame!=g_low_previous_frame+1||now<=g_low_previous_ns||!continuous;
    id<MTLTexture>previous=reset?s->world:g_low_previous;float dt=reset?1.f/60:(now-g_low_previous_ns)*1e-9f;
    id<MTLCommandBuffer>cb=[g_temporal_async_queue commandBuffer];if(!cb){pthread_mutex_unlock(&g_temporal_async_mu);low_ui_release(packet,producer,producerValue);return 0;}
    [cb encodeWaitForEvent:producer value:producerValue];
    float matrix[16];nvn_cam_mul(camera.previous_world_to_clip,camera.clip_to_world,matrix);
    id<MTLComputeCommandEncoder>motion=[cb computeCommandEncoder];[motion setComputePipelineState:g_temporal_motion_ps];[motion setBytes:matrix length:sizeof matrix atIndex:0];[motion setTexture:s->depth atIndex:0];[motion setTexture:s->motion atIndex:1];[motion dispatchThreadgroups:MTLSizeMake(192,108,1)threadsPerThreadgroup:MTLSizeMake(8,8,1)];[motion endEncoding];
    id<MTLFXFrameInterpolator>fi=g_low_interpolator;fi.colorTexture=s->world;fi.prevColorTexture=previous;fi.depthTexture=s->depth;fi.motionTexture=s->motion;fi.outputTexture=s->generated;fi.uiTexture=nil;fi.uiTextureComposited=NO;
    fi.motionVectorScaleX=fi.motionVectorScaleY=1;fi.nearPlane=camera.near_plane;fi.farPlane=camera.far_plane;fi.fieldOfView=camera.fov*180/M_PI;fi.aspectRatio=16.f/9;fi.deltaTime=dt;fi.shouldResetHistory=reset;fi.depthReversed=NO;
    fi.worldToViewMatrix=camera.has_render_matrices?temporal_camera_matrix(camera.world_to_view):matrix_identity_float4x4;fi.viewToClipMatrix=camera.has_render_matrices?temporal_camera_matrix(camera.view_to_clip):matrix_identity_float4x4;[fi encodeToCommandBuffer:cb];
    id<MTLFXSpatialScaler>ns=g_low_native_scaler,gs=g_low_generated_scaler;ns.colorTexture=s->world;ns.outputTexture=s->nativeFinal;ns.inputContentWidth=1536;ns.inputContentHeight=864;[ns encodeToCommandBuffer:cb];gs.colorTexture=s->generated;gs.outputTexture=s->generatedFinal;gs.inputContentWidth=1536;gs.inputContentHeight=864;[gs encodeToCommandBuffer:cb];
    uint64_t generation=++g_temporal_async_sequence;[cb encodeSignalEvent:g_temporal_async_done value:generation];
    s->generationReader=generation;atomic_store(&g_temporal_snap[slot].async_reader,generation);atomic_store(&g_temporal_snap[slot].async_ready,generation);
    for(unsigned i=0;i<SNAP_MAX;i++)if(g_low_slots[i].world==previous){g_low_slots[i].generationReader=generation;atomic_store(&g_temporal_snap[i].async_reader,generation);}
    NSArray*resources=@[s->world,previous,s->depth,s->motion,s->generated,s->nativeFinal,s->generatedFinal,fi,ns,gs];
    [cb addCompletedHandler:^(id<MTLCommandBuffer>done){(void)resources.count;if(done.error){MLOG("[RED] low-route generation: %s",done.error.description.UTF8String);gpu_lost_check(done.error,"low-route generation");}double begin=done.GPUStartTime,end=done.GPUEndTime;dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{temporal_async_note_gpu(frame,"low generation plus spatial",begin,end);});}];
    [cb commit];atomic_store(&g_temporal_async_latest,generation);
    [s->allocator reset];id<MTL4CommandBuffer>ui=s->commands;[ui beginCommandBufferWithAllocator:s->allocator];
    NSArray*nativeOwners=low_ui_render(packet,ui,s->nativeFinal),*generatedOwners=nativeOwners?low_ui_render(packet,ui,s->generatedFinal):nil;
    NSArray *captured=nil;NSDictionary *captureInfo=nil;
    const char *capturePath=getenv("DM_LOW_ROUTE_CAPTURE");
    if(nativeOwners&&generatedOwners&&capturePath&&*capturePath&&!g_low_captured&&g_low_age>=8&&g_low_capture_buffers.count==2){
        g_low_captured=YES;captured=g_low_capture_buffers;NSUInteger pitch=(g_low_width*8+255)&~255;
        id<MTL4ComputeCommandEncoder>read=[ui computeCommandEncoder];[read barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
        id<MTLTexture>textures[]={s->nativeFinal,s->generatedFinal};
        for(unsigned i=0;i<2;i++){id<MTLBuffer>b=captured[i];[read copyFromTexture:textures[i]sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)sourceSize:MTLSizeMake(g_low_width,g_low_height,1)toBuffer:b destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:b.length];}
        [read barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];[read endEncoding];
        captureInfo=@{@"frame":@(frame),@"slot":@(slot),@"source_generation_event":@(generation),@"width":@(g_low_width),@"height":@(g_low_height),@"row_bytes":@(pitch),@"history_age":@(g_low_age),@"reset":@(reset),@"delta_time":@(dt),@"folder":@(capturePath)};
    }
    [ui endCommandBuffer];
    if(!nativeOwners||!generatedOwners){atomic_fetch_add(&g_low_ui_failures,1);g_low_previous=nil;pthread_mutex_unlock(&g_temporal_async_mu);low_ui_release(packet,g_temporal_async_done,generation);return 0;}
    uint64_t uiValue=++g_low_ui_sequence;id<MTL4CommandAllocator>allocator=s->allocator;
    MTL4CommitOptions*options=[MTL4CommitOptions new];[options addFeedbackHandler:^(id<MTL4CommitFeedback>f){(void)nativeOwners.count;(void)generatedOwners.count;(void)allocator;(void)ui;if(f.error){MLOG("[RED] low-route HUD: %s",f.error.description.UTF8String);gpu_lost_check(f.error,"low-route HUD");}double begin=f.GPUStartTime,end=f.GPUEndTime;NSError *failure=f.error;dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        temporal_async_note_gpu(frame,"low HUD",begin,end);
        if(captured&&captureInfo){@autoreleasepool{
            NSMutableArray *images=[NSMutableArray array];NSUInteger w=[captureInfo[@"width"]unsignedIntegerValue],h=[captureInfo[@"height"]unsignedIntegerValue],pitch=[captureInfo[@"row_bytes"]unsignedIntegerValue];NSString*folder=captureInfo[@"folder"];
            NSArray *names=@[@"native-final.rgba16f",@"generated-final.rgba16f"];
            for(unsigned i=0;i<2;i++){
                id<MTLBuffer>b=captured[i];const __fp16*data=b.contents;uint64_t bad=0,negative=0,alpha=0,nonzero=0;double maximum=0;
                for(NSUInteger y=0;y<h;y++)for(NSUInteger x=0;x<w;x++){NSUInteger k=y*(pitch/2)+x*4;BOOL nz=NO;for(int c=0;c<4;c++){float v=data[k+c];bad+=!isfinite(v);if(c<3){negative+=v<0;maximum=fmax(maximum,v);nz|=v!=0;}else alpha+=v!=1;}nonzero+=nz;}
                BOOL written=[[NSData dataWithBytes:b.contents length:b.length]writeToFile:[folder stringByAppendingPathComponent:names[i]]atomically:YES];
                [images addObject:@{@"file":names[i],@"nonfinite":@(bad),@"negative_rgb":@(negative),@"alpha_not_one":@(alpha),@"nonzero_pixels":@(nonzero),@"maximum_rgb":@(maximum),@"written":@(written)}];
            }
            NSMutableDictionary *info=[captureInfo mutableCopy];info[@"images"]=images;info[@"gpu_error"]=failure.description?:@"";
            NSData*j=[NSJSONSerialization dataWithJSONObject:info options:NSJSONWritingPrettyPrinted error:nil];[j writeToFile:[folder stringByAppendingPathComponent:@"capture.json"]atomically:YES];
        }}
    });}];
    [g_low_ui_queue waitForEvent:g_temporal_async_done value:generation];pthread_mutex_lock(&g_res_commit_mu);[g_low_ui_queue commit:&ui count:1 options:options];[g_low_ui_queue signalEvent:g_low_ui_done value:uiValue];pthread_mutex_unlock(&g_res_commit_mu);
    BOOL shadow=getenv("DM_LOW_ROUTE_SHADOW")&&atoi(getenv("DM_LOW_ROUTE_SHADOW"));
    s->uiReader=uiValue;s->frame=frame;s->active=!shadow;
    if(!g_temporal_snap[slot].serial)g_temporal_snap[slot].serial=(1ull<<60)+slot+1;
    if(!shadow){if(serial_get(g_temporal_snap[slot].serial)!=s->generatedFinal)serial_put(g_temporal_snap[slot].serial,s->generatedFinal);g_temporal_snap[slot].display=s->generatedFinal;g_temporal_snap[slot].compact_display=YES;}
    g_low_previous=s->world;g_low_previous_frame=frame;g_low_previous_ns=now;g_low_camera=camera;g_low_age=reset?0:g_low_age+1;
    *generatedSerial=!shadow&&g_low_age>=2?g_temporal_snap[slot].serial:0;atomic_fetch_add(&g_low_frames,1);if(s->sdr&&!shadow)atomic_fetch_add(&g_low_sdr_accepted,1);
    pthread_mutex_unlock(&g_temporal_async_mu);low_ui_release(packet,g_low_ui_done,uiValue);return shadow?0:1;
}
static NSDictionary*low_route_stats(void){return@{@"sdr_observer":low_sdr_observer_stats(),@"accepted_sdr":@(atomic_load(&g_low_sdr_accepted)),@"fallback_sdr":@(atomic_load(&g_low_sdr_fallback)),@"low_frames":@(atomic_load(&g_low_frames)),@"fallbacks":@(atomic_load(&g_low_fallbacks)),@"ui_failures":@(atomic_load(&g_low_ui_failures)),@"rebuilds":@(atomic_load(&g_low_rebuilds)),@"width":@(g_low_width),@"height":@(g_low_height),@"armed_frame":@(g_low_armed_frame),@"armed_packet_stats":g_low_armed_stats?:@{},@"packet":low_ui_stats()};}
static void low_route_export_stats(void){const char*path=getenv("DM_LOW_ROUTE_REPORT");if(!path||!*path)return;NSData*data=[NSJSONSerialization dataWithJSONObject:low_route_stats()options:NSJSONWritingPrettyPrinted error:nil];[data writeToFile:@(path)atomically:YES];}

// Called outside g_temporal_async_mu. Only live, accepted SDR slots carry this tag.
static int low_route_sdr_texture(id<MTLTexture> texture){
    if(!texture)return 0;int found=0;pthread_mutex_lock(&g_temporal_async_mu);
    for(unsigned i=0;i<SNAP_MAX;i++){low_route_slot*s=&g_low_slots[i];if(s->active&&s->sdr&&(texture==s->nativeFinal||texture==s->generatedFinal)){found=1;break;}}
    pthread_mutex_unlock(&g_temporal_async_mu);return found;
}
static int low_route_present_source(uint64_t serial,id<MTLTexture>*source,int crop[4]){
    int found=0;pthread_mutex_lock(&g_temporal_async_mu);
    for(unsigned i=0;i<SNAP_MAX;i++){id<MTLTexture>texture=nil;id<MTLSharedEvent>event=nil;uint64_t value=0;
        if(low_route_display(i,serial,&texture,&event,&value)==2){*source=texture;crop[0]=crop[1]=0;crop[2]=(int)texture.width;crop[3]=(int)texture.height;found=1;break;}}
    pthread_mutex_unlock(&g_temporal_async_mu);return found;
}
