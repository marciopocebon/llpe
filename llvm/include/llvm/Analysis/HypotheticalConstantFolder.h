
#ifndef LLVM_HYPO_CONSTFOLD_H
#define LLVM_HYPO_CONSTFOLD_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/ValueMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Pass.h"
#include "llvm/Value.h"
#include "llvm/Constant.h"

#include <string>
#include <vector>

#define LPDEBUG(x) DEBUG(do { printDebugHeader(dbgs()); dbgs() << ": " << x; } while(0))

namespace llvm {

class Function;
class BasicBlock;
class Instruction;
class TargetData;
class AliasAnalysis;
class PHINode;
class MemDepResult;
class NonLocalDepResult;
class LoadInst;
class raw_ostream;
class ConstantInt;
class Type;
class Argument;
class CallInst;
class StoreInst;
class MemTransferInst;
class MemIntrinsic;
class CmpInst;

class IntegrationAttempt;

typedef struct { Value* first; IntegrationAttempt* second; } ValCtx;

inline bool operator==(ValCtx V1, ValCtx V2) {
  return V1.first == V2.first && V1.second == V2.second;
}

inline bool operator!=(ValCtx V1, ValCtx V2) {
   return !(V1 == V2);
}

inline bool operator<(ValCtx V1, ValCtx V2) {
  return V1.first < V2.first || (V1.first == V2.first && V1.second < V2.second);
}

inline bool operator<=(ValCtx V1, ValCtx V2) {
  return V1 < V2 || V1 == V2;
}

inline bool operator>(ValCtx V1, ValCtx V2) {
  return !(V1 <= V2);
}

inline bool operator>=(ValCtx V1, ValCtx V2) {
  return !(V1 < V2);
}

raw_ostream& operator<<(raw_ostream&, const ValCtx&);
raw_ostream& operator<<(raw_ostream&, const MemDepResult&);
raw_ostream& operator<<(raw_ostream&, const IntegrationAttempt&);

#define VCNull (make_vc(0, 0))

inline ValCtx make_vc(Value* V, IntegrationAttempt* H) {

  ValCtx newCtx = {V, H};
  return newCtx;

}

inline ValCtx const_vc(Constant* C) {

  return make_vc(C, 0);

}

// Characteristics for using ValCtxs in hashsets (DenseSet, or as keys in DenseMaps)
template<> struct DenseMapInfo<ValCtx> {
  
  typedef DenseMapInfo<Value*> VInfo;
  typedef DenseMapInfo<IntegrationAttempt*> IAInfo;
  typedef DenseMapInfo<std::pair<Value*, IntegrationAttempt*> > PairInfo;

  static inline ValCtx getEmptyKey() {
    return make_vc(VInfo::getEmptyKey(), IAInfo::getEmptyKey());
  }

  static inline ValCtx getTombstoneKey() {
    return make_vc(VInfo::getTombstoneKey(), IAInfo::getTombstoneKey());
  }

  static unsigned getHashValue(const ValCtx& VC) {
    return PairInfo::getHashValue(std::make_pair(VC.first, VC.second));
  }

  static bool isEqual(const ValCtx& V1, const ValCtx& V2) {
    return V1 == V2;
  }

 };

// Define PartialVal, a container that gives a resolution to a load attempt, either wholly with a ValCtx
// or partially with a Constant plus a byte extent and offset from which that Constant should be read.

enum PartialValType {

  PVInvalid,
  PVTotal,
  PVPartial

};

struct PartialVal {

  PartialValType type;
  ValCtx TotalVC;
  uint64_t FirstDef;
  uint64_t FirstNotDef;
  Constant* C;
  uint64_t ReadOffset;

 PartialVal(ValCtx Total) : 
  type(PVTotal), TotalVC(Total), FirstDef(0), FirstNotDef(0), C(0), ReadOffset(0) { }
 PartialVal(uint64_t FD, uint64_t FND, Constant* _C, uint64_t Off) : 
  type(PVPartial), TotalVC(VCNull), FirstDef(FD), FirstNotDef(FND), C(_C), ReadOffset(Off) { }
 PartialVal() :
  type(PVInvalid), TotalVC(VCNull), FirstDef(0), FirstNotDef(0), C(0), ReadOffset(0) { }

  bool isPartial() { return type == PVPartial; }
  bool isTotal() { return type == PVTotal; }
  
  static PartialVal getPartial(uint64_t FD, uint64_t FND, Constant* _C, uint64_t Off) {
    return PartialVal(FD, FND, _C, Off);
  }

  static PartialVal getTotal(ValCtx VC) {
    return PartialVal(VC);
  }

};

#define PVNull PartialVal()

inline bool operator==(PartialVal V1, PartialVal V2) {
  if(V1.type == PVInvalid && V2.type == PVInvalid)
    return true;
  else if(V1.type == PVTotal && V2.type == PVTotal)
    return V1.TotalVC == V2.TotalVC;
  else if(V1.type == PVPartial && V2.type == PVPartial)
    return ((V1.FirstDef == V2.FirstDef) &&
	    (V1.FirstNotDef == V2.FirstNotDef) &&
	    (V1.C == V2.C) &&
	    (V1.ReadOffset == V2.ReadOffset));

  return false;
}

inline bool operator!=(PartialVal V1, PartialVal V2) {
   return !(V1 == V2);
}

enum SymSubclasses {

