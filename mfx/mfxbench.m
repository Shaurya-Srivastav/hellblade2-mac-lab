// mfxbench: measures MetalFX temporal scaling and frame interpolation GPU cost.
// usage: mfxbench <inW> <inH> <outW> <outH> [iterations] [noscaler]
//   noscaler: create the frame interpolator without a chained temporal scaler
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <mach/mach_time.h>

static id<MTLTexture> tex(id<MTLDevice> d, MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u) {
    MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    t.usage = u | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    t.storageMode = MTLStorageModePrivate;
    if (f == MTLPixelFormatDepth32Float) t.usage = u | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    return [d newTextureWithDescriptor:t];
}

static double ms(id<MTLCommandBuffer> cb) { return (cb.GPUEndTime - cb.GPUStartTime) * 1000.0; }

static int cmp(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc < 5) { fprintf(stderr, "usage: mfxbench inW inH outW outH [iterations] [noscaler]\n"); return 1; }
        NSUInteger inW = atoi(argv[1]), inH = atoi(argv[2]), outW = atoi(argv[3]), outH = atoi(argv[4]);
        int iterations = argc > 5 ? atoi(argv[5]) : 60;
        BOOL chain = !(argc > 6 && strcmp(argv[6], "noscaler") == 0);
#if defined(__x86_64__)
        const char *arch = "x86_64";
#else
        const char *arch = "arm64";
