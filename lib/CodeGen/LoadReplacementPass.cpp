#include "LoadReplacementPass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineBranchProbabilityInfo.h"
#include "llvm/CodeGen/TargetSchedule.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "llvm/IR/ModuleSlotTracker.h"

#include <cstdio>
#include <climits>
#include <vector>
#include <stack>
#include <queue>

using namespace llvm;

namespace llvm {
  class LoadRegister;
  class BasicBlockLoadUseInfo;

  typedef std::vector<BasicBlockLoadUseInfo*> TraversalPath;

  enum class RegState {
    Unused,
    Used,
    Overwritten
  };

  // This class is a wrapper around MachineInstr so that can hold risk
  // information.
  class InstrHolder {
  private:
    // Machine instruction corresponding to this holder
    MachineInstr* mi;

    // Risks of use (pre) or define (post)
    unsigned pre_risk;
    unsigned post_risk;

  public:
    // // Default Constructor
    // InstrHolder()
    //   : InstrHolder(nullptr, 0, 0)
    // {}

    // Construct and setup risks
    InstrHolder(MachineInstr* mi, unsigned pre_risk, unsigned post_risk)
      : mi(mi),
        pre_risk(pre_risk),
        post_risk(post_risk)
    {
      assert(mi && "Machine instruction cannot be null");
    }

    InstrHolder(const InstrHolder& mih, unsigned pre_risk, unsigned post_risk)
      : mi(mih.mi),
        pre_risk(pre_risk),
        post_risk(post_risk)
    {}

    // Default Constructor
    InstrHolder(const InstrHolder& other)
      : InstrHolder(other.mi, other.getPreRisk(), other.getPostRisk())
    {}

    // Default Constructor
    InstrHolder(const InstrHolder&& other)
      : InstrHolder(other.mi, other.getPreRisk(), other.getPostRisk())
    {}

    // Get the risk of instructions before this instruction in the basic block
    unsigned getPreRisk() const {
      return pre_risk;
    }

    // Get the risk of instructions after this instruction in the basic block
    unsigned getPostRisk() const {
      return post_risk;
    }

    // Get the corresponding machine instruction.
    MachineInstr* operator->() const {
      return mi;
    }

    InstrHolder& operator=(const InstrHolder& other) {
      mi = other.mi;
      pre_risk = other.getPreRisk();
      post_risk = other.getPostRisk();
      return *this;
    }
  };

  class LoadRegister {
    RegState path_state;

    const BasicBlockLoadUseInfo& parent;
    const MachineOperand& mo;
    InstrHolder instr;

    std::shared_ptr<std::vector<InstrHolder>> users;

    LoadRegister(const LoadRegister* other)
      : path_state(other->getPathState()),
        parent(other->parent),
        mo(other->mo),
        instr(other->instr),
        users(other->users)
    {}

    RegState getPathState() const {
      return path_state;
    }

  public:
    LoadRegister(const MachineOperand& mo, const BasicBlockLoadUseInfo& parent, InstrHolder instr)
      : path_state(RegState::Unused),
        parent(parent),
        mo(mo),
        instr(instr),
        users(new std::vector<InstrHolder>()) {
      assert(instr->mayLoad() && "LoadInstr must be loadable");
    }

    std::shared_ptr<LoadRegister> forkOnBranch() const {
      return std::shared_ptr<LoadRegister>(new LoadRegister(this));
    }

    const BasicBlockLoadUseInfo& getParent() const {
      return parent;
    }

    unsigned getReg() const {
      return mo.getReg();
    }

    InstrHolder getInstr() const {
      return instr;
    }

    bool isUsed() const {
      return !users->empty();
    }

    bool isUnusedOnPath() const {
      return getPathState() == RegState::Unused;
    }

    bool isUsedOnPath() const {
      return getPathState() == RegState::Used;
    }

    bool isOverwrittenOnPath() const {
      return getPathState() == RegState::Overwritten;
    }

    // Check if this Load instruction is used by another basic block
    //
    // returns true if a use appears in another basic block
    bool isUsedInOtherBB() const {
      for (auto user : *users) {
        if (instr->getParent() != user->getParent())
          return true;
      }
      return false;
    }