  SThunk,
  SGEP,
  SCast

};

class SymExpr { 

public:
  static inline bool classof(const SymExpr*) { return true; }
  virtual void describe(raw_ostream& OS) = 0;
  virtual int getSymType() const = 0;

};

class SymThunk : public SymExpr {

public:
  static inline bool classof(const SymExpr* S) { return S->getSymType() == SThunk; }
  static inline bool classof(const SymThunk*) { return true; }
  ValCtx RealVal;

  SymThunk(ValCtx R) : RealVal(R) { }
  void describe(raw_ostream& OS);
  int getSymType() const { return SThunk; }

};

class SymGEP : public SymExpr {

public:
  static inline bool classof(const SymExpr* S) { return S->getSymType() == SGEP; }
  static inline bool classof(const SymGEP*) { return true; }
  SmallVector<Value*, 4> Offsets; // Really all ConstantInts

  SymGEP(SmallVector<Value*, 4> Offs) : Offsets(Offs) { }

  void describe(raw_ostream& OS);
  int getSymType() const { return SGEP; }

};

class SymCast : public SymExpr {

public:
  static inline bool classof(const SymExpr* S) { return S->getSymType() == SCast; }
  static inline bool classof(const SymCast*) { return true; }
  const Type* ToType;

  SymCast(const Type* T) : ToType(T) { }

  void describe(raw_ostream& OS);
  int getSymType() const { return SCast; }

};

class LoadForwardAttempt;
class LFARealization;
class LFAQueryable;

struct OpenStatus {

  std::string Name;
  bool FDEscapes;
  ValCtx LatestResolvedUser;

  OpenStatus(ValCtx O, std::string N, bool Esc) : Name(N), FDEscapes(Esc), LatestResolvedUser(O) { }

OpenStatus() : Name(""), FDEscapes(false), LatestResolvedUser(VCNull) {}

};

struct ReadFile {

  struct OpenStatus* openArg;
  uint64_t incomingOffset;
  uint32_t readSize;

  ReadFile(struct OpenStatus* O, uint64_t IO, uint32_t RS) : openArg(O), incomingOffset(IO), readSize(RS) { }

ReadFile() : openArg(0), incomingOffset(0), readSize(0) { }

};

struct SeekFile {

  struct OpenStatus* openArg;
  uint64_t newOffset;

SeekFile(struct OpenStatus* O, uint64_t Off) : openArg(O), newOffset(Off) { }
  
SeekFile() : openArg(0), newOffset(0) { }

};

class InlineAttempt;
class PeelAttempt;
class IntegrationHeuristicsPass;

class Function;
class LoopInfo;
class TargetData;
class AliasAnalysis;
class Loop;

class Callable {
 public:

  virtual void callback(IntegrationAttempt*) = 0;

};

class UnaryPred {
 public:
  virtual bool operator()(Value*) = 0;

};

class OpCallback {
 public:
  virtual void callback(IntegrationAttempt* Ctx, Value* V) = 0;

};

class VisitorContext {
public:

  virtual void visit(IntegrationAttempt* Context, Instruction* UserI) = 0;
  virtual void notifyUsersMissed() = 0;
  virtual bool shouldContinue() = 0;

};

enum IterationStatus {

  IterationStatusUnknown,
  IterationStatusFinal,
  IterationStatusNonFinal

};

enum IntegratorType {

  IntegratorTypeIA,
  IntegratorTypePA

};

struct IntegratorTag {

  IntegratorType type;
  void* ptr;

};

class IntegrationAttempt {

protected:

  IntegrationHeuristicsPass* pass;

  // Analyses created by the Pass.
  DenseMap<Function*, LoopInfo*> LI;
  TargetData* TD;
  AliasAnalysis* AA;

  Function& F;

  DenseMap<Instruction*, const Loop*>& invariantInsts;
  DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>& invariantEdges;
  DenseMap<BasicBlock*, const Loop*>& invariantBlocks;

  DenseMap<Value*, ValCtx> improvedValues;

  SmallSet<BasicBlock*, 4> deadBlocks;
  SmallSet<std::pair<BasicBlock*, BasicBlock*>, 4> deadEdges;
  SmallSet<BasicBlock*, 8> certainBlocks;

  // Store, allocation and other instructions which, whilst they cannot be folded away, are not used
  // when folded / forwarded instructions are discounted.
  DenseSet<Value*> deadValues;
  // A list of dead stores and allocations which in the course of being found dead traversed this context.
  // These should be discounted as dead values if we are folded.
  DenseSet<ValCtx> deadValuesTraversingThisContext;

  int improvableInstructions;
  int improvedInstructions;
  SmallVector<CallInst*, 4> unexploredCalls;
  SmallVector<const Loop*, 4> unexploredLoops;

  SmallVector<std::pair<IntegrationAttempt*, LoadInst*>, 4> CFGBlockedLoads;
  DenseMap<Instruction*, SmallVector<std::pair<IntegrationAttempt*, LoadInst*>, 4> > InstBlockedLoads;

  SmallVector<std::pair<ValCtx, ValCtx>, 4> CFGBlockedOpens;
  DenseMap<Instruction*, SmallVector<std::pair<ValCtx, ValCtx>, 4> > InstBlockedOpens;

