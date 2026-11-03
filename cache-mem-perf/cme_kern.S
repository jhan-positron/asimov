// cme_kern.S - the memory kernels of latency.c and bandwidth.c run through the
// CME (the Arm C1-SME2 unit, one per DSU) instead of the core's own load/store
// unit. Every function enters streaming SVE mode (SMSTART SM), in which SVE
// loads and stores are sent to the CME and use its vector length, SVL (64 B on
// Asimov), and leaves it before returning. The functions are plain AAPCS64
// calls: nothing in them makes a system call or touches Advanced SIMD, both of
// which are illegal or state-destroying in streaming mode. SMSTART and SMSTOP
// zero the Z and P registers, so d8-d15 (callee-saved) and FPCR are kept on
// the stack around the streaming span.
//
// The read kernels fold every loaded vector with EOR and return the XOR of
// all 8-byte words, so cme_test.c can check them against a scalar fold. Each
// load register has its own accumulator (z16 to z31 for the four-vector
// reader, z16 to z23 for the single-vector one), so an accumulator is updated
// once per loop pass and the EOR latency (4 cycles in the LLVM C1-Nano model)
// never chains inside a pass. The fold is still part of the kernel: a read row
// is the throughput of this whole loop, not a load-only ceiling. The
// multi-vector forms move 256 B per instruction (four vectors of SVL = 64 B);
// the single-vector forms move 64 B.
//
//   uint64_t cme_svl(void)                              SVL in bytes (RDSVL; legal outside streaming mode)
//   uint64_t cme_chase(uint64_t p, uint64_t hops)       dependent loads: LD1D of the line at p, LASTB moves
//                                                       the first 8 B (the next address) to x0; hops >= 1
//   uint64_t cme_roundtrip(uint64_t x, uint64_t n)      the same chain without memory: X -> Z (DUP) -> X (LASTB);
//                                                       the core-to-CME-to-core register round trip per hop
//   uint64_t cme_read4(const void *p, uint64_t bytes)   LD1B of 4 vectors, 4 per KiB; bytes % 1024 == 0; returns a fold
//   void     cme_write4(void *p, uint64_t bytes, uint64_t v)   ST1B of 4 vectors, every 8 B = v
//   void     cme_copy4(void *d, const void *s, uint64_t bytes) LD1B + ST1B of 4 vectors
//   uint64_t cme_read1(const void *p, uint64_t bytes)   LD1B of 1 vector (64 B), 16 per KiB; returns a fold
//   void     cme_write1(void *p, uint64_t bytes, uint64_t v)   ST1B of 1 vector
//
// Streaming-mode forms are from the Arm C1-Nano SOG section 2.33 and the
// asimov-bench cme-perf chase.S / stream.S (the same LD1D + LASTB chase and
// LD1B x4 stream that produced the October 2026 numbers), so the rows here
// compare with those.
	.arch	armv9.3-a+sve2+sme2
	.text