    void setUsedOnPath() {
      assert(getPathState() == RegState::Unused &&
             "Only state transitions from unused or uninitialized are allowed");
      path_state = RegState::Used;
    }

    void setOverwrittenOnPath() {
      assert(getPathState() == RegState::Unused &&
             "Only state transitions from unused to another state are allowed");
      path_state = RegState::Overwritten;
    }

    void addUse(InstrHolder user) {
      users->push_back(user);
    }

    // Insert machine instruction into user list if the register defined in the
    // load instruction appears in the use instruction.
    //
    // returns true if successfully added to the user list otherwise false.
    bool addUseCandidate(InstrHolder candidate) {
      for (unsigned i = 0, e = candidate->getNumOperands(); i != e; ++i) {
        MachineOperand& op = candidate->getOperand(i);

        // Check if we are using a register for the first time.
        // TODO: Check that this check for use is correct. (it probably isn't)
        if (op.isReg() && !op.isDef()) {
          unsigned op_reg = op.getReg();

          if (op_reg == getReg()) {
            assert(isUnusedOnPath() && "LoadRegister cannot have any users on this path");
            setUsedOnPath();
            users->push_back(candidate);
            return true;
          }
        }
      }

      return false;
    }

    unsigned shortestDistance();
    unsigned averageDistance();

    void print_instr(raw_ostream &os, ModuleSlotTracker &mst, unsigned indent) const {
      os.indent(indent);
      os << "(BB#" << instr->getParent()->getNumber() << ") ";
      instr->print(os, mst);
    }

    void print_users(raw_ostream &os, ModuleSlotTracker &mst, unsigned indent) const {
      for (auto user : *users) {
        os.indent(indent);
        os << "(BB#" << user->getParent()->getNumber() << " ";
        os.changeColor(raw_ostream::RED);
        os << "[Risk: " << user.getPreRisk() << ']';
        os.resetColor();
        os << ")";
        user->print(os, mst);
      }
    }

    void print(raw_ostream& os, ModuleSlotTracker& mst, const TargetRegisterInfo* tri, unsigned cur_indent) const {
      auto color = raw_ostream::GREEN;

      if (isUnusedOnPath() && isUsed()) {
        color = raw_ostream::CYAN;
      }
      else if (isUnusedOnPath()) {
        color = raw_ostream::BLUE;
      }
      else if (isOverwrittenOnPath()) {
        color = raw_ostream::RED;
      }

      os.indent(cur_indent);

      os << "Load on reg ";
      os.changeColor(color);
      os << "R" << getReg() << ": " << PrintReg(getReg(), tri, mo.getSubReg());
      os.resetColor();
      os << " in instruction (Post-Risk: " << instr.getPostRisk() << "): ";
      print_instr(os, mst, 0);

      print_users(os, mst, cur_indent + 2);
    }

    bool operator==(const LoadRegister& other) const {
      return users == other.users;
    }
  };

  typedef std::vector<std::shared_ptr<LoadRegister>> LoadRegPtrVec;

  class BasicBlockLoadUseInfo {
    MachineBasicBlock& mbb;

    unsigned total_risk;

    std::vector<RegState> first_reg_state_changes;
    std::vector<Optional<InstrHolder>> first_reg_users;
    LoadRegPtrVec load_regs;
    LoadRegPtrVec final_active_regs;

    void processFirstStateChanges(InstrHolder mi,
                                  const MachineOperand& mop,
                                  unsigned reg,
                                  RegState new_state) {
      assert(new_state != RegState::Unused);

      if (first_reg_state_changes[reg] == RegState::Unused) {
        assert(!first_reg_users[reg] && "User is not null");

        first_reg_state_changes[reg] = new_state;
        first_reg_users[reg] = Optional<InstrHolder>(mi);
      }
    }

    void processInstr(InstrHolder mi) {
      std::vector<const MachineOperand*> def_regs;

      for (unsigned i = 0, e = mi->getNumOperands(); i != e; ++i) {
        const MachineOperand& mop = mi->getOperand(i);
        if (mop.isReg()) {
          // Process register
          if (!mop.isDef())
            // Process the use of the register
            // TODO: Verify use
            processUse(mi, mop);
          else
            // Save defining registers for processing later
            def_regs.push_back(&mop);
        }
      }

      // Evaluate defining operands last
      bool may_load = mi->mayLoad();
      for (const MachineOperand* mop : def_regs) {
        if (may_load) {
          processLoad(mi, *mop);
        } else {
          processDefine(mi, *mop);
        }
      }
    }

