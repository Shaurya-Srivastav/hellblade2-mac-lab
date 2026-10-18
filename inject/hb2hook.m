// Injected into Wine's game process (x86_64, under Rosetta) with DYLD_INSERT_LIBRARIES.
// Replaces -[MTLFXFrameInterpolatorDescriptor newFrameInterpolatorWithDevice:] so every
// frame interpolator D3DMetal creates passes through us; logs to /tmp/hb2hook.log.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <unistd.h>

static FILE *logFile;

static void hooklog(NSString *message) {
    if (!logFile) return;
    fprintf(logFile, "[%d] %s\n", getpid(), message.UTF8String);
    fflush(logFile);
}

static IMP originalNewInterpolator;

static id hookedNewInterpolator(id descriptor, SEL selector, id device) {
    NSMutableString *info = [NSMutableString stringWithString:@"newFrameInterpolatorWithDevice:"];
    for (NSString *key in @[ @"inputWidth", @"inputHeight", @"outputWidth", @"outputHeight",
                             @"colorTextureFormat", @"outputTextureFormat", @"depthTextureFormat",
                             @"motionTextureFormat", @"uiTextureFormat" ]) {
        @try {
            [info appendFormat:@" %@=%@", key, [descriptor valueForKey:key]];
        } @catch (NSException *exception) {
            [info appendFormat:@" %@=?", key];
        }
    }
    id interpolator = ((id (*)(id, SEL, id))originalNewInterpolator)(descriptor, selector, device);
    [info appendFormat:@" -> %@", NSStringFromClass([interpolator class])];
    hooklog(info);
    return interpolator;
}

__attribute__((constructor)) static void hb2hookInit(void) {
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return;
    logFile = fopen("/tmp/hb2hook.log", "a");
    if (getenv("HB2HOOK_TRACE")) hooklog([NSString stringWithFormat:@"seen %s", path]);
    if (!strstr(path, "wine64-preloader")) {
        fclose(logFile);
        logFile = NULL;
        return;
    }
#if __x86_64__
    const char *arch = "x86_64";
#else
    const char *arch = "arm64";
#endif
    hooklog([NSString stringWithFormat:@"loaded into %s (%s)", path, arch]);

    if (!dlopen("/System/Library/Frameworks/MetalFX.framework/MetalFX", RTLD_LAZY)) {
        hooklog([NSString stringWithFormat:@"MetalFX dlopen failed: %s", dlerror()]);
        return;
    }
    Class descriptorClass = NSClassFromString(@"MTLFXFrameInterpolatorDescriptor");
    Method method = descriptorClass
        ? class_getInstanceMethod(descriptorClass, @selector(newFrameInterpolatorWithDevice:))
        : NULL;
    if (!method) {
        hooklog(@"MTLFXFrameInterpolatorDescriptor newFrameInterpolatorWithDevice: not found");
        return;
    }
    originalNewInterpolator = method_setImplementation(method, (IMP)hookedNewInterpolator);
    hooklog(@"hooked -[MTLFXFrameInterpolatorDescriptor newFrameInterpolatorWithDevice:]");
}
