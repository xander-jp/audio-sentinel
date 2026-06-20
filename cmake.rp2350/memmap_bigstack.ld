/* Custom RP2350 memmap: relocate core0 stack from SCRATCH_Y (4 KB) into
 * main RAM so libopus's SILK WB 20 ms fixed-point decoder fits.
 *
 * What this changes vs. SDK pico_crt0/rp2350/memmap_default.ld:
 *   - .stack_dummy now lives in RAM, not SCRATCH_Y. PICO_STACK_SIZE
 *     (set to 20 KB / 0x5000 in CMakeLists.txt) is taken from the BSS budget.
 *   - __StackTop / __StackBottom are computed from RAM, not SCRATCH_Y.
 *   - __HeapLimit shrinks to __StackBottom so sbrk() can't grow into
 *     the relocated stack region.
 *
 * Why: opus_decode + the pcm[480] frame buffer + ~3 KB of SILK VLAs
 * exceed SCRATCH_Y's 4 KB. The dispatcher (handle_core0_notify) would
 * silently corrupt its return address and never come back, wedging
 * the IC FIFO permanently. Audio stayed silent because forward_pump
 * eventually hit ic_send_avail=0 on every call.
 *
 * Core1's stack (.stack1_dummy, 4 KB) lives in SCRATCH_Y; SCRATCH_X is
 * annexed by core0's stack (physically contiguous with RAM top), giving
 * core0 20 KB (0x2007c000..0x20081000) at zero heap cost. The 20 KB was
 * sized for the old VAR_ARRAYS build, where opus_encode peaked at 18,912 B
 * on this stack; codec scratch has since moved to the BSS pseudostack
 * (src/opus_scratch.c), so the live HW-stack peak is now a few KB and the
 * size is kept conservatively (see CMakeLists.txt). Core1 no longer runs
 * opus_decode.
 */

MEMORY
{
    INCLUDE "pico_flash_region.ld"
    RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 512k
    SCRATCH_X(rwx) : ORIGIN = 0x20080000, LENGTH = 4k
    SCRATCH_Y(rwx) : ORIGIN = 0x20081000, LENGTH = 4k
}

ENTRY(_entry_point)

