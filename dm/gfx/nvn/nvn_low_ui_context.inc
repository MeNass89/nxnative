static int lui_opcode(uint32_t op) { return op==RO_BAR_QUEUE || op==RO_BAR_AFTER_STAGES || op==RO_BAR_ENCODER || op==RO_R_TABLE || op==RO_R_PSO || op==RO_R_DSS || op==RO_R_VIEWPORT || op==RO_R_SCISSOR || op==RO_R_CULL || op==RO_R_WINDING || op==RO_R_FILL || op==RO_R_BIAS || op==RO_R_CLIP || op==RO_R_SREF || op==RO_R_BLEND || op==RO_R_VIS || op==RO_R_AMP || op==RO_R_DRAW || op==RO_R_DRAW_INST || op==RO_R_DRAW_IDX || op==RO_T_ADDR || op==RO_T_TEX || op==RO_T_SAMP || op==RO_LABEL || op==RO_UI_META || op==RO_R_STORE_C; }
static void lui_table_write(const rec_t *r,id<MTL4ArgumentTable> table) {
    if(r->op==RO_T_ADDR)[table setAddress:r->a[1] atIndex:r->a[2]];
    if(r->op==RO_T_TEX)[table setTexture:(MTLResourceID){r->a[1]} atIndex:r->a[2]];
    if(r->op==RO_T_SAMP)[table setSamplerState:(MTLResourceID){r->a[1]} atIndex:r->a[2]];
}
static NSArray *lui_replay_on(id<MTL4CommandBuffer> cb,NSData *records,NSArray *initial,MTL4RenderPassDescriptor *descriptor,id<MTLTexture> output,NSDictionary *tables,NSDictionary *pipelines) {
    const rec_t *ops=records.bytes;BOOL floating=output.pixelFormat==MTLPixelFormatRGBA16Float;
    NSMutableArray *owners=[NSMutableArray array];
    [owners addObjectsFromArray:tables.allValues];
    for(NSData *raw in initial){rec_t r;memcpy(&r,raw.bytes,sizeof r);id t=tables[@(r.a[0])];if(t)lui_table_write(&r,t);}
    double sx=(double)output.width/descriptor.colorAttachments[0].texture.width,sy=(double)output.height/descriptor.colorAttachments[0].texture.height;
    MTL4RenderPassDescriptor *pd=[descriptor copy];pd.colorAttachments[0].texture=output;pd.colorAttachments[0].storeAction=MTLStoreActionStore;
    pd.visibilityResultBuffer=nil;pd.renderTargetWidth=output.width;pd.renderTargetHeight=output.height;
    if(floating && pd.colorAttachments[0].loadAction==MTLLoadActionClear){MTLClearColor c=pd.colorAttachments[0].clearColor;c.alpha=1;pd.colorAttachments[0].clearColor=c;}
    id<MTL4RenderCommandEncoder>e=[cb renderCommandEncoderWithDescriptor:pd];if(!e)return nil;id<MTL4ArgumentTable>vt=nil,ft=nil;NSDictionary *plan=nil;
    for(NSUInteger i=0;i<records.length/sizeof(rec_t);i++){
        rec_t r=ops[i];uint64_t *a=r.a;
        if(r.op==RO_R_STORE_C)continue;
        if(r.op==RO_UI_META){
            NSDictionary *meta=rec_id(a[0]);plan=meta[@"private_plan"];NSDictionary *va=plan[@"vs_argument"],*fa=plan[@"fs_argument"];
            [vt setAddress:[va[@"destination"]unsignedLongLongValue]atIndex:[va[@"slot"]unsignedIntegerValue]];
            [ft setAddress:[fa[@"destination"]unsignedLongLongValue]atIndex:[fa[@"slot"]unsignedIntegerValue]];
            for(NSDictionary *vb in plan[@"vertex_bindings"])[vt setAddress:[vb[@"destination"]unsignedLongLongValue]atIndex:[vb[@"slot"]unsignedIntegerValue]];
            continue;
        }
        if(r.op==RO_R_TABLE || r.op==RO_T_ADDR || r.op==RO_T_TEX || r.op==RO_T_SAMP){
            id t=tables[@(a[0])];if(!t)continue;
            if(r.op==RO_R_TABLE){if(a[1]&MTLRenderStageVertex)vt=t;if(a[1]&MTLRenderStageFragment)ft=t;}
            a[0]=(uint64_t)(uintptr_t)(__bridge void*)t;
        }
        if(r.op==RO_R_DRAW_IDX){NSDictionary *index=plan[@"index"];a[3]=[index[@"destination"]unsignedLongLongValue];a[4]=[index[@"bytes"]unsignedLongLongValue];}
        switch(r.op){
    case RO_BAR_QUEUE: [(id<MTL4CommandEncoder>)e barrierAfterQueueStages:a[0] beforeStages:a[1] visibilityOptions:a[2]]; break;
    case RO_BAR_AFTER_STAGES: [(id<MTL4CommandEncoder>)e barrierAfterStages:a[0] beforeQueueStages:a[1] visibilityOptions:a[2]]; break;
    case RO_BAR_ENCODER: [(id<MTL4CommandEncoder>)e barrierAfterEncoderStages:a[0] beforeEncoderStages:a[1] visibilityOptions:a[2]]; break;
    case RO_R_TABLE: [(id<MTL4RenderCommandEncoder>)e setArgumentTable:rec_id(a[0]) atStages:a[1]]; break;
    case RO_R_PSO: [e setRenderPipelineState:floating?pipelines[@(a[0])]:rec_id(a[0])]; break;
    case RO_R_DSS: [(id<MTL4RenderCommandEncoder>)e setDepthStencilState:rec_id(a[0])]; break;
    case RO_R_VIEWPORT: { MTLViewport v;memcpy(&v,a,sizeof v);v.originX*=sx;v.originY*=sy;v.width*=sx;v.height*=sy;[e setViewport:v];break; }
    case RO_R_SCISSOR: { MTLScissorRect v;memcpy(&v,a,sizeof v);
        double x0=fmax(0,fmin(output.width,ceil(v.x*sx-.5))),y0=fmax(0,fmin(output.height,ceil(v.y*sy-.5)));
        double x1=fmax(x0,fmin(output.width,ceil((v.x+v.width)*sx-.5))),y1=fmax(y0,fmin(output.height,ceil((v.y+v.height)*sy-.5)));
        [e setScissorRect:(MTLScissorRect){(NSUInteger)x0,(NSUInteger)y0,(NSUInteger)(x1-x0),(NSUInteger)(y1-y0)}];break; }
    case RO_R_CULL: [(id<MTL4RenderCommandEncoder>)e setCullMode:a[0]]; break;
    case RO_R_WINDING: [(id<MTL4RenderCommandEncoder>)e setFrontFacingWinding:a[0]]; break;
    case RO_R_FILL: [(id<MTL4RenderCommandEncoder>)e setTriangleFillMode:a[0]]; break;
    case RO_R_BIAS: [(id<MTL4RenderCommandEncoder>)e setDepthBias:rec_fv(a[0]) slopeScale:rec_fv(a[1]) clamp:rec_fv(a[2])]; break;
    case RO_R_CLIP: [(id<MTL4RenderCommandEncoder>)e setDepthClipMode:a[0]]; break;
    case RO_R_SREF: [(id<MTL4RenderCommandEncoder>)e setStencilReferenceValue:(uint32_t)a[0]]; break;
    case RO_R_BLEND: [(id<MTL4RenderCommandEncoder>)e setBlendColorRed:rec_fv(a[0]) green:rec_fv(a[1]) blue:rec_fv(a[2]) alpha:rec_fv(a[3])]; break;
    case RO_R_VIS: [e setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0]; break;
    case RO_R_AMP: { MTLVertexAmplificationViewMapping m = {(uint32_t)a[1], (uint32_t)a[2]}; [(id<MTL4RenderCommandEncoder>)e setVertexAmplificationCount:a[0] viewMappings:a[0] ? &m : NULL]; break; }
    case RO_R_DRAW: [(id<MTL4RenderCommandEncoder>)e drawPrimitives:a[0] vertexStart:a[1] vertexCount:a[2]]; break;
    case RO_R_DRAW_INST: [(id<MTL4RenderCommandEncoder>)e drawPrimitives:a[0] vertexStart:a[1] vertexCount:a[2] instanceCount:a[3] baseInstance:a[4]]; break;
    case RO_R_DRAW_IDX: [(id<MTL4RenderCommandEncoder>)e drawIndexedPrimitives:a[0] indexCount:a[1] indexType:a[2] indexBuffer:a[3] indexBufferLength:a[4] instanceCount:a[5] baseVertex:(NSInteger)a[6] baseInstance:a[7]]; break;
    case RO_T_ADDR: [(id<MTL4ArgumentTable>)rec_id(a[0]) setAddress:a[1] atIndex:a[2]]; break;
    case RO_T_TEX: [(id<MTL4ArgumentTable>)rec_id(a[0]) setTexture:(MTLResourceID){a[1]} atIndex:a[2]]; break;
    case RO_T_SAMP: [(id<MTL4ArgumentTable>)rec_id(a[0]) setSamplerState:(MTLResourceID){a[1]} atIndex:a[2]]; break;
    case RO_LABEL: [(id<MTL4CommandEncoder>)e setLabel:rec_id(a[0])]; break;
        default: [e endEncoding];return nil; // preflight covers every case before any draw.
        }
    }
    [e endEncoding];return owners;
}
