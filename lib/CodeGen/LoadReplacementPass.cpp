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

  enum RegState {
    Unused,
    Used,
    Overwritten
  };

  class LoadRegister {
    RegState path_state;
    unsigned bb_cost;

    const BasicBlockLoadUseInfo& parent;
    const MachineOperand& mo;
    MachineInstr* instr;

    std::shared_ptr<std::vector<MachineInstr*>> users;

    LoadRegister(const LoadRegister* other)
      : path_state(other->getPathState()),
        bb_cost(other->bb_cost),
        parent(other->parent),
        mo(other->mo),
        instr(other->getInstr()),
        users(other->users) {}

    RegState getPathState() const {
      return path_state;
    }

  public:
    LoadRegister(const MachineOperand& mo, const BasicBlockLoadUseInfo& parent, MachineInstr* instr, unsigned cost)
      : path_state(Unused),
        bb_cost(cost),
        parent(parent),
        mo(mo),
        instr(instr),
        users(new std::vector<MachineInstr*>()) {
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

    MachineInstr* getInstr() const {
      return instr;
    }

    bool isUsed() const {
      return users->empty();
    }

    bool isUnusedOnPath() const {
      return getPathState() == Unused;
    }

    bool isUsedOnPath() const {
      return getPathState() == Used;
    }

    bool isOverwrittenOnPath() const {
      return getPathState() == Overwritten;
    }

    void setUsedOnPath() {
      assert(getPathState() == Unused &&
             "Only state transitions from unused or uninitialized are allowed");
      path_state = Used;
    }

    void setOverwrittenOnPath() {
      assert(getPathState() == Unused &&
             "Only state transitions from unused to another state are allowed");
      path_state = Overwritten;
    }

    bool usedInOtherBB() const {
      for (auto user : *users) {
        if (instr->getParent() != user->getParent())
          return false;
      }
      return true;
    }

    void addUse(MachineInstr* user) {
      users->push_back(user);
    }

    // Insert machine instruction into user list if the register defined in the
    // load instruction appears in the use instruction.
    //
    // returns true if successfully added to the user list otherwise false.
    bool addUseCandidate(MachineInstr* candidate) {
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
        os << "(BB#" << user->getParent()->getNumber() << ") ";
        user->print(os, mst);
      }
    }

    void print(raw_ostream& os, ModuleSlotTracker& mst, const TargetRegisterInfo* tri, unsigned cur_indent) const {
      auto color = raw_ostream::GREEN;

      if (isUnusedOnPath()) {
        color = raw_ostream::YELLOW;
      }
      else if (isOverwrittenOnPath()) {
        color = raw_ostream::RED;
      }

      os.indent(cur_indent);

      os << "Load on reg ";
      os.changeColor(color);
      os << "R" << getReg() << ": " << PrintReg(getReg(), tri, mo.getSubReg());
      os.resetColor();
      os << " in instruction (Cost: " << bb_cost << "): ";
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

    unsigned total_cost;

    std::vector<RegState> first_reg_state_changes;
    std::vector<MachineInstr*> first_reg_users;
    LoadRegPtrVec load_regs;
    LoadRegPtrVec final_active_regs;

    void processFirstStateChanges(MachineInstr* mi,
                                  const MachineOperand& mop,
                                  unsigned reg,
                                  RegState new_state) {
      assert(new_state != Unused);

      if (first_reg_state_changes[reg] == Unused) {
        assert(!first_reg_users[reg] && "User is not null");

        first_reg_state_changes[reg] = new_state;
        first_reg_users[reg] = mi;
      }
    }

    void processInstr(MachineInstr* mi) {
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

    void processLoad(MachineInstr* mi, const MachineOperand& mop) {
      assert(mop.isReg() && "MachineOperand must be a register for processing");
      assert(mi->mayLoad() && "MachineInstruction mi must be a load instruction");
      assert(mop.isDef() && "A load MachineOperand is expected to define a "
             "register");

      unsigned reg = mop.getReg();

      processFirstStateChanges(mi, mop, reg, Overwritten);

      auto lr = std::shared_ptr<LoadRegister>(new LoadRegister(mop, *this, mi, getCost()));

      if (final_active_regs[reg] && final_active_regs[reg]->isUnusedOnPath()) {
        final_active_regs[reg]->setOverwrittenOnPath();
      }

      final_active_regs[reg] = lr;
      load_regs.push_back(lr);
    }

    void processDefine(MachineInstr* mi, const MachineOperand& mop) {
      assert(mop.isReg() && "MachineOperand must be a register for processing");
      assert(!mi->mayLoad() && "MachineInstruction mi must not be a load instruction");
      assert(mop.isDef() && "A load MachineOperand is expected to define a "
             "register");

      unsigned reg = mop.getReg();

      // Evaluate first state change if applicable
      processFirstStateChanges(mi, mop, reg, Overwritten);

      if (final_active_regs[reg] && final_active_regs[reg]->isUnusedOnPath()) {
        final_active_regs[reg]->setOverwrittenOnPath();
        final_active_regs[reg] = nullptr;
      }
    }

    void processUse(MachineInstr* mi, const MachineOperand& mop) {
      assert(mop.isReg() && "MachineOperand must be a register for processing");
      assert(!mop.isDef() && "This will overwrite nullifying legitimate uses");

      unsigned reg = mop.getReg();

      // Evaluate first state change if applicable
      processFirstStateChanges(mi, mop, reg, Used);

      if (final_active_regs[reg] && final_active_regs[reg]->isUnusedOnPath()) {
        final_active_regs[reg]->setUsedOnPath();
        final_active_regs[reg]->addUse(mi);
        final_active_regs[reg] = nullptr;
      }
    }

  public:
    BasicBlockLoadUseInfo(MachineBasicBlock& mbb, const TargetInstrInfo* tii, unsigned num_regs)
      : mbb(mbb),
        total_cost(0),
        first_reg_state_changes(std::vector<RegState>(num_regs, Unused)),
        first_reg_users(std::vector<MachineInstr*>(num_regs, nullptr)),
        final_active_regs(std::vector<std::shared_ptr<LoadRegister>>(num_regs, nullptr)) {
      TargetSchedModel tsm;
      const InstrItineraryData* iid = tsm.getInstrItineraries();

      for (MachineBasicBlock::iterator mii = mbb.begin(), mie = mbb.end(); mii != mie; ) {
        MachineInstr *mi = &*mii;
        unsigned instr_cost = tii->getInstrLatency(iid, *mi, nullptr);
        ++mii;

        processInstr(mi);
        total_cost += instr_cost;
      }
    }

    unsigned getCost() const {
      return total_cost;
    }

    MachineBasicBlock& getMachineBasicBlock() {
      return mbb;
    }


    const MachineBasicBlock& getMachineBasicBlock() const {
      return mbb;
    }

    LoadRegPtrVec traverse(const LoadRegPtrVec& prev_unused, LoadRegPtrVec& accumulator, bool& accumulator_changed) const {
      LoadRegPtrVec new_unused(prev_unused.size(), nullptr);

      // Match unused loads in prev_unused with first_reg_state_changes
      for (auto lr : prev_unused) {
        if (lr) {
          auto lrf = lr->forkOnBranch();
          unsigned mreg = lrf->getReg();

          if (first_reg_state_changes[mreg] == Used) {
            assert(first_reg_users[mreg] &&
                   "The vectors first_reg_state_changes and first_reg_users must "
                   "mirror eachother");

            lrf->addUse(first_reg_users[mreg]);
            lrf->setUsedOnPath();
          }
          else if (first_reg_state_changes[mreg] == Overwritten) {
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

      // Pass final_active_regs through to new_unused
      for (auto lr : final_active_regs) {
        if (lr) {
          unsigned mreg = lr->getReg();
          assert(!new_unused[mreg] && "Should be nullptr");
          new_unused[mreg] = lr;
        }
      }

      // Pass load_regs into accumulator if unique
      for (auto lr : load_regs) {
        assert(lr && "Should not be nullptr");
        bool unique = true;

        for (auto aclr : accumulator) {
          if (*aclr == *lr) {
            assert(aclr && "Should not be nullptr");
            unique = false;
            //break;
          }
        }

        //if (unique)
          accumulator.push_back(lr);
      }

      return new_unused;
    }

    void print(raw_ostream &os, ModuleSlotTracker &mst, unsigned cur_indent) const {
      os.changeColor(raw_ostream::CYAN);
      os << "BB#" << mbb.getNumber() << ": " << mbb.getFullName() << "(Cost: " << total_cost << ")" << "{\n";
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

  typedef std::vector<BasicBlockLoadUseInfo*> TraversalPath;


  bool hasVisited(BasicBlockLoadUseInfo& node, TraversalPath& path) {
    for (auto p : path) {
      if (node == *p) {
        return true;
      }
    }

    return false;
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
    FunctionLoadInfo(MachineFunction& mf)
      : mf(mf),
        regs(),
        active_regs(std::vector<std::shared_ptr<LoadRegister>>(mf.getSubtarget().getRegisterInfo()->getNumRegs(), nullptr)) {
      const TargetInstrInfo* tii = mf.getSubtarget().getInstrInfo();

      unsigned num_regs = mf.getSubtarget().getRegisterInfo()->getNumRegs();
      unsigned num_blocks = mf.getNumBlockIDs();

      ModuleSlotTracker mst(mf.getFunction()->getParent());

      mst.incorporateFunction(*mf.getFunction());

      // Create the basic block list
      for (unsigned id = 0; id < num_blocks; ++id) {
        BasicBlockLoadUseInfo bb_new(*mf.getBlockNumbered(id), tii, num_regs);
        bb_list.push_back(bb_new);
        bb_new.print(outs(), mst, 0);
        //bb_list.emplace_back(mf.getBlockNumbered(id), tii, num_regs);
      }

      BasicBlockLoadUseInfo& cur_mbb = bb_list[mf.front().getNumber()];
      TraversalPath visited;
      LoadRegPtrVec accumulator;
      LoadRegPtrVec cur_active_regs(num_regs, nullptr);

      walkFunction(cur_mbb, visited, accumulator, cur_active_regs);
      printLoadRegPtrVec(accumulator, mst, mf.getSubtarget().getRegisterInfo());
    }

    void walkFunction(BasicBlockLoadUseInfo& cur,
                      TraversalPath visited_list,
                      LoadRegPtrVec& accumulator,
                      LoadRegPtrVec cur_active_regs) {
      bool acc_changed;
      LoadRegPtrVec next_active_regs = cur.traverse(cur_active_regs, accumulator, acc_changed);

      if (hasVisited(cur, visited_list))
        return;

      visited_list.push_back(&cur);

      for (MachineBasicBlock::const_succ_iterator si = cur.getMachineBasicBlock().succ_begin(), e = cur.getMachineBasicBlock().succ_end(); si != e; ++si) {
        BasicBlockLoadUseInfo& next = bb_list[(*si)->getNumber()];
        walkFunction(next, visited_list, accumulator, next_active_regs);
      }
    }

      ///// Instruction cost
      // TargetSchedModel tsm;
      // const InstrItineraryData* iid = tsm.getInstrItineraries();
      // const TargetInstrInfo* tii = mf.getSubtarget().getInstrInfo();
      // tii->getOperandLatency(iid, mi1, ??, mi2, ??)
      //
      // tii->getInstrLatency(iid, mi, nullptr?)

      ///////////////////////////////
      // unsigned num_regs = mf.getSubtarget().getRegisterInfo()->getNumRegs();
      // const TargetInstrInfo* tii = mf.getSubtarget().getInstrInfo();
      // ModuleSlotTracker mst(mf.getFunction()->getParent());

      // mst.incorporateFunction(*mf.getFunction());

      // for (MachineBasicBlock& mbb : mf) {
      //   auto bblui = BasicBlockLoadUseInfo(mbb, tii, num_regs);
      //   raw_fd_ostream os(1, false);
      //   bblui.print(os, mst, 0);
      // }

      ///////////////////////////////

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

    // bool extractLoadRegisters(MachineInstr* candidate) {
    //   bool accepted = false;

    //   if (candidate->mayLoad()) {
    //     // Iterate over operands looking for all register operands marked as
    //     // "defined"
    //     for (unsigned i = 0, e = candidate->getNumOperands(); i != e; ++i) {
    //       MachineOperand& op = candidate->getOperand(i);

    //       if (op.isReg() && op.isDef()) {
    //         unsigned reg = op.getReg();

    //         if (active_regs[reg] && active_regs[reg]->isUnusedOnPath())
    //           active_regs[reg]->setOverwrittenOnPath();

    //         auto lr = std::shared_ptr<LoadRegister>(new LoadRegister(op, *this, candidate, total_cost));
    //         active_regs[reg] = lr;
    //         regs.push_back(lr);
    //         accepted = true;
    //       }
    //     }
    //   }

    //   return accepted;
    // }
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

bool LoadReplacementPass::runOnMachineFunction(MachineFunction& mf) {
  auto os = llvm::make_unique<raw_fd_ostream>(1, false);

  ModuleSlotTracker MST(mf.getFunction()->getParent());
  MST.incorporateFunction(*mf.getFunction());

  FunctionLoadInfo fli(mf);
  return false;
  // for (MachineBasicBlock &MBB : mf) {
  //   // Original basic block printer
  //   //MBB.print(*os, MST, nullptr);

  //   os->indent(0*indent);

  //   // Print basic block information
  //   *os << "BB#" << MBB.getNumber() << ": " << MBB.getFullName();

  //   // Print predecessor information
  //   if (!MBB.pred_empty()){
  //     *os << " PREDESSORS:";
  //     for (MachineBasicBlock::const_pred_iterator PI = MBB.pred_begin(), E = MBB.pred_end(); PI != E; ++PI) {
  //       *os << " BB#" << (*PI)->getNumber();
  //     }
  //   }
  //   *os << " {\n";

  //   // Iterate over instructions
  //   for (MachineBasicBlock::iterator MII = MBB.begin(), MIE = MBB.end(); MII != MIE; ) {
  //     MachineInstr *MI = &*MII;
  //     ++MII;

  //     os->indent(1*indent);

  //     // Print machine instruction
  //     MI->print(*os, MST);

  //     if (MI->mayLoad()){
  //       // This instruction probably loads from memory. It most likely loads
  //       // into any register that is marked as "defined" in this instruction.
  //       // This means we can track that register and watch for it being used in
  //       // the control flow of the instructions and basic blocks in a function.

  //       // Iterate over operands looking for all register operands marked as
  //       // "defined"
  //       for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
  //         MachineOperand& MIOP = MI->getOperand(i);
  //         if (MIOP.isReg() && MIOP.isDef()) {
  //           // TODO: Track this register.
  //           os->indent(2*indent);
  //           *os << "DEF:";
  //           MI->getOperand(i).print(*os, MST, mf.getSubtarget().getRegisterInfo(), mf.getTarget().getIntrinsicInfo());
  //           *os << "\n";
  //         }
  //       }
  //     }
  //     else {
  //       // This is not a load instruction but it may be a use. Iterate through
  //       // the operands, looking for register usages that have not occured since
  //       // the last load instruction on that register.
  //       for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
  //         MachineOperand& MIOP = MI->getOperand(i);

  //         // Check if we are using a register for the first time.
  //         // TODO: Check that this check for use is sufficient.
  //         if (MIOP.isReg() && MIOP.isImplicit() && !MIOP.isDef()) {
  //           // TODO: Mark register as used
  //           os->indent(2*indent);
  //           *os << "USE: ";
  //           MI->getOperand(i).print(*os, MST, mf.getSubtarget().getRegisterInfo(), mf.getTarget().getIntrinsicInfo());
  //           *os << "\n";
  //         }
  //       }
  //     }
  //   }

  //   *os << "}";

  //   // We may want to explore probabilistic branching for determining distance
  //   // later. For now we just print the successors and their branching
  //   // probabilities.
  //   if (!MBB.succ_empty()){
  //     MachineBranchProbabilityInfo MBPI;

  //     *os << " SUCCESSORS:";
  //     for (MachineBasicBlock::const_succ_iterator SI = MBB.succ_begin(), E = MBB.succ_end(); SI != E; ++SI) {
  //       *os << " BB#" << (*SI)->getNumber();
  //       if (MBB.hasSuccessorProbabilities()){
  //         *os << '(' << MBPI.getEdgeProbability(&MBB, SI) << ')';
  //       }
  //     }
  //   }
  //   *os << "\n\n";
  // }

  // return false;
}
