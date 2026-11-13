// The existing host copies W into its own private history texture before UI.
// This is an observed read of C, not an untracked C writer. All other compute/copy cases refuse.
static BOOL low_ui_known_read(const rec_t*r,id<MTLTexture>target){
    if(r->op!=RO_C_COPY_TT)return NO;
    id<MTLTexture>source=rec_id(r->a[0]),destination=rec_id(r->a[1]);
    return source&&destination&&lui_root(source)==lui_root(target)&&destination==g_temporal_full_scene&&
        lui_root(destination)!=lui_root(target)&&destination.storageMode==MTLStorageModePrivate&&!destination.buffer&&!destination.heap&&
        source.pixelFormat==destination.pixelFormat&&source.width==destination.width&&source.height==destination.height&&
        !r->a[2]&&!r->a[3]&&!r->a[4]&&!r->a[5]&&!r->a[6]&&r->a[7]==source.width&&r->a[8]==source.height&&r->a[9]==1&&
        !r->a[10]&&!r->a[11]&&!r->a[12]&&!r->a[13]&&!r->a[14];
}
