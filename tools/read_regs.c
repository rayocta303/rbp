#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int pid = atoi(argv[1]);
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0) {
        perror("ATTACH");
        return 1;
    }
    waitpid(pid, NULL, 0);
    /* For 32-bit ARM compat task on AArch64: NT_PRSTATUS provides 18 32-bit registers (arm_regs) */
    unsigned int arm_regs[18];
    struct iovec iov = { arm_regs, sizeof(arm_regs) };
    if (ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) == 0) {
        printf("ARM32 REGS: r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x\n"
               "            r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x\n"
               "            r8=0x%08x r9=0x%08x r10=0x%08x fp=0x%08x\n"
               "            ip=0x%08x sp=0x%08x lr=0x%08x pc=0x%08x cpsr=0x%08x\n",
               arm_regs[0], arm_regs[1], arm_regs[2], arm_regs[3],
               arm_regs[4], arm_regs[5], arm_regs[6], arm_regs[7],
               arm_regs[8], arm_regs[9], arm_regs[10], arm_regs[11],
               arm_regs[12], arm_regs[13], arm_regs[14], arm_regs[15], arm_regs[16]);
    } else {
        perror("GETREGSET");
    }
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    return 0;
}