#endif
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        printf("%s %s in=%lux%lu out=%lux%lu chain=%d\n", arch, dev.name.UTF8String,
               (unsigned long)inW, (unsigned long)inH, (unsigned long)outW, (unsigned long)outH, chain);

        MTLFXTemporalScalerDescriptor *sd = [MTLFXTemporalScalerDescriptor new];
        sd.colorTextureFormat = MTLPixelFormatRGBA16Float;
        sd.depthTextureFormat = MTLPixelFormatDepth32Float;
        sd.motionTextureFormat = MTLPixelFormatRG16Float;
        sd.outputTextureFormat = MTLPixelFormatRGBA16Float;
        sd.inputWidth = inW; sd.inputHeight = inH; sd.outputWidth = outW; sd.outputHeight = outH;
        sd.autoExposureEnabled = YES;
        sd.inputContentPropertiesEnabled = YES;
        sd.inputContentMinScale = 1.0f; sd.inputContentMaxScale = 3.0f;
        uint64_t t0 = mach_absolute_time();
        id<MTLFXTemporalScaler> scaler = [sd newTemporalScalerWithDevice:dev];
        uint64_t t1 = mach_absolute_time();
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        printf("temporal scaler: %s, created in %.0f ms\n", scaler ? "ok" : "FAILED", (t1 - t0) * tb.numer / tb.denom / 1e6);
        if (!scaler) return 2;

        MTLFXFrameInterpolatorDescriptor *fd = [MTLFXFrameInterpolatorDescriptor new];
        fd.colorTextureFormat = MTLPixelFormatRGBA16Float;
        fd.outputTextureFormat = MTLPixelFormatRGB10A2Unorm;
        fd.depthTextureFormat = MTLPixelFormatDepth32Float;
        fd.motionTextureFormat = MTLPixelFormatRG16Float;
        fd.uiTextureFormat = MTLPixelFormatRGBA8Unorm;
        fd.inputWidth = inW; fd.inputHeight = inH; fd.outputWidth = outW; fd.outputHeight = outH;
        if (chain) fd.scaler = scaler;
        t0 = mach_absolute_time();
        id<MTLFXFrameInterpolator> interp = [MTLFXFrameInterpolatorDescriptor supportsDevice:dev] ? [fd newFrameInterpolatorWithDevice:dev] : nil;
        t1 = mach_absolute_time();
        printf("frame interpolator: %s, created in %.0f ms\n", interp ? "ok" : "FAILED", (t1 - t0) * tb.numer / tb.denom / 1e6);

        id<MTLTexture> color = tex(dev, sd.colorTextureFormat, inW, inH, scaler.colorTextureUsage);
        id<MTLTexture> depth = tex(dev, sd.depthTextureFormat, inW, inH, scaler.depthTextureUsage);
        id<MTLTexture> motion = tex(dev, sd.motionTextureFormat, inW, inH, scaler.motionTextureUsage);
        id<MTLTexture> up[2] = { tex(dev, sd.outputTextureFormat, outW, outH, scaler.outputTextureUsage | (interp ? interp.colorTextureUsage : 0)),
                                 tex(dev, sd.outputTextureFormat, outW, outH, scaler.outputTextureUsage | (interp ? interp.colorTextureUsage : 0)) };
        id<MTLTexture> ui = interp ? tex(dev, fd.uiTextureFormat, outW, outH, interp.uiTextureUsage) : nil;
        id<MTLTexture> out = interp ? tex(dev, fd.outputTextureFormat, outW, outH, interp.outputTextureUsage) : nil;

        id<MTLCommandQueue> q = [dev newCommandQueue];
        double *sms = calloc(iterations, sizeof(double)), *ims = calloc(iterations, sizeof(double));
        for (int i = 0; i < iterations; i++) {
            id<MTLTexture> cur = up[i & 1], prev = up[(i + 1) & 1];
            scaler.colorTexture = color; scaler.depthTexture = depth; scaler.motionTexture = motion;
            scaler.outputTexture = cur;
            scaler.inputContentWidth = inW; scaler.inputContentHeight = inH;
            scaler.jitterOffsetX = ((i * 7) % 16) / 16.0f - 0.5f; scaler.jitterOffsetY = ((i * 11) % 16) / 16.0f - 0.5f;
            scaler.motionVectorScaleX = inW; scaler.motionVectorScaleY = inH;
            scaler.reset = i == 0; scaler.depthReversed = YES;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [scaler encodeToCommandBuffer:cb];
            [cb commit];
            [cb waitUntilCompleted];
            sms[i] = ms(cb);
            if (cb.error) { printf("scaler error: %s\n", cb.error.description.UTF8String); return 3; }

            if (interp) {
                interp.colorTexture = cur; interp.prevColorTexture = prev;
                interp.depthTexture = depth; interp.motionTexture = motion;
                interp.uiTexture = ui; interp.outputTexture = out;
                interp.motionVectorScaleX = inW; interp.motionVectorScaleY = inH;
                interp.deltaTime = 1.0f / 30.0f; interp.fieldOfView = 60.0f; interp.aspectRatio = (float)outW / outH;
                interp.nearPlane = 0.1f; interp.farPlane = 10000.0f;
                interp.jitterOffsetX = scaler.jitterOffsetX; interp.jitterOffsetY = scaler.jitterOffsetY;
                interp.shouldResetHistory = i == 0; interp.depthReversed = YES;
                id<MTLCommandBuffer> ib = [q commandBuffer];
                [interp encodeToCommandBuffer:ib];
                [ib commit];
                [ib waitUntilCompleted];
                ims[i] = ms(ib);
                if (ib.error) { printf("interpolator error: %s\n", ib.error.description.UTF8String); return 4; }
            }
        }
        int skip = iterations / 4, n = iterations - skip;
        qsort(sms + skip, n, sizeof(double), cmp);
        qsort(ims + skip, n, sizeof(double), cmp);
        printf("RESULT %s in=%lux%lu out=%lux%lu chain=%d scaler_ms median=%.2f p10=%.2f p90=%.2f",
               arch, (unsigned long)inW, (unsigned long)inH, (unsigned long)outW, (unsigned long)outH, chain,
               sms[skip + n / 2], sms[skip + n / 10], sms[skip + n * 9 / 10]);
        if (interp) printf(" interp_ms median=%.2f p10=%.2f p90=%.2f", ims[skip + n / 2], ims[skip + n / 10], ims[skip + n * 9 / 10]);
        printf("\n");
    }
    return 0;
}
