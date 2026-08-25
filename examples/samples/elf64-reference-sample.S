// Source for elf64-reference-sample.o. Rebuild on an x86-64 system with:
//   cc -c elf64-reference-sample.S -o elf64-reference-sample.o
//
// The resulting real ELF64 relocatable object deliberately contains file-backed
// .text/.rodata/.data sections and a non-file-backed .bss section so the example
// schema demonstrates both navigable and when-null section references.

    .section .text
    .global elf64_reference_sample
    .type elf64_reference_sample, @function
elf64_reference_sample:
    mov $42, %eax
    ret
    .size elf64_reference_sample, .-elf64_reference_sample

    .section .rodata
sample_message:
    .asciz "BrecoLang references"

    .section .data
sample_counter:
    .quad 7

    .section .bss
    .align 8
sample_scratch:
    .zero 16

    .section .note.GNU-stack,"",@progbits