.macro	SM_ENTER
	stp	d8, d9, [sp, #-80]!
	stp	d10, d11, [sp, #16]
	stp	d12, d13, [sp, #32]
	stp	d14, d15, [sp, #48]
	mrs	x9, fpcr
	str	x9, [sp, #64]
	smstart	sm
.endm

.macro	SM_LEAVE
	smstop	sm
	ldr	x9, [sp, #64]
	msr	fpcr, x9
	ldp	d14, d15, [sp, #48]
	ldp	d12, d13, [sp, #32]
	ldp	d10, d11, [sp, #16]
	ldp	d8, d9, [sp], #80
.endm

	.global	cme_svl
	.type	cme_svl, %function
cme_svl:
	rdsvl	x0, #1
	ret
	.size	cme_svl, . - cme_svl

	.global	cme_chase
	.type	cme_chase, %function
	.p2align 6
cme_chase:
	SM_ENTER
	ptrue	p0.d, vl1
	.p2align 6
1:	ld1d	{z0.d}, p0/z, [x0]
	lastb	x0, p0, z0.d
	subs	x1, x1, #1
	b.ne	1b
	SM_LEAVE
	ret
	.size	cme_chase, . - cme_chase

	.global	cme_roundtrip
	.type	cme_roundtrip, %function
	.p2align 6
cme_roundtrip:
	SM_ENTER
	ptrue	p0.d, vl1
	.p2align 6
1:	mov	z0.d, x0
	lastb	x0, p0, z0.d
	subs	x1, x1, #1
	b.ne	1b
	SM_LEAVE
	ret
	.size	cme_roundtrip, . - cme_roundtrip

	.global	cme_read4
	.type	cme_read4, %function
	.p2align 6
cme_read4:
	SM_ENTER
	ptrue	pn8.b
	ptrue	p0.d
	.irp	n, 16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	mov	z\n\().d, #0
	.endr
	add	x2, x0, x1
	.p2align 6
1:	ld1b	{z0.b - z3.b}, pn8/z, [x0]
	ld1b	{z4.b - z7.b}, pn8/z, [x0, #4, mul vl]
	ld1b	{z8.b - z11.b}, pn8/z, [x0, #8, mul vl]
	ld1b	{z12.b - z15.b}, pn8/z, [x0, #12, mul vl]
	eor	z16.d, z16.d, z0.d
	eor	z17.d, z17.d, z1.d
	eor	z18.d, z18.d, z2.d
	eor	z19.d, z19.d, z3.d
	eor	z20.d, z20.d, z4.d
	eor	z21.d, z21.d, z5.d
	eor	z22.d, z22.d, z6.d
	eor	z23.d, z23.d, z7.d
	eor	z24.d, z24.d, z8.d
	eor	z25.d, z25.d, z9.d
	eor	z26.d, z26.d, z10.d
	eor	z27.d, z27.d, z11.d
	eor	z28.d, z28.d, z12.d
	eor	z29.d, z29.d, z13.d
	eor	z30.d, z30.d, z14.d
	eor	z31.d, z31.d, z15.d
	add	x0, x0, #1024
	cmp	x0, x2
	b.lo	1b
	.irp	n, 17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	eor	z16.d, z16.d, z\n\().d
	.endr
	eorv	d0, p0, z16.d
	fmov	x0, d0
	SM_LEAVE
	ret
	.size	cme_read4, . - cme_read4

	.global	cme_write4
	.type	cme_write4, %function
	.p2align 6
cme_write4:
	SM_ENTER
	ptrue	pn8.b
	mov	z0.d, x2
	mov	z1.d, x2
	mov	z2.d, x2
	mov	z3.d, x2
	add	x3, x0, x1
	.p2align 6
1:	st1b	{z0.b - z3.b}, pn8, [x0]
	st1b	{z0.b - z3.b}, pn8, [x0, #4, mul vl]
	st1b	{z0.b - z3.b}, pn8, [x0, #8, mul vl]
	st1b	{z0.b - z3.b}, pn8, [x0, #12, mul vl]
	add	x0, x0, #1024
	cmp	x0, x3
	b.lo	1b
	SM_LEAVE
	ret
	.size	cme_write4, . - cme_write4

	.global	cme_copy4
	.type	cme_copy4, %function
	.p2align 6
cme_copy4:
	SM_ENTER
	ptrue	pn8.b
	add	x3, x1, x2
	.p2align 6
1:	ld1b	{z0.b - z3.b}, pn8/z, [x1]
	ld1b	{z4.b - z7.b}, pn8/z, [x1, #4, mul vl]
	ld1b	{z8.b - z11.b}, pn8/z, [x1, #8, mul vl]
	ld1b	{z12.b - z15.b}, pn8/z, [x1, #12, mul vl]
	st1b	{z0.b - z3.b}, pn8, [x0]
	st1b	{z4.b - z7.b}, pn8, [x0, #4, mul vl]
	st1b	{z8.b - z11.b}, pn8, [x0, #8, mul vl]
	st1b	{z12.b - z15.b}, pn8, [x0, #12, mul vl]
	add	x0, x0, #1024
	add	x1, x1, #1024
	cmp	x1, x3
	b.lo	1b
	SM_LEAVE
	ret
	.size	cme_copy4, . - cme_copy4

	.global	cme_read1
	.type	cme_read1, %function
	.p2align 6
cme_read1:
	SM_ENTER
	ptrue	p0.b
	ptrue	p1.d
	.irp	n, 16,17,18,19,20,21,22,23
	mov	z\n\().d, #0
	.endr
	add	x2, x0, x1
	.p2align 6
1:	ld1b	{z0.b}, p0/z, [x0]
	ld1b	{z1.b}, p0/z, [x0, #1, mul vl]
	ld1b	{z2.b}, p0/z, [x0, #2, mul vl]
	ld1b	{z3.b}, p0/z, [x0, #3, mul vl]
	ld1b	{z4.b}, p0/z, [x0, #4, mul vl]
	ld1b	{z5.b}, p0/z, [x0, #5, mul vl]
	ld1b	{z6.b}, p0/z, [x0, #6, mul vl]
	ld1b	{z7.b}, p0/z, [x0, #7, mul vl]
	eor	z16.d, z16.d, z0.d
	eor	z17.d, z17.d, z1.d
	eor	z18.d, z18.d, z2.d
	eor	z19.d, z19.d, z3.d
	eor	z20.d, z20.d, z4.d
	eor	z21.d, z21.d, z5.d
	eor	z22.d, z22.d, z6.d
	eor	z23.d, z23.d, z7.d
	add	x0, x0, #512
	cmp	x0, x2
	b.lo	1b
	.irp	n, 17,18,19,20,21,22,23
	eor	z16.d, z16.d, z\n\().d
	.endr
	eorv	d0, p1, z16.d
	fmov	x0, d0
	SM_LEAVE
	ret
	.size	cme_read1, . - cme_read1

	.global	cme_write1
	.type	cme_write1, %function
	.p2align 6
cme_write1:
	SM_ENTER
	ptrue	p0.b
	mov	z0.d, x2
	add	x3, x0, x1
	.p2align 6
1:	st1b	{z0.b}, p0, [x0]
	st1b	{z0.b}, p0, [x0, #1, mul vl]
	st1b	{z0.b}, p0, [x0, #2, mul vl]
	st1b	{z0.b}, p0, [x0, #3, mul vl]
	st1b	{z0.b}, p0, [x0, #4, mul vl]
	st1b	{z0.b}, p0, [x0, #5, mul vl]
	st1b	{z0.b}, p0, [x0, #6, mul vl]
	st1b	{z0.b}, p0, [x0, #7, mul vl]
	add	x0, x0, #512
	cmp	x0, x3
	b.lo	1b
	SM_LEAVE
	ret
	.size	cme_write1, . - cme_write1

	.section .note.GNU-stack, "", %progbits
