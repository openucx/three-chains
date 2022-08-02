#
# Copyright (C) 2021-2022 Arm Ltd. All rights reserved.
#
AC_ARG_WITH([llvm],
            [AC_HELP_STRING([--with-llvm(=DIR)],
                            [Where to find the LLVM libraries and header files]
                           )], [], [with_llvm=no])

AS_IF([test "x$with_llvm" != xno],
      [
      AC_CHECK_FILE([$with_llvm/include/llvm-c/Core.h], [llvm_happy="yes"], [llvm_happy="no"])
      AS_IF([test "x$llvm_happy" = xyes],
            [
            AC_SUBST(LLVM_CPPFLAGS,  "-I$with_llvm/include")
            AC_SUBST(LLVM_LDFLAGS,   "-L$with_llvm/lib -lLLVM")
            AC_DEFINE([HAVE_LLVM], [1], [LLVM support])
            llvm_enable=enabled], [llvm_enable=disabled])],
      [])

AM_CONDITIONAL([HAVE_LLVM],[test "x$HAVE_LLVM" != "xno"])
