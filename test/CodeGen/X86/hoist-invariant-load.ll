; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; REQUIRES: asserts
; RUN: llc -mcpu=haswell < %s -O2 2>&1 | FileCheck %s
; For test:
; 2 invariant loads, 1 for OBJC_SELECTOR_REFERENCES_
; and 1 for objc_msgSend from the GOT
; For test_multi_def:
; 2 invariant load (full multiply, both loads should be hoisted.)
; For test_div_def:
; 2 invariant load (full divide, both loads should be hoisted.) 1 additional instruction for a zeroing edx that gets hoisted and then rematerialized.

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx10.7.2"

@"\01L_OBJC_METH_VAR_NAME_" = internal global [4 x i8] c"foo\00", section "__TEXT,__objc_methname,cstring_literals", align 1
@"\01L_OBJC_SELECTOR_REFERENCES_" = internal global i8* getelementptr inbounds ([4 x i8], [4 x i8]* @"\01L_OBJC_METH_VAR_NAME_", i64 0, i64 0), section "__DATA, __objc_selrefs, literal_pointers, no_dead_strip"
@"\01L_OBJC_IMAGE_INFO" = internal constant [2 x i32] [i32 0, i32 16], section "__DATA, __objc_imageinfo, regular, no_dead_strip"
@llvm.used = appending global [3 x i8*] [i8* bitcast ([4 x i8]* @"\01L_OBJC_METH_VAR_NAME_" to i8*), i8* bitcast (i8** @"\01L_OBJC_SELECTOR_REFERENCES_" to i8*), i8* bitcast ([2 x i32]* @"\01L_OBJC_IMAGE_INFO" to i8*)], section "llvm.metadata"

define void @test(i8* %x) uwtable ssp {
; CHECK-LABEL: test:
; CHECK:       ## %bb.0: ## %entry
; CHECK-NEXT:    pushq %rbp
; CHECK-NEXT:    .cfi_def_cfa_offset 16
; CHECK-NEXT:    pushq %r15
; CHECK-NEXT:    .cfi_def_cfa_offset 24
; CHECK-NEXT:    pushq %r14
; CHECK-NEXT:    .cfi_def_cfa_offset 32
; CHECK-NEXT:    pushq %rbx
; CHECK-NEXT:    .cfi_def_cfa_offset 40
; CHECK-NEXT:    pushq %rax
; CHECK-NEXT:    .cfi_def_cfa_offset 48
; CHECK-NEXT:    .cfi_offset %rbx, -40
; CHECK-NEXT:    .cfi_offset %r14, -32
; CHECK-NEXT:    .cfi_offset %r15, -24
; CHECK-NEXT:    .cfi_offset %rbp, -16
; CHECK-NEXT:    movq %rdi, %rbx
; CHECK-NEXT:    movl $10000, %ebp ## imm = 0x2710
; CHECK-NEXT:    movq {{.*}}(%rip), %r14
; CHECK-NEXT:    movq _objc_msgSend@{{.*}}(%rip), %r15
; CHECK-NEXT:    .p2align 4, 0x90
; CHECK-NEXT:  LBB0_1: ## %for.body
; CHECK-NEXT:    ## =>This Inner Loop Header: Depth=1
; CHECK-NEXT:    movq %rbx, %rdi
; CHECK-NEXT:    movq %r14, %rsi
; CHECK-NEXT:    callq *%r15
; CHECK-NEXT:    decl %ebp
; CHECK-NEXT:    jne LBB0_1
; CHECK-NEXT:  ## %bb.2: ## %for.end
; CHECK-NEXT:    addq $8, %rsp
; CHECK-NEXT:    popq %rbx
; CHECK-NEXT:    popq %r14
; CHECK-NEXT:    popq %r15
; CHECK-NEXT:    popq %rbp
; CHECK-NEXT:    retq
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %i.01 = phi i32 [ 0, %entry ], [ %inc, %for.body ]
  %0 = load i8*, i8** @"\01L_OBJC_SELECTOR_REFERENCES_", align 8, !invariant.load !0
  %call = tail call i8* bitcast (i8* (i8*, i8*, ...)* @objc_msgSend to i8* (i8*, i8*)*)(i8* %x, i8* %0)
  %inc = add i32 %i.01, 1
  %exitcond = icmp eq i32 %inc, 10000
  br i1 %exitcond, label %for.end, label %for.body

for.end:                                          ; preds = %for.body
  ret void
}

