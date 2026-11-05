/*
 *  sigsegv.h - Minimal replacement of BasiliskII/src/CrossPlatform/sigsegv.h
 *              for the bare-metal Circle port.
 *
 *  Faults of the emulated Mac are handled by the ARM data abort handler in
 *  rpi_mmu.cpp, so these declarations only exist to keep the shared code
 *  compiling.
 */

#ifndef SIGSEGV_H
#define SIGSEGV_H

typedef char *sigsegv_address_t;

enum sigsegv_return_t {
	SIGSEGV_RETURN_SUCCESS,
	SIGSEGV_RETURN_FAILURE,
	SIGSEGV_RETURN_SKIP_INSTRUCTION
};

struct sigsegv_info_t {
	sigsegv_address_t addr;
	sigsegv_address_t pc;
};

static inline sigsegv_address_t sigsegv_get_fault_address(sigsegv_info_t *sip) { return sip->addr; }
static inline sigsegv_address_t sigsegv_get_fault_instruction_address(sigsegv_info_t *sip) { return sip->pc; }

typedef sigsegv_return_t (*sigsegv_fault_handler_t)(sigsegv_info_t *sip);
static inline bool sigsegv_install_handler(sigsegv_fault_handler_t) { return true; }

#endif /* SIGSEGV_H */