SECTIONS
{
    .flash_begin : {
        __flash_binary_start = .;
    } > FLASH

    .text : {
        __logical_binary_start = .;
        KEEP (*(.vectors))
        KEEP (*(.binary_info_header))
        __binary_info_header_end = .;
        KEEP (*(.embedded_block))
        __embedded_block_end = .;
        KEEP (*(.reset))
        *(.init)
        *libgcc.a:cmse_nonsecure_call.o
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*)
        *(.fini)
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        . = ALIGN(4);
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(SORT(.preinit_array.*)))
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__fini_array_start = .);
        *(SORT(.fini_array.*))
        *(.fini_array)
        PROVIDE_HIDDEN (__fini_array_end = .);

        *(.eh_frame*)
        . = ALIGN(4);
    } > FLASH

    .boot2 : {
        __boot2_start__ = .;
        *(.boot2)
        __boot2_end__ = .;
    } > FLASH

    ASSERT(__boot2_end__ - __boot2_start__ <= 256,
        "ERROR: Pico second stage bootloader must be no more than 256 bytes in size")

    .rodata : {
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .rodata*)
        *(.srodata*)
        . = ALIGN(4);
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.flashdata*)))
        . = ALIGN(4);
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH
    __exidx_end = .;

    . = ALIGN(4);
    __binary_info_start = .;
    .binary_info :
    {
        KEEP(*(.binary_info.keep.*))
        *(.binary_info.*)
    } > FLASH
    __binary_info_end = .;
    . = ALIGN(4);

    .ram_vector_table (NOLOAD): {
        *(.ram_vector_table)
    } > RAM

    .uninitialized_data (NOLOAD): {
        . = ALIGN(4);
        *(.uninitialized_data*)
    } > RAM

    .data : {
        __data_start__ = .;
        *(vtable)

        *(.time_critical*)

        *(.text*)
        . = ALIGN(4);
        *(.rodata*)
        . = ALIGN(4);

        *(.data*)
        *(.sdata*)

        . = ALIGN(4);
        *(.after_data.*)
        . = ALIGN(4);
        PROVIDE_HIDDEN (__mutex_array_start = .);
        KEEP(*(SORT(.mutex_array.*)))
        KEEP(*(.mutex_array))
        PROVIDE_HIDDEN (__mutex_array_end = .);

        *(.jcr)
        . = ALIGN(4);
    } > RAM AT> FLASH

    .tdata : {
        . = ALIGN(4);
		*(.tdata .tdata.* .gnu.linkonce.td.*)
        __tdata_end = .;
    } > RAM AT> FLASH
    PROVIDE(__data_end__ = .);

    __etext = LOADADDR(.data);

    .tbss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        __tls_base = .;
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)

        __tls_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __tbss_end = .;

        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.bss*)))
        *(COMMON)
        PROVIDE(__global_pointer$ = . + 2K);
        *(.sbss*)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    .heap (NOLOAD):
    {
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
    } > RAM

    /* Start and end symbols must be word-aligned */
    .scratch_x : {
        __scratch_x_start__ = .;
        *(.scratch_x.*)
        . = ALIGN(4);
        __scratch_x_end__ = .;
    } > SCRATCH_X AT > FLASH
    __scratch_x_source__ = LOADADDR(.scratch_x);

    .scratch_y : {
        __scratch_y_start__ = .;
        *(.scratch_y.*)
        . = ALIGN(4);
        __scratch_y_end__ = .;
    } > SCRATCH_Y AT > FLASH
    __scratch_y_source__ = LOADADDR(.scratch_y);

    /* Core 1's stack moves to SCRATCH_Y (was SCRATCH_X) so SCRATCH_X can
     * be annexed by core0's stack below. Size is still bounded by
     * PICO_CORE1_STACK_SIZE; core1 no longer runs opus_decode so its
     * 4 KB budget is fine. */
    .stack1_dummy (NOLOAD):
    {
        *(.stack1*)
    } > SCRATCH_Y

    /* Core 0's stack tops out at the END of SCRATCH_X (0x20081000):
     * SCRATCH_X (SRAM8) is physically contiguous with the top of main
     * RAM, so annexing it buys 4 KB of stack WITHOUT lowering
     * __StackBottom — i.e. zero cost to the newlib heap, whose break
     * high-water (0x2007a000 measured) already sits only 8 KB below.
     * Sized for the old VAR_ARRAYS build, where opus_encode peaked core0's
     * stack at 18,912 B — past the previous 16 KB region. That scratch now
     * lives in the BSS pseudostack (src/opus_scratch.c), so the live peak is
     * a few KB; the annex is retained for headroom (see CMakeLists.txt). Heap
     * grows up from __end__ toward __StackBottom; the ASSERT below catches
     * collisions at link time. */
    .stack_dummy (NOLOAD):
    {
        . = ALIGN(8);
        KEEP(*(.stack*))
    } > RAM

    .flash_end : {
        KEEP(*(.embedded_end_block*))
        PROVIDE(__flash_binary_end = .);
    } > FLASH =0xaa

    __StackOneTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);
    __StackOneBottom = __StackOneTop - SIZEOF(.stack1_dummy);

    /* Top of SCRATCH_X, not top of RAM — see .stack_dummy comment. With
     * PICO_STACK_SIZE=0x5000 this keeps __StackBottom at 0x2007c000. */
    __StackTop = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);
    __StackBottom = __StackTop - SIZEOF(.stack_dummy);
    /* Heap must not grow into the relocated stack region. */
    __HeapLimit = __StackBottom;
    /* historically the "limit" name meant "max heap end", so keep
     * the symbol pointing at the same boundary. */
    __StackLimit = __StackBottom;
    PROVIDE(__stack = __StackTop);

    /* picolibc and LLVM */
    PROVIDE (__heap_start = __end__);
    PROVIDE (__heap_end = __HeapLimit);
    PROVIDE( __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)) );
    PROVIDE( __tls_size_align = (__tls_size + __tls_align - 1) & ~(__tls_align - 1));
    PROVIDE( __arm32_tls_tcb_offset = MAX(8, __tls_align) );

    /* llvm-libc */
    PROVIDE (_end = __end__);
    PROVIDE (__llvm_libc_heap_limit = __HeapLimit);

    /* Check that data + heap doesn't grow past the relocated stack base. */
    ASSERT(__StackBottom >= __end__, "region RAM overflowed: heap meets stack")

    /* Core0's stack annexes SCRATCH_X, but the .scratch_x output section
     * still targets that bank — the linker would NOT flag the overlap
     * (the stack is only symbols, not a section). Refuse to link if
     * anything ever lands in .scratch_x.*. */
    ASSERT(__scratch_x_end__ == __scratch_x_start__,
           ".scratch_x must stay empty: SCRATCH_X is annexed by core0's stack")

    ASSERT( __binary_info_header_end - __logical_binary_start <= 1024, "Binary info must be in first 1024 bytes of the binary")
    ASSERT( __embedded_block_end - __logical_binary_start <= 4096, "Embedded block must be in first 4096 bytes of the binary")
}