define void @test_unordered(i8* %x) uwtable ssp {
; CHECK-LABEL: test_unordered:
; CHECK:       ## %bb.0: ## %entry
; CHECK-NEXT:    pushq %rbp
; CHECK-NEXT:    .cfi_def_cfa_offset 16
; CHECK-NEXT:    pushq %r15
; CHECK-NEXT:    .cfi_def_cfa_offset 24
; CHECK-NEXT:    pushq %r14
; CHECK-NEXT:    .cfi_def_cfa_offset 32
; CHECK-NEXT:    pushq %rbx
; CHECK-NEXT:    .cfi_def_cfa_offset 40
; CHECK-NEXT:    pushq %rax
; CHECK-NEXT:    .cfi_def_cfa_offset 48
; CHECK-NEXT:    .cfi_offset %rbx, -40
; CHECK-NEXT:    .cfi_offset %r14, -32
; CHECK-NEXT:    .cfi_offset %r15, -24
; CHECK-NEXT:    .cfi_offset %rbp, -16
; CHECK-NEXT:    movq %rdi, %rbx
; CHECK-NEXT:    movl $10000, %ebp ## imm = 0x2710
; CHECK-NEXT:    movq {{.*}}(%rip), %r14
; CHECK-NEXT:    movq _objc_msgSend@{{.*}}(%rip), %r15
; CHECK-NEXT:    .p2align 4, 0x90
; CHECK-NEXT:  LBB1_1: ## %for.body
; CHECK-NEXT:    ## =>This Inner Loop Header: Depth=1
; CHECK-NEXT:    movq %rbx, %rdi
; CHECK-NEXT:    movq %r14, %rsi
; CHECK-NEXT:    callq *%r15
; CHECK-NEXT:    decl %ebp
; CHECK-NEXT:    jne LBB1_1
; CHECK-NEXT:  ## %bb.2: ## %for.end
; CHECK-NEXT:    addq $8, %rsp
; CHECK-NEXT:    popq %rbx
; CHECK-NEXT:    popq %r14
; CHECK-NEXT:    popq %r15
; CHECK-NEXT:    popq %rbp
; CHECK-NEXT:    retq
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %i.01 = phi i32 [ 0, %entry ], [ %inc, %for.body ]
  %0 = load atomic i8*, i8** @"\01L_OBJC_SELECTOR_REFERENCES_" unordered, align 8, !invariant.load !0
  %call = tail call i8* bitcast (i8* (i8*, i8*, ...)* @objc_msgSend to i8* (i8*, i8*)*)(i8* %x, i8* %0)
  %inc = add i32 %i.01, 1
  %exitcond = icmp eq i32 %inc, 10000
  br i1 %exitcond, label %for.end, label %for.body

for.end:                                          ; preds = %for.body
  ret void
}