  DenseMap<CallInst*, OpenStatus> forwardableOpenCalls;
  DenseMap<CallInst*, ReadFile> resolvedReadCalls;
  DenseMap<CallInst*, SeekFile> resolvedSeekCalls;

  // Inline attempts / peel attempts which are currently ignored because they've been opted out.
  // These may include inlines / peels which are logically two loop levels deep, 
  // because they were created when a parent loop was opted out but then opted in again.
  DenseSet<CallInst*> ignoreIAs;
  DenseSet<const Loop*> ignorePAs;

  // A map from the Values used in all of the above to the clones of Instructions produced at commit time
  ValueMap<const Value*, Value*> CommittedValues;

  std::string nestingIndent() const;

  int nesting_depth;

 public:

  struct IntegratorTag tag;

  IntegrationAttempt* parent;

  DenseMap<CallInst*, InlineAttempt*> inlineChildren;
  DenseMap<const Loop*, PeelAttempt*> peelChildren;
    
 IntegrationAttempt(IntegrationHeuristicsPass* Pass, IntegrationAttempt* P, Function& _F, DenseMap<Function*, LoopInfo*>& _LI, TargetData* _TD, AliasAnalysis* _AA,
		    DenseMap<Instruction*, const Loop*>& _invariantInsts, DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>& _invariantEdges, DenseMap<BasicBlock*, const Loop*>& _invariantBlocks, int depth) : 
  pass(Pass),
    LI(_LI),
    TD(_TD), 
    AA(_AA), 
    F(_F),
    invariantInsts(_invariantInsts),
    invariantEdges(_invariantEdges),
    invariantBlocks(_invariantBlocks),
    improvableInstructions(0),
    improvedInstructions(0),
    nesting_depth(depth),
    parent(P)
      { 
	this->tag.ptr = (void*)this;
	this->tag.type = IntegratorTypeIA;
      }

  ~IntegrationAttempt();

  virtual AliasAnalysis* getAA();

  virtual ValCtx getDefaultVC(Value*);
  virtual ValCtx getReplacement(Value* V);
  virtual bool edgeIsDead(BasicBlock*, BasicBlock*);
  virtual bool blockIsDead(BasicBlock*);

  // Helpers for the above:

  const Loop* getValueScope(Value*);
  ValCtx getLocalReplacement(Value*);
  ValCtx getReplacementUsingScope(Value* V, const Loop* LScope);
  void callWithScope(Callable& C, const Loop* LScope);
  ValCtx getDefaultVCWithScope(Value*, const Loop*);

  const Loop* getEdgeScope(BasicBlock*, BasicBlock*);
  bool edgeIsDeadWithScope(BasicBlock*, BasicBlock*, const Loop*);

  const Loop* getBlockScope(BasicBlock*);
  bool blockIsDeadWithScope(BasicBlock*, const Loop*);

  bool blockIsCertain(BasicBlock*);

  bool shouldForwardValue(ValCtx);

  virtual ValCtx getUltimateUnderlyingObject(Value*);

  virtual BasicBlock* getEntryBlock() = 0;
  virtual bool hasParent();
  
  // Pure virtuals to be implemented by PeelIteration or InlineAttempt:

  virtual const Loop* getLoopContext() = 0;
  virtual Instruction* getEntryInstruction() = 0;
  virtual void collectAllLoopStats() = 0;
  virtual void printHeader(raw_ostream& OS) const = 0;
  virtual void queueTryEvaluateOwnCall() = 0;

  // Simple state-tracking helpers:

  virtual void setReplacement(Value*, ValCtx);
  void eraseReplacement(Value*);
  bool isUnresolved(Value*);
  void setEdgeDead(BasicBlock*, BasicBlock*);
  void setBlockDead(BasicBlock*);

  virtual Constant* getConstReplacement(Value* V);
  
  virtual InlineAttempt* getFunctionRoot() = 0;

  // Constant propagation:

  virtual bool shouldTryEvaluate(Value* ArgV, bool verbose = true);

  ValCtx getPHINodeValue(PHINode*);
  virtual bool getLoopHeaderPHIValue(PHINode* PN, ValCtx& result);
  void tryEvaluate(Value*);
  virtual ValCtx tryEvaluateResult(Value*);
  virtual void investigateUsers(Value* V);

  void queueTryEvaluateGeneric(Instruction* UserI, Value* Used);

  // CFG analysis:

  bool shouldCheckBlock(BasicBlock* BB);
  void checkBlock(BasicBlock* BB);
  void checkEdge(BasicBlock*, BasicBlock*);
  void checkVariantEdge(BasicBlock*, BasicBlock*, const Loop* Scope);
  void checkLocalEdge(BasicBlock*, BasicBlock*);
  virtual bool checkLoopSpecialEdge(BasicBlock*, BasicBlock*);
  
  // Child (inlines, peels) management

  InlineAttempt* getInlineAttempt(CallInst* CI);
  InlineAttempt* getOrCreateInlineAttempt(CallInst* CI);
 
  PeelAttempt* getPeelAttempt(const Loop*);
  PeelAttempt* getOrCreatePeelAttempt(const Loop*);

  // Load forwarding:

  void checkLoad(LoadInst* LI);
  ValCtx tryForwardLoad(LoadInst*);
  ValCtx tryForwardLoad(LoadForwardAttempt&, Instruction* StartBefore);
  MemDepResult tryResolveLoad(LoadForwardAttempt&);
  MemDepResult tryResolveLoad(LoadForwardAttempt&, Instruction* StartBefore, ValCtx& ConstResult);
  ValCtx getForwardedValue(LoadForwardAttempt&, MemDepResult Res);
  bool tryResolveLoadFromConstant(LoadInst*, ValCtx& Result);
  