    void processLoad(InstrHolder mi, const MachineOperand& mop) {
      assert(mop.isReg() && "MachineOperand must be a register for processing");
      assert(mi->mayLoad() && "MachineInstruction mi must be a load instruction");
      assert(mop.isDef() && "A load MachineOperand is expected to define a "
             "register");

      unsigned reg = mop.getReg();

      processFirstStateChanges(mi, mop, reg, RegState::Overwritten);

      auto lr = std::shared_ptr<LoadRegister>(new LoadRegister(mop, *this, mi));

      if (final_active_regs[reg] && final_active_regs[reg]->isUnusedOnPath()) {
        final_active_regs[reg]->setOverwrittenOnPath();
      }

      final_active_regs[reg] = lr;
      load_regs.push_back(lr);
    }

    void processDefine(InstrHolder mi, const MachineOperand& mop) {
      assert(mop.isReg() && "MachineOperand must be a register for processing");
      assert(!mi->mayLoad() && "MachineInstruction mi must not be a load instruction");
      assert(mop.isDef() && "A load MachineOperand is expected to define a "
             "register");

      unsigned reg = mop.getReg();

      // Evaluate first state change if applicable
      processFirstStateChanges(mi, mop, reg, RegState::Overwritten);

      if (final_active_regs[reg] && final_active_regs[reg]->isUnusedOnPath()) {
        final_active_regs[reg]->setOverwrittenOnPath();
        final_active_regs[reg] = nullptr;
      }
    }

    void processUse(InstrHolder mi, const MachineOperand& mop) {
      assert(mop.isReg() && "MachineOperand must be a register for processing");
      assert(!mop.isDef() && "This will overwrite nullifying legitimate uses");

      unsigned reg = mop.getReg();

      // Evaluate first state change if applicable
      processFirstStateChanges(mi, mop, reg, RegState::Used);

      if (final_active_regs[reg] && final_active_regs[reg]->isUnusedOnPath()) {
        InstrHolder used(mi, mi.getPreRisk() - final_active_regs[reg]->getInstr().getPreRisk(), mi.getPostRisk());
        final_active_regs[reg]->setUsedOnPath();
        final_active_regs[reg]->addUse(used);
        final_active_regs[reg] = nullptr;
      }
    }

  public:
    BasicBlockLoadUseInfo(MachineBasicBlock& mbb, const TargetInstrInfo* tii, unsigned num_regs)
      : mbb(mbb),
        total_risk(0),
        first_reg_state_changes(std::vector<RegState>(num_regs, RegState::Unused)),
        first_reg_users(std::vector<Optional<InstrHolder>>(num_regs)),
        final_active_regs(std::vector<std::shared_ptr<LoadRegister>>(num_regs, nullptr)) {
      TargetSchedModel tsm;
      const InstrItineraryData* iid = tsm.getInstrItineraries();

      unsigned accum_risk = 0;

      // First pass should measure the risk of the basic block
      for (MachineBasicBlock::iterator mii = mbb.begin(), mie = mbb.end(); mii != mie; ++mii) {
        MachineInstr *mi = &*mii;
        unsigned instr_risk = tii->getInstrLatency(iid, *mi, nullptr);

        total_risk += instr_risk;
      }

      // Second pass measures the distances from the begining and end of the
      // basic block
      for (MachineBasicBlock::iterator mii = mbb.begin(), mie = mbb.end(); mii != mie; ++mii) {
        MachineInstr *mi = &*mii;
        unsigned instr_risk = tii->getInstrLatency(iid, *mi, nullptr);
        InstrHolder mih(mi, accum_risk, total_risk - accum_risk);

        accum_risk += instr_risk;
        processInstr(mih);
      }
    }

    unsigned getRisk() const {
      return total_risk;
    }

    MachineBasicBlock& getMachineBasicBlock() {
      return mbb;
    }

    const MachineBasicBlock& getMachineBasicBlock() const {
      return mbb;
    }