define void @test_volatile(i8* %x) uwtable ssp {
; CHECK-LABEL: test_volatile:
; CHECK:       ## %bb.0: ## %entry
; CHECK-NEXT:    pushq %rbp
; CHECK-NEXT:    .cfi_def_cfa_offset 16
; CHECK-NEXT:    pushq %r14
; CHECK-NEXT:    .cfi_def_cfa_offset 24
; CHECK-NEXT:    pushq %rbx
; CHECK-NEXT:    .cfi_def_cfa_offset 32
; CHECK-NEXT:    .cfi_offset %rbx, -32
; CHECK-NEXT:    .cfi_offset %r14, -24
; CHECK-NEXT:    .cfi_offset %rbp, -16
; CHECK-NEXT:    movq %rdi, %rbx
; CHECK-NEXT:    movl $10000, %ebp ## imm = 0x2710
; CHECK-NEXT:    movq _objc_msgSend@{{.*}}(%rip), %r14
; CHECK-NEXT:    .p2align 4, 0x90
; CHECK-NEXT:  LBB2_1: ## %for.body
; CHECK-NEXT:    ## =>This Inner Loop Header: Depth=1
; CHECK-NEXT:    movq {{.*}}(%rip), %rsi
; CHECK-NEXT:    movq %rbx, %rdi
; CHECK-NEXT:    callq *%r14
; CHECK-NEXT:    decl %ebp
; CHECK-NEXT:    jne LBB2_1
; CHECK-NEXT:  ## %bb.2: ## %for.end
; CHECK-NEXT:    popq %rbx
; CHECK-NEXT:    popq %r14
; CHECK-NEXT:    popq %rbp
; CHECK-NEXT:    retq
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %i.01 = phi i32 [ 0, %entry ], [ %inc, %for.body ]
  %0 = load volatile i8*, i8** @"\01L_OBJC_SELECTOR_REFERENCES_", align 8, !invariant.load !0
  %call = tail call i8* bitcast (i8* (i8*, i8*, ...)* @objc_msgSend to i8* (i8*, i8*)*)(i8* %x, i8* %0)
  %inc = add i32 %i.01, 1
  %exitcond = icmp eq i32 %inc, 10000
  br i1 %exitcond, label %for.end, label %for.body

for.end:                                          ; preds = %for.body
  ret void
}

define void @test_seq_cst(i8* %x) uwtable ssp {
; CHECK-LABEL: test_seq_cst:
; CHECK:       ## %bb.0: ## %entry
; CHECK-NEXT:    pushq %rbp
; CHECK-NEXT:    .cfi_def_cfa_offset 16
; CHECK-NEXT:    pushq %r14
; CHECK-NEXT:    .cfi_def_cfa_offset 24
; CHECK-NEXT:    pushq %rbx
; CHECK-NEXT:    .cfi_def_cfa_offset 32
; CHECK-NEXT:    .cfi_offset %rbx, -32
; CHECK-NEXT:    .cfi_offset %r14, -24
; CHECK-NEXT:    .cfi_offset %rbp, -16
; CHECK-NEXT:    movq %rdi, %rbx
; CHECK-NEXT:    movl $10000, %ebp ## imm = 0x2710
; CHECK-NEXT:    movq _objc_msgSend@{{.*}}(%rip), %r14
; CHECK-NEXT:    .p2align 4, 0x90
; CHECK-NEXT:  LBB3_1: ## %for.body
; CHECK-NEXT:    ## =>This Inner Loop Header: Depth=1
; CHECK-NEXT:    movq {{.*}}(%rip), %rsi
; CHECK-NEXT:    movq %rbx, %rdi
; CHECK-NEXT:    callq *%r14
; CHECK-NEXT:    decl %ebp
; CHECK-NEXT:    jne LBB3_1
; CHECK-NEXT:  ## %bb.2: ## %for.end
; CHECK-NEXT:    popq %rbx
; CHECK-NEXT:    popq %r14
; CHECK-NEXT:    popq %rbp
; CHECK-NEXT:    retq
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %i.01 = phi i32 [ 0, %entry ], [ %inc, %for.body ]
  %0 = load atomic i8*, i8** @"\01L_OBJC_SELECTOR_REFERENCES_" seq_cst, align 8, !invariant.load !0
  %call = tail call i8* bitcast (i8* (i8*, i8*, ...)* @objc_msgSend to i8* (i8*, i8*)*)(i8* %x, i8* %0)
  %inc = add i32 %i.01, 1
  %exitcond = icmp eq i32 %inc, 10000
  br i1 %exitcond, label %for.end, label %for.body

for.end:                                          ; preds = %for.body
  ret void
}

declare i8* @objc_msgSend(i8*, i8*, ...) nonlazybind

