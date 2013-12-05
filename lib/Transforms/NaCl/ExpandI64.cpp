//===- ExpandI64.cpp - Expand out variable argument function calls-----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===------------------------------------------------------------------===//
//
// This pass expands and lowers all i64 operations, into 32-bit operations
// that can be handled by JS in a natural way.
//
// 64-bit variables become pairs of 2 32-bit variables, for the low and
// high 32 bit chunks. This happens for both registers and function
// arguments. Function return values become a return of the low 32 bits
// and a store of the high 32-bits in tempRet0, a global helper variable.
//
// Many operations then become simple pairs of operations, for example
// bitwise AND becomes and AND of each 32-bit chunk. More complex operations
// like addition are lowered into calls into library support code in
// Emscripten (i64Add for example).
//
//===------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/NaCl.h"

#include "llvm/Support/raw_ostream.h"
#include <stdio.h>
#define dump(x) fprintf(stderr, x "\n")
#define dumpv(x, ...) fprintf(stderr, x "\n", __VA_ARGS__)
#define dumpIR(value) { \
  std::string temp; \
  raw_string_ostream stream(temp); \
  stream << *(value); \
  dumpv("%s\n", temp.c_str()); \
}

using namespace llvm;

namespace {
  // This is a ModulePass because the pass recreates functions in
  // order to expand i64 arguments to pairs of i32s.
  class ExpandI64 : public ModulePass {
    typedef std::vector<Value*> SplitParts;

    std::map<Value*, SplitParts> Splits; // old i64 value to new insts

    // splits a 64-bit instruction into 32-bit chunks. We do
    // not yet have the values yet, as they depend on other
    // splits, so store the parts in Splits, for FinalizeInst.
    void SplitInst(Instruction *I, DataLayout& DL);

    void FinalizeInst(Instruction *I);

  public:
    static char ID;
    ExpandI64() : ModulePass(ID) {
      initializeExpandI64Pass(*PassRegistry::getPassRegistry());
    }

    virtual bool runOnModule(Module &M);
  };
}

char ExpandI64::ID = 0;
INITIALIZE_PASS(ExpandI64, "expand-i64",
                "Expand and lower i64 operations into 32-bit chunks",
                false, false)

//static void ExpandI64Func(Function *Func) {
//}

void ExpandI64::SplitInst(Instruction *I, DataLayout& DL) {
  switch (I->getOpcode()) {
    case Instruction::SExt: {
      // x = sext i32 to i64  ==>  xl = x ; test = x < 0 ; xh = test ? -1 : 0
      Value *Input = I->getOperand(0);
      Type *T = Input->getType();
      assert(T->getIntegerBitWidth() == 32); // FIXME: sext from smaller
      Value *Zero  = Constant::getNullValue(T);
      Value *Ones  = Constant::getAllOnesValue(T);

      Value *Check = CopyDebug(new ICmpInst(I, ICmpInst::ICMP_SLE, Input, Zero), I);
      Value *High  = CopyDebug(SelectInst::Create(Check, Ones, Zero, "", I), I);
      SplitParts &Split = Splits[I];
      Split.push_back(Check);
      Split.push_back(High);
      break;
    }
    case Instruction::Store: {
      // store i64 A, i64* P  =>  ai = P ; P4 = ai+4 ; lp = P to i32* ; hp = P4 to i32* ; store l, lp ; store h, hp
      StoreInst *SI = dyn_cast<StoreInst>(I);
      Type *I32 = Type::getInt32Ty(I->getContext());
      Type *I32P = I32->getPointerTo(); // XXX DL->getIntPtrType(I->getContext())
      Value *Zero  = Constant::getNullValue(I32);

      Value *AI = CopyDebug(new PtrToIntInst(SI->getPointerOperand(), I32, "", I), I);
      Value *P4 = CopyDebug(BinaryOperator::Create(Instruction::Add, AI, ConstantInt::get(I32, 4), "", I), I);
      Value *LP = CopyDebug(new IntToPtrInst(AI, I32P, "", I), I);
      Value *HP = CopyDebug(new IntToPtrInst(P4, I32P, "", I), I);
      Value *SL = CopyDebug(new StoreInst(Zero, LP, I), I); // will be fixed
      Value *SH = CopyDebug(new StoreInst(Zero, HP, I), I); // will be fixed
      SplitParts &Split = Splits[I];
      Split.push_back(AI);
      Split.push_back(P4);
      Split.push_back(LP);
      Split.push_back(HP);
      Split.push_back(SL);
      Split.push_back(SH);
      break;
    }
    //default: // FIXME: abort if we hit something we don't support
  }
}

bool ExpandI64::runOnModule(Module &M) {
  bool Changed = false;
  DataLayout DL(&M);
  for (Module::iterator Iter = M.begin(), E = M.end(); Iter != E; ) {
    Function *Func = Iter++;
    for (Function::iterator BB = Func->begin(), E = Func->end();
         BB != E;
         ++BB) {
      for (BasicBlock::iterator Iter = BB->begin(), E = BB->end();
           Iter != E; ) {
        Instruction *I = Iter++;
        Type *T = I->getType();
        if (T->isIntegerTy() && T->getIntegerBitWidth() == 64) {
          Changed = true;
          SplitInst(I, DL);
          continue;
        }
        if (I->getNumOperands() >= 1) {
          T = I->getOperand(0)->getType();
          if (T->isIntegerTy() && T->getIntegerBitWidth() == 64) {
            Changed = true;
            SplitInst(I, DL);
            continue;
          }
        }
      }
    }
  }
  return Changed;
}

ModulePass *llvm::createExpandI64Pass() {
  return new ExpandI64();
}