    LoadRegPtrVec getFinalActiveRegs() const {
      return final_active_regs;
    }

    LoadRegPtrVec getLoadRegs() const {
      return load_regs;
    }

    // Accumulate information about this basic block
    //
    // TODO: This will probably need to be changed for use with our new
    // algorithm
    LoadRegPtrVec traverse(const LoadRegPtrVec& prev_unused,
                           LoadRegPtrVec& accumulator,
                           unsigned& path_risk) const {
      LoadRegPtrVec new_unused(prev_unused.size(), nullptr);

      // Match unused loads in prev_unused with first_reg_state_changes
      for (auto lr : prev_unused) {
        if (lr) {
          auto lrf = lr->forkOnBranch();
          unsigned mreg = lrf->getReg();

          if (first_reg_state_changes[mreg] == RegState::Used) {
            assert(first_reg_users[mreg] &&
                   "The vectors first_reg_state_changes and first_reg_users must "
                   "mirror eachother");

            unsigned post_load = lr->getInstr().getPostRisk();
            unsigned pre_use = first_reg_users[mreg]->getPreRisk();
            unsigned post_use = first_reg_users[mreg]->getPostRisk();
            InstrHolder used_instr(*first_reg_users[mreg], post_load + path_risk + pre_use, post_use);
            lrf->addUse(used_instr);
            lrf->setUsedOnPath();
          }
          else if (first_reg_state_changes[mreg] == RegState::Overwritten) {
            assert(first_reg_users[mreg] &&
                   "The vectors first_reg_state_changes and first_reg_users must "
                   "mirror eachother");

            lrf->setOverwrittenOnPath();
          }
          else {
            // The register is still unused. Pass it through to new_unused.
            new_unused[mreg] = lrf;
          }
        }
      }

      return new_unused;
    }

    void print(raw_ostream &os, ModuleSlotTracker &mst, unsigned cur_indent) const {
      os.changeColor(raw_ostream::CYAN);
      os << "BB#" << mbb.getNumber() << ": " << mbb.getFullName() << "(Risk: " << total_risk << ")" << "{\n";
      os.resetColor();

      os.changeColor(raw_ostream::MAGENTA);
      mbb.print(os, mst);
      os.resetColor();

      const TargetRegisterInfo* tri = mbb.getParent()->getSubtarget().getRegisterInfo();
      for (auto reg : load_regs)
        reg->print(os, mst, tri, cur_indent + 2);

      os.changeColor(raw_ostream::CYAN);
      os << "}\n";
      os.resetColor();
    }

    bool operator==(const BasicBlockLoadUseInfo& other) const {
      return getMachineBasicBlock().getNumber() == other.getMachineBasicBlock().getNumber();
    }
  };

  bool hasVisited(BasicBlockLoadUseInfo& node, TraversalPath& path) {
    for (auto p : path) {
      if (node == *p) {
        return true;
      }
    }

    return false;
  }

  bool isAllUsed(const LoadRegPtrVec& vec) {
    for (auto e : vec) {
      if (e) {
        return false;
      }
    }

    return true;
  }

  void printLoadRegPtrVec(const LoadRegPtrVec& vec, ModuleSlotTracker& mst, const TargetRegisterInfo* tri) {
    for (auto e : vec) {
      if (e) {
        e->print(outs(), mst, tri, 0);
      }
    }
  }

  class FunctionLoadInfo {
    const MachineFunction& mf;

    std::vector<BasicBlockLoadUseInfo> bb_list;
    LoadRegPtrVec regs;
    LoadRegPtrVec active_regs;