define void @test_multi_def(i64* dereferenceable(8) %x1,
; CHECK-LABEL: test_multi_def:
; CHECK:       ## %bb.0: ## %entry
; CHECK-NEXT:    movq %rdx, %r8
; CHECK-NEXT:    xorl %r9d, %r9d
; CHECK-NEXT:    movq (%rdi), %rdi
; CHECK-NEXT:    movq (%rsi), %rsi
; CHECK-NEXT:    .p2align 4, 0x90
; CHECK-NEXT:  LBB4_2: ## %for.body
; CHECK-NEXT:    ## =>This Inner Loop Header: Depth=1
; CHECK-NEXT:    movq %rdi, %rax
; CHECK-NEXT:    mulq %rsi
; CHECK-NEXT:    addq %rax, (%r8)
; CHECK-NEXT:    adcq %rdx, 8(%r8)
; CHECK-NEXT:  ## %bb.1: ## %for.check
; CHECK-NEXT:    ## in Loop: Header=BB4_2 Depth=1
; CHECK-NEXT:    incq %r9
; CHECK-NEXT:    addq $16, %r8
; CHECK-NEXT:    cmpq %rcx, %r9
; CHECK-NEXT:    jl LBB4_2
; CHECK-NEXT:  ## %bb.3: ## %exit
; CHECK-NEXT:    retq
                            i64* dereferenceable(8) %x2,
                            i128* %y, i64 %count) nounwind {
entry:
  br label %for.body

for.check:
  %inc = add nsw i64 %i, 1
  %done = icmp sge i64 %inc, %count
  br i1 %done, label %exit, label %for.body

for.body:
  %i = phi i64 [ 0, %entry ], [ %inc, %for.check ]
  %x1_load = load i64, i64* %x1, align 8, !invariant.load !0
  %x1_zext = zext i64 %x1_load to i128
  %x2_load = load i64, i64* %x2, align 8, !invariant.load !0
  %x2_zext = zext i64 %x2_load to i128
  %x_prod = mul i128 %x1_zext, %x2_zext
  %y_elem = getelementptr inbounds i128, i128* %y, i64 %i
  %y_load = load i128, i128* %y_elem, align 8
  %y_plus = add i128 %x_prod, %y_load
  store i128 %y_plus, i128* %y_elem, align 8
  br label %for.check

exit:
  ret void
}

define void @test_div_def(i32* dereferenceable(8) %x1,
; CHECK-LABEL: test_div_def:
; CHECK:       ## %bb.0: ## %entry
; CHECK-NEXT:    movq %rdx, %r8
; CHECK-NEXT:    xorl %r9d, %r9d
; CHECK-NEXT:    movl (%rdi), %edi
; CHECK-NEXT:    movl (%rsi), %esi
; CHECK-NEXT:    .p2align 4, 0x90
; CHECK-NEXT:  LBB5_2: ## %for.body
; CHECK-NEXT:    ## =>This Inner Loop Header: Depth=1
; CHECK-NEXT:    movl %edi, %eax
; CHECK-NEXT:    xorl %edx, %edx
; CHECK-NEXT:    divl %esi
; CHECK-NEXT:    addl %eax, (%r8,%r9,4)
; CHECK-NEXT:  ## %bb.1: ## %for.check
; CHECK-NEXT:    ## in Loop: Header=BB5_2 Depth=1
; CHECK-NEXT:    incq %r9
; CHECK-NEXT:    cmpl %ecx, %r9d
; CHECK-NEXT:    jl LBB5_2
; CHECK-NEXT:  ## %bb.3: ## %exit
; CHECK-NEXT:    retq
                          i32* dereferenceable(8) %x2,
                          i32* %y, i32 %count) nounwind {
entry:
  br label %for.body

for.check:
  %inc = add nsw i32 %i, 1
  %done = icmp sge i32 %inc, %count
  br i1 %done, label %exit, label %for.body

for.body:
  %i = phi i32 [ 0, %entry ], [ %inc, %for.check ]
  %x1_load = load i32, i32* %x1, align 8, !invariant.load !0
  %x2_load = load i32, i32* %x2, align 8, !invariant.load !0
  %x_quot = udiv i32 %x1_load, %x2_load
  %y_elem = getelementptr inbounds i32, i32* %y, i32 %i
  %y_load = load i32, i32* %y_elem, align 8
  %y_plus = add i32 %x_quot, %y_load
  store i32 %y_plus, i32* %y_elem, align 8
  br label %for.check

exit:
  ret void
}

!0 = !{}
