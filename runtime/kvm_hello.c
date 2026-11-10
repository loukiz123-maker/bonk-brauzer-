/*
 * LynxDroid: minimal x86 KVM guest execution experiment, NOT Android.
 *
 * No external image or guest code is accepted. A built-in test guest outputs
 * three bytes on debug port 0xe9, halts, then the process destroys the VM. This is a
 * low-level hardware experiment, not a virtual machine manager.
 *
 * Build on x86_64 Linux: cc -std=c11 -O2 -Wall -Wextra -Werror -o kvm-hello kvm_hello.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kvm_io.h"
#include "serial8250.h"

#if defined(__x86_64__)
#define GUEST_MEMORY_SIZE 0x10000u
#define GUEST_CODE_ADDRESS 0x1000u
#define MAX_RUN_MAPPING (16u * 1024u * 1024u)

static volatile sig_atomic_t timed_out = 0;

static void on_alarm(int signum) {
    (void)signum;
    timed_out = 1;
}

static int run_experiment(void) {
    int result = 1, kvm_fd = -1, vm_fd = -1, vcpu_fd = -1;
    void *guest_memory = MAP_FAILED;
    struct kvm_run *run = MAP_FAILED;
    size_t run_size = 0;
    struct sigaction old_action;
    int alarm_installed = 0;
    struct kvm_userspace_memory_region memory = {0};
    struct kvm_sregs sregs;
    struct kvm_regs regs = {0};

    kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (kvm_fd < 0) { perror("open /dev/kvm"); goto done; }
    struct stat device_stat;
    if (fstat(kvm_fd, &device_stat) < 0) { perror("fstat /dev/kvm"); goto done; }
    if (!S_ISCHR(device_stat.st_mode)) { fprintf(stderr, "/dev/kvm is not a character device\n"); goto done; }
    int api = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
    if (api < 0) { perror("KVM_GET_API_VERSION"); goto done; }
    if (api != KVM_API_VERSION) { fprintf(stderr, "Unexpected KVM API: %d\n", api); goto done; }
    vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
    if (vm_fd < 0) { perror("KVM_CREATE_VM"); goto done; }
    if (fcntl(vm_fd, F_SETFD, FD_CLOEXEC) < 0) { perror("vm FD_CLOEXEC"); goto done; }
    guest_memory = mmap(NULL, GUEST_MEMORY_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (guest_memory == MAP_FAILED) { perror("guest mmap"); goto done; }
    /* Output OK on debug port, U on COM1, then halt. */
    static const uint8_t guest_code[] = {
        0xb0, 'O', 0xe6, 0xe9, 0xb0, 'K', 0xe6, 0xe9,
        0xb0, '\n', 0xe6, 0xe9, 0xba, 0xf8, 0x03,
        0xb0, 'U', 0xee, 0xf4
    };
    memcpy((uint8_t *)guest_memory + GUEST_CODE_ADDRESS, guest_code, sizeof(guest_code));
    memory.slot = 0;
    memory.guest_phys_addr = 0;
    memory.memory_size = GUEST_MEMORY_SIZE;
    memory.userspace_addr = (uintptr_t)guest_memory;
    if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memory) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION"); goto done;
    }
    vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
    if (vcpu_fd < 0) { perror("KVM_CREATE_VCPU"); goto done; }
    if (fcntl(vcpu_fd, F_SETFD, FD_CLOEXEC) < 0) { perror("vcpu FD_CLOEXEC"); goto done; }
    int size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (size < 0) { perror("KVM_GET_VCPU_MMAP_SIZE"); goto done; }
    if ((size_t)size < sizeof(struct kvm_run) || (size_t)size > MAX_RUN_MAPPING) {
        fprintf(stderr, "Invalid vCPU run mapping size: %d\n", size); goto done;
    }
    run_size = (size_t)size;
    run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
    if (run == MAP_FAILED) { perror("vCPU mmap"); goto done; }
    if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) < 0) { perror("KVM_GET_SREGS"); goto done; }
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) < 0) { perror("KVM_SET_SREGS"); goto done; }
    regs.rip = GUEST_CODE_ADDRESS;
    regs.rflags = 0x2; /* reserved bit set, interrupts disabled */
    if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) < 0) { perror("KVM_SET_REGS"); goto done; }
    struct sigaction action = {0};
    action.sa_handler = on_alarm;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, &old_action) < 0) { perror("sigaction"); goto done; }
    alarm_installed = 1;
    static const char expected[] = "OK\n";
    size_t received = 0;
    struct lynx_serial serial = {0};
    alarm(5);
    for (unsigned int exits = 0; exits < 5; ++exits) {
        if (timed_out) { fprintf(stderr, "KVM_RUN timed out after 5 seconds\n"); goto done; }
        int status = ioctl(vcpu_fd, KVM_RUN, 0);
        int run_errno = errno;
        if (status < 0) {
            errno = run_errno;
            if (timed_out && errno == EINTR) fprintf(stderr, "KVM_RUN timed out after 5 seconds\n");
            else perror("KVM_RUN");
            goto done;
        }
        if (run->exit_reason == KVM_EXIT_IO) {
            int handled;
            if (run->io.port == LYNX_DEBUG_PORT)
                handled = lynx_consume_output(run, run_size, expected, sizeof(expected) - 1, &received);
            else
                handled = lynx_serial_io(&serial, run, run_size);
            if (handled < 0) { fprintf(stderr, "Invalid or unexpected guest I/O\n"); goto done; }
        } else if (run->exit_reason == KVM_EXIT_HLT && received == sizeof(expected) - 1 &&
                   serial.output_size == 1 && serial.output[0] == 'U') {
            alarm(0);
            puts("KVM debug and serial output succeeded; Android was not started.");
            result = 0;
            goto done;
        } else {
            fprintf(stderr, "Unexpected KVM exit or incomplete output: %u\n", run->exit_reason);
            goto done;
        }
    }
    fprintf(stderr, "Too many KVM exits\n");
done:
    if (alarm_installed) { alarm(0); sigaction(SIGALRM, &old_action, NULL); }
    if (run != MAP_FAILED) munmap(run, run_size);
    if (vcpu_fd >= 0) close(vcpu_fd);
    if (vm_fd >= 0) close(vm_fd);
    if (guest_memory != MAP_FAILED) munmap(guest_memory, GUEST_MEMORY_SIZE);
    if (kvm_fd >= 0) close(kvm_fd);
    return result;
}
#endif

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--confirm") != 0) {
        fprintf(stderr, "Usage: %s --confirm (creates a temporary VM and runs a built-in x86 test guest)\n", argv[0]);
        return 2;
    }
#if defined(__x86_64__)
    return run_experiment();
#else
    fprintf(stderr, "This experiment currently supports x86_64 Linux only.\n");
    return 1;
#endif
}