  public:
    // Create FunctionLoadInfo and walk function CFG
    FunctionLoadInfo(MachineFunction& mf)
      : mf(mf),
        regs(),
        active_regs(std::vector<std::shared_ptr<LoadRegister>>(mf.getSubtarget().getRegisterInfo()->getNumRegs(), nullptr)) {
      // Get the register count
      unsigned num_regs = mf.getSubtarget().getRegisterInfo()->getNumRegs();
      // Get the number of basic blocks
      unsigned num_blocks = mf.getNumBlockIDs();

      const TargetInstrInfo* tii = mf.getSubtarget().getInstrInfo();
      ModuleSlotTracker mst(mf.getFunction()->getParent());
      mst.incorporateFunction(*mf.getFunction());

      // Create the basic block usage analysis list
      for (unsigned id = 0; id < num_blocks; ++id) {
        BasicBlockLoadUseInfo bb_new(*mf.getBlockNumbered(id), tii, num_regs);
        bb_list.push_back(bb_new);
        bb_new.print(outs(), mst, 0);
      }

      // Walk over all paths for all basic blocks
      for (auto bb : bb_list) {
        TraversalPath visited;
        LoadRegPtrVec accumulator;

        // Do the walk of this basic block
        startWalkFunction(bb, visited, accumulator);
        printLoadRegPtrVec(accumulator, mst, mf.getSubtarget().getRegisterInfo());
      }
    }

    void startWalkFunction(BasicBlockLoadUseInfo& cur,
                           TraversalPath visited_list,
                           LoadRegPtrVec& accumulator) {
      for (auto lr : cur.getLoadRegs()) {
        if (lr)
          accumulator.push_back(lr);
      }

      LoadRegPtrVec cur_active_regs = cur.getFinalActiveRegs();

      if (isAllUsed(cur_active_regs)) {
        errs() << "Skipping BB#" << cur.getMachineBasicBlock().getNumber() << ": No active registers\n";
        return;
      }

      visited_list.push_back(&cur);

      for (MachineBasicBlock::const_succ_iterator si = cur.getMachineBasicBlock().succ_begin(), e = cur.getMachineBasicBlock().succ_end(); si != e; ++si) {
        BasicBlockLoadUseInfo& next = bb_list[(*si)->getNumber()];
        walkFunction(next, visited_list, accumulator, cur_active_regs, 0);
      }
    }

    // Perform a depth first traversal of the function basic blocks taking note
    // of loads and usages
    void walkFunction(BasicBlockLoadUseInfo& cur,
                      TraversalPath visited_list,
                      LoadRegPtrVec& accumulator,
                      LoadRegPtrVec cur_active_regs,
                      unsigned risk) {
      LoadRegPtrVec next_active_regs;
      bool hv = hasVisited(cur, visited_list);
      bool au = isAllUsed(cur_active_regs);

      if ((!hv || cur.getMachineBasicBlock().getNumber() == visited_list[0]->getMachineBasicBlock().getNumber()) && !au) {
        next_active_regs = cur.traverse(cur_active_regs, accumulator, risk);
      }

      visited_list.push_back(&cur);
      risk += cur.getRisk();

      for (auto bb : visited_list) {
        errs() << bb->getMachineBasicBlock().getNumber() << " ";
      }
      errs() << "(" << hv << au << ")" << "\n";

      if (hv || au) {
        return;
      }

      for (MachineBasicBlock::const_succ_iterator si = cur.getMachineBasicBlock().succ_begin(), e = cur.getMachineBasicBlock().succ_end(); si != e; ++si) {
        BasicBlockLoadUseInfo& next = bb_list[(*si)->getNumber()];
        walkFunction(next, visited_list, accumulator, next_active_regs, risk);
      }
    }

    //     // We may want to explore probabilistic branching for determining distance
    //     // later. For now we just print the successors and their branching
    //     // probabilities.
    //     if (!mbb.succ_empty()){
    //       MachineBranchProbabilityInfo mbpi;

    //       for (MachineBasicBlock::const_succ_iterator si = mbb.succ_begin(), e = mbb.succ_end(); si != e; ++si) {
    //         if (mbb.hasSuccessorProbabilities()){
    //           //*os << '(' << MBPI.getEdgeProbability(&MBB, SI) << ')';
    //         }
    //       }
    //     }
    //   }
  };
}

char LoadReplacementPass::ID;

LoadReplacementPass::LoadReplacementPass() : MachineFunctionPass(ID) {
}

StringRef LoadReplacementPass::getPassName() const {
  return "Load Replacement";
}

void LoadReplacementPass::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
}

// Entry function into LoadReplacementPass
bool LoadReplacementPass::runOnMachineFunction(MachineFunction& mf) {
  mf.viewCFGOnly();

  FunctionLoadInfo fli(mf);
  return false;
}
