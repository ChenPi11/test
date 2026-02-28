savedcmd_linux_ufs.o := ld -m elf_x86_64 -z noexecstack --no-warn-rwx-segments   -r -o linux_ufs.o @linux_ufs.mod 
