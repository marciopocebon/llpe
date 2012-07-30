//===- LibCallAliasAnalysis.h - Implement AliasAnalysis for libcalls ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the LibCallAliasAnalysis class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_LIBCALL_AA_H
#define LLVM_ANALYSIS_LIBCALL_AA_H

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Pass.h"

namespace llvm {
  class LibCallInfo;
  struct LibCallFunctionInfo;
  
  /// LibCallAliasAnalysis - Alias analysis driven from LibCallInfo.
  struct LibCallAliasAnalysis : public ModulePass, public AliasAnalysis {
    static char ID; // Class identification
    
    LibCallInfo *LCI;
    
    explicit LibCallAliasAnalysis(LibCallInfo *LC = 0)
      : ModulePass(ID), LCI(LC) {
    }
    explicit LibCallAliasAnalysis(char &ID, LibCallInfo *LC)
      : ModulePass(ID), LCI(LC) {
    }
    ~LibCallAliasAnalysis();
    
    ModRefResult getModRefInfo(ImmutableCallSite CS,
                               const Value *P, unsigned Size, IntegrationAttempt* CSCtx = 0, IntegrationAttempt* PCtx = 0);
    
    ModRefResult getModRefInfo(ImmutableCallSite CS1, ImmutableCallSite CS2, 
			       IntegrationAttempt* CS1Ctx = 0, IntegrationAttempt* CS2Ctx = 0) {
      // TODO: Could compare two direct calls against each other if we cared to.
      return AliasAnalysis::getModRefInfo(CS1, CS2, CS1Ctx, CS2Ctx);
    }
    
    virtual void getAnalysisUsage(AnalysisUsage &AU) const;
    
    virtual bool runOnModule(Module &M) {
      InitializeAliasAnalysis(this);                 // set up super class
      return false;
    }
    
    /// getAdjustedAnalysisPointer - This method is used when a pass implements
    /// an analysis interface through multiple inheritance.  If needed, it
    /// should override this to adjust the this pointer as needed for the
    /// specified pass info.
    virtual void *getAdjustedAnalysisPointer(const void *PI) {
      if (PI == &AliasAnalysis::ID)
        return (AliasAnalysis*)this;
      return this;
    }
    
  private:
    ModRefResult AnalyzeLibCallDetails(const LibCallFunctionInfo *FI,
                                       ImmutableCallSite CS,
                                       const Value *P, unsigned Size,
				       IntegrationAttempt* CSCtx,
				       IntegrationAttempt* PCtx);
  };
}  // End of llvm namespace

#endif
