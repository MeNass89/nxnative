#include <metal_stdlib>
using namespace metal;
struct CopyJob {
    const device uchar *source;
    device uchar *destination;
    ulong bytes;
};
kernel void ui_snapshot_copy(const device CopyJob *jobs [[buffer(0)]],
                             uint3 group [[threadgroup_position_in_grid]],
                             uint lane [[thread_index_in_threadgroup]]) {
    CopyJob job=jobs[group.x];
    for (ulong k=lane; k<job.bytes; k+=256) job.destination[k]=job.source[k];
}

kernel void ui_float_copy(texture2d<float,access::read> source [[texture(0)]],texture2d<half,access::write> target [[texture(1)]],uint2 p [[thread_position_in_grid]]) { if(p.x<target.get_width()&&p.y<target.get_height())target.write(half4(half3(source.read(p).rgb),half(1)),p); }
