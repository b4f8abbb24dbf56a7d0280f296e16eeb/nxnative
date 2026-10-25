/* Portable command-format contract checks, not Metal/completion execution.
 * The public recorder and completion FIFO are exercised by t_nvn.c.
 * Run from the repository root: make -C dm/gfx/tests check_report_value
 */
#include "../nvn/nvn_obj.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* Existing streams and numeric diagnostics keep their opcode identities. */
_Static_assert(OP_SIGNAL_EVENT == 4 && OP_REPORT_COUNTER == 5, "existing side-effect opcodes moved");
_Static_assert(OP_PRIM_RESTART == 48 && OP_REPORT_VALUE == 49, "ReportValue must be appended");
_Static_assert(OP_COUNT_ <= 64, "opcode diagnostics need more capacity");
_Static_assert(sizeof(op_hdr) + sizeof(p_report_value) == 24, "record size changed");
_Static_assert(offsetof(p_report_value, addr) == 0, "GPU address offset changed");
_Static_assert(offsetof(p_report_value, value) == 8, "report value offset changed");
_Static_assert(offsetof(p_report_value, _pad) == 12, "padding offset changed");
_Static_assert(_Generic(((p_report_value *)0)->addr, NVNbufferAddress: 1, default: 0), "report operand must be a GPU address, not a CPU pointer");

static unsigned checks;
#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "[RED] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

int main(void) {
    const uint64_t addresses[] = {0, UINT64_C(1) << 32, UINT64_C(1) << 63,
        UINT64_C(0xfedcba9876543210), UINT64_MAX};
    const uint32_t values[] = {0, 1, UINT32_C(0x80000000), UINT32_MAX};
    for (size_t i = 0; i < sizeof addresses / sizeof *addresses; i++) {
        for (size_t j = 0; j < sizeof values / sizeof *values; j++) {
            /* Inspect the actual shared payload against a hand-laid-out command.
             * Synthetic addresses are never converted to pointers or dereferenced. */
            unsigned char bytes[32]; memset(bytes, 0xa5, sizeof bytes);
            uint32_t opcode = 49, size = 24, pad = 0;
            memcpy(bytes, &opcode, 4); memcpy(bytes + 4, &size, 4);
            memcpy(bytes + 8, &addresses[i], 8); memcpy(bytes + 16, &values[j], 4); memcpy(bytes + 20, &pad, 4);
            uint32_t end = 0, end_size = 8; memcpy(bytes + 24, &end, 4); memcpy(bytes + 28, &end_size, 4);
            op_hdr h; p_report_value p; memcpy(&h, bytes, sizeof h); memcpy(&p, bytes + sizeof h, sizeof p);
            CHECK(h.op == OP_REPORT_VALUE && h.op != OP_SIGNAL_EVENT);
            CHECK(h.size == sizeof h + sizeof p);
            CHECK(p.addr == addresses[i] && p.value == values[j] && p._pad == 0);
            p_report_value recorded = {addresses[i], values[j], 0};
            CHECK(memcmp(bytes + 8, &recorded, sizeof recorded) == 0);
            memcpy(&h, bytes + h.size, sizeof h);
            CHECK(h.op == OP_END && h.size == 8);
        }
    }
    printf("ReportValue command format: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
