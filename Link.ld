ENTRY( _start )

__stack_size = 512;

PROVIDE( _stack_size = __stack_size );

MEMORY
{
    FLASH (rx) : ORIGIN = 0x0, LENGTH = FLASH_SIZE
    RAM (xrw) : ORIGIN = 0x20000000, LENGTH = RAM_SIZE
}

SECTIONS
{
    .init :
    {
        _sinit = .;
        . = ALIGN(4);
        KEEP(*(SORT_NONE(.init)))
        . = ALIGN(4);
        _einit = .;
    } >FLASH AT>FLASH

    .vector :
    {
        *(.vector);
        . = ALIGN(4);
    } >FLASH AT>FLASH

    .highcode_initlalign :
    {
        . = ALIGN(4);
        PROVIDE(_highcode_init_lma = .);
    } >FLASH AT>FLASH

    .highcode_init :
    {
        PROVIDE(_highcode_init_vma_start = .);
        . = ALIGN(1024);
        *(.vector)
        KEEP(*(SORT_NONE(.vector_handler)))
        *(.highcode_init)
        *(.highcode_init.*)
        . = ALIGN(4);
        PROVIDE(_highcode_init_vma_end = .);
    } >RAM AT>FLASH

    .highcodelalign :
    {
        . = ALIGN(4);
        PROVIDE(_highcode_lma = .);
    } >FLASH AT>FLASH

    .highcode :
    {
        . = ALIGN(4);
        PROVIDE(_highcode_vma_start = .);
        *(.highcode)
        *(.highcode.*)
        *(.highcode_lp)
        *(.highcode_lp.*)
        . = ALIGN(4);
        PROVIDE(_highcode_vma_end = .);
    } >RAM AT>FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata*)
        *(.gnu.linkonce.t.*)
        . = ALIGN(4);
    } >FLASH AT>FLASH

    .fini :
    {
        KEEP(*(SORT_NONE(.fini)))
        . = ALIGN(4);
    } >FLASH AT>FLASH

    PROVIDE( _etext = . );
    PROVIDE( _eitcm = . );

    .dalign :
    {
        . = ALIGN(4);
        PROVIDE(_data_vma = .);
    } >RAM AT>FLASH

    .dlalign :
    {
        . = ALIGN(4);
        PROVIDE(_data_lma = .);
    } >FLASH AT>FLASH

    .data :
    {
        *(.gnu.linkonce.r.*)
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(8);
        PROVIDE( __global_pointer$ = . + 0x800 );
        *(.sdata .sdata.*)
        *(.sdata2.*)
        *(.gnu.linkonce.s.*)
        . = ALIGN(8);
        *(.srodata.cst16)
        *(.srodata.cst8)
        *(.srodata.cst4)
        *(.srodata.cst2)
        *(.srodata .srodata.*)
        . = ALIGN(4);
        PROVIDE( _edata = .);
    } >RAM AT>FLASH

    .bss :
    {
        . = ALIGN(4);
        PROVIDE( _sbss = .);
        *(.sbss*)
        *(.gnu.linkonce.sb.*)
        *(.bss*)
        *(.gnu.linkonce.b.*)
        *(COMMON*)
        . = ALIGN(4);
        PROVIDE( _ebss = .);
    } >RAM AT>FLASH

    PROVIDE( _end = _ebss);
    PROVIDE( end = . );

    .stack ORIGIN(RAM) + LENGTH(RAM) - __stack_size :
    {
        PROVIDE( _heap_end = . );
        . = ALIGN(4);
        PROVIDE(_susrstack = . );
        . = . + __stack_size;
        PROVIDE( _eusrstack = .);
        __freertos_irq_stack_top = .;
    } >RAM

    ASSERT(_ebss <= ORIGIN(RAM) + LENGTH(RAM) - __stack_size, "RAM overflow into stack")
}
