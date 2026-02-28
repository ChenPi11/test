savedcmd_linux_ufs.mod := printf '%s\n'   super.o inode.o dir.o file.o | awk '!x[$$0]++ { print("./"$$0) }' > linux_ufs.mod
