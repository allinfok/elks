//------------------------------------------------------------------------------
// #include <string.h>
// size_t strlen (const char * s);
//------------------------------------------------------------------------------

#include <libc-private/call-cvt.h>

	.code16

	.text

	.global strlen

strlen:
	push %bp
	mov %sp,%bp

	// Save DI ES

	mov %es,%dx

	mov %ds,%ax
	mov %ax,%es

	mov %di,%bx

	// Do the scan

	mov 4+FAR_ADJ_(%bp),%di  // s
	mov $-1,%cx
	xor %ax,%ax

	cld
	repne
	scasb

	mov %cx,%ax
	not %ax
	dec %ax

	// Restore DI ES

	mov %bx,%di
	mov %dx,%es

	pop %bp
	RET_(2)

//------------------------------------------------------------------------------

