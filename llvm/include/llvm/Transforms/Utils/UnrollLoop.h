//===- llvm/Transforms/Utils/UnrollLoop.h - Unrolling utilities -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines some loop unrolling utilities. It does not define any
// actual pass or policy, but provides a single function to perform loop
// unrolling.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_UNROLLLOOP_H
#define LLVM_TRANSFORMS_UTILS_UNROLLLOOP_H

#include <map>
#include <vector>
#include "llvm/ADT/ValueMap.h"

namespace llvm {

class Loop;
class LoopInfo;
class LPPassManager;

  Loop* cloneLoop(Loop* oldLoop, std::map<Loop*, Loop*>& oldToNewMap);

  bool UnrollLoop(Loop *L, unsigned Count, LoopInfo* LI, LPPassManager* LPM, bool doPeel = false, bool CompletelyUnroll = false, std::vector<ValueMap<const Value *, Value*>* >* Iterations = 0);

}

#endif
