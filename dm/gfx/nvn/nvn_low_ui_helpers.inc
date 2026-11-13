// Persistent GPU tables are packet-owned. Only sealing a warm packet may grow this pool.
static NSArray *lui_packet_table_sets(LowUIPacket *packet,NSArray *passes,BOOL allowGrow) {
    BOOL grow=allowGrow&&!low_ui_armed();
    NSMutableDictionary *pool=packet.tablePool;
    if(!pool){if(!grow)return nil;pool=[NSMutableDictionary dictionary];}
    NSMutableDictionary *pending=[NSMutableDictionary dictionary];
    NSMutableArray *outputs=[NSMutableArray arrayWithCapacity:2];
    for(NSUInteger render=0;render<2;render++) {
        NSMutableArray *perPass=[NSMutableArray arrayWithCapacity:passes.count];
        for(NSUInteger pass=0;pass<passes.count;pass++) {
            NSData *records=passes[pass][@"records"];
            if(records.length%sizeof(rec_t))return nil;
            const rec_t *ops=records.bytes;
            NSMutableDictionary *tables=[NSMutableDictionary dictionary];
            for(NSUInteger i=0;i<records.length/sizeof(rec_t);i++)if(ops[i].op==RO_R_TABLE) {
                NSNumber *native=@(ops[i].a[0]);if(tables[native])continue;
                if(!ops[i].a[0])return nil;
                NSString *key=[NSString stringWithFormat:@"%lu/%lu/%llx",(unsigned long)render,(unsigned long)pass,(unsigned long long)ops[i].a[0]];
                id table=pending[key]?:pool[key];
                if(!table) {
                    // There is no GPU allocation in a runtime/ARM lookup. New IDs need another warmup.
                    if(!grow||low_ui_armed()){atomic_fetch_add(&g_lui_stats[LS_TABLE_MISS],1);return nil;}
                    MTL4ArgumentTableDescriptor *d=[MTL4ArgumentTableDescriptor new];
                    d.maxBufferBindCount=31;d.maxTextureBindCount=1;d.maxSamplerStateBindCount=1;
                    NSError *error=nil;table=[g_dev newArgumentTableWithDescriptor:d error:&error];
                    if(!table)return nil;
                    atomic_fetch_add(&g_lui_stats[LS_TABLE_ALLOC],1);pending[key]=table;
                }
                tables[native]=table;
            }
            [perPass addObject:[tables copy]];
        }
        [outputs addObject:[perPass copy]];
    }
    // Transaction: no partial pool coverage is published when any required table is missing.
    if(pending.count)[pool addEntriesFromDictionary:pending];
    if(grow)packet.tablePool=pool;
    return [outputs copy];
}
static NSNumber *lui_slot_key(uint64_t table,uint64_t slot) { return @((table<<5)^slot); }
// Validate before any private pixel work. Logical state tracks only original writes,
// so a cloned binding inherited by the next draw is compared to its original source.
static NSString *lui_validate(NSData *records,NSArray *initial) {
    const rec_t *ops=records.bytes;NSMutableDictionary *slots=[NSMutableDictionary dictionary];
    for (NSData *raw in initial) { rec_t r;memcpy(&r,raw.bytes,sizeof r);
        if (r.op==RO_T_ADDR) slots[lui_slot_key(r.a[0],r.a[2])]=@(r.a[1]); }
    uint64_t vt=0,ft=0;NSDictionary *plan=nil;unsigned notes=0,draws=0;
    for (NSUInteger i=0;i<records.length/sizeof(rec_t);i++) {
        rec_t r=ops[i];
        if (r.op==RO_R_TABLE) { if(r.a[1]&MTLRenderStageVertex)vt=r.a[0];if(r.a[1]&MTLRenderStageFragment)ft=r.a[0]; }
        if (r.op==RO_T_ADDR) slots[lui_slot_key(r.a[0],r.a[2])]=@(r.a[1]);
        if (r.op==RO_UI_META) {
            NSDictionary *meta=rec_id(r.a[0]);plan=meta[@"private_plan"];notes++;
            if (![plan[@"valid"] boolValue]) return plan[@"reason"]?:@"missing detached plan";
            if (!vt || !ft) return @"missing vertex or fragment table";
            NSDictionary *ab[2]={plan[@"vs_argument"],plan[@"fs_argument"]};uint64_t tables[2]={vt,ft};
            for(int st=0;st<2;st++)if(!ab[st] || [slots[lui_slot_key(tables[st],[ab[st][@"slot"] unsignedLongLongValue])] unsignedLongLongValue]!=[ab[st][@"source"] unsignedLongLongValue])return @"effective argument address differs";
            for(NSDictionary *vb in plan[@"vertex_bindings"])if([slots[lui_slot_key(vt,[vb[@"slot"] unsignedLongLongValue])] unsignedLongLongValue]!=[vb[@"source"] unsignedLongLongValue])return @"effective vertex address differs";
        }
        if(r.op>=RO_R_DRAW && r.op<=RO_R_DRAW_IDX_IND) {
            draws++;if(!plan)return @"draw without detached plan";
            id index=plan[@"index"];
            if(r.op==RO_R_DRAW_IDX) {
                if(![index isKindOfClass:NSDictionary.class])return @"indexed draw has no snapshot";
                if(r.a[3]!=[index[@"source"] unsignedLongLongValue] || r.a[1]!=[index[@"count"] unsignedLongLongValue] || r.a[2]!=[index[@"type"] unsignedLongLongValue])return @"actual index record differs from snapshot";
            } else if(r.op==RO_R_DRAW_IND || r.op==RO_R_DRAW_IDX_IND)return @"indirect replay unsupported";
            else if(index && index!=NSNull.null)return @"index snapshot for nonindexed draw";
            plan=nil;
        }
    }
    return draws && notes==draws?nil:@"draw/plan coverage differs";
}
static void lui_encode_snapshots(NSData *records,id<MTL4CommandBuffer> cb) {
    const rec_t *ops=records.bytes;
    id<MTL4ComputeCommandEncoder> e=[cb computeCommandEncoder];lui_barrier(e,1);
    for(NSUInteger i=0;i<records.length/sizeof(rec_t);i++)if(ops[i].op==RO_UI_META) {
        NSDictionary *meta=rec_id(ops[i].a[0]),*p=meta[@"private_plan"];
        NSUInteger count=[p[@"job_count"] unsignedIntegerValue];
        if(count) {
            [g_uid_copy_table setAddress:[p[@"jobs_gpu"] unsignedLongLongValue] atIndex:0];
            [e setArgumentTable:g_uid_copy_table];[e setComputePipelineState:g_uid_copy_pipeline];
            [e dispatchThreadgroups:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(256,1,1)];
        }
        for(NSDictionary *pair in p[@"texture_copies"]) {
            id<MTLTexture> source=pair[@"source"],destination=pair[@"destination"];
            [e copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(source.width,source.height,1) toTexture:destination destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        }
    }
    lui_barrier(e,0);[e endEncoding];
}