  bool forwardLoadIsNonLocal(LFAQueryable&, MemDepResult& Result);
  ValCtx getDefn(const MemDepResult& Res);
  MemDepResult getUniqueDependency(LFAQueryable&);

  virtual MemDepResult tryForwardExprFromParent(LoadForwardAttempt&) = 0;
  MemDepResult tryResolveLoadAtChildSite(IntegrationAttempt* IA, LoadForwardAttempt&);
  bool tryResolveExprFrom(LoadForwardAttempt& LFA, Instruction* Where, MemDepResult& Result);
  bool tryResolveExprFrom(LoadForwardAttempt& LFA, Instruction* Where, MemDepResult& Result, ValCtx& ConstResult);
  bool tryResolveExprUsing(LFARealization& LFAR, MemDepResult& Result);

  virtual bool tryForwardLoadThroughCall(LoadForwardAttempt&, CallInst*, MemDepResult&);
  virtual bool tryForwardLoadThroughLoopFromBB(BasicBlock* BB, LoadForwardAttempt&, BasicBlock*& PreheaderOut, SmallVectorImpl<NonLocalDepResult> &Result);

  void addBlockedLoad(Instruction* BlockedOn, IntegrationAttempt* RetryCtx, LoadInst* RetryLI);
  void addCFGBlockedLoad(IntegrationAttempt* RetryCtx, LoadInst* RetryLI);
  virtual void queueLoopExpansionBlockedLoad(Instruction*, IntegrationAttempt*, LoadInst*);

  void queueWorkBlockedOn(Instruction* SI);
  void queueCFGBlockedLoads();
  void queueCheckAllLoadsInScope(const Loop*);

  // VFS call forwarding:

  virtual bool isForwardableOpenCall(Value*);
  virtual int64_t tryGetIncomingOffset(Value*);
  virtual ReadFile* tryGetReadFile(CallInst* CI);
  void tryPromoteOpenCall(CallInst* CI);
  void tryPromoteAllCalls();
  void queueInitialWork();
  void tryPushOpen(CallInst*, ValCtx);
  virtual bool tryPushOpenFrom(ValCtx&, ValCtx, ValCtx, OpenStatus&, bool);
  ValCtx getSuccessorVC(BasicBlock* BB);
  virtual bool checkLoopIterationOrExit(BasicBlock* PresentBlock, BasicBlock* NextBlock, ValCtx& Start) = 0;
  bool vfsCallBlocksOpen(CallInst*, ValCtx, ValCtx, OpenStatus&, bool&, bool&);
  ValCtx tryFoldOpenCmp(CmpInst* CmpI, ConstantInt* CmpInt, bool flip);
  virtual void resolveReadCall(CallInst*, struct ReadFile);
  virtual void resolveSeekCall(CallInst*, struct SeekFile);
  virtual void addBlockedOpen(ValCtx, ValCtx);
  void queueCFGBlockedOpens();
  virtual bool isResolvedVFSCall(const Instruction*);

  // Tricky load forwarding (stolen from GVN)

  ValCtx handlePartialDefnByConst(LoadForwardAttempt&, uint64_t, uint64_t, Constant*, ValCtx);
  ValCtx handlePartialDefn(LoadForwardAttempt&, uint64_t, uint64_t, ValCtx);
  ValCtx handleTotalDefn(LoadForwardAttempt&, ValCtx);
  ValCtx handleTotalDefn(LoadForwardAttempt&, Constant*);

  bool AnalyzeLoadFromClobberingWrite(LoadForwardAttempt&,
				     Value *WritePtr, IntegrationAttempt* WriteCtx,
				     uint64_t WriteSizeInBits,
				     uint64_t& FirstDef, 
				     uint64_t& FirstNotDef, 
				     uint64_t& ReadOffset);

  bool AnalyzeLoadFromClobberingWrite(LoadForwardAttempt&,
				     ValCtx StoreBase, int64_t StoreOffset,
				     uint64_t WriteSizeInBits,
				     uint64_t& FirstDef, 
				     uint64_t& FirstNotDef, 
				     uint64_t& ReadOffset);
				     
  bool AnalyzeLoadFromClobberingStore(LoadForwardAttempt&, StoreInst *DepSI, IntegrationAttempt* DepSICtx,
				     uint64_t& FirstDef, 
				     uint64_t& FirstNotDef, 
				     uint64_t& ReadOffset);

  bool AnalyzeLoadFromClobberingMemInst(LoadForwardAttempt&, MemIntrinsic *MI, IntegrationAttempt* MICtx,
				       uint64_t& FirstDef, 
				       uint64_t& FirstNotDef, 
				       uint64_t& ReadOffset);

