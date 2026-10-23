# Binds the C stdio calls of PCSX2's own libraries to the port's virtual file layer
# (frontend/vfs), which serves paths under its mounts (network shares, drives the title's sandbox
# hides) and passes every other stream to libc.
#
# The names are redirected with object-like macros, so the SDK's <stdio.h> itself declares the
# ps5vfs_ function under each prototype it has, and libc++'s <cstdio> brings them into std::.
# feof, ferror and clearerr cannot be bound this way: the SDK defines them as macros in C, and
# libc++ undefines whatever macro has their name. A virtual stream is therefore a real FILE to
# libc, and the layer keeps that FILE's end-of-file indicator in step with the virtual stream's
# (frontend/vfs/ps5_vfs.cpp). fileno is bound in C++ only, for the same reason.
set(PS5PX_STDIO_REDIRECTS
  fopen fclose fread fwrite fseek fseeko ftell ftello rewind fgets fgetc ungetc fflush)
set(PS5PX_STDIO_REDIRECTS_CXX
  fileno)

function(ps5_redirect_stdio target)
  target_compile_definitions(${target} PRIVATE PS5PX_VFS=1)
  target_include_directories(${target} PRIVATE "${PS5PX_ROOT}/frontend/vfs")
  foreach(name IN LISTS PS5PX_STDIO_REDIRECTS)
    target_compile_definitions(${target} PRIVATE "${name}=ps5vfs_${name}")
  endforeach()
  foreach(name IN LISTS PS5PX_STDIO_REDIRECTS_CXX)
    target_compile_definitions(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${name}=ps5vfs_${name}>")
  endforeach()
endfunction()
