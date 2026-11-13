#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>

// Independent reference: Metal's hardware conversion into an sRGB attachment.
// Candidate: the same fragment values, explicitly encoded into an unorm attachment.
// No game code or data, display, audio or global setting is used.
enum { W = 512, H = 64, N = W * H };
typedef struct { float r, g, b, a; } Pixel;
static NSString * const source =
@"#include <metal_stdlib>\nusing namespace metal;\n"
"struct V { float4 p [[position]]; };\n"
"vertex V vs(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return {float4(p[i],0,1)}; }\n"
"fragment float4 reference(V v [[stage_in]],device const float4 *p [[buffer(0)]]) { return p[uint(v.p.y)*512+uint(v.p.x)]; }\n"
"float3 encode_sdr(float3 x) { x=clamp(x,0.0f,1.0f);return select(1.055f*pow(x,float3(1.0f/2.4f))-0.055f,12.92f*x,x<=0.0031308f); }\n"
"fragment float4 candidate(V v [[stage_in]],device const float4 *p [[buffer(0)]]) { float4 c=p[uint(v.p.y)*512+uint(v.p.x)];return float4(encode_sdr(c.rgb),c.a); }\n";

int main(void) { @autoreleasepool {
    NSError *error = nil;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return 2;
    MTLCompileOptions *compile = [MTLCompileOptions new];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:compile error:&error];
    if (!library) { fprintf(stderr,"compile: %s\n",error.description.UTF8String); return 3; }
    id<MTLBuffer> input = [device newBufferWithLength:N*sizeof(Pixel) options:MTLResourceStorageModeShared];
    Pixel *pixels = input.contents;
    for (unsigned i=0;i<N;i++) {
        float x=(float)(i%8192)/8191;
        switch (i/8192) {
            case 1: x=.0031308f+((int)(i%8192)-4096)*0.00000001f; break;
            case 2: x=(float)((__fp16)x); break;
            case 3: x=-.1f+1.2f*x; break;
        }
        pixels[i]=(Pixel){x,(float)((i*73u)%N)/(N-1),(float)((__fp16)((float)((i*107u)%N)/(N-1))),1};
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLTexture> textures[2];
    for (unsigned arm=0;arm<2;arm++) {
        MTLPixelFormat format=arm?MTLPixelFormatRGBA8Unorm:MTLPixelFormatRGBA8Unorm_sRGB;
        MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:W height:H mipmapped:NO];
        td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;
        textures[arm]=[device newTextureWithDescriptor:td];
        MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
        pd.vertexFunction=[library newFunctionWithName:@"vs"];
        pd.fragmentFunction=[library newFunctionWithName:arm?@"candidate":@"reference"];
        pd.colorAttachments[0].pixelFormat=format;
        id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!pipeline || !textures[arm]) { fprintf(stderr,"pipeline: %s\n",error.description.UTF8String);return 4; }
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=textures[arm];
        pass.colorAttachments[0].loadAction=MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder=[commands renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];[encoder setFragmentBuffer:input offset:0 atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];
    }
    [commands commit];[commands waitUntilCompleted];
    if (commands.error) { fprintf(stderr,"GPU: %s\n",commands.error.description.UTF8String);return 5; }
    NSMutableData *data[2];
    for(unsigned arm=0;arm<2;arm++) {
        data[arm]=[NSMutableData dataWithLength:N*4];
        [textures[arm] getBytes:data[arm].mutableBytes bytesPerRow:W*4 fromRegion:MTLRegionMake2D(0,0,W,H) mipmapLevel:0];
    }
    const uint8_t *a=data[0].bytes,*b=data[1].bytes;
    unsigned maximum=0,different=0,overOne=0,alphaBad=0,range=0;
    unsigned perGroup[4]={0},overGroup[4]={0};
    for(unsigned i=0;i<N;i++) for(unsigned c=0;c<4;c++) {
        unsigned d=(unsigned)abs((int)a[4*i+c]-(int)b[4*i+c]);
        if(c<3){maximum=MAX(maximum,d);different+=d!=0;overOne+=d>1;perGroup[i/8192]+=d!=0;overGroup[i/8192]+=d>1;range+=a[4*i+c]!=0;}
        else alphaBad+=a[4*i+c]!=255||b[4*i+c]!=255;
    }
    NSDictionary *result=@{@"device":device.name,@"pixels":@(N),@"rgb_channels":@(N*3),@"max_lsb":@(maximum),@"different_channels":@(different),@"channels_over_one_lsb":@(overOne),@"alpha_errors":@(alphaBad),@"nonzero_reference_channels":@(range),@"different_by_input_group":@[@(perGroup[0]),@(perGroup[1]),@(perGroup[2]),@(perGroup[3])],@"over_one_by_input_group":@[@(overGroup[0]),@(overGroup[1]),@(overGroup[2]),@(overGroup[3])],@"gpu_error":@"",@"purpose":@"Explicit SDR output transfer versus hardware sRGB attachment, not game image qualification"};
    NSData *json=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil];
    fwrite(json.bytes,1,json.length,stdout);fputc('\n',stdout);
    return maximum<=1 && !alphaBad && range ? 0 : 6;
} }