  Constant* offsetConstantInt(Constant* SourceC, int64_t Offset, const Type* targetTy);
  ValCtx GetBaseWithConstantOffset(Value *Ptr, IntegrationAttempt* PtrCtx, int64_t &Offset);
  bool CanCoerceMustAliasedValueToLoad(Value *StoredVal, const Type *LoadTy);
  Constant* CoerceConstExprToLoadType(Constant *StoredVal, const Type *LoadedTy);
  bool GetDefinedRange(ValCtx DefinedBase, int64_t DefinedOffset, uint64_t DefinedSizeBits,
		       ValCtx DefinerBase, int64_t DefinerOffset, uint64_t DefinerSizeBits,
		       uint64_t& FirstDef, uint64_t& FirstNotDef, uint64_t& ReadOffset);
  PartialVal tryResolveClobber(LoadForwardAttempt& LFA, ValCtx Clobber);

  // Dead store and allocation elim:

  bool tryKillStore(StoreInst* SI);
  bool tryKillMemset(MemIntrinsic* MI);
  bool tryKillMTI(MemTransferInst* MTI);
  bool tryKillAlloc(Instruction* Alloc);
  bool tryKillWriterTo(Instruction* Writer, Value* WritePtr, uint64_t Size);
  bool DSEHandleWrite(ValCtx Writer, uint64_t WriteSize, ValCtx StorePtr, uint64_t Size, ValCtx StoreBase, int64_t StoreOffset, bool* deadBytes);
  bool isLifetimeEnd(ValCtx Alloc, Instruction* I);
  void addTraversingInst(ValCtx);
  virtual bool tryKillStoreFrom(ValCtx& Start, ValCtx StorePtr, ValCtx StoreBase, int64_t StoreOffset, bool* deadBytes, uint64_t Size, bool skipFirst, bool& Killed);
  bool CollectMTIsFrom(ValCtx& Start, std::vector<ValCtx>& MTIs);
  void tryKillAllMTIs();
  bool tryKillAllStoresFrom(ValCtx& Start);
  void tryKillAllStores();
  bool tryKillAllAllocsFrom(ValCtx& Start);
  void tryKillAllAllocs();

  // User visitors:
  
  virtual bool visitNextIterationPHI(Instruction* I, VisitorContext& Visitor);
  virtual void visitExitPHI(Instruction* UserI, VisitorContext& Visitor);
  void visitUsers(Value* V, VisitorContext& Visitor);

  // Operand visitors:

  void walkOperand(Value* V, OpCallback& CB);
  virtual bool walkHeaderPHIOperands(PHINode*, OpCallback&) = 0;
  virtual void walkOperands(Value* V, OpCallback& CB);

  // Dead instruction elim:

  bool valueIsDead(Value* V);
  bool shouldDIE(Value* V);
  void queueDIE(Value* V, IntegrationAttempt* Ctx);
  void queueDIE(Value* V);
  bool localValueIsDead(Value* V);
  bool inDeadValues(Value* V);
  void queueDIEOperands(Value* V);
  void tryKillValue(Value* V);
  virtual void queueAllLiveValuesMatching(UnaryPred& P);
  void queueAllReturnInsts();
  void queueAllLiveValues();

  // Enabling / disabling exploration:

  void enablePeel(const Loop*);
  void disablePeel(const Loop*);
  bool loopIsEnabled(const Loop*);
  void enableInline(CallInst*);
  void disableInline(CallInst*);
  bool inlineIsEnabled(CallInst*);
  virtual bool isEnabled() = 0;
  virtual void setEnabled(bool) = 0;
  bool isAvailable();
  void revertDSEandDAE();
  void revertDeadValue(Value*);
  void tryKillAndQueue(Instruction*);
  void getRetryStoresAndAllocs(std::vector<ValCtx>&);
  void retryStoresAndAllocs(std::vector<ValCtx>&);
  void revertLoadsFromFoldedContexts();
  void retryLoadsFromFoldedContexts();
  void walkLoadsFromFoldedContexts(bool revert);

  // DOT export:

  void printRHS(Instruction* I, raw_ostream& Out);
  void describeBlockAsDOT(BasicBlock* BB, SmallVector<std::pair<BasicBlock*, BasicBlock*>, 4 >* deferEdges, SmallVector<std::string, 4>* deferredEdges, raw_ostream& Out);
  void describeAsDOT(raw_ostream& Out);
  std::string getInstructionColour(Instruction* I);
  std::string getGraphPath(std::string prefix);
  void describeTreeAsDOT(std::string path);
  virtual bool getSpecialEdgeDescription(BasicBlock* FromBB, BasicBlock* ToBB, raw_ostream& Out) = 0;

  // Data export for the Integrator pass:
  virtual std::string getShortHeader() = 0;
  virtual IntegratorTag* getParentTag() = 0;
  bool hasChildren();
  unsigned getNumChildren();
  IntegratorTag* getChildTag(unsigned);
  virtual bool canDisable() = 0;
  unsigned getTotalInstructions();
  unsigned getElimdInstructions();

  // Saving our results as a bitcode file:

  void commit();
  void commitInContext(LoopInfo* MasterLI, ValueMap<const Value*, Value*>& valMap);
  void commitPointers();
  void deleteInstruction(Instruction*);
  void tryDeleteDeadBlock(BasicBlock*);
  virtual void deleteDeadBlocks() = 0;
  void commitLocalConstants();
  Instruction* getCommittedValue(Value*);
  void commitLocalPointers();

  // Stat collection and printing:

  void collectAllBlockStats();
  void collectBlockStats(BasicBlock* BB);
  void collectLoopStats(const Loop*);
  void collectStats();

  void print(raw_ostream& OS) const;
  // Callable from GDB
  void dump() const;

