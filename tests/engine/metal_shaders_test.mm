#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <string>

#include "../../engine/src/gpu/gx_render_metal_shaders.h"

namespace shaders = wp::gx::render::metal_shaders;

namespace {

int g_failures = 0;

id<MTLLibrary> build(id<MTLDevice> device, const std::string& source, const char* what) {
    MTLCompileOptions* options = [MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    options.preserveInvariance = YES;
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
    if (!library) {
        std::printf("FAIL %s: %s\n", what, error ? [[error localizedDescription] UTF8String] : "unknown error");
        g_failures++;
    } else {
        std::printf("ok   %s\n", what);
    }
    return library;
}

void pipeline(id<MTLDevice> device, id<MTLFunction> vertex, id<MTLFunction> fragment, MTLPixelFormat format, bool depth, bool dual, const char* what) {
    if (!vertex || !fragment) {
        std::printf("FAIL %s: missing function\n", what);
        g_failures++;
        return;
    }
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.colorAttachments[0].pixelFormat = format;
    if (depth) {
        descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    }
    if (dual) {
        descriptor.colorAttachments[0].blendingEnabled = YES;
        descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSource1Alpha;
        descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSource1Alpha;
    }
    NSError* error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!state) {
        std::printf("FAIL %s: %s\n", what, error ? [[error localizedDescription] UTF8String] : "unknown error");
        g_failures++;
    } else {
        std::printf("ok   %s\n", what);
    }
}

std::string variant(bool early, bool keeps_all = false) {
    std::string source = shaders::kCommon;
    source += "#define WP_VARIANT\n";
    source += early ? "#define WP_SKIP_LATE\n" : "#define WP_SKIP_EARLY\n";
    if (keeps_all) {
        source += "#define WP_NO_DISCARD\n";
    }
    source += "constant uint4 kStage[16] = {";
    for (int i = 0; i < 16; i++) {
        source += i < 2 ? "uint4(0x8FFFFu, 0x8FFFu, 0x48u, 0x1Cu)" : "uint4(0u, 0u, 0u, 0u)";
        source += i + 1 < 16 ? ", " : "};\n";
    }
    source += "constant uint4 kTevind[16] = {";
    for (int i = 0; i < 16; i++) {
        source += "uint4(0u, 0u, 0u, 0u)";
        source += i + 1 < 16 ? ", " : "};\n";
    }
    source += "constant uint4 kSwaps = uint4(228u, 228u, 228u, 228u);\nconstant uint4 kIndirect = uint4(0x100u, 0u, 0u, 0u);\n";
    source += "#define STAGE(i) kStage[i]\n#define TEVIND(i) kTevind[i]\n#define SWAPS kSwaps\n#define INDIRECT kIndirect\n#define COUNT 2u\n";
    source += shaders::kTev;
    return source;
}

}

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::printf("no Metal device on this machine, shaders not checked\n");
            return 0;
        }
        std::printf("Metal device: %s\n", [[device name] UTF8String]);
        std::string all = std::string(shaders::kCommon) + shaders::kVertices + shaders::kTev + shaders::kUtility;
        id<MTLLibrary> library = build(device, all, "generic shaders");
        id<MTLLibrary> late = build(device, variant(false), "pixel shader variant");
        id<MTLLibrary> early = build(device, variant(true), "pixel shader variant with early depth");
        id<MTLLibrary> opaque = build(device, variant(false, true), "pixel shader variant without alpha discard");
        if (library) {
            id<MTLFunction> tev_vertex = [library newFunctionWithName:@"tev_vertex"];
            id<MTLFunction> run_vertex = [library newFunctionWithName:@"run_vertex"];
            id<MTLFunction> fullscreen = [library newFunctionWithName:@"fullscreen_vertex"];
            pipeline(device, tev_vertex, [library newFunctionWithName:@"tev_fragment"], MTLPixelFormatRGBA8Unorm, true, true, "screen vertices with the generic shader");
            pipeline(device, run_vertex, [library newFunctionWithName:@"tev_fragment_early"], MTLPixelFormatRGBA8Unorm, true, false, "model vertices with early depth");
            pipeline(device, fullscreen, [library newFunctionWithName:@"copy_fragment"], MTLPixelFormatRGBA8Unorm, false, false, "EFB copy");
            pipeline(device, fullscreen, [library newFunctionWithName:@"clear_fragment"], MTLPixelFormatRGBA8Unorm, true, false, "EFB clear");
            pipeline(device, fullscreen, [library newFunctionWithName:@"present_fragment"], MTLPixelFormatBGRA8Unorm, false, false, "present");
            pipeline(device, fullscreen, [library newFunctionWithName:@"present_smooth_fragment"], MTLPixelFormatBGRA8Unorm, false, false, "smooth present");
            pipeline(device, fullscreen, [library newFunctionWithName:@"overlay_fragment"], MTLPixelFormatBGRA8Unorm, false, false, "overlay");
            if (late) {
                pipeline(device, run_vertex, [late newFunctionWithName:@"tev_fragment"], MTLPixelFormatRGBA8Unorm, true, true, "model vertices with a variant");
            }
            if (opaque) {
                pipeline(device, run_vertex, [opaque newFunctionWithName:@"tev_fragment"], MTLPixelFormatRGBA8Unorm, true, false, "model vertices with an opaque variant");
            }
            if (early) {
                pipeline(device, tev_vertex, [early newFunctionWithName:@"tev_fragment_early"], MTLPixelFormatRGBA8Unorm, true, false, "screen vertices with an early variant");
            }
        }
    }
    std::printf("%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
