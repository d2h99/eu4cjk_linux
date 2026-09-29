set pagination off
set confirm off
set breakpoint pending on
set print thread-events off
# adjust to your build path:
set environment LD_PRELOAD=<repo>/build/libeu4cjk.so

# glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data)
#   rdi rsi edx ecx(w) r8d(h) r9d [rsp+8]
break glCompressedTexImage2D if $ecx >= 1024
commands
silent
printf "[C-TEX] w=%d h=%d ifmt=0x%x level=%d imgsize=%d\n", $ecx, $r8d, $edx, $esi, *(int*)($rsp+8)
continue
end

# glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels)
#   rdi rsi edx ecx(w) r8d(h) r9d [rsp+8]=fmt [rsp+16]=type
break glTexImage2D if $ecx >= 1024
commands
silent
printf "[U-TEX] w=%d h=%d ifmt=0x%x level=%d fmt=0x%x type=0x%x\n", $ecx, $r8d, $edx, $esi, *(int*)($rsp+8), *(int*)($rsp+16)
continue
end

# glTexStorage2D(target, levels, internalformat, width, height): rdi rsi edx ecx(w) r8d(h)
break glTexStorage2D if $ecx >= 1024
commands
silent
printf "[S-TEX] w=%d h=%d ifmt=0x%x levels=%d\n", $ecx, $r8d, $edx, $esi
continue
end

# glCompressedTexSubImage2D(target, level, xoff, yoff, width, height, format, imageSize, data)
break glCompressedTexSubImage2D if $r8d >= 1024
commands
silent
printf "[C-SUB] w=%d h=%d fmt=0x%x level=%d\n", $r8d, *(int*)($rsp+8), $ecx, $esi
continue
end

run -skiplauncher