  virtual void describe(raw_ostream& Stream) const = 0;

  void printDebugHeader(raw_ostream& Str) {
    printHeader(Str);
  }

};

class PeelIteration : public IntegrationAttempt {

  int iterationCount;
  const Loop* L;
  PeelAttempt* parentPA;

  PeelIteration* getNextIteration();
  PeelIteration* getOrCreateNextIteration();

public:

 PeelIteration(IntegrationHeuristicsPass* Pass, IntegrationAttempt* P, PeelAttempt* PP, Function& F, DenseMap<Function*, LoopInfo*>& _LI, TargetData* _TD,
	       AliasAnalysis* _AA, const Loop* _L, DenseMap<Instruction*, const Loop*>& _invariantInsts, DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>& _invariantEdges, 
	       DenseMap<BasicBlock*, const Loop*>& _invariantBlocks, int iter, int depth) :
  IntegrationAttempt(Pass, P, F, _LI, _TD, _AA, _invariantInsts, _invariantEdges, _invariantBlocks, depth),
    iterationCount(iter),
    L(_L),
    parentPA(PP),
    iterStatus(IterationStatusUnknown)
    { }

  IterationStatus iterStatus;

  virtual Instruction* getEntryInstruction();
  virtual BasicBlock* getEntryBlock();
  virtual const Loop* getLoopContext();

  virtual bool checkLoopSpecialEdge(BasicBlock*, BasicBlock*);
  virtual bool getLoopHeaderPHIValue(PHINode* PN, ValCtx& result);
  virtual void queueTryEvaluateOwnCall();

  void queueCheckExitBlock(BasicBlock* BB);
  void checkFinalIteration();

  virtual MemDepResult tryForwardExprFromParent(LoadForwardAttempt&);

  virtual bool checkLoopIterationOrExit(BasicBlock* PresentBlock, BasicBlock* NextBlock, ValCtx& Start);

  virtual InlineAttempt* getFunctionRoot();

  virtual void visitExitPHI(Instruction* UserI, VisitorContext& Visitor);
  void visitVariant(Instruction* VI, const Loop* VILoop, VisitorContext& Visitor);
  virtual bool visitNextIterationPHI(Instruction* I, VisitorContext& Visitor);

  virtual bool walkHeaderPHIOperands(PHINode* PN, OpCallback& CB);

  virtual bool getSpecialEdgeDescription(BasicBlock* FromBB, BasicBlock* ToBB, raw_ostream& Out);

  virtual void describe(raw_ostream& Stream) const;

  virtual void collectAllLoopStats();
  virtual void printHeader(raw_ostream&) const;

  virtual std::string getShortHeader();
  virtual IntegratorTag* getParentTag();

  virtual bool canDisable();
  virtual bool isEnabled();
  virtual void setEnabled(bool);

  virtual void deleteDeadBlocks();

};

class ProcessExternalCallback;

class PeelAttempt {
   // Not a subclass of IntegrationAttempt -- this is just a helper.

   IntegrationHeuristicsPass* pass;
   IntegrationAttempt* parent;
   Function& F;

   DenseMap<Function*, LoopInfo*>& LI;
   TargetData* TD;
   AliasAnalysis* AA;

   const Loop* L;

   DenseMap<Instruction*, const Loop*>& invariantInsts;
   DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>& invariantEdges;
   DenseMap<BasicBlock*, const Loop*>& invariantBlocks;

   int nesting_depth;
   int debugIndent;

 public:

   struct IntegratorTag tag;

   std::vector<BasicBlock*> LoopBlocks;
   std::vector<PeelIteration*> Iterations;

   PeelAttempt(IntegrationHeuristicsPass* Pass, IntegrationAttempt* P, Function& _F, DenseMap<Function*, LoopInfo*>& _LI, TargetData* _TD, AliasAnalysis* _AA, 
	       DenseMap<Instruction*, const Loop*>& _invariantInsts, DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>& invariantEdges, DenseMap<BasicBlock*, const Loop*>& _invariantBlocks, const Loop* _L, int depth);
   ~PeelAttempt();

   SmallVector<std::pair<BasicBlock*, BasicBlock*>, 4> ExitEdges;

   PeelIteration* getIteration(unsigned iter);
   PeelIteration* getOrCreateIteration(unsigned iter);

   ValCtx getReplacement(Value* V, int frameIndex, int sourceIteration);

   MemDepResult tryForwardExprFromParent(LoadForwardAttempt&, int originIter);
   bool tryForwardExprFromIter(LoadForwardAttempt&, int originIter, MemDepResult& Result);

   void queueTryEvaluateVariant(Instruction* VI, const Loop* VILoop, Value* Used);

   bool tryForwardLoadThroughLoopFromBB(BasicBlock* BB, LoadForwardAttempt& LFA, BasicBlock*& PreheaderOut, SmallVectorImpl<NonLocalDepResult> &Result);

   void visitVariant(Instruction* VI, const Loop* VILoop, VisitorContext& Visitor);
   void queueAllLiveValuesMatching(UnaryPred& P);
   
   void revertDSEandDAE();
   void revertExternalUsers();
   void callExternalUsers(ProcessExternalCallback& PEC);
   void retryExternalUsers();
   void getRetryStoresAndAllocs(std::vector<llvm::ValCtx>&);
   void walkLoadsFromFoldedContexts(bool);
   
