#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600
#endif

#include "wp/fiber.h"

#if defined(__ANDROID__) && !defined(WP_ASM_FIBERS)
#define WP_ASM_FIBERS
#endif

#ifdef _WIN32
#include <windows.h>
#elif defined(WP_ASM_FIBERS)
#include <cstdint>
#include <cstdlib>
#include <cstring>
#else
#include <ucontext.h>

#include <cstdlib>
#endif

namespace wp::fiber {

#ifdef _WIN32

namespace {

struct Start {
    Entry entry;
    void* parameter;
};

thread_local Handle g_current = nullptr;

VOID CALLBACK run(PVOID data) {
    Start start = *static_cast<Start*>(data);
    delete static_cast<Start*>(data);
    start.entry(start.parameter);
}

}

Handle adopt_current_thread() {
    g_current = ConvertThreadToFiber(nullptr);
    return g_current;
}

Handle current() {
    return g_current;
}

Handle create(size_t stack_size, Entry entry, void* parameter) {
    return CreateFiber(stack_size, run, new Start{entry, parameter});
}

void switch_to(Handle target) {
    g_current = target;
    SwitchToFiber(target);
}

void destroy(Handle fiber) {
    DeleteFiber(fiber);
}

#elif defined(WP_ASM_FIBERS)

extern "C" void wp_fiber_swap(void** save, void* load);
extern "C" void wp_fiber_begin();

namespace {

struct Fiber {
    void* sp = nullptr;
    void* stack = nullptr;
    Entry entry = nullptr;
    void* parameter = nullptr;
};

thread_local Fiber* g_current = nullptr;

}

extern "C" [[noreturn]] void wp_fiber_main(void* data) {
    Fiber* self = static_cast<Fiber*>(data);
    self->entry(self->parameter);
    std::abort();
}

#if defined(__aarch64__)
asm(R"ASM(
.text
.globl wp_fiber_swap
.type wp_fiber_swap, %function
.p2align 4
wp_fiber_swap:
    sub sp, sp, #160
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x2, sp
    str x2, [x0]
    mov sp, x1
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #160
    ret
.size wp_fiber_swap, .-wp_fiber_swap

.globl wp_fiber_begin
.type wp_fiber_begin, %function
.p2align 4
wp_fiber_begin:
    mov x0, x19
    mov x29, #0
    mov x30, #0
    b wp_fiber_main
.size wp_fiber_begin, .-wp_fiber_begin
)ASM");

constexpr size_t kSavedBytes = 160;
constexpr size_t kReturnSlot = 88;
constexpr size_t kFiberSlot = 0;
constexpr size_t kControlSlot = 0;
constexpr bool kControlWords = false;
#elif defined(__x86_64__)
asm(R"ASM(
.text
.globl wp_fiber_swap
.type wp_fiber_swap, @function
.p2align 4
wp_fiber_swap:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    ldmxcsr (%rsp)
    fldcw 4(%rsp)
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size wp_fiber_swap, .-wp_fiber_swap

.globl wp_fiber_begin
.type wp_fiber_begin, @function
.p2align 4
wp_fiber_begin:
    movq %rbx, %rdi
    xorl %ebp, %ebp
    pushq $0
    jmp wp_fiber_main
.size wp_fiber_begin, .-wp_fiber_begin
)ASM");

constexpr size_t kSavedBytes = 64;
constexpr size_t kReturnSlot = 56;
constexpr size_t kFiberSlot = 40;
constexpr size_t kControlSlot = 0;
constexpr bool kControlWords = true;
#else
#error "no fiber switch for this processor"
#endif

Handle adopt_current_thread() {
    g_current = new Fiber;
    return g_current;
}

Handle current() {
    return g_current;
}

Handle create(size_t stack_size, Entry entry, void* parameter) {
    Fiber* fiber = new Fiber;
    fiber->stack = std::malloc(stack_size);
    fiber->entry = entry;
    fiber->parameter = parameter;
    uintptr_t top = (reinterpret_cast<uintptr_t>(fiber->stack) + stack_size) & ~static_cast<uintptr_t>(15);
    uint8_t* frame = reinterpret_cast<uint8_t*>(top - kSavedBytes);
    std::memset(frame, 0, kSavedBytes);
    void* begin = reinterpret_cast<void*>(&wp_fiber_begin);
    std::memcpy(frame + kReturnSlot, &begin, sizeof begin);
    std::memcpy(frame + kFiberSlot, &fiber, sizeof fiber);
    if (kControlWords) {
        uint32_t mxcsr = 0x1F80;
        uint16_t fcw = 0x037F;
#if defined(__x86_64__)
        asm volatile("stmxcsr %0" : "=m"(mxcsr));
        asm volatile("fnstcw %0" : "=m"(fcw));
#endif
        std::memcpy(frame + kControlSlot, &mxcsr, sizeof mxcsr);
        std::memcpy(frame + kControlSlot + 4, &fcw, sizeof fcw);
    }
    fiber->sp = frame;
    return fiber;
}

void switch_to(Handle target) {
    Fiber* from = g_current;
    Fiber* to = static_cast<Fiber*>(target);
    g_current = to;
    wp_fiber_swap(&from->sp, to->sp);
}

void destroy(Handle fiber) {
    Fiber* target = static_cast<Fiber*>(fiber);
    std::free(target->stack);
    delete target;
}

#else

namespace {

struct Fiber {
    ucontext_t context;
    void* stack = nullptr;
    Entry entry = nullptr;
    void* parameter = nullptr;
};

thread_local Fiber* g_current = nullptr;
thread_local Fiber* g_starting = nullptr;

void run() {
    Fiber* self = g_starting;
    self->entry(self->parameter);
    std::abort();
}

}

Handle adopt_current_thread() {
    g_current = new Fiber;
    return g_current;
}

Handle current() {
    return g_current;
}

Handle create(size_t stack_size, Entry entry, void* parameter) {
    Fiber* fiber = new Fiber;
    fiber->stack = std::malloc(stack_size);
    fiber->entry = entry;
    fiber->parameter = parameter;
    getcontext(&fiber->context);
    fiber->context.uc_stack.ss_sp = fiber->stack;
    fiber->context.uc_stack.ss_size = stack_size;
    fiber->context.uc_link = nullptr;
    makecontext(&fiber->context, run, 0);
    return fiber;
}

void switch_to(Handle target) {
    Fiber* from = g_current;
    Fiber* to = static_cast<Fiber*>(target);
    g_current = to;
    g_starting = to;
    swapcontext(&from->context, &to->context);
}

void destroy(Handle fiber) {
    Fiber* target = static_cast<Fiber*>(fiber);
    std::free(target->stack);
    delete target;
}

#endif

}