   void describeTreeAsDOT(std::string path);

   void collectStats();
   void printHeader(raw_ostream& OS) const;
   void printDebugHeader(raw_ostream& OS) const {
     printHeader(OS);
   }
   void print(raw_ostream& OS) const;

   std::string nestingIndent() const;

   std::string getShortHeader();
   IntegratorTag* getParentTag();
   bool hasChildren();
   unsigned getNumChildren();
   IntegratorTag* getChildTag(unsigned);
   bool isEnabled();
   void setEnabled(bool);

 };

class InlineAttempt : public IntegrationAttempt { 

  CallInst* CI;
  BasicBlock* UniqueReturnBlock;

 public:

  InlineAttempt(IntegrationHeuristicsPass* Pass, IntegrationAttempt* P, Function& F, DenseMap<Function*, LoopInfo*>& LI, TargetData* TD, AliasAnalysis* AA, CallInst* _CI, 
		DenseMap<Instruction*, const Loop*>& _invariantInsts, DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>& _invariantEdges, DenseMap<BasicBlock*, const Loop*>& _invariantBlocks, int depth);

  virtual ValCtx tryEvaluateResult(Value*);
  
  virtual Instruction* getEntryInstruction();
  virtual BasicBlock* getEntryBlock();
  virtual const Loop* getLoopContext();
  
  virtual MemDepResult tryForwardExprFromParent(LoadForwardAttempt&);
  bool tryForwardLoadFromExit(LoadForwardAttempt&, MemDepResult&);

  virtual void queueTryEvaluateOwnCall();
  
  ValCtx tryGetReturnValue();
  
  ValCtx getImprovedCallArgument(Argument* A);

  virtual bool checkLoopIterationOrExit(BasicBlock* PresentBlock, BasicBlock* NextBlock, ValCtx& Start);

  virtual InlineAttempt* getFunctionRoot();

  bool isOwnCallUnused();

  virtual bool walkHeaderPHIOperands(PHINode* PN, OpCallback& CB);
  virtual void walkOperands(Value* V, OpCallback& CB);
  virtual void queueAllLiveValuesMatching(UnaryPred& P);

  virtual bool getSpecialEdgeDescription(BasicBlock* FromBB, BasicBlock* ToBB, raw_ostream& Out);
  
  virtual void describe(raw_ostream& Stream) const;
  
  virtual void collectAllLoopStats();
  virtual void printHeader(raw_ostream&) const;

  virtual std::string getShortHeader();
  virtual IntegratorTag* getParentTag();

  virtual bool canDisable();
  virtual bool isEnabled();
  virtual void setEnabled(bool);

  virtual void deleteDeadBlocks();

};

class LoadForwardAttempt;

class LFAQueryable {

 public:

  virtual LoadInst* getOriginalInst() = 0;
  virtual IntegrationAttempt* getOriginalCtx() = 0;
  virtual LoadInst* getQueryInst() = 0;
  virtual LoadForwardAttempt& getLFA() = 0;

};

class LoadForwardAttempt : public LFAQueryable {

  LoadInst* LI;
  IntegrationAttempt* originalCtx;
  SmallVector<SymExpr*, 4> Expr;
  bool ExprValid;
  int64_t ExprOffset;

  DenseMap<std::pair<BasicBlock*, const Loop*>, MemDepResult> lastIterCache;
  DenseMap<const Loop*, MemDepResult> otherItersCache;

  ValCtx Result;
  uint64_t* partialBuf;
  bool* partialValidBuf;
  uint64_t partialBufBytes;
  bool mayBuildFromBytes;

  const Type* targetType;

  TargetData* TD;

  bool buildSymExpr(Value* Ptr);

 public:

  virtual LoadInst* getOriginalInst();
  virtual IntegrationAttempt* getOriginalCtx();
  virtual LoadInst* getQueryInst();
  virtual LoadForwardAttempt& getLFA();  

  void describeSymExpr(raw_ostream& Str);
  bool tryBuildSymExpr(Value* Ptr = 0);
  bool canBuildSymExpr(Value* Ptr = 0);
  int64_t getSymExprOffset();
  void setSymExprOffset(int64_t);

  SmallVector<SymExpr*, 4>* getSymExpr();

  ValCtx getBaseVC();
  IntegrationAttempt* getBaseContext();

  std::pair<DenseMap<std::pair<BasicBlock*, const Loop*>, MemDepResult>::iterator, bool> getLastIterCache(BasicBlock* FromBB, const Loop* L);
  std::pair<DenseMap<const Loop*, MemDepResult>::iterator, bool> getOtherItersCache(const Loop* L);

  unsigned char* getPartialBuf(uint64_t nbytes);
  bool* getBufValid();
  bool* tryGetBufValid();

  // This might not equal the type of the original load!
  // This happens when we're making proxy or sub-queries.
  const Type* getTargetTy();

  bool allowTotalDefnImplicitCast(const Type* From, const Type* To);
  bool addPartialVal(PartialVal&);
  bool isComplete();
  ValCtx getResult();

  Constant* extractAggregateMemberAt(Constant* From, uint64_t Offset, const Type* Target, uint64_t TargetSize);
  Constant* constFromBytes(unsigned char*, const Type*);
  uint64_t markPaddingBytes(bool*, const Type*);

  LoadForwardAttempt(LoadInst* _LI, IntegrationAttempt* C, TargetData*, const Type* T = 0);
  ~LoadForwardAttempt();

  void printDebugHeader(raw_ostream& Str) { 
    originalCtx->printDebugHeader(Str);
  }

};

class LFARealization : public LFAQueryable {

  LoadForwardAttempt& LFA;
  LoadInst* QueryInst;
  Instruction* FakeBase;
  Instruction* InsertPoint;
  SmallVector<Instruction*, 4> tempInstructions;
  
 public:

  virtual LoadInst* getOriginalInst();
  virtual IntegrationAttempt* getOriginalCtx();
  virtual LoadInst* getQueryInst();
  virtual LoadForwardAttempt& getLFA();

  LFARealization(LoadForwardAttempt& LFA, IntegrationAttempt* Ctx, Instruction* Insert);
  ~LFARealization();

  Instruction* getFakeBase();

  void printDebugHeader(raw_ostream& Str) { 
    LFA.getOriginalCtx()->printDebugHeader(Str);
  }

};

class LFARMapping {

  LFARealization& LFAR;
  IntegrationAttempt* Ctx;

 public:

  LFARMapping(LFARealization& LFAR, IntegrationAttempt* Ctx);
  ~LFARMapping();

};

enum IntegratorWQItemType {

   TryEval,
   CheckBlock,
   CheckLoad,
   OpenPush

};

// A cheesy hack to make a value type that acts like a dynamic dispatch hierarchy
class IntegratorWQItem {

  IntegrationAttempt* ctx;
  IntegratorWQItemType type;
  union {
    LoadInst* LI;
    Value* V;
    BasicBlock* BB;
    struct {
      CallInst* OpenI;
      ValCtx OpenProgress;
    } OpenArgs;
  } u;

public:

 IntegratorWQItem(IntegrationAttempt* c, LoadInst* L) : ctx(c), type(CheckLoad) { u.LI = L; }
 IntegratorWQItem(IntegrationAttempt* c, Value* V) : ctx(c), type(TryEval) { u.V = V; }
 IntegratorWQItem(IntegrationAttempt* c, BasicBlock* BB) : ctx(c), type(CheckBlock) { u.BB = BB; }
 IntegratorWQItem(IntegrationAttempt* c, CallInst* OpenI, ValCtx OpenProgress) : ctx(c), type(OpenPush) { u.OpenArgs.OpenI = OpenI; u.OpenArgs.OpenProgress = OpenProgress; }

  void execute();
  void describe(raw_ostream& s);

};

class IntegrationHeuristicsPass : public ModulePass {

   DenseMap<Function*, LoopInfo*> LIs;
   DenseMap<Function*, DenseMap<Instruction*, const Loop*> > invariantInstScopes;
   DenseMap<Function*, DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*> > invariantEdgeScopes;
   DenseMap<Function*, DenseMap<BasicBlock*, const Loop*> > invariantBlockScopes;

   DenseMap<Function*, BasicBlock*> uniqueReturnBlocks;

   TargetData* TD;
   AliasAnalysis* AA;

   SmallVector<IntegratorWQItem, 64> workQueue1;
   SmallVector<IntegratorWQItem, 64> workQueue2;

   SmallVector<IntegratorWQItem, 64>* produceQueue;

   SmallVector<ValCtx, 64> dieQueue1;
   SmallVector<ValCtx, 64> dieQueue2;

   SmallVector<ValCtx, 64>* produceDIEQueue;

   IntegrationAttempt* RootIA;

 public:

   static char ID;

   explicit IntegrationHeuristicsPass() : ModulePass(ID) { 

     produceDIEQueue = &dieQueue2;
     produceQueue = &workQueue2;

   }

   bool runOnModule(Module& M);

   void queueTryEvaluate(IntegrationAttempt* ctx, Value* val);
   void queueCheckBlock(IntegrationAttempt* ctx, BasicBlock* BB);
   void queueCheckLoad(IntegrationAttempt* ctx, LoadInst* LI);
   void queueOpenPush(ValCtx OpenInst, ValCtx OpenProgress);
   void queueDIE(IntegrationAttempt* ctx, Value* val);

   void print(raw_ostream &OS, const Module* M) const;

   void releaseMemory(void);

   void createInvariantScopes(Function*, DenseMap<Instruction*, const Loop*>*&, DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>*&, DenseMap<BasicBlock*, const Loop*>*&);
   DenseMap<Instruction*, const Loop*>& getInstScopes(Function* F);
   DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>& getEdgeScopes(Function* F);
   DenseMap<BasicBlock*, const Loop*>& getBlockScopes(Function* F);

   BasicBlock* getUniqueReturnBlock(Function* F);

   void runQueue();
   void runDIEQueue();

   void revertLoadsFromFoldedContexts();
   void retryLoadsFromFoldedContexts();

   virtual void getAnalysisUsage(AnalysisUsage &AU) const;

   IntegrationAttempt* getRoot() { return RootIA; }
   void commit() { RootIA->commit(); }

 };

 std::string ind(int i);
 const Loop* immediateChildLoop(const Loop* Parent, const Loop* Child);
 Constant* getConstReplacement(Value*, IntegrationAttempt*);
 Constant* intFromBytes(const uint64_t*, unsigned, unsigned, llvm::LLVMContext&);
 bool containsPointerTypes(const Type*);

} // Namespace LLVM

#endif
