//===-- PPCISelDAGToDAG.cpp - PPC --pattern matching inst selector --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines a pattern matching instruction selector for PowerPC,
// converting from a legalized dag to a PPC dag.
//
//===----------------------------------------------------------------------===//

#include "PPC.h"
#include "MCTargetDesc/PPCPredicates.h"
#include "PPCMachineFunctionInfo.h"
#include "PPCTargetMachine.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetOptions.h"
using namespace llvm;

#define DEBUG_TYPE "ppc-codegen"

// FIXME: Remove this once the bug has been fixed!
cl::opt<bool> ANDIGlueBug("expose-ppc-andi-glue-bug",
cl::desc("expose the ANDI glue bug on PPC"), cl::Hidden);

cl::opt<bool> UseBitPermRewriter("ppc-use-bit-perm-rewriter", cl::init(true),
  cl::desc("use aggressive ppc isel for bit permutations"), cl::Hidden);
cl::opt<bool> BPermRewriterNoMasking("ppc-bit-perm-rewriter-stress-rotates",
  cl::desc("stress rotate selection in aggressive ppc isel for "
           "bit permutations"), cl::Hidden);

namespace llvm {
  void initializePPCDAGToDAGISelPass(PassRegistry&);
}

namespace {
  //===--------------------------------------------------------------------===//
  /// PPCDAGToDAGISel - PPC specific code to select PPC machine
  /// instructions for SelectionDAG operations.
  ///
  class PPCDAGToDAGISel : public SelectionDAGISel {
    const PPCTargetMachine &TM;
    const PPCTargetLowering *PPCLowering;
    const PPCSubtarget *PPCSubTarget;
    unsigned GlobalBaseReg;
  public:
    explicit PPCDAGToDAGISel(PPCTargetMachine &tm)
        : SelectionDAGISel(tm), TM(tm),
          PPCLowering(TM.getSubtargetImpl()->getTargetLowering()),
          PPCSubTarget(TM.getSubtargetImpl()) {
      initializePPCDAGToDAGISelPass(*PassRegistry::getPassRegistry());
    }

    bool runOnMachineFunction(MachineFunction &MF) override {
      // Make sure we re-emit a set of the global base reg if necessary
      GlobalBaseReg = 0;
      PPCLowering = TM.getSubtargetImpl()->getTargetLowering();
      PPCSubTarget = TM.getSubtargetImpl();
      SelectionDAGISel::runOnMachineFunction(MF);

      if (!PPCSubTarget->isSVR4ABI())
        InsertVRSaveCode(MF);

      return true;
    }

    void PreprocessISelDAG() override;
    void PostprocessISelDAG() override;

    /// getI32Imm - Return a target constant with the specified value, of type
    /// i32.
    inline SDValue getI32Imm(unsigned Imm) {
      return CurDAG->getTargetConstant(Imm, MVT::i32);
    }

    /// getI64Imm - Return a target constant with the specified value, of type
    /// i64.
    inline SDValue getI64Imm(uint64_t Imm) {
      return CurDAG->getTargetConstant(Imm, MVT::i64);
    }

    /// getSmallIPtrImm - Return a target constant of pointer type.
    inline SDValue getSmallIPtrImm(unsigned Imm) {
      return CurDAG->getTargetConstant(Imm, PPCLowering->getPointerTy());
    }

    /// isRunOfOnes - Returns true iff Val consists of one contiguous run of 1s
    /// with any number of 0s on either side.  The 1s are allowed to wrap from
    /// LSB to MSB, so 0x000FFF0, 0x0000FFFF, and 0xFF0000FF are all runs.
    /// 0x0F0F0000 is not, since all 1s are not contiguous.
    static bool isRunOfOnes(unsigned Val, unsigned &MB, unsigned &ME);


    /// isRotateAndMask - Returns true if Mask and Shift can be folded into a
    /// rotate and mask opcode and mask operation.
    static bool isRotateAndMask(SDNode *N, unsigned Mask, bool isShiftMask,
                                unsigned &SH, unsigned &MB, unsigned &ME);

    /// getGlobalBaseReg - insert code into the entry mbb to materialize the PIC
    /// base register.  Return the virtual register that holds this value.
    SDNode *getGlobalBaseReg();

    SDNode *getFrameIndex(SDNode *SN, SDNode *N, unsigned Offset = 0);

    // Select - Convert the specified operand from a target-independent to a
    // target-specific node if it hasn't already been changed.
    SDNode *Select(SDNode *N) override;

    SDNode *SelectBitfieldInsert(SDNode *N);
    SDNode *SelectBitPermutation(SDNode *N);

    /// SelectCC - Select a comparison of the specified values with the
    /// specified condition code, returning the CR# of the expression.
    SDValue SelectCC(SDValue LHS, SDValue RHS, ISD::CondCode CC, SDLoc dl);

    /// SelectAddrImm - Returns true if the address N can be represented by
    /// a base register plus a signed 16-bit displacement [r+imm].
    bool SelectAddrImm(SDValue N, SDValue &Disp,
                       SDValue &Base) {
      return PPCLowering->SelectAddressRegImm(N, Disp, Base, *CurDAG, false);
    }

    /// SelectAddrImmOffs - Return true if the operand is valid for a preinc
    /// immediate field.  Note that the operand at this point is already the
    /// result of a prior SelectAddressRegImm call.
    bool SelectAddrImmOffs(SDValue N, SDValue &Out) const {
      if (N.getOpcode() == ISD::TargetConstant ||
          N.getOpcode() == ISD::TargetGlobalAddress) {
        Out = N;
        return true;
      }

      return false;
    }

    /// SelectAddrIdx - Given the specified addressed, check to see if it can be
    /// represented as an indexed [r+r] operation.  Returns false if it can
    /// be represented by [r+imm], which are preferred.
    bool SelectAddrIdx(SDValue N, SDValue &Base, SDValue &Index) {
      return PPCLowering->SelectAddressRegReg(N, Base, Index, *CurDAG);
    }

    /// SelectAddrIdxOnly - Given the specified addressed, force it to be
    /// represented as an indexed [r+r] operation.
    bool SelectAddrIdxOnly(SDValue N, SDValue &Base, SDValue &Index) {
      return PPCLowering->SelectAddressRegRegOnly(N, Base, Index, *CurDAG);
    }

    /// SelectAddrImmX4 - Returns true if the address N can be represented by
    /// a base register plus a signed 16-bit displacement that is a multiple of 4.
    /// Suitable for use by STD and friends.
    bool SelectAddrImmX4(SDValue N, SDValue &Disp, SDValue &Base) {
      return PPCLowering->SelectAddressRegImm(N, Disp, Base, *CurDAG, true);
    }

    // Select an address into a single register.
    bool SelectAddr(SDValue N, SDValue &Base) {
      Base = N;
      return true;
    }

    /// SelectInlineAsmMemoryOperand - Implement addressing mode selection for
    /// inline asm expressions.  It is always correct to compute the value into
    /// a register.  The case of adding a (possibly relocatable) constant to a
    /// register can be improved, but it is wrong to substitute Reg+Reg for
    /// Reg in an asm, because the load or store opcode would have to change.
    bool SelectInlineAsmMemoryOperand(const SDValue &Op,
                                      char ConstraintCode,
                                      std::vector<SDValue> &OutOps) override {
      // We need to make sure that this one operand does not end up in r0
      // (because we might end up lowering this as 0(%op)).
      const TargetRegisterInfo *TRI = TM.getSubtargetImpl()->getRegisterInfo();
      const TargetRegisterClass *TRC = TRI->getPointerRegClass(*MF, /*Kind=*/1);
      SDValue RC = CurDAG->getTargetConstant(TRC->getID(), MVT::i32);
      SDValue NewOp =
        SDValue(CurDAG->getMachineNode(TargetOpcode::COPY_TO_REGCLASS,
                                       SDLoc(Op), Op.getValueType(),
                                       Op, RC), 0);

      OutOps.push_back(NewOp);
      return false;
    }

    void InsertVRSaveCode(MachineFunction &MF);

    const char *getPassName() const override {
      return "PowerPC DAG->DAG Pattern Instruction Selection";
    }

// Include the pieces autogenerated from the target description.
#include "PPCGenDAGISel.inc"

private:
    SDNode *SelectSETCC(SDNode *N);

    void PeepholePPC64();
    void PeepholePPC64ZExt();
    void PeepholeCROps();

    SDValue combineToCMPB(SDNode *N);

    bool AllUsersSelectZero(SDNode *N);
    void SwapAllSelectUsers(SDNode *N);
  };
}

/// InsertVRSaveCode - Once the entire function has been instruction selected,
/// all virtual registers are created and all machine instructions are built,
/// check to see if we need to save/restore VRSAVE.  If so, do it.
void PPCDAGToDAGISel::InsertVRSaveCode(MachineFunction &Fn) {
  // Check to see if this function uses vector registers, which means we have to
  // save and restore the VRSAVE register and update it with the regs we use.
  //
  // In this case, there will be virtual registers of vector type created
  // by the scheduler.  Detect them now.
  bool HasVectorVReg = false;
  for (unsigned i = 0, e = RegInfo->getNumVirtRegs(); i != e; ++i) {
    unsigned Reg = TargetRegisterInfo::index2VirtReg(i);
    if (RegInfo->getRegClass(Reg) == &PPC::VRRCRegClass) {
      HasVectorVReg = true;
      break;
    }
  }
  if (!HasVectorVReg) return;  // nothing to do.

  // If we have a vector register, we want to emit code into the entry and exit
  // blocks to save and restore the VRSAVE register.  We do this here (instead
  // of marking all vector instructions as clobbering VRSAVE) for two reasons:
  //
  // 1. This (trivially) reduces the load on the register allocator, by not
  //    having to represent the live range of the VRSAVE register.
  // 2. This (more significantly) allows us to create a temporary virtual
  //    register to hold the saved VRSAVE value, allowing this temporary to be
  //    register allocated, instead of forcing it to be spilled to the stack.

  // Create two vregs - one to hold the VRSAVE register that is live-in to the
  // function and one for the value after having bits or'd into it.
  unsigned InVRSAVE = RegInfo->createVirtualRegister(&PPC::GPRCRegClass);
  unsigned UpdatedVRSAVE = RegInfo->createVirtualRegister(&PPC::GPRCRegClass);

  const TargetInstrInfo &TII = *TM.getSubtargetImpl()->getInstrInfo();
  MachineBasicBlock &EntryBB = *Fn.begin();
  DebugLoc dl;
  // Emit the following code into the entry block:
  // InVRSAVE = MFVRSAVE
  // UpdatedVRSAVE = UPDATE_VRSAVE InVRSAVE
  // MTVRSAVE UpdatedVRSAVE
  MachineBasicBlock::iterator IP = EntryBB.begin();  // Insert Point
  BuildMI(EntryBB, IP, dl, TII.get(PPC::MFVRSAVE), InVRSAVE);
  BuildMI(EntryBB, IP, dl, TII.get(PPC::UPDATE_VRSAVE),
          UpdatedVRSAVE).addReg(InVRSAVE);
  BuildMI(EntryBB, IP, dl, TII.get(PPC::MTVRSAVE)).addReg(UpdatedVRSAVE);

  // Find all return blocks, outputting a restore in each epilog.
  for (MachineFunction::iterator BB = Fn.begin(), E = Fn.end(); BB != E; ++BB) {
    if (!BB->empty() && BB->back().isReturn()) {
      IP = BB->end(); --IP;

      // Skip over all terminator instructions, which are part of the return
      // sequence.
      MachineBasicBlock::iterator I2 = IP;
      while (I2 != BB->begin() && (--I2)->isTerminator())
        IP = I2;

      // Emit: MTVRSAVE InVRSave
      BuildMI(*BB, IP, dl, TII.get(PPC::MTVRSAVE)).addReg(InVRSAVE);
    }
  }
}


/// getGlobalBaseReg - Output the instructions required to put the
/// base address to use for accessing globals into a register.
///
SDNode *PPCDAGToDAGISel::getGlobalBaseReg() {
  if (!GlobalBaseReg) {
    const TargetInstrInfo &TII = *TM.getSubtargetImpl()->getInstrInfo();
    // Insert the set of GlobalBaseReg into the first MBB of the function
    MachineBasicBlock &FirstMBB = MF->front();
    MachineBasicBlock::iterator MBBI = FirstMBB.begin();
    const Module *M = MF->getFunction()->getParent();
    DebugLoc dl;

    if (PPCLowering->getPointerTy() == MVT::i32) {
      if (PPCSubTarget->isTargetELF()) {
        GlobalBaseReg = PPC::R30;
        if (M->getPICLevel() == PICLevel::Small) {
          BuildMI(FirstMBB, MBBI, dl, TII.get(PPC::MoveGOTtoLR));
          BuildMI(FirstMBB, MBBI, dl, TII.get(PPC::MFLR), GlobalBaseReg);
        } else {
          BuildMI(FirstMBB, MBBI, dl, TII.get(PPC::MovePCtoLR));
          BuildMI(FirstMBB, MBBI, dl, TII.get(PPC::MFLR), GlobalBaseReg);
          unsigned TempReg = RegInfo->createVirtualRegister(&PPC::GPRCRegClass);
          BuildMI(FirstMBB, MBBI, dl,
                  TII.get(PPC::UpdateGBR)).addReg(GlobalBaseReg)
                  .addReg(TempReg, RegState::Define).addReg(GlobalBaseReg);
          MF->getInfo<PPCFunctionInfo>()->setUsesPICBase(true);
        }
      } else {
        GlobalBaseReg =
          RegInfo->createVirtualRegister(&PPC::GPRC_NOR0RegClass);
        BuildMI(FirstMBB, MBBI, dl, TII.get(PPC::MovePCtoLR));
        BuildMI(FirstMBB, MBBI, dl, TII.get(PPC::MFLR), GlobalBaseReg);
      }
    } else {
      GlobalBaseReg = RegInfo->createVirtualRegister(&PPC::G8RC_NOX0RegClass);
      BuildMI(FirstMBB, MBBI, dl, TII.get(PPC::MovePCtoLR8));
      BuildMI(FirstMBB, MBBI, dl, TII.get(PPC::MFLR8), GlobalBaseReg);
    }
  }
  return CurDAG->getRegister(GlobalBaseReg,
                             PPCLowering->getPointerTy()).getNode();
}

/// isIntS16Immediate - This method tests to see if the node is either a 32-bit
/// or 64-bit immediate, and if the value can be accurately represented as a
/// sign extension from a 16-bit value.  If so, this returns true and the
/// immediate.
static bool isIntS16Immediate(SDNode *N, short &Imm) {
  if (N->getOpcode() != ISD::Constant)
    return false;

  Imm = (short)cast<ConstantSDNode>(N)->getZExtValue();
  if (N->getValueType(0) == MVT::i32)
    return Imm == (int32_t)cast<ConstantSDNode>(N)->getZExtValue();
  else
    return Imm == (int64_t)cast<ConstantSDNode>(N)->getZExtValue();
}

static bool isIntS16Immediate(SDValue Op, short &Imm) {
  return isIntS16Immediate(Op.getNode(), Imm);
}


/// isInt32Immediate - This method tests to see if the node is a 32-bit constant
/// operand. If so Imm will receive the 32-bit value.
static bool isInt32Immediate(SDNode *N, unsigned &Imm) {
  if (N->getOpcode() == ISD::Constant && N->getValueType(0) == MVT::i32) {
    Imm = cast<ConstantSDNode>(N)->getZExtValue();
    return true;
  }
  return false;
}

/// isInt64Immediate - This method tests to see if the node is a 64-bit constant
/// operand.  If so Imm will receive the 64-bit value.
static bool isInt64Immediate(SDNode *N, uint64_t &Imm) {
  if (N->getOpcode() == ISD::Constant && N->getValueType(0) == MVT::i64) {
    Imm = cast<ConstantSDNode>(N)->getZExtValue();
    return true;
  }
  return false;
}

// isInt32Immediate - This method tests to see if a constant operand.
// If so Imm will receive the 32 bit value.
static bool isInt32Immediate(SDValue N, unsigned &Imm) {
  return isInt32Immediate(N.getNode(), Imm);
}


// isOpcWithIntImmediate - This method tests to see if the node is a specific
// opcode and that it has a immediate integer right operand.
// If so Imm will receive the 32 bit value.
static bool isOpcWithIntImmediate(SDNode *N, unsigned Opc, unsigned& Imm) {
  return N->getOpcode() == Opc
         && isInt32Immediate(N->getOperand(1).getNode(), Imm);
}

SDNode *PPCDAGToDAGISel::getFrameIndex(SDNode *SN, SDNode *N, unsigned Offset) {
  SDLoc dl(SN);
  int FI = cast<FrameIndexSDNode>(N)->getIndex();
  SDValue TFI = CurDAG->getTargetFrameIndex(FI, N->getValueType(0));
  unsigned Opc = N->getValueType(0) == MVT::i32 ? PPC::ADDI : PPC::ADDI8;
  if (SN->hasOneUse())
    return CurDAG->SelectNodeTo(SN, Opc, N->getValueType(0), TFI,
                                getSmallIPtrImm(Offset));
  return CurDAG->getMachineNode(Opc, dl, N->getValueType(0), TFI,
                                getSmallIPtrImm(Offset));
}

bool PPCDAGToDAGISel::isRunOfOnes(unsigned Val, unsigned &MB, unsigned &ME) {
  if (!Val)
    return false;

  if (isShiftedMask_32(Val)) {
    // look for the first non-zero bit
    MB = countLeadingZeros(Val);
    // look for the first zero bit after the run of ones
    ME = countLeadingZeros((Val - 1) ^ Val);
    return true;
  } else {
    Val = ~Val; // invert mask
    if (isShiftedMask_32(Val)) {
      // effectively look for the first zero bit
      ME = countLeadingZeros(Val) - 1;
      // effectively look for the first one bit after the run of zeros
      MB = countLeadingZeros((Val - 1) ^ Val) + 1;
      return true;
    }
  }
  // no run present
  return false;
}

bool PPCDAGToDAGISel::isRotateAndMask(SDNode *N, unsigned Mask,
                                      bool isShiftMask, unsigned &SH,
                                      unsigned &MB, unsigned &ME) {
  // Don't even go down this path for i64, since different logic will be
  // necessary for rldicl/rldicr/rldimi.
  if (N->getValueType(0) != MVT::i32)
    return false;

  unsigned Shift  = 32;
  unsigned Indeterminant = ~0;  // bit mask marking indeterminant results
  unsigned Opcode = N->getOpcode();
  if (N->getNumOperands() != 2 ||
      !isInt32Immediate(N->getOperand(1).getNode(), Shift) || (Shift > 31))
    return false;

  if (Opcode == ISD::SHL) {
    // apply shift left to mask if it comes first
    if (isShiftMask) Mask = Mask << Shift;
    // determine which bits are made indeterminant by shift
    Indeterminant = ~(0xFFFFFFFFu << Shift);
  } else if (Opcode == ISD::SRL) {
    // apply shift right to mask if it comes first
    if (isShiftMask) Mask = Mask >> Shift;
    // determine which bits are made indeterminant by shift
    Indeterminant = ~(0xFFFFFFFFu >> Shift);
    // adjust for the left rotate
    Shift = 32 - Shift;
  } else if (Opcode == ISD::ROTL) {
    Indeterminant = 0;
  } else {
    return false;
  }

  // if the mask doesn't intersect any Indeterminant bits
  if (Mask && !(Mask & Indeterminant)) {
    SH = Shift & 31;
    // make sure the mask is still a mask (wrap arounds may not be)
    return isRunOfOnes(Mask, MB, ME);
  }
  return false;
}

/// SelectBitfieldInsert - turn an or of two masked values into
/// the rotate left word immediate then mask insert (rlwimi) instruction.
SDNode *PPCDAGToDAGISel::SelectBitfieldInsert(SDNode *N) {
  SDValue Op0 = N->getOperand(0);
  SDValue Op1 = N->getOperand(1);
  SDLoc dl(N);

  APInt LKZ, LKO, RKZ, RKO;
  CurDAG->computeKnownBits(Op0, LKZ, LKO);
  CurDAG->computeKnownBits(Op1, RKZ, RKO);

  unsigned TargetMask = LKZ.getZExtValue();
  unsigned InsertMask = RKZ.getZExtValue();

  if ((TargetMask | InsertMask) == 0xFFFFFFFF) {
    unsigned Op0Opc = Op0.getOpcode();
    unsigned Op1Opc = Op1.getOpcode();
    unsigned Value, SH = 0;
    TargetMask = ~TargetMask;
    InsertMask = ~InsertMask;

    // If the LHS has a foldable shift and the RHS does not, then swap it to the
    // RHS so that we can fold the shift into the insert.
    if (Op0Opc == ISD::AND && Op1Opc == ISD::AND) {
      if (Op0.getOperand(0).getOpcode() == ISD::SHL ||
          Op0.getOperand(0).getOpcode() == ISD::SRL) {
        if (Op1.getOperand(0).getOpcode() != ISD::SHL &&
            Op1.getOperand(0).getOpcode() != ISD::SRL) {
          std::swap(Op0, Op1);
          std::swap(Op0Opc, Op1Opc);
          std::swap(TargetMask, InsertMask);
        }
      }
    } else if (Op0Opc == ISD::SHL || Op0Opc == ISD::SRL) {
      if (Op1Opc == ISD::AND && Op1.getOperand(0).getOpcode() != ISD::SHL &&
          Op1.getOperand(0).getOpcode() != ISD::SRL) {
        std::swap(Op0, Op1);
        std::swap(Op0Opc, Op1Opc);
        std::swap(TargetMask, InsertMask);
      }
    }

    unsigned MB, ME;
    if (isRunOfOnes(InsertMask, MB, ME)) {
      SDValue Tmp1, Tmp2;

      if ((Op1Opc == ISD::SHL || Op1Opc == ISD::SRL) &&
          isInt32Immediate(Op1.getOperand(1), Value)) {
        Op1 = Op1.getOperand(0);
        SH  = (Op1Opc == ISD::SHL) ? Value : 32 - Value;
      }
      if (Op1Opc == ISD::AND) {
       // The AND mask might not be a constant, and we need to make sure that
       // if we're going to fold the masking with the insert, all bits not
       // know to be zero in the mask are known to be one.
        APInt MKZ, MKO;
        CurDAG->computeKnownBits(Op1.getOperand(1), MKZ, MKO);
        bool CanFoldMask = InsertMask == MKO.getZExtValue();

        unsigned SHOpc = Op1.getOperand(0).getOpcode();
        if ((SHOpc == ISD::SHL || SHOpc == ISD::SRL) && CanFoldMask &&
            isInt32Immediate(Op1.getOperand(0).getOperand(1), Value)) {
          // Note that Value must be in range here (less than 32) because
          // otherwise there would not be any bits set in InsertMask.
          Op1 = Op1.getOperand(0).getOperand(0);
          SH  = (SHOpc == ISD::SHL) ? Value : 32 - Value;
        }
      }

      SH &= 31;
      SDValue Ops[] = { Op0, Op1, getI32Imm(SH), getI32Imm(MB),
                          getI32Imm(ME) };
      return CurDAG->getMachineNode(PPC::RLWIMI, dl, MVT::i32, Ops);
    }
  }
  return nullptr;
}

// Predict the number of instructions that would be generated by calling
// SelectInt64(N).
static unsigned SelectInt64CountDirect(int64_t Imm) {
  // Assume no remaining bits.
  unsigned Remainder = 0;
  // Assume no shift required.
  unsigned Shift = 0;

  // If it can't be represented as a 32 bit value.
  if (!isInt<32>(Imm)) {
    Shift = countTrailingZeros<uint64_t>(Imm);
    int64_t ImmSh = static_cast<uint64_t>(Imm) >> Shift;

    // If the shifted value fits 32 bits.
    if (isInt<32>(ImmSh)) {
      // Go with the shifted value.
      Imm = ImmSh;
    } else {
      // Still stuck with a 64 bit value.
      Remainder = Imm;
      Shift = 32;
      Imm >>= 32;
    }
  }

  // Intermediate operand.
  unsigned Result = 0;

  // Handle first 32 bits.
  unsigned Lo = Imm & 0xFFFF;
  unsigned Hi = (Imm >> 16) & 0xFFFF;

  // Simple value.
  if (isInt<16>(Imm)) {
    // Just the Lo bits.
    ++Result;
  } else if (Lo) {
    // Handle the Hi bits and Lo bits.
    Result += 2;
  } else {
    // Just the Hi bits.
    ++Result;
  }

  // If no shift, we're done.
  if (!Shift) return Result;

  // Shift for next step if the upper 32-bits were not zero.
  if (Imm)
    ++Result;

  // Add in the last bits as required.
  if ((Hi = (Remainder >> 16) & 0xFFFF))
    ++Result;
  if ((Lo = Remainder & 0xFFFF))
    ++Result;

  return Result;
}

static uint64_t Rot64(uint64_t Imm, unsigned R) {
  return (Imm << R) | (Imm >> (64 - R));
}

static unsigned SelectInt64Count(int64_t Imm) {
  unsigned Count = SelectInt64CountDirect(Imm);
  if (Count == 1)
    return Count;

  for (unsigned r = 1; r < 63; ++r) {
    uint64_t RImm = Rot64(Imm, r);
    unsigned RCount = SelectInt64CountDirect(RImm) + 1;
    Count = std::min(Count, RCount);

    // See comments in SelectInt64 for an explanation of the logic below.
    unsigned LS = findLastSet(RImm);
    if (LS != r-1)
      continue;

    uint64_t OnesMask = -(int64_t) (UINT64_C(1) << (LS+1));
    uint64_t RImmWithOnes = RImm | OnesMask;

    RCount = SelectInt64CountDirect(RImmWithOnes) + 1;
    Count = std::min(Count, RCount);
  }

  return Count;
}

// Select a 64-bit constant. For cost-modeling purposes, SelectInt64Count
// (above) needs to be kept in sync with this function.
static SDNode *SelectInt64Direct(SelectionDAG *CurDAG, SDLoc dl, int64_t Imm) {
  // Assume no remaining bits.
  unsigned Remainder = 0;
  // Assume no shift required.
  unsigned Shift = 0;

  // If it can't be represented as a 32 bit value.
  if (!isInt<32>(Imm)) {
    Shift = countTrailingZeros<uint64_t>(Imm);
    int64_t ImmSh = static_cast<uint64_t>(Imm) >> Shift;

    // If the shifted value fits 32 bits.
    if (isInt<32>(ImmSh)) {
      // Go with the shifted value.
      Imm = ImmSh;
    } else {
      // Still stuck with a 64 bit value.
      Remainder = Imm;
      Shift = 32;
      Imm >>= 32;
    }
  }

  // Intermediate operand.
  SDNode *Result;

  // Handle first 32 bits.
  unsigned Lo = Imm & 0xFFFF;
  unsigned Hi = (Imm >> 16) & 0xFFFF;

  auto getI32Imm = [CurDAG](unsigned Imm) {
      return CurDAG->getTargetConstant(Imm, MVT::i32);
  };

  // Simple value.
  if (isInt<16>(Imm)) {
    // Just the Lo bits.
    Result = CurDAG->getMachineNode(PPC::LI8, dl, MVT::i64, getI32Imm(Lo));
  } else if (Lo) {
    // Handle the Hi bits.
    unsigned OpC = Hi ? PPC::LIS8 : PPC::LI8;
    Result = CurDAG->getMachineNode(OpC, dl, MVT::i64, getI32Imm(Hi));
    // And Lo bits.
    Result = CurDAG->getMachineNode(PPC::ORI8, dl, MVT::i64,
                                    SDValue(Result, 0), getI32Imm(Lo));
  } else {
    // Just the Hi bits.
    Result = CurDAG->getMachineNode(PPC::LIS8, dl, MVT::i64, getI32Imm(Hi));
  }

  // If no shift, we're done.
  if (!Shift) return Result;

  // Shift for next step if the upper 32-bits were not zero.
  if (Imm) {
    Result = CurDAG->getMachineNode(PPC::RLDICR, dl, MVT::i64,
                                    SDValue(Result, 0),
                                    getI32Imm(Shift),
                                    getI32Imm(63 - Shift));
  }

  // Add in the last bits as required.
  if ((Hi = (Remainder >> 16) & 0xFFFF)) {
    Result = CurDAG->getMachineNode(PPC::ORIS8, dl, MVT::i64,
                                    SDValue(Result, 0), getI32Imm(Hi));
  }
  if ((Lo = Remainder & 0xFFFF)) {
    Result = CurDAG->getMachineNode(PPC::ORI8, dl, MVT::i64,
                                    SDValue(Result, 0), getI32Imm(Lo));
  }

  return Result;
}

static SDNode *SelectInt64(SelectionDAG *CurDAG, SDLoc dl, int64_t Imm) {
  unsigned Count = SelectInt64CountDirect(Imm);
  if (Count == 1)
    return SelectInt64Direct(CurDAG, dl, Imm);

  unsigned RMin = 0;

  int64_t MatImm;
  unsigned MaskEnd;

  for (unsigned r = 1; r < 63; ++r) {
    uint64_t RImm = Rot64(Imm, r);
    unsigned RCount = SelectInt64CountDirect(RImm) + 1;
    if (RCount < Count) {
      Count = RCount;
      RMin = r;
      MatImm = RImm;
      MaskEnd = 63;
    }

    // If the immediate to generate has many trailing zeros, it might be
    // worthwhile to generate a rotated value with too many leading ones
    // (because that's free with li/lis's sign-extension semantics), and then
    // mask them off after rotation.

    unsigned LS = findLastSet(RImm);
    // We're adding (63-LS) higher-order ones, and we expect to mask them off
    // after performing the inverse rotation by (64-r). So we need that:
    //   63-LS == 64-r => LS == r-1
    if (LS != r-1)
      continue;

    uint64_t OnesMask = -(int64_t) (UINT64_C(1) << (LS+1));
    uint64_t RImmWithOnes = RImm | OnesMask;

    RCount = SelectInt64CountDirect(RImmWithOnes) + 1;
    if (RCount < Count) {
      Count = RCount;
      RMin = r;
      MatImm = RImmWithOnes;
      MaskEnd = LS;
    }
  }

  if (!RMin)
    return SelectInt64Direct(CurDAG, dl, Imm);

  auto getI32Imm = [CurDAG](unsigned Imm) {
      return CurDAG->getTargetConstant(Imm, MVT::i32);
  };

  SDValue Val = SDValue(SelectInt64Direct(CurDAG, dl, MatImm), 0);
  return CurDAG->getMachineNode(PPC::RLDICR, dl, MVT::i64, Val,
                                getI32Imm(64 - RMin), getI32Imm(MaskEnd));
}

// Select a 64-bit constant.
static SDNode *SelectInt64(SelectionDAG *CurDAG, SDNode *N) {
  SDLoc dl(N);

  // Get 64 bit value.
  int64_t Imm = cast<ConstantSDNode>(N)->getZExtValue();
  return SelectInt64(CurDAG, dl, Imm);
}

namespace {
class BitPermutationSelector {
  struct ValueBit {
    SDValue V;

    // The bit number in the value, using a convention where bit 0 is the
    // lowest-order bit.
    unsigned Idx;

    enum Kind {
      ConstZero,
      Variable
    } K;

    ValueBit(SDValue V, unsigned I, Kind K = Variable)
      : V(V), Idx(I), K(K) {}
    ValueBit(Kind K = Variable)
      : V(SDValue(nullptr, 0)), Idx(UINT32_MAX), K(K) {}

    bool isZero() const {
      return K == ConstZero;
    }

    bool hasValue() const {
      return K == Variable;
    }

    SDValue getValue() const {
      assert(hasValue() && "Cannot get the value of a constant bit");
      return V;
    }

    unsigned getValueBitIndex() const {
      assert(hasValue() && "Cannot get the value bit index of a constant bit");
      return Idx;
    }
  };

  // A bit group has the same underlying value and the same rotate factor.
  struct BitGroup {
    SDValue V;
    unsigned RLAmt;
    unsigned StartIdx, EndIdx;

    // This rotation amount assumes that the lower 32 bits of the quantity are
    // replicated in the high 32 bits by the rotation operator (which is done
    // by rlwinm and friends in 64-bit mode).
    bool Repl32;
    // Did converting to Repl32 == true change the rotation factor? If it did,
    // it decreased it by 32.
    bool Repl32CR;
    // Was this group coalesced after setting Repl32 to true?
    bool Repl32Coalesced;

    BitGroup(SDValue V, unsigned R, unsigned S, unsigned E)
      : V(V), RLAmt(R), StartIdx(S), EndIdx(E), Repl32(false), Repl32CR(false),
        Repl32Coalesced(false) {
      DEBUG(dbgs() << "\tbit group for " << V.getNode() << " RLAmt = " << R <<
                      " [" << S << ", " << E << "]\n");
    }
  };

  // Information on each (Value, RLAmt) pair (like the number of groups
  // associated with each) used to choose the lowering method.
  struct ValueRotInfo {
    SDValue V;
    unsigned RLAmt;
    unsigned NumGroups;
    unsigned FirstGroupStartIdx;
    bool Repl32;

    ValueRotInfo()
      : RLAmt(UINT32_MAX), NumGroups(0), FirstGroupStartIdx(UINT32_MAX),
        Repl32(false) {}

    // For sorting (in reverse order) by NumGroups, and then by
    // FirstGroupStartIdx.
    bool operator < (const ValueRotInfo &Other) const {
      // We need to sort so that the non-Repl32 come first because, when we're
      // doing masking, the Repl32 bit groups might be subsumed into the 64-bit
      // masking operation.
      if (Repl32 < Other.Repl32)
        return true;
      else if (Repl32 > Other.Repl32)
        return false;
      else if (NumGroups > Other.NumGroups)
        return true;
      else if (NumGroups < Other.NumGroups)
        return false;
      else if (FirstGroupStartIdx < Other.FirstGroupStartIdx)
        return true;
      return false;
    }
  };

  // Return true if something interesting was deduced, return false if we're
  // providing only a generic representation of V (or something else likewise
  // uninteresting for instruction selection).
  bool getValueBits(SDValue V, SmallVector<ValueBit, 64> &Bits) {
    switch (V.getOpcode()) {
    default: break;
    case ISD::ROTL:
      if (isa<ConstantSDNode>(V.getOperand(1))) {
        unsigned RotAmt = V.getConstantOperandVal(1);

        SmallVector<ValueBit, 64> LHSBits(Bits.size());
        getValueBits(V.getOperand(0), LHSBits);

        for (unsigned i = 0; i < Bits.size(); ++i)
          Bits[i] = LHSBits[i < RotAmt ? i + (Bits.size() - RotAmt) : i - RotAmt];

        return true;
      }
      break;
    case ISD::SHL:
      if (isa<ConstantSDNode>(V.getOperand(1))) {
        unsigned ShiftAmt = V.getConstantOperandVal(1);

        SmallVector<ValueBit, 64> LHSBits(Bits.size());
        getValueBits(V.getOperand(0), LHSBits);

        for (unsigned i = ShiftAmt; i < Bits.size(); ++i)
          Bits[i] = LHSBits[i - ShiftAmt];

        for (unsigned i = 0; i < ShiftAmt; ++i)
          Bits[i] = ValueBit(ValueBit::ConstZero);

        return true;
      }
      break;
    case ISD::SRL:
      if (isa<ConstantSDNode>(V.getOperand(1))) {
        unsigned ShiftAmt = V.getConstantOperandVal(1);

        SmallVector<ValueBit, 64> LHSBits(Bits.size());
        getValueBits(V.getOperand(0), LHSBits);

        for (unsigned i = 0; i < Bits.size() - ShiftAmt; ++i)
          Bits[i] = LHSBits[i + ShiftAmt];

        for (unsigned i = Bits.size() - ShiftAmt; i < Bits.size(); ++i)
          Bits[i] = ValueBit(ValueBit::ConstZero);

        return true;
      }
      break;
    case ISD::AND:
      if (isa<ConstantSDNode>(V.getOperand(1))) {
        uint64_t Mask = V.getConstantOperandVal(1);

        SmallVector<ValueBit, 64> LHSBits(Bits.size());
        bool LHSTrivial = getValueBits(V.getOperand(0), LHSBits);

        for (unsigned i = 0; i < Bits.size(); ++i)
          if (((Mask >> i) & 1) == 1)
            Bits[i] = LHSBits[i];
          else
            Bits[i] = ValueBit(ValueBit::ConstZero);

        // Mark this as interesting, only if the LHS was also interesting. This
        // prevents the overall procedure from matching a single immediate 'and'
        // (which is non-optimal because such an and might be folded with other
        // things if we don't select it here).
        return LHSTrivial;
      }
      break;
    case ISD::OR: {
      SmallVector<ValueBit, 64> LHSBits(Bits.size()), RHSBits(Bits.size());
      getValueBits(V.getOperand(0), LHSBits);
      getValueBits(V.getOperand(1), RHSBits);

      bool AllDisjoint = true;
      for (unsigned i = 0; i < Bits.size(); ++i)
        if (LHSBits[i].isZero())
          Bits[i] = RHSBits[i];
        else if (RHSBits[i].isZero())
          Bits[i] = LHSBits[i];
        else {
          AllDisjoint = false;
          break;
        }

      if (!AllDisjoint)
        break;

      return true;
    }
    }

    for (unsigned i = 0; i < Bits.size(); ++i)
      Bits[i] = ValueBit(V, i);

    return false;
  }

  // For each value (except the constant ones), compute the left-rotate amount
  // to get it from its original to final position.
  void computeRotationAmounts() {
    HasZeros = false;
    RLAmt.resize(Bits.size());
    for (unsigned i = 0; i < Bits.size(); ++i)
      if (Bits[i].hasValue()) {
        unsigned VBI = Bits[i].getValueBitIndex();
        if (i >= VBI)
          RLAmt[i] = i - VBI;
        else
          RLAmt[i] = Bits.size() - (VBI - i);
      } else if (Bits[i].isZero()) {
        HasZeros = true;
        RLAmt[i] = UINT32_MAX;
      } else {
        llvm_unreachable("Unknown value bit type");
      }
  }

  // Collect groups of consecutive bits with the same underlying value and
  // rotation factor. If we're doing late masking, we ignore zeros, otherwise
  // they break up groups.
  void collectBitGroups(bool LateMask) {
    BitGroups.clear();

    unsigned LastRLAmt = RLAmt[0];
    SDValue LastValue = Bits[0].hasValue() ? Bits[0].getValue() : SDValue();
    unsigned LastGroupStartIdx = 0;
    for (unsigned i = 1; i < Bits.size(); ++i) {
      unsigned ThisRLAmt = RLAmt[i];
      SDValue ThisValue = Bits[i].hasValue() ? Bits[i].getValue() : SDValue();
      if (LateMask && !ThisValue) {
        ThisValue = LastValue;
        ThisRLAmt = LastRLAmt;
        // If we're doing late masking, then the first bit group always starts
        // at zero (even if the first bits were zero).
        if (BitGroups.empty())
          LastGroupStartIdx = 0;
      }

      // If this bit has the same underlying value and the same rotate factor as
      // the last one, then they're part of the same group.
      if (ThisRLAmt == LastRLAmt && ThisValue == LastValue)
        continue;

      if (LastValue.getNode())
        BitGroups.push_back(BitGroup(LastValue, LastRLAmt, LastGroupStartIdx,
                                     i-1));
      LastRLAmt = ThisRLAmt;
      LastValue = ThisValue;
      LastGroupStartIdx = i;
    }
    if (LastValue.getNode())
      BitGroups.push_back(BitGroup(LastValue, LastRLAmt, LastGroupStartIdx,
                                   Bits.size()-1));

    if (BitGroups.empty())
      return;

    // We might be able to combine the first and last groups.
    if (BitGroups.size() > 1) {
      // If the first and last groups are the same, then remove the first group
      // in favor of the last group, making the ending index of the last group
      // equal to the ending index of the to-be-removed first group.
      if (BitGroups[0].StartIdx == 0 &&
          BitGroups[BitGroups.size()-1].EndIdx == Bits.size()-1 &&
          BitGroups[0].V == BitGroups[BitGroups.size()-1].V &&
          BitGroups[0].RLAmt == BitGroups[BitGroups.size()-1].RLAmt) {
        DEBUG(dbgs() << "\tcombining final bit group with inital one\n");
        BitGroups[BitGroups.size()-1].EndIdx = BitGroups[0].EndIdx;
        BitGroups.erase(BitGroups.begin());
      }
    }
  }

  // Take all (SDValue, RLAmt) pairs and sort them by the number of groups
  // associated with each. If there is a degeneracy, pick the one that occurs
  // first (in the final value).
  void collectValueRotInfo() {
    ValueRots.clear();

    for (auto &BG : BitGroups) {
      unsigned RLAmtKey = BG.RLAmt + (BG.Repl32 ? 64 : 0);
      ValueRotInfo &VRI = ValueRots[std::make_pair(BG.V, RLAmtKey)];
      VRI.V = BG.V;
      VRI.RLAmt = BG.RLAmt;
      VRI.Repl32 = BG.Repl32;
      VRI.NumGroups += 1;
      VRI.FirstGroupStartIdx = std::min(VRI.FirstGroupStartIdx, BG.StartIdx);
    }

    // Now that we've collected the various ValueRotInfo instances, we need to
    // sort them.
    ValueRotsVec.clear();
    for (auto &I : ValueRots) {
      ValueRotsVec.push_back(I.second);
    }
    std::sort(ValueRotsVec.begin(), ValueRotsVec.end());
  }

  // In 64-bit mode, rlwinm and friends have a rotation operator that
  // replicates the low-order 32 bits into the high-order 32-bits. The mask
  // indices of these instructions can only be in the lower 32 bits, so they
  // can only represent some 64-bit bit groups. However, when they can be used,
  // the 32-bit replication can be used to represent, as a single bit group,
  // otherwise separate bit groups. We'll convert to replicated-32-bit bit
  // groups when possible. Returns true if any of the bit groups were
  // converted.
  void assignRepl32BitGroups() {
    // If we have bits like this:
    //
    // Indices:    15 14 13 12 11 10 9 8  7  6  5  4  3  2  1  0
    // V bits: ... 7  6  5  4  3  2  1 0 31 30 29 28 27 26 25 24
    // Groups:    |      RLAmt = 8      |      RLAmt = 40       |
    //
    // But, making use of a 32-bit operation that replicates the low-order 32
    // bits into the high-order 32 bits, this can be one bit group with a RLAmt
    // of 8.

    auto IsAllLow32 = [this](BitGroup & BG) {
      if (BG.StartIdx <= BG.EndIdx) {
        for (unsigned i = BG.StartIdx; i <= BG.EndIdx; ++i) {
          if (!Bits[i].hasValue())
            continue;
          if (Bits[i].getValueBitIndex() >= 32)
            return false;
        }
      } else {
        for (unsigned i = BG.StartIdx; i < Bits.size(); ++i) {
          if (!Bits[i].hasValue())
            continue;
          if (Bits[i].getValueBitIndex() >= 32)
            return false;
        }
        for (unsigned i = 0; i <= BG.EndIdx; ++i) {
          if (!Bits[i].hasValue())
            continue;
          if (Bits[i].getValueBitIndex() >= 32)
            return false;
        }
      }

      return true;
    };

    for (auto &BG : BitGroups) {
      if (BG.StartIdx < 32 && BG.EndIdx < 32) {
        if (IsAllLow32(BG)) {
          if (BG.RLAmt >= 32) {
            BG.RLAmt -= 32;
            BG.Repl32CR = true;
          }

          BG.Repl32 = true;

          DEBUG(dbgs() << "\t32-bit replicated bit group for " <<
                          BG.V.getNode() << " RLAmt = " << BG.RLAmt <<
                          " [" << BG.StartIdx << ", " << BG.EndIdx << "]\n");
        }
      }
    }

    // Now walk through the bit groups, consolidating where possible.
    for (auto I = BitGroups.begin(); I != BitGroups.end();) {
      // We might want to remove this bit group by merging it with the previous
      // group (which might be the ending group).
      auto IP = (I == BitGroups.begin()) ?
                std::prev(BitGroups.end()) : std::prev(I);
      if (I->Repl32 && IP->Repl32 && I->V == IP->V && I->RLAmt == IP->RLAmt &&
          I->StartIdx == (IP->EndIdx + 1) % 64 && I != IP) {

        DEBUG(dbgs() << "\tcombining 32-bit replicated bit group for " <<
                        I->V.getNode() << " RLAmt = " << I->RLAmt <<
                        " [" << I->StartIdx << ", " << I->EndIdx <<
                        "] with group with range [" <<
                        IP->StartIdx << ", " << IP->EndIdx << "]\n");

        IP->EndIdx = I->EndIdx;
        IP->Repl32CR = IP->Repl32CR || I->Repl32CR;
        IP->Repl32Coalesced = true;
        I = BitGroups.erase(I);
        continue;
      } else {
        // There is a special case worth handling: If there is a single group
        // covering the entire upper 32 bits, and it can be merged with both
        // the next and previous groups (which might be the same group), then
        // do so. If it is the same group (so there will be only one group in
        // total), then we need to reverse the order of the range so that it
        // covers the entire 64 bits.
        if (I->StartIdx == 32 && I->EndIdx == 63) {
          assert(std::next(I) == BitGroups.end() &&
                 "bit group ends at index 63 but there is another?");
          auto IN = BitGroups.begin();

          if (IP->Repl32 && IN->Repl32 && I->V == IP->V && I->V == IN->V && 
              (I->RLAmt % 32) == IP->RLAmt && (I->RLAmt % 32) == IN->RLAmt &&
              IP->EndIdx == 31 && IN->StartIdx == 0 && I != IP &&
              IsAllLow32(*I)) {

            DEBUG(dbgs() << "\tcombining bit group for " <<
                            I->V.getNode() << " RLAmt = " << I->RLAmt <<
                            " [" << I->StartIdx << ", " << I->EndIdx <<
                            "] with 32-bit replicated groups with ranges [" <<
                            IP->StartIdx << ", " << IP->EndIdx << "] and [" <<
                            IN->StartIdx << ", " << IN->EndIdx << "]\n");

            if (IP == IN) {
              // There is only one other group; change it to cover the whole
              // range (backward, so that it can still be Repl32 but cover the
              // whole 64-bit range).
              IP->StartIdx = 31;
              IP->EndIdx = 30;
              IP->Repl32CR = IP->Repl32CR || I->RLAmt >= 32;
              IP->Repl32Coalesced = true;
              I = BitGroups.erase(I);
            } else {
              // There are two separate groups, one before this group and one
              // after us (at the beginning). We're going to remove this group,
              // but also the group at the very beginning.
              IP->EndIdx = IN->EndIdx;
              IP->Repl32CR = IP->Repl32CR || IN->Repl32CR || I->RLAmt >= 32;
              IP->Repl32Coalesced = true;
              I = BitGroups.erase(I);
              BitGroups.erase(BitGroups.begin());
            }

            // This must be the last group in the vector (and we might have
            // just invalidated the iterator above), so break here.
            break;
          }
        }
      }

      ++I;
    }
  }

  SDValue getI32Imm(unsigned Imm) {
    return CurDAG->getTargetConstant(Imm, MVT::i32);
  }

  uint64_t getZerosMask() {
    uint64_t Mask = 0;
    for (unsigned i = 0; i < Bits.size(); ++i) {
      if (Bits[i].hasValue())
        continue;
      Mask |= (UINT64_C(1) << i);
    }

    return ~Mask;
  }

  // Depending on the number of groups for a particular value, it might be
  // better to rotate, mask explicitly (using andi/andis), and then or the
  // result. Select this part of the result first.
  void SelectAndParts32(SDLoc dl, SDValue &Res, unsigned *InstCnt) {
    if (BPermRewriterNoMasking)
      return;

    for (ValueRotInfo &VRI : ValueRotsVec) {
      unsigned Mask = 0;
      for (unsigned i = 0; i < Bits.size(); ++i) {
        if (!Bits[i].hasValue() || Bits[i].getValue() != VRI.V)
          continue;
        if (RLAmt[i] != VRI.RLAmt)
          continue;
        Mask |= (1u << i);
      }

      // Compute the masks for andi/andis that would be necessary.
      unsigned ANDIMask = (Mask & UINT16_MAX), ANDISMask = Mask >> 16;
      assert((ANDIMask != 0 || ANDISMask != 0) &&
             "No set bits in mask for value bit groups");
      bool NeedsRotate = VRI.RLAmt != 0;

      // We're trying to minimize the number of instructions. If we have one
      // group, using one of andi/andis can break even.  If we have three
      // groups, we can use both andi and andis and break even (to use both
      // andi and andis we also need to or the results together). We need four
      // groups if we also need to rotate. To use andi/andis we need to do more
      // than break even because rotate-and-mask instructions tend to be easier
      // to schedule.

      // FIXME: We've biased here against using andi/andis, which is right for
      // POWER cores, but not optimal everywhere. For example, on the A2,
      // andi/andis have single-cycle latency whereas the rotate-and-mask
      // instructions take two cycles, and it would be better to bias toward
      // andi/andis in break-even cases.

      unsigned NumAndInsts = (unsigned) NeedsRotate +
                             (unsigned) (ANDIMask != 0) +
                             (unsigned) (ANDISMask != 0) +
                             (unsigned) (ANDIMask != 0 && ANDISMask != 0) +
                             (unsigned) (bool) Res;

      DEBUG(dbgs() << "\t\trotation groups for " << VRI.V.getNode() <<
                      " RL: " << VRI.RLAmt << ":" <<
                      "\n\t\t\tisel using masking: " << NumAndInsts <<
                      " using rotates: " << VRI.NumGroups << "\n");

      if (NumAndInsts >= VRI.NumGroups)
        continue;

      DEBUG(dbgs() << "\t\t\t\tusing masking\n");

      if (InstCnt) *InstCnt += NumAndInsts;

      SDValue VRot;
      if (VRI.RLAmt) {
        SDValue Ops[] =
          { VRI.V, getI32Imm(VRI.RLAmt), getI32Imm(0), getI32Imm(31) };
        VRot = SDValue(CurDAG->getMachineNode(PPC::RLWINM, dl, MVT::i32,
                                              Ops), 0);
      } else {
        VRot = VRI.V;
      }

      SDValue ANDIVal, ANDISVal;
      if (ANDIMask != 0)
        ANDIVal = SDValue(CurDAG->getMachineNode(PPC::ANDIo, dl, MVT::i32,
                            VRot, getI32Imm(ANDIMask)), 0);
      if (ANDISMask != 0)
        ANDISVal = SDValue(CurDAG->getMachineNode(PPC::ANDISo, dl, MVT::i32,
                             VRot, getI32Imm(ANDISMask)), 0);

      SDValue TotalVal;
      if (!ANDIVal)
        TotalVal = ANDISVal;
      else if (!ANDISVal)
        TotalVal = ANDIVal;
      else
        TotalVal = SDValue(CurDAG->getMachineNode(PPC::OR, dl, MVT::i32,
                             ANDIVal, ANDISVal), 0);

      if (!Res)
        Res = TotalVal;
      else
        Res = SDValue(CurDAG->getMachineNode(PPC::OR, dl, MVT::i32,
                        Res, TotalVal), 0);

      // Now, remove all groups with this underlying value and rotation
      // factor.
      for (auto I = BitGroups.begin(); I != BitGroups.end();) {
        if (I->V == VRI.V && I->RLAmt == VRI.RLAmt)
          I = BitGroups.erase(I);
        else
          ++I;
      }
    }
  }

  // Instruction selection for the 32-bit case.
  SDNode *Select32(SDNode *N, bool LateMask, unsigned *InstCnt) {
    SDLoc dl(N);
    SDValue Res;

    if (InstCnt) *InstCnt = 0;

    // Take care of cases that should use andi/andis first.
    SelectAndParts32(dl, Res, InstCnt);

    // If we've not yet selected a 'starting' instruction, and we have no zeros
    // to fill in, select the (Value, RLAmt) with the highest priority (largest
    // number of groups), and start with this rotated value.
    if ((!HasZeros || LateMask) && !Res) {
      ValueRotInfo &VRI = ValueRotsVec[0];
      if (VRI.RLAmt) {
        if (InstCnt) *InstCnt += 1;
        SDValue Ops[] =
          { VRI.V, getI32Imm(VRI.RLAmt), getI32Imm(0), getI32Imm(31) };
        Res = SDValue(CurDAG->getMachineNode(PPC::RLWINM, dl, MVT::i32, Ops), 0);
      } else {
        Res = VRI.V;
      }

      // Now, remove all groups with this underlying value and rotation factor.
      for (auto I = BitGroups.begin(); I != BitGroups.end();) {
        if (I->V == VRI.V && I->RLAmt == VRI.RLAmt)
          I = BitGroups.erase(I);
        else
          ++I;
      }
    }

    if (InstCnt) *InstCnt += BitGroups.size();

    // Insert the other groups (one at a time).
    for (auto &BG : BitGroups) {
      if (!Res) {
        SDValue Ops[] =
          { BG.V, getI32Imm(BG.RLAmt), getI32Imm(Bits.size() - BG.EndIdx - 1),
            getI32Imm(Bits.size() - BG.StartIdx - 1) };
        Res = SDValue(CurDAG->getMachineNode(PPC::RLWINM, dl, MVT::i32, Ops), 0);
      } else {
        SDValue Ops[] =
          { Res, BG.V, getI32Imm(BG.RLAmt), getI32Imm(Bits.size() - BG.EndIdx - 1),
            getI32Imm(Bits.size() - BG.StartIdx - 1) };
        Res = SDValue(CurDAG->getMachineNode(PPC::RLWIMI, dl, MVT::i32, Ops), 0);
      }
    }

    if (LateMask) {
      unsigned Mask = (unsigned) getZerosMask();

      unsigned ANDIMask = (Mask & UINT16_MAX), ANDISMask = Mask >> 16;
      assert((ANDIMask != 0 || ANDISMask != 0) &&
             "No set bits in zeros mask?");

      if (InstCnt) *InstCnt += (unsigned) (ANDIMask != 0) +
                               (unsigned) (ANDISMask != 0) +
                               (unsigned) (ANDIMask != 0 && ANDISMask != 0);

      SDValue ANDIVal, ANDISVal;
      if (ANDIMask != 0)
        ANDIVal = SDValue(CurDAG->getMachineNode(PPC::ANDIo, dl, MVT::i32,
                            Res, getI32Imm(ANDIMask)), 0);
      if (ANDISMask != 0)
        ANDISVal = SDValue(CurDAG->getMachineNode(PPC::ANDISo, dl, MVT::i32,
                             Res, getI32Imm(ANDISMask)), 0);

      if (!ANDIVal)
        Res = ANDISVal;
      else if (!ANDISVal)
        Res = ANDIVal;
      else
        Res = SDValue(CurDAG->getMachineNode(PPC::OR, dl, MVT::i32,
                        ANDIVal, ANDISVal), 0);
    }

    return Res.getNode();
  }

  unsigned SelectRotMask64Count(unsigned RLAmt, bool Repl32,
                                unsigned MaskStart, unsigned MaskEnd,
                                bool IsIns) {
    // In the notation used by the instructions, 'start' and 'end' are reversed
    // because bits are counted from high to low order.
    unsigned InstMaskStart = 64 - MaskEnd - 1,
             InstMaskEnd   = 64 - MaskStart - 1;

    if (Repl32)
      return 1;

    if ((!IsIns && (InstMaskEnd == 63 || InstMaskStart == 0)) ||
        InstMaskEnd == 63 - RLAmt)
      return 1;

    return 2;
  }

  // For 64-bit values, not all combinations of rotates and masks are
  // available. Produce one if it is available.
  SDValue SelectRotMask64(SDValue V, SDLoc dl, unsigned RLAmt, bool Repl32,
                          unsigned MaskStart, unsigned MaskEnd,
                          unsigned *InstCnt = nullptr) {
    // In the notation used by the instructions, 'start' and 'end' are reversed
    // because bits are counted from high to low order.
    unsigned InstMaskStart = 64 - MaskEnd - 1,
             InstMaskEnd   = 64 - MaskStart - 1;

    if (InstCnt) *InstCnt += 1;

    if (Repl32) {
      // This rotation amount assumes that the lower 32 bits of the quantity
      // are replicated in the high 32 bits by the rotation operator (which is
      // done by rlwinm and friends).
      assert(InstMaskStart >= 32 && "Mask cannot start out of range");
      assert(InstMaskEnd   >= 32 && "Mask cannot end out of range");
      SDValue Ops[] =
        { V, getI32Imm(RLAmt), getI32Imm(InstMaskStart - 32),
          getI32Imm(InstMaskEnd - 32) };
      return SDValue(CurDAG->getMachineNode(PPC::RLWINM8, dl, MVT::i64,
                                            Ops), 0);
    }

    if (InstMaskEnd == 63) {
      SDValue Ops[] =
        { V, getI32Imm(RLAmt), getI32Imm(InstMaskStart) };
      return SDValue(CurDAG->getMachineNode(PPC::RLDICL, dl, MVT::i64, Ops), 0);
    }

    if (InstMaskStart == 0) {
      SDValue Ops[] =
        { V, getI32Imm(RLAmt), getI32Imm(InstMaskEnd) };
      return SDValue(CurDAG->getMachineNode(PPC::RLDICR, dl, MVT::i64, Ops), 0);
    }

    if (InstMaskEnd == 63 - RLAmt) {
      SDValue Ops[] =
        { V, getI32Imm(RLAmt), getI32Imm(InstMaskStart) };
      return SDValue(CurDAG->getMachineNode(PPC::RLDIC, dl, MVT::i64, Ops), 0);
    }

    // We cannot do this with a single instruction, so we'll use two. The
    // problem is that we're not free to choose both a rotation amount and mask
    // start and end independently. We can choose an arbitrary mask start and
    // end, but then the rotation amount is fixed. Rotation, however, can be
    // inverted, and so by applying an "inverse" rotation first, we can get the
    // desired result.
    if (InstCnt) *InstCnt += 1;

    // The rotation mask for the second instruction must be MaskStart.
    unsigned RLAmt2 = MaskStart;
    // The first instruction must rotate V so that the overall rotation amount
    // is RLAmt.
    unsigned RLAmt1 = (64 + RLAmt - RLAmt2) % 64;
    if (RLAmt1)
      V = SelectRotMask64(V, dl, RLAmt1, false, 0, 63);
    return SelectRotMask64(V, dl, RLAmt2, false, MaskStart, MaskEnd);
  }

  // For 64-bit values, not all combinations of rotates and masks are
  // available. Produce a rotate-mask-and-insert if one is available.
  SDValue SelectRotMaskIns64(SDValue Base, SDValue V, SDLoc dl, unsigned RLAmt,
                             bool Repl32, unsigned MaskStart,
                             unsigned MaskEnd, unsigned *InstCnt = nullptr) {
    // In the notation used by the instructions, 'start' and 'end' are reversed
    // because bits are counted from high to low order.
    unsigned InstMaskStart = 64 - MaskEnd - 1,
             InstMaskEnd   = 64 - MaskStart - 1;

    if (InstCnt) *InstCnt += 1;

    if (Repl32) {
      // This rotation amount assumes that the lower 32 bits of the quantity
      // are replicated in the high 32 bits by the rotation operator (which is
      // done by rlwinm and friends).
      assert(InstMaskStart >= 32 && "Mask cannot start out of range");
      assert(InstMaskEnd   >= 32 && "Mask cannot end out of range");
      SDValue Ops[] =
        { Base, V, getI32Imm(RLAmt), getI32Imm(InstMaskStart - 32),
          getI32Imm(InstMaskEnd - 32) };
      return SDValue(CurDAG->getMachineNode(PPC::RLWIMI8, dl, MVT::i64,
                                            Ops), 0);
    }

    if (InstMaskEnd == 63 - RLAmt) {
      SDValue Ops[] =
        { Base, V, getI32Imm(RLAmt), getI32Imm(InstMaskStart) };
      return SDValue(CurDAG->getMachineNode(PPC::RLDIMI, dl, MVT::i64, Ops), 0);
    }

    // We cannot do this with a single instruction, so we'll use two. The
    // problem is that we're not free to choose both a rotation amount and mask
    // start and end independently. We can choose an arbitrary mask start and
    // end, but then the rotation amount is fixed. Rotation, however, can be
    // inverted, and so by applying an "inverse" rotation first, we can get the
    // desired result.
    if (InstCnt) *InstCnt += 1;

    // The rotation mask for the second instruction must be MaskStart.
    unsigned RLAmt2 = MaskStart;
    // The first instruction must rotate V so that the overall rotation amount
    // is RLAmt.
    unsigned RLAmt1 = (64 + RLAmt - RLAmt2) % 64;
    if (RLAmt1)
      V = SelectRotMask64(V, dl, RLAmt1, false, 0, 63);
    return SelectRotMaskIns64(Base, V, dl, RLAmt2, false, MaskStart, MaskEnd);
  }

  void SelectAndParts64(SDLoc dl, SDValue &Res, unsigned *InstCnt) {
    if (BPermRewriterNoMasking)
      return;

    // The idea here is the same as in the 32-bit version, but with additional
    // complications from the fact that Repl32 might be true. Because we
    // aggressively convert bit groups to Repl32 form (which, for small
    // rotation factors, involves no other change), and then coalesce, it might
    // be the case that a single 64-bit masking operation could handle both
    // some Repl32 groups and some non-Repl32 groups. If converting to Repl32
    // form allowed coalescing, then we must use a 32-bit rotaton in order to
    // completely capture the new combined bit group.

    for (ValueRotInfo &VRI : ValueRotsVec) {
      uint64_t Mask = 0;

      // We need to add to the mask all bits from the associated bit groups.
      // If Repl32 is false, we need to add bits from bit groups that have
      // Repl32 true, but are trivially convertable to Repl32 false. Such a
      // group is trivially convertable if it overlaps only with the lower 32
      // bits, and the group has not been coalesced.
      auto MatchingBG = [VRI](BitGroup &BG) {
        if (VRI.V != BG.V)
          return false;

        unsigned EffRLAmt = BG.RLAmt;
        if (!VRI.Repl32 && BG.Repl32) {
          if (BG.StartIdx < 32 && BG.EndIdx < 32 && BG.StartIdx <= BG.EndIdx &&
              !BG.Repl32Coalesced) {
            if (BG.Repl32CR)
              EffRLAmt += 32;
          } else {
            return false;
          }
        } else if (VRI.Repl32 != BG.Repl32) {
          return false;
        }

        if (VRI.RLAmt != EffRLAmt)
          return false;

        return true;
      };

      for (auto &BG : BitGroups) {
        if (!MatchingBG(BG))
          continue;

        if (BG.StartIdx <= BG.EndIdx) {
          for (unsigned i = BG.StartIdx; i <= BG.EndIdx; ++i)
            Mask |= (UINT64_C(1) << i);
        } else {
          for (unsigned i = BG.StartIdx; i < Bits.size(); ++i)
            Mask |= (UINT64_C(1) << i);
          for (unsigned i = 0; i <= BG.EndIdx; ++i)
            Mask |= (UINT64_C(1) << i);
        }
      }

      // We can use the 32-bit andi/andis technique if the mask does not
      // require any higher-order bits. This can save an instruction compared
      // to always using the general 64-bit technique.
      bool Use32BitInsts = isUInt<32>(Mask);
      // Compute the masks for andi/andis that would be necessary.
      unsigned ANDIMask = (Mask & UINT16_MAX),
               ANDISMask = (Mask >> 16) & UINT16_MAX;

      bool NeedsRotate = VRI.RLAmt || (VRI.Repl32 && !isUInt<32>(Mask));

      unsigned NumAndInsts = (unsigned) NeedsRotate +
                             (unsigned) (bool) Res;
      if (Use32BitInsts)
        NumAndInsts += (unsigned) (ANDIMask != 0) + (unsigned) (ANDISMask != 0) +
                       (unsigned) (ANDIMask != 0 && ANDISMask != 0);
      else
        NumAndInsts += SelectInt64Count(Mask) + /* and */ 1;

      unsigned NumRLInsts = 0;
      bool FirstBG = true;
      for (auto &BG : BitGroups) {
        if (!MatchingBG(BG))
          continue;
        NumRLInsts +=
          SelectRotMask64Count(BG.RLAmt, BG.Repl32, BG.StartIdx, BG.EndIdx,
                               !FirstBG);
        FirstBG = false;
      }

      DEBUG(dbgs() << "\t\trotation groups for " << VRI.V.getNode() <<
                      " RL: " << VRI.RLAmt << (VRI.Repl32 ? " (32):" : ":") <<
                      "\n\t\t\tisel using masking: " << NumAndInsts <<
                      " using rotates: " << NumRLInsts << "\n");

      // When we'd use andi/andis, we bias toward using the rotates (andi only
      // has a record form, and is cracked on POWER cores). However, when using
      // general 64-bit constant formation, bias toward the constant form,
      // because that exposes more opportunities for CSE.
      if (NumAndInsts > NumRLInsts)
        continue;
      if (Use32BitInsts && NumAndInsts == NumRLInsts)
        continue;

      DEBUG(dbgs() << "\t\t\t\tusing masking\n");

      if (InstCnt) *InstCnt += NumAndInsts;

      SDValue VRot;
      // We actually need to generate a rotation if we have a non-zero rotation
      // factor or, in the Repl32 case, if we care about any of the
      // higher-order replicated bits. In the latter case, we generate a mask
      // backward so that it actually includes the entire 64 bits.
      if (VRI.RLAmt || (VRI.Repl32 && !isUInt<32>(Mask)))
        VRot = SelectRotMask64(VRI.V, dl, VRI.RLAmt, VRI.Repl32,
                               VRI.Repl32 ? 31 : 0, VRI.Repl32 ? 30 : 63);
      else
        VRot = VRI.V;

      SDValue TotalVal;
      if (Use32BitInsts) {
        assert((ANDIMask != 0 || ANDISMask != 0) &&
               "No set bits in mask when using 32-bit ands for 64-bit value");

        SDValue ANDIVal, ANDISVal;
        if (ANDIMask != 0)
          ANDIVal = SDValue(CurDAG->getMachineNode(PPC::ANDIo8, dl, MVT::i64,
                              VRot, getI32Imm(ANDIMask)), 0);
        if (ANDISMask != 0)
          ANDISVal = SDValue(CurDAG->getMachineNode(PPC::ANDISo8, dl, MVT::i64,
                               VRot, getI32Imm(ANDISMask)), 0);

        if (!ANDIVal)
          TotalVal = ANDISVal;
        else if (!ANDISVal)
          TotalVal = ANDIVal;
        else
          TotalVal = SDValue(CurDAG->getMachineNode(PPC::OR8, dl, MVT::i64,
                               ANDIVal, ANDISVal), 0);
      } else {
        TotalVal = SDValue(SelectInt64(CurDAG, dl, Mask), 0);
        TotalVal =
          SDValue(CurDAG->getMachineNode(PPC::AND8, dl, MVT::i64,
                                         VRot, TotalVal), 0);
     }

      if (!Res)
        Res = TotalVal;
      else
        Res = SDValue(CurDAG->getMachineNode(PPC::OR8, dl, MVT::i64,
                                             Res, TotalVal), 0);

      // Now, remove all groups with this underlying value and rotation
      // factor.
      for (auto I = BitGroups.begin(); I != BitGroups.end();) {
        if (MatchingBG(*I))
          I = BitGroups.erase(I);
        else
          ++I;
      }
    }
  }

  // Instruction selection for the 64-bit case.
  SDNode *Select64(SDNode *N, bool LateMask, unsigned *InstCnt) {
    SDLoc dl(N);
    SDValue Res;

    if (InstCnt) *InstCnt = 0;

    // Take care of cases that should use andi/andis first.
    SelectAndParts64(dl, Res, InstCnt);

    // If we've not yet selected a 'starting' instruction, and we have no zeros
    // to fill in, select the (Value, RLAmt) with the highest priority (largest
    // number of groups), and start with this rotated value.
    if ((!HasZeros || LateMask) && !Res) {
      // If we have both Repl32 groups and non-Repl32 groups, the non-Repl32
      // groups will come first, and so the VRI representing the largest number
      // of groups might not be first (it might be the first Repl32 groups).
      unsigned MaxGroupsIdx = 0;
      if (!ValueRotsVec[0].Repl32) {
        for (unsigned i = 0, ie = ValueRotsVec.size(); i < ie; ++i)
          if (ValueRotsVec[i].Repl32) {
            if (ValueRotsVec[i].NumGroups > ValueRotsVec[0].NumGroups)
              MaxGroupsIdx = i;
            break;
          }
      }

      ValueRotInfo &VRI = ValueRotsVec[MaxGroupsIdx];
      bool NeedsRotate = false;
      if (VRI.RLAmt) {
        NeedsRotate = true;
      } else if (VRI.Repl32) {
        for (auto &BG : BitGroups) {
          if (BG.V != VRI.V || BG.RLAmt != VRI.RLAmt ||
              BG.Repl32 != VRI.Repl32)
            continue;

          // We don't need a rotate if the bit group is confined to the lower
          // 32 bits.
          if (BG.StartIdx < 32 && BG.EndIdx < 32 && BG.StartIdx < BG.EndIdx)
            continue;

          NeedsRotate = true;
          break;
        }
      }

      if (NeedsRotate)
        Res = SelectRotMask64(VRI.V, dl, VRI.RLAmt, VRI.Repl32,
                              VRI.Repl32 ? 31 : 0, VRI.Repl32 ? 30 : 63,
                              InstCnt);
      else
        Res = VRI.V;

      // Now, remove all groups with this underlying value and rotation factor.
      if (Res)
        for (auto I = BitGroups.begin(); I != BitGroups.end();) {
          if (I->V == VRI.V && I->RLAmt == VRI.RLAmt && I->Repl32 == VRI.Repl32)
            I = BitGroups.erase(I);
          else
            ++I;
        }
    }

    // Because 64-bit rotates are more flexible than inserts, we might have a
    // preference regarding which one we do first (to save one instruction).
    if (!Res)
      for (auto I = BitGroups.begin(), IE = BitGroups.end(); I != IE; ++I) {
        if (SelectRotMask64Count(I->RLAmt, I->Repl32, I->StartIdx, I->EndIdx,
                                false) <
            SelectRotMask64Count(I->RLAmt, I->Repl32, I->StartIdx, I->EndIdx,
                                true)) {
          if (I != BitGroups.begin()) {
            BitGroup BG = *I;
            BitGroups.erase(I);
            BitGroups.insert(BitGroups.begin(), BG);
          }

          break;
        }
      }

    // Insert the other groups (one at a time).
    for (auto &BG : BitGroups) {
      if (!Res)
        Res = SelectRotMask64(BG.V, dl, BG.RLAmt, BG.Repl32, BG.StartIdx,
                              BG.EndIdx, InstCnt);
      else
        Res = SelectRotMaskIns64(Res, BG.V, dl, BG.RLAmt, BG.Repl32,
                                 BG.StartIdx, BG.EndIdx, InstCnt);
    }

    if (LateMask) {
      uint64_t Mask = getZerosMask();

      // We can use the 32-bit andi/andis technique if the mask does not
      // require any higher-order bits. This can save an instruction compared
      // to always using the general 64-bit technique.
      bool Use32BitInsts = isUInt<32>(Mask);
      // Compute the masks for andi/andis that would be necessary.
      unsigned ANDIMask = (Mask & UINT16_MAX),
               ANDISMask = (Mask >> 16) & UINT16_MAX;

      if (Use32BitInsts) {
        assert((ANDIMask != 0 || ANDISMask != 0) &&
               "No set bits in mask when using 32-bit ands for 64-bit value");

        if (InstCnt) *InstCnt += (unsigned) (ANDIMask != 0) +
                                 (unsigned) (ANDISMask != 0) +
                                 (unsigned) (ANDIMask != 0 && ANDISMask != 0);

        SDValue ANDIVal, ANDISVal;
        if (ANDIMask != 0)
          ANDIVal = SDValue(CurDAG->getMachineNode(PPC::ANDIo8, dl, MVT::i64,
                              Res, getI32Imm(ANDIMask)), 0);
        if (ANDISMask != 0)
          ANDISVal = SDValue(CurDAG->getMachineNode(PPC::ANDISo8, dl, MVT::i64,
                               Res, getI32Imm(ANDISMask)), 0);

        if (!ANDIVal)
          Res = ANDISVal;
        else if (!ANDISVal)
          Res = ANDIVal;
        else
          Res = SDValue(CurDAG->getMachineNode(PPC::OR8, dl, MVT::i64,
                          ANDIVal, ANDISVal), 0);
      } else {
        if (InstCnt) *InstCnt += SelectInt64Count(Mask) + /* and */ 1;

        SDValue MaskVal = SDValue(SelectInt64(CurDAG, dl, Mask), 0);
        Res =
          SDValue(CurDAG->getMachineNode(PPC::AND8, dl, MVT::i64,
                                         Res, MaskVal), 0);
      }
    }

    return Res.getNode();
  }

  SDNode *Select(SDNode *N, bool LateMask, unsigned *InstCnt = nullptr) {
    // Fill in BitGroups.
    collectBitGroups(LateMask);
    if (BitGroups.empty())
      return nullptr;

    // For 64-bit values, figure out when we can use 32-bit instructions.
    if (Bits.size() == 64)
      assignRepl32BitGroups();

    // Fill in ValueRotsVec.
    collectValueRotInfo();

    if (Bits.size() == 32) {
      return Select32(N, LateMask, InstCnt);
    } else {
      assert(Bits.size() == 64 && "Not 64 bits here?");
      return Select64(N, LateMask, InstCnt);
    }

    return nullptr;
  }

  SmallVector<ValueBit, 64> Bits;

  bool HasZeros;
  SmallVector<unsigned, 64> RLAmt;

  SmallVector<BitGroup, 16> BitGroups;

  DenseMap<std::pair<SDValue, unsigned>, ValueRotInfo> ValueRots;
  SmallVector<ValueRotInfo, 16> ValueRotsVec;

  SelectionDAG *CurDAG;

public:
  BitPermutationSelector(SelectionDAG *DAG)
    : CurDAG(DAG) {}

  // Here we try to match complex bit permutations into a set of
  // rotate-and-shift/shift/and/or instructions, using a set of heuristics
  // known to produce optimial code for common cases (like i32 byte swapping).
  SDNode *Select(SDNode *N) {
    Bits.resize(N->getValueType(0).getSizeInBits());
    if (!getValueBits(SDValue(N, 0), Bits))
      return nullptr;

    DEBUG(dbgs() << "Considering bit-permutation-based instruction"
                    " selection for:    ");
    DEBUG(N->dump(CurDAG));

    // Fill it RLAmt and set HasZeros.
    computeRotationAmounts();

    if (!HasZeros)
      return Select(N, false);

    // We currently have two techniques for handling results with zeros: early
    // masking (the default) and late masking. Late masking is sometimes more
    // efficient, but because the structure of the bit groups is different, it
    // is hard to tell without generating both and comparing the results. With
    // late masking, we ignore zeros in the resulting value when inserting each
    // set of bit groups, and then mask in the zeros at the end. With early
    // masking, we only insert the non-zero parts of the result at every step.

    unsigned InstCnt, InstCntLateMask;
    DEBUG(dbgs() << "\tEarly masking:\n");
    SDNode *RN = Select(N, false, &InstCnt);
    DEBUG(dbgs() << "\t\tisel would use " << InstCnt << " instructions\n");

    DEBUG(dbgs() << "\tLate masking:\n");
    SDNode *RNLM = Select(N, true, &InstCntLateMask);
    DEBUG(dbgs() << "\t\tisel would use " << InstCntLateMask <<
                    " instructions\n");

    if (InstCnt <= InstCntLateMask) {
      DEBUG(dbgs() << "\tUsing early-masking for isel\n");
      return RN;
    }

    DEBUG(dbgs() << "\tUsing late-masking for isel\n");
    return RNLM;
  }
};
} // anonymous namespace

SDNode *PPCDAGToDAGISel::SelectBitPermutation(SDNode *N) {
  if (N->getValueType(0) != MVT::i32 &&
      N->getValueType(0) != MVT::i64)
    return nullptr;

  if (!UseBitPermRewriter)
    return nullptr;

  switch (N->getOpcode()) {
  default: break;
  case ISD::ROTL:
  case ISD::SHL:
  case ISD::SRL:
  case ISD::AND:
  case ISD::OR: {
    BitPermutationSelector BPS(CurDAG);
    return BPS.Select(N);
  }
  }

  return nullptr;
}

/// SelectCC - Select a comparison of the specified values with the specified
/// condition code, returning the CR# of the expression.
SDValue PPCDAGToDAGISel::SelectCC(SDValue LHS, SDValue RHS,
                                    ISD::CondCode CC, SDLoc dl) {
  // Always select the LHS.
  unsigned Opc;

  if (LHS.getValueType() == MVT::i32) {
    unsigned Imm;
    if (CC == ISD::SETEQ || CC == ISD::SETNE) {
      if (isInt32Immediate(RHS, Imm)) {
        // SETEQ/SETNE comparison with 16-bit immediate, fold it.
        if (isUInt<16>(Imm))
          return SDValue(CurDAG->getMachineNode(PPC::CMPLWI, dl, MVT::i32, LHS,
                                                getI32Imm(Imm & 0xFFFF)), 0);
        // If this is a 16-bit signed immediate, fold it.
        if (isInt<16>((int)Imm))
          return SDValue(CurDAG->getMachineNode(PPC::CMPWI, dl, MVT::i32, LHS,
                                                getI32Imm(Imm & 0xFFFF)), 0);

        // For non-equality comparisons, the default code would materialize the
        // constant, then compare against it, like this:
        //   lis r2, 4660
        //   ori r2, r2, 22136
        //   cmpw cr0, r3, r2
        // Since we are just comparing for equality, we can emit this instead:
        //   xoris r0,r3,0x1234
        //   cmplwi cr0,r0,0x5678
        //   beq cr0,L6
        SDValue Xor(CurDAG->getMachineNode(PPC::XORIS, dl, MVT::i32, LHS,
                                           getI32Imm(Imm >> 16)), 0);
        return SDValue(CurDAG->getMachineNode(PPC::CMPLWI, dl, MVT::i32, Xor,
                                              getI32Imm(Imm & 0xFFFF)), 0);
      }
      Opc = PPC::CMPLW;
    } else if (ISD::isUnsignedIntSetCC(CC)) {
      if (isInt32Immediate(RHS, Imm) && isUInt<16>(Imm))
        return SDValue(CurDAG->getMachineNode(PPC::CMPLWI, dl, MVT::i32, LHS,
                                              getI32Imm(Imm & 0xFFFF)), 0);
      Opc = PPC::CMPLW;
    } else {
      short SImm;
      if (isIntS16Immediate(RHS, SImm))
        return SDValue(CurDAG->getMachineNode(PPC::CMPWI, dl, MVT::i32, LHS,
                                              getI32Imm((int)SImm & 0xFFFF)),
                         0);
      Opc = PPC::CMPW;
    }
  } else if (LHS.getValueType() == MVT::i64) {
    uint64_t Imm;
    if (CC == ISD::SETEQ || CC == ISD::SETNE) {
      if (isInt64Immediate(RHS.getNode(), Imm)) {
        // SETEQ/SETNE comparison with 16-bit immediate, fold it.
        if (isUInt<16>(Imm))
          return SDValue(CurDAG->getMachineNode(PPC::CMPLDI, dl, MVT::i64, LHS,
                                                getI32Imm(Imm & 0xFFFF)), 0);
        // If this is a 16-bit signed immediate, fold it.
        if (isInt<16>(Imm))
          return SDValue(CurDAG->getMachineNode(PPC::CMPDI, dl, MVT::i64, LHS,
                                                getI32Imm(Imm & 0xFFFF)), 0);

        // For non-equality comparisons, the default code would materialize the
        // constant, then compare against it, like this:
        //   lis r2, 4660
        //   ori r2, r2, 22136
        //   cmpd cr0, r3, r2
        // Since we are just comparing for equality, we can emit this instead:
        //   xoris r0,r3,0x1234
        //   cmpldi cr0,r0,0x5678
        //   beq cr0,L6
        if (isUInt<32>(Imm)) {
          SDValue Xor(CurDAG->getMachineNode(PPC::XORIS8, dl, MVT::i64, LHS,
                                             getI64Imm(Imm >> 16)), 0);
          return SDValue(CurDAG->getMachineNode(PPC::CMPLDI, dl, MVT::i64, Xor,
                                                getI64Imm(Imm & 0xFFFF)), 0);
        }
      }
      Opc = PPC::CMPLD;
    } else if (ISD::isUnsignedIntSetCC(CC)) {
      if (isInt64Immediate(RHS.getNode(), Imm) && isUInt<16>(Imm))
        return SDValue(CurDAG->getMachineNode(PPC::CMPLDI, dl, MVT::i64, LHS,
                                              getI64Imm(Imm & 0xFFFF)), 0);
      Opc = PPC::CMPLD;
    } else {
      short SImm;
      if (isIntS16Immediate(RHS, SImm))
        return SDValue(CurDAG->getMachineNode(PPC::CMPDI, dl, MVT::i64, LHS,
                                              getI64Imm(SImm & 0xFFFF)),
                         0);
      Opc = PPC::CMPD;
    }
  } else if (LHS.getValueType() == MVT::f32) {
    Opc = PPC::FCMPUS;
  } else {
    assert(LHS.getValueType() == MVT::f64 && "Unknown vt!");
    Opc = PPCSubTarget->hasVSX() ? PPC::XSCMPUDP : PPC::FCMPUD;
  }
  return SDValue(CurDAG->getMachineNode(Opc, dl, MVT::i32, LHS, RHS), 0);
}

static PPC::Predicate getPredicateForSetCC(ISD::CondCode CC) {
  switch (CC) {
  case ISD::SETUEQ:
  case ISD::SETONE:
  case ISD::SETOLE:
  case ISD::SETOGE:
    llvm_unreachable("Should be lowered by legalize!");
  default: llvm_unreachable("Unknown condition!");
  case ISD::SETOEQ:
  case ISD::SETEQ:  return PPC::PRED_EQ;
  case ISD::SETUNE:
  case ISD::SETNE:  return PPC::PRED_NE;
  case ISD::SETOLT:
  case ISD::SETLT:  return PPC::PRED_LT;
  case ISD::SETULE:
  case ISD::SETLE:  return PPC::PRED_LE;
  case ISD::SETOGT:
  case ISD::SETGT:  return PPC::PRED_GT;
  case ISD::SETUGE:
  case ISD::SETGE:  return PPC::PRED_GE;
  case ISD::SETO:   return PPC::PRED_NU;
  case ISD::SETUO:  return PPC::PRED_UN;
    // These two are invalid for floating point.  Assume we have int.
  case ISD::SETULT: return PPC::PRED_LT;
  case ISD::SETUGT: return PPC::PRED_GT;
  }
}

/// getCRIdxForSetCC - Return the index of the condition register field
/// associated with the SetCC condition, and whether or not the field is
/// treated as inverted.  That is, lt = 0; ge = 0 inverted.
static unsigned getCRIdxForSetCC(ISD::CondCode CC, bool &Invert) {
  Invert = false;
  switch (CC) {
  default: llvm_unreachable("Unknown condition!");
  case ISD::SETOLT:
  case ISD::SETLT:  return 0;                  // Bit #0 = SETOLT
  case ISD::SETOGT:
  case ISD::SETGT:  return 1;                  // Bit #1 = SETOGT
  case ISD::SETOEQ:
  case ISD::SETEQ:  return 2;                  // Bit #2 = SETOEQ
  case ISD::SETUO:  return 3;                  // Bit #3 = SETUO
  case ISD::SETUGE:
  case ISD::SETGE:  Invert = true; return 0;   // !Bit #0 = SETUGE
  case ISD::SETULE:
  case ISD::SETLE:  Invert = true; return 1;   // !Bit #1 = SETULE
  case ISD::SETUNE:
  case ISD::SETNE:  Invert = true; return 2;   // !Bit #2 = SETUNE
  case ISD::SETO:   Invert = true; return 3;   // !Bit #3 = SETO
  case ISD::SETUEQ:
  case ISD::SETOGE:
  case ISD::SETOLE:
  case ISD::SETONE:
    llvm_unreachable("Invalid branch code: should be expanded by legalize");
  // These are invalid for floating point.  Assume integer.
  case ISD::SETULT: return 0;
  case ISD::SETUGT: return 1;
  }
}

// getVCmpInst: return the vector compare instruction for the specified
// vector type and condition code. Since this is for altivec specific code,
// only support the altivec types (v16i8, v8i16, v4i32, and v4f32).
static unsigned int getVCmpInst(MVT VecVT, ISD::CondCode CC,
                                bool HasVSX, bool &Swap, bool &Negate) {
  Swap = false;
  Negate = false;

  if (VecVT.isFloatingPoint()) {
    /* Handle some cases by swapping input operands.  */
    switch (CC) {
      case ISD::SETLE: CC = ISD::SETGE; Swap = true; break;
      case ISD::SETLT: CC = ISD::SETGT; Swap = true; break;
      case ISD::SETOLE: CC = ISD::SETOGE; Swap = true; break;
      case ISD::SETOLT: CC = ISD::SETOGT; Swap = true; break;
      case ISD::SETUGE: CC = ISD::SETULE; Swap = true; break;
      case ISD::SETUGT: CC = ISD::SETULT; Swap = true; break;
      default: break;
    }
    /* Handle some cases by negating the result.  */
    switch (CC) {
      case ISD::SETNE: CC = ISD::SETEQ; Negate = true; break;
      case ISD::SETUNE: CC = ISD::SETOEQ; Negate = true; break;
      case ISD::SETULE: CC = ISD::SETOGT; Negate = true; break;
      case ISD::SETULT: CC = ISD::SETOGE; Negate = true; break;
      default: break;
    }
    /* We have instructions implementing the remaining cases.  */
    switch (CC) {
      case ISD::SETEQ:
      case ISD::SETOEQ:
        if (VecVT == MVT::v4f32)
          return HasVSX ? PPC::XVCMPEQSP : PPC::VCMPEQFP;
        else if (VecVT == MVT::v2f64)
          return PPC::XVCMPEQDP;
        break;
      case ISD::SETGT:
      case ISD::SETOGT:
        if (VecVT == MVT::v4f32)
          return HasVSX ? PPC::XVCMPGTSP : PPC::VCMPGTFP;
        else if (VecVT == MVT::v2f64)
          return PPC::XVCMPGTDP;
        break;
      case ISD::SETGE:
      case ISD::SETOGE:
        if (VecVT == MVT::v4f32)
          return HasVSX ? PPC::XVCMPGESP : PPC::VCMPGEFP;
        else if (VecVT == MVT::v2f64)
          return PPC::XVCMPGEDP;
        break;
      default:
        break;
    }
    llvm_unreachable("Invalid floating-point vector compare condition");
  } else {
    /* Handle some cases by swapping input operands.  */
    switch (CC) {
      case ISD::SETGE: CC = ISD::SETLE; Swap = true; break;
      case ISD::SETLT: CC = ISD::SETGT; Swap = true; break;
      case ISD::SETUGE: CC = ISD::SETULE; Swap = true; break;
      case ISD::SETULT: CC = ISD::SETUGT; Swap = true; break;
      default: break;
    }
    /* Handle some cases by negating the result.  */
    switch (CC) {
      case ISD::SETNE: CC = ISD::SETEQ; Negate = true; break;
      case ISD::SETUNE: CC = ISD::SETUEQ; Negate = true; break;
      case ISD::SETLE: CC = ISD::SETGT; Negate = true; break;
      case ISD::SETULE: CC = ISD::SETUGT; Negate = true; break;
      default: break;
    }
    /* We have instructions implementing the remaining cases.  */
    switch (CC) {
      case ISD::SETEQ:
      case ISD::SETUEQ:
        if (VecVT == MVT::v16i8)
          return PPC::VCMPEQUB;
        else if (VecVT == MVT::v8i16)
          return PPC::VCMPEQUH;
        else if (VecVT == MVT::v4i32)
          return PPC::VCMPEQUW;
        break;
      case ISD::SETGT:
        if (VecVT == MVT::v16i8)
          return PPC::VCMPGTSB;
        else if (VecVT == MVT::v8i16)
          return PPC::VCMPGTSH;
        else if (VecVT == MVT::v4i32)
          return PPC::VCMPGTSW;
        break;
      case ISD::SETUGT:
        if (VecVT == MVT::v16i8)
          return PPC::VCMPGTUB;
        else if (VecVT == MVT::v8i16)
          return PPC::VCMPGTUH;
        else if (VecVT == MVT::v4i32)
          return PPC::VCMPGTUW;
        break;
      default:
        break;
    }
    llvm_unreachable("Invalid integer vector compare condition");
  }
}

SDNode *PPCDAGToDAGISel::SelectSETCC(SDNode *N) {
  SDLoc dl(N);
  unsigned Imm;
  ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(2))->get();
  EVT PtrVT = CurDAG->getTargetLoweringInfo().getPointerTy();
  bool isPPC64 = (PtrVT == MVT::i64);

  if (!PPCSubTarget->useCRBits() &&
      isInt32Immediate(N->getOperand(1), Imm)) {
    // We can codegen setcc op, imm very efficiently compared to a brcond.
    // Check for those cases here.
    // setcc op, 0
    if (Imm == 0) {
      SDValue Op = N->getOperand(0);
      switch (CC) {
      default: break;
      case ISD::SETEQ: {
        Op = SDValue(CurDAG->getMachineNode(PPC::CNTLZW, dl, MVT::i32, Op), 0);
        SDValue Ops[] = { Op, getI32Imm(27), getI32Imm(5), getI32Imm(31) };
        return CurDAG->SelectNodeTo(N, PPC::RLWINM, MVT::i32, Ops);
      }
      case ISD::SETNE: {
        if (isPPC64) break;
        SDValue AD =
          SDValue(CurDAG->getMachineNode(PPC::ADDIC, dl, MVT::i32, MVT::Glue,
                                         Op, getI32Imm(~0U)), 0);
        return CurDAG->SelectNodeTo(N, PPC::SUBFE, MVT::i32, AD, Op,
                                    AD.getValue(1));
      }
      case ISD::SETLT: {
        SDValue Ops[] = { Op, getI32Imm(1), getI32Imm(31), getI32Imm(31) };
        return CurDAG->SelectNodeTo(N, PPC::RLWINM, MVT::i32, Ops);
      }
      case ISD::SETGT: {
        SDValue T =
          SDValue(CurDAG->getMachineNode(PPC::NEG, dl, MVT::i32, Op), 0);
        T = SDValue(CurDAG->getMachineNode(PPC::ANDC, dl, MVT::i32, T, Op), 0);
        SDValue Ops[] = { T, getI32Imm(1), getI32Imm(31), getI32Imm(31) };
        return CurDAG->SelectNodeTo(N, PPC::RLWINM, MVT::i32, Ops);
      }
      }
    } else if (Imm == ~0U) {        // setcc op, -1
      SDValue Op = N->getOperand(0);
      switch (CC) {
      default: break;
      case ISD::SETEQ:
        if (isPPC64) break;
        Op = SDValue(CurDAG->getMachineNode(PPC::ADDIC, dl, MVT::i32, MVT::Glue,
                                            Op, getI32Imm(1)), 0);
        return CurDAG->SelectNodeTo(N, PPC::ADDZE, MVT::i32,
                              SDValue(CurDAG->getMachineNode(PPC::LI, dl,
                                                             MVT::i32,
                                                             getI32Imm(0)), 0),
                                      Op.getValue(1));
      case ISD::SETNE: {
        if (isPPC64) break;
        Op = SDValue(CurDAG->getMachineNode(PPC::NOR, dl, MVT::i32, Op, Op), 0);
        SDNode *AD = CurDAG->getMachineNode(PPC::ADDIC, dl, MVT::i32, MVT::Glue,
                                            Op, getI32Imm(~0U));
        return CurDAG->SelectNodeTo(N, PPC::SUBFE, MVT::i32, SDValue(AD, 0),
                                    Op, SDValue(AD, 1));
      }
      case ISD::SETLT: {
        SDValue AD = SDValue(CurDAG->getMachineNode(PPC::ADDI, dl, MVT::i32, Op,
                                                    getI32Imm(1)), 0);
        SDValue AN = SDValue(CurDAG->getMachineNode(PPC::AND, dl, MVT::i32, AD,
                                                    Op), 0);
        SDValue Ops[] = { AN, getI32Imm(1), getI32Imm(31), getI32Imm(31) };
        return CurDAG->SelectNodeTo(N, PPC::RLWINM, MVT::i32, Ops);
      }
      case ISD::SETGT: {
        SDValue Ops[] = { Op, getI32Imm(1), getI32Imm(31), getI32Imm(31) };
        Op = SDValue(CurDAG->getMachineNode(PPC::RLWINM, dl, MVT::i32, Ops),
                     0);
        return CurDAG->SelectNodeTo(N, PPC::XORI, MVT::i32, Op,
                                    getI32Imm(1));
      }
      }
    }
  }

  SDValue LHS = N->getOperand(0);
  SDValue RHS = N->getOperand(1);

  // Altivec Vector compare instructions do not set any CR register by default and
  // vector compare operations return the same type as the operands.
  if (LHS.getValueType().isVector()) {
    EVT VecVT = LHS.getValueType();
    bool Swap, Negate;
    unsigned int VCmpInst = getVCmpInst(VecVT.getSimpleVT(), CC,
                                        PPCSubTarget->hasVSX(), Swap, Negate);
    if (Swap)
      std::swap(LHS, RHS);

    if (Negate) {
      SDValue VCmp(CurDAG->getMachineNode(VCmpInst, dl, VecVT, LHS, RHS), 0);
      return CurDAG->SelectNodeTo(N, PPCSubTarget->hasVSX() ? PPC::XXLNOR :
                                                              PPC::VNOR,
                                  VecVT, VCmp, VCmp);
    }

    return CurDAG->SelectNodeTo(N, VCmpInst, VecVT, LHS, RHS);
  }

  if (PPCSubTarget->useCRBits())
    return nullptr;

  bool Inv;
  unsigned Idx = getCRIdxForSetCC(CC, Inv);
  SDValue CCReg = SelectCC(LHS, RHS, CC, dl);
  SDValue IntCR;

  // Force the ccreg into CR7.
  SDValue CR7Reg = CurDAG->getRegister(PPC::CR7, MVT::i32);

  SDValue InFlag(nullptr, 0);  // Null incoming flag value.
  CCReg = CurDAG->getCopyToReg(CurDAG->getEntryNode(), dl, CR7Reg, CCReg,
                               InFlag).getValue(1);

  IntCR = SDValue(CurDAG->getMachineNode(PPC::MFOCRF, dl, MVT::i32, CR7Reg,
                                         CCReg), 0);

  SDValue Ops[] = { IntCR, getI32Imm((32-(3-Idx)) & 31),
                      getI32Imm(31), getI32Imm(31) };
  if (!Inv)
    return CurDAG->SelectNodeTo(N, PPC::RLWINM, MVT::i32, Ops);

  // Get the specified bit.
  SDValue Tmp =
    SDValue(CurDAG->getMachineNode(PPC::RLWINM, dl, MVT::i32, Ops), 0);
  return CurDAG->SelectNodeTo(N, PPC::XORI, MVT::i32, Tmp, getI32Imm(1));
}


// Select - Convert the specified operand from a target-independent to a
// target-specific node if it hasn't already been changed.
SDNode *PPCDAGToDAGISel::Select(SDNode *N) {
  SDLoc dl(N);
  if (N->isMachineOpcode()) {
    N->setNodeId(-1);
    return nullptr;   // Already selected.
  }

  // In case any misguided DAG-level optimizations form an ADD with a
  // TargetConstant operand, crash here instead of miscompiling (by selecting
  // an r+r add instead of some kind of r+i add).
  if (N->getOpcode() == ISD::ADD &&
      N->getOperand(1).getOpcode() == ISD::TargetConstant)
    llvm_unreachable("Invalid ADD with TargetConstant operand");

  // Try matching complex bit permutations before doing anything else.
  if (SDNode *NN = SelectBitPermutation(N))
    return NN;

  switch (N->getOpcode()) {
  default: break;

  case ISD::Constant: {
    if (N->getValueType(0) == MVT::i64)
      return SelectInt64(CurDAG, N);
    break;
  }

  case ISD::SETCC: {
    SDNode *SN = SelectSETCC(N);
    if (SN)
      return SN;
    break;
  }
  case PPCISD::GlobalBaseReg:
    return getGlobalBaseReg();

  case ISD::FrameIndex:
    return getFrameIndex(N, N);

  case PPCISD::MFOCRF: {
    SDValue InFlag = N->getOperand(1);
    return CurDAG->getMachineNode(PPC::MFOCRF, dl, MVT::i32,
                                  N->getOperand(0), InFlag);
  }

  case PPCISD::READ_TIME_BASE: {
    return CurDAG->getMachineNode(PPC::ReadTB, dl, MVT::i32, MVT::i32,
                                  MVT::Other, N->getOperand(0));
  }

  case PPCISD::SRA_ADDZE: {
    SDValue N0 = N->getOperand(0);
    SDValue ShiftAmt =
      CurDAG->getTargetConstant(*cast<ConstantSDNode>(N->getOperand(1))->
                                  getConstantIntValue(), N->getValueType(0));
    if (N->getValueType(0) == MVT::i64) {
      SDNode *Op =
        CurDAG->getMachineNode(PPC::SRADI, dl, MVT::i64, MVT::Glue,
                               N0, ShiftAmt);
      return CurDAG->SelectNodeTo(N, PPC::ADDZE8, MVT::i64,
                                  SDValue(Op, 0), SDValue(Op, 1));
    } else {
      assert(N->getValueType(0) == MVT::i32 &&
             "Expecting i64 or i32 in PPCISD::SRA_ADDZE");
      SDNode *Op =
        CurDAG->getMachineNode(PPC::SRAWI, dl, MVT::i32, MVT::Glue,
                               N0, ShiftAmt);
      return CurDAG->SelectNodeTo(N, PPC::ADDZE, MVT::i32,
                                  SDValue(Op, 0), SDValue(Op, 1));
    }
  }

  case ISD::LOAD: {
    // Handle preincrement loads.
    LoadSDNode *LD = cast<LoadSDNode>(N);
    EVT LoadedVT = LD->getMemoryVT();

    // Normal loads are handled by code generated from the .td file.
    if (LD->getAddressingMode() != ISD::PRE_INC)
      break;

    SDValue Offset = LD->getOffset();
    if (Offset.getOpcode() == ISD::TargetConstant ||
        Offset.getOpcode() == ISD::TargetGlobalAddress) {

      unsigned Opcode;
      bool isSExt = LD->getExtensionType() == ISD::SEXTLOAD;
      if (LD->getValueType(0) != MVT::i64) {
        // Handle PPC32 integer and normal FP loads.
        assert((!isSExt || LoadedVT == MVT::i16) && "Invalid sext update load");
        switch (LoadedVT.getSimpleVT().SimpleTy) {
          default: llvm_unreachable("Invalid PPC load type!");
          case MVT::f64: Opcode = PPC::LFDU; break;
          case MVT::f32: Opcode = PPC::LFSU; break;
          case MVT::i32: Opcode = PPC::LWZU; break;
          case MVT::i16: Opcode = isSExt ? PPC::LHAU : PPC::LHZU; break;
          case MVT::i1:
          case MVT::i8:  Opcode = PPC::LBZU; break;
        }
      } else {
        assert(LD->getValueType(0) == MVT::i64 && "Unknown load result type!");
        assert((!isSExt || LoadedVT == MVT::i16) && "Invalid sext update load");
        switch (LoadedVT.getSimpleVT().SimpleTy) {
          default: llvm_unreachable("Invalid PPC load type!");
          case MVT::i64: Opcode = PPC::LDU; break;
          case MVT::i32: Opcode = PPC::LWZU8; break;
          case MVT::i16: Opcode = isSExt ? PPC::LHAU8 : PPC::LHZU8; break;
          case MVT::i1:
          case MVT::i8:  Opcode = PPC::LBZU8; break;
        }
      }

      SDValue Chain = LD->getChain();
      SDValue Base = LD->getBasePtr();
      SDValue Ops[] = { Offset, Base, Chain };
      return CurDAG->getMachineNode(Opcode, dl, LD->getValueType(0),
                                    PPCLowering->getPointerTy(),
                                    MVT::Other, Ops);
    } else {
      unsigned Opcode;
      bool isSExt = LD->getExtensionType() == ISD::SEXTLOAD;
      if (LD->getValueType(0) != MVT::i64) {
        // Handle PPC32 integer and normal FP loads.
        assert((!isSExt || LoadedVT == MVT::i16) && "Invalid sext update load");
        switch (LoadedVT.getSimpleVT().SimpleTy) {
          default: llvm_unreachable("Invalid PPC load type!");
          case MVT::f64: Opcode = PPC::LFDUX; break;
          case MVT::f32: Opcode = PPC::LFSUX; break;
          case MVT::i32: Opcode = PPC::LWZUX; break;
          case MVT::i16: Opcode = isSExt ? PPC::LHAUX : PPC::LHZUX; break;
          case MVT::i1:
          case MVT::i8:  Opcode = PPC::LBZUX; break;
        }
      } else {
        assert(LD->getValueType(0) == MVT::i64 && "Unknown load result type!");
        assert((!isSExt || LoadedVT == MVT::i16 || LoadedVT == MVT::i32) &&
               "Invalid sext update load");
        switch (LoadedVT.getSimpleVT().SimpleTy) {
          default: llvm_unreachable("Invalid PPC load type!");
          case MVT::i64: Opcode = PPC::LDUX; break;
          case MVT::i32: Opcode = isSExt ? PPC::LWAUX  : PPC::LWZUX8; break;
          case MVT::i16: Opcode = isSExt ? PPC::LHAUX8 : PPC::LHZUX8; break;
          case MVT::i1:
          case MVT::i8:  Opcode = PPC::LBZUX8; break;
        }
      }

      SDValue Chain = LD->getChain();
      SDValue Base = LD->getBasePtr();
      SDValue Ops[] = { Base, Offset, Chain };
      return CurDAG->getMachineNode(Opcode, dl, LD->getValueType(0),
                                    PPCLowering->getPointerTy(),
                                    MVT::Other, Ops);
    }
  }

  case ISD::AND: {
    unsigned Imm, Imm2, SH, MB, ME;
    uint64_t Imm64;

    // If this is an and of a value rotated between 0 and 31 bits and then and'd
    // with a mask, emit rlwinm
    if (isInt32Immediate(N->getOperand(1), Imm) &&
        isRotateAndMask(N->getOperand(0).getNode(), Imm, false, SH, MB, ME)) {
      SDValue Val = N->getOperand(0).getOperand(0);
      SDValue Ops[] = { Val, getI32Imm(SH), getI32Imm(MB), getI32Imm(ME) };
      return CurDAG->SelectNodeTo(N, PPC::RLWINM, MVT::i32, Ops);
    }
    // If this is just a masked value where the input is not handled above, and
    // is not a rotate-left (handled by a pattern in the .td file), emit rlwinm
    if (isInt32Immediate(N->getOperand(1), Imm) &&
        isRunOfOnes(Imm, MB, ME) &&
        N->getOperand(0).getOpcode() != ISD::ROTL) {
      SDValue Val = N->getOperand(0);
      SDValue Ops[] = { Val, getI32Imm(0), getI32Imm(MB), getI32Imm(ME) };
      return CurDAG->SelectNodeTo(N, PPC::RLWINM, MVT::i32, Ops);
    }
    // If this is a 64-bit zero-extension mask, emit rldicl.
    if (isInt64Immediate(N->getOperand(1).getNode(), Imm64) &&
        isMask_64(Imm64)) {
      SDValue Val = N->getOperand(0);
      MB = 64 - CountTrailingOnes_64(Imm64);
      SH = 0;

      // If the operand is a logical right shift, we can fold it into this
      // instruction: rldicl(rldicl(x, 64-n, n), 0, mb) -> rldicl(x, 64-n, mb)
      // for n <= mb. The right shift is really a left rotate followed by a
      // mask, and this mask is a more-restrictive sub-mask of the mask implied
      // by the shift.
      if (Val.getOpcode() == ISD::SRL &&
          isInt32Immediate(Val.getOperand(1).getNode(), Imm) && Imm <= MB) {
        assert(Imm < 64 && "Illegal shift amount");
        Val = Val.getOperand(0);
        SH = 64 - Imm;
      }

      SDValue Ops[] = { Val, getI32Imm(SH), getI32Imm(MB) };
      return CurDAG->SelectNodeTo(N, PPC::RLDICL, MVT::i64, Ops);
    }
    // AND X, 0 -> 0, not "rlwinm 32".
    if (isInt32Immediate(N->getOperand(1), Imm) && (Imm == 0)) {
      ReplaceUses(SDValue(N, 0), N->getOperand(1));
      return nullptr;
    }
    // ISD::OR doesn't get all the bitfield insertion fun.
    // (and (or x, c1), c2) where isRunOfOnes(~(c1^c2)) is a bitfield insert
    if (isInt32Immediate(N->getOperand(1), Imm) &&
        N->getOperand(0).getOpcode() == ISD::OR &&
        isInt32Immediate(N->getOperand(0).getOperand(1), Imm2)) {
      unsigned MB, ME;
      Imm = ~(Imm^Imm2);
      if (isRunOfOnes(Imm, MB, ME)) {
        SDValue Ops[] = { N->getOperand(0).getOperand(0),
                            N->getOperand(0).getOperand(1),
                            getI32Imm(0), getI32Imm(MB),getI32Imm(ME) };
        return CurDAG->getMachineNode(PPC::RLWIMI, dl, MVT::i32, Ops);
      }
    }

    // Other cases are autogenerated.
    break;
  }
  case ISD::OR: {
    if (N->getValueType(0) == MVT::i32)
      if (SDNode *I = SelectBitfieldInsert(N))
        return I;

    short Imm;
    if (N->getOperand(0)->getOpcode() == ISD::FrameIndex &&
        isIntS16Immediate(N->getOperand(1), Imm)) {
      APInt LHSKnownZero, LHSKnownOne;
      CurDAG->computeKnownBits(N->getOperand(0), LHSKnownZero, LHSKnownOne);

      // If this is equivalent to an add, then we can fold it with the
      // FrameIndex calculation.
      if ((LHSKnownZero.getZExtValue()|~(uint64_t)Imm) == ~0ULL)
        return getFrameIndex(N, N->getOperand(0).getNode(), (int)Imm);
    }

    // Other cases are autogenerated.
    break;
  }
  case ISD::ADD: {
    short Imm;
    if (N->getOperand(0)->getOpcode() == ISD::FrameIndex &&
        isIntS16Immediate(N->getOperand(1), Imm))
      return getFrameIndex(N, N->getOperand(0).getNode(), (int)Imm);

    break;
  }
  case ISD::SHL: {
    unsigned Imm, SH, MB, ME;
    if (isOpcWithIntImmediate(N->getOperand(0).getNode(), ISD::AND, Imm) &&
        isRotateAndMask(N, Imm, true, SH, MB, ME)) {
      SDValue Ops[] = { N->getOperand(0).getOperand(0),
                          getI32Imm(SH), getI32Imm(MB), getI32Imm(ME) };
      return CurDAG->SelectNodeTo(N, PPC::RLWINM, MVT::i32, Ops);
    }

    // Other cases are autogenerated.
    break;
  }
  case ISD::SRL: {
    unsigned Imm, SH, MB, ME;
    if (isOpcWithIntImmediate(N->getOperand(0).getNode(), ISD::AND, Imm) &&
        isRotateAndMask(N, Imm, true, SH, MB, ME)) {
      SDValue Ops[] = { N->getOperand(0).getOperand(0),
                          getI32Imm(SH), getI32Imm(MB), getI32Imm(ME) };
      return CurDAG->SelectNodeTo(N, PPC::RLWINM, MVT::i32, Ops);
    }

    // Other cases are autogenerated.
    break;
  }
  // FIXME: Remove this once the ANDI glue bug is fixed:
  case PPCISD::ANDIo_1_EQ_BIT:
  case PPCISD::ANDIo_1_GT_BIT: {
    if (!ANDIGlueBug)
      break;

    EVT InVT = N->getOperand(0).getValueType();
    assert((InVT == MVT::i64 || InVT == MVT::i32) &&
           "Invalid input type for ANDIo_1_EQ_BIT");

    unsigned Opcode = (InVT == MVT::i64) ? PPC::ANDIo8 : PPC::ANDIo;
    SDValue AndI(CurDAG->getMachineNode(Opcode, dl, InVT, MVT::Glue,
                                        N->getOperand(0),
                                        CurDAG->getTargetConstant(1, InVT)), 0);
    SDValue CR0Reg = CurDAG->getRegister(PPC::CR0, MVT::i32);
    SDValue SRIdxVal =
      CurDAG->getTargetConstant(N->getOpcode() == PPCISD::ANDIo_1_EQ_BIT ?
                                PPC::sub_eq : PPC::sub_gt, MVT::i32);

    return CurDAG->SelectNodeTo(N, TargetOpcode::EXTRACT_SUBREG, MVT::i1,
                                CR0Reg, SRIdxVal,
                                SDValue(AndI.getNode(), 1) /* glue */);
  }
  case ISD::SELECT_CC: {
    ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(4))->get();
    EVT PtrVT = CurDAG->getTargetLoweringInfo().getPointerTy();
    bool isPPC64 = (PtrVT == MVT::i64);

    // If this is a select of i1 operands, we'll pattern match it.
    if (PPCSubTarget->useCRBits() &&
        N->getOperand(0).getValueType() == MVT::i1)
      break;

    // Handle the setcc cases here.  select_cc lhs, 0, 1, 0, cc
    if (!isPPC64)
      if (ConstantSDNode *N1C = dyn_cast<ConstantSDNode>(N->getOperand(1)))
        if (ConstantSDNode *N2C = dyn_cast<ConstantSDNode>(N->getOperand(2)))
          if (ConstantSDNode *N3C = dyn_cast<ConstantSDNode>(N->getOperand(3)))
            if (N1C->isNullValue() && N3C->isNullValue() &&
                N2C->getZExtValue() == 1ULL && CC == ISD::SETNE &&
                // FIXME: Implement this optzn for PPC64.
                N->getValueType(0) == MVT::i32) {
              SDNode *Tmp =
                CurDAG->getMachineNode(PPC::ADDIC, dl, MVT::i32, MVT::Glue,
                                       N->getOperand(0), getI32Imm(~0U));
              return CurDAG->SelectNodeTo(N, PPC::SUBFE, MVT::i32,
                                          SDValue(Tmp, 0), N->getOperand(0),
                                          SDValue(Tmp, 1));
            }

    SDValue CCReg = SelectCC(N->getOperand(0), N->getOperand(1), CC, dl);

    if (N->getValueType(0) == MVT::i1) {
      // An i1 select is: (c & t) | (!c & f).
      bool Inv;
      unsigned Idx = getCRIdxForSetCC(CC, Inv);

      unsigned SRI;
      switch (Idx) {
      default: llvm_unreachable("Invalid CC index");
      case 0: SRI = PPC::sub_lt; break;
      case 1: SRI = PPC::sub_gt; break;
      case 2: SRI = PPC::sub_eq; break;
      case 3: SRI = PPC::sub_un; break;
      }

      SDValue CCBit = CurDAG->getTargetExtractSubreg(SRI, dl, MVT::i1, CCReg);

      SDValue NotCCBit(CurDAG->getMachineNode(PPC::CRNOR, dl, MVT::i1,
                                              CCBit, CCBit), 0);
      SDValue C =    Inv ? NotCCBit : CCBit,
              NotC = Inv ? CCBit    : NotCCBit;

      SDValue CAndT(CurDAG->getMachineNode(PPC::CRAND, dl, MVT::i1,
                                           C, N->getOperand(2)), 0);
      SDValue NotCAndF(CurDAG->getMachineNode(PPC::CRAND, dl, MVT::i1,
                                              NotC, N->getOperand(3)), 0);

      return CurDAG->SelectNodeTo(N, PPC::CROR, MVT::i1, CAndT, NotCAndF);
    }

    unsigned BROpc = getPredicateForSetCC(CC);

    unsigned SelectCCOp;
    if (N->getValueType(0) == MVT::i32)
      SelectCCOp = PPC::SELECT_CC_I4;
    else if (N->getValueType(0) == MVT::i64)
      SelectCCOp = PPC::SELECT_CC_I8;
    else if (N->getValueType(0) == MVT::f32)
      SelectCCOp = PPC::SELECT_CC_F4;
    else if (N->getValueType(0) == MVT::f64)
      if (PPCSubTarget->hasVSX())
        SelectCCOp = PPC::SELECT_CC_VSFRC;
      else
        SelectCCOp = PPC::SELECT_CC_F8;
    else if (N->getValueType(0) == MVT::v2f64 ||
             N->getValueType(0) == MVT::v2i64)
      SelectCCOp = PPC::SELECT_CC_VSRC;
    else
      SelectCCOp = PPC::SELECT_CC_VRRC;

    SDValue Ops[] = { CCReg, N->getOperand(2), N->getOperand(3),
                        getI32Imm(BROpc) };
    return CurDAG->SelectNodeTo(N, SelectCCOp, N->getValueType(0), Ops);
  }
  case ISD::VSELECT:
    if (PPCSubTarget->hasVSX()) {
      SDValue Ops[] = { N->getOperand(2), N->getOperand(1), N->getOperand(0) };
      return CurDAG->SelectNodeTo(N, PPC::XXSEL, N->getValueType(0), Ops);
    }

    break;
  case ISD::VECTOR_SHUFFLE:
    if (PPCSubTarget->hasVSX() && (N->getValueType(0) == MVT::v2f64 ||
                                  N->getValueType(0) == MVT::v2i64)) {
      ShuffleVectorSDNode *SVN = cast<ShuffleVectorSDNode>(N);
      
      SDValue Op1 = N->getOperand(SVN->getMaskElt(0) < 2 ? 0 : 1),
              Op2 = N->getOperand(SVN->getMaskElt(1) < 2 ? 0 : 1);
      unsigned DM[2];

      for (int i = 0; i < 2; ++i)
        if (SVN->getMaskElt(i) <= 0 || SVN->getMaskElt(i) == 2)
          DM[i] = 0;
        else
          DM[i] = 1;

      // For little endian, we must swap the input operands and adjust
      // the mask elements (reverse and invert them).
      if (PPCSubTarget->isLittleEndian()) {
        std::swap(Op1, Op2);
        unsigned tmp = DM[0];
        DM[0] = 1 - DM[1];
        DM[1] = 1 - tmp;
      }

      SDValue DMV = CurDAG->getTargetConstant(DM[1] | (DM[0] << 1), MVT::i32);

      if (Op1 == Op2 && DM[0] == 0 && DM[1] == 0 &&
          Op1.getOpcode() == ISD::SCALAR_TO_VECTOR &&
          isa<LoadSDNode>(Op1.getOperand(0))) {
        LoadSDNode *LD = cast<LoadSDNode>(Op1.getOperand(0));
        SDValue Base, Offset;

        if (LD->isUnindexed() &&
            SelectAddrIdxOnly(LD->getBasePtr(), Base, Offset)) {
          SDValue Chain = LD->getChain();
          SDValue Ops[] = { Base, Offset, Chain };
          return CurDAG->SelectNodeTo(N, PPC::LXVDSX,
                                      N->getValueType(0), Ops);
        }
      }

      SDValue Ops[] = { Op1, Op2, DMV };
      return CurDAG->SelectNodeTo(N, PPC::XXPERMDI, N->getValueType(0), Ops);
    }

    break;
  case PPCISD::BDNZ:
  case PPCISD::BDZ: {
    bool IsPPC64 = PPCSubTarget->isPPC64();
    SDValue Ops[] = { N->getOperand(1), N->getOperand(0) };
    return CurDAG->SelectNodeTo(N, N->getOpcode() == PPCISD::BDNZ ?
                                   (IsPPC64 ? PPC::BDNZ8 : PPC::BDNZ) :
                                   (IsPPC64 ? PPC::BDZ8 : PPC::BDZ),
                                MVT::Other, Ops);
  }
  case PPCISD::COND_BRANCH: {
    // Op #0 is the Chain.
    // Op #1 is the PPC::PRED_* number.
    // Op #2 is the CR#
    // Op #3 is the Dest MBB
    // Op #4 is the Flag.
    // Prevent PPC::PRED_* from being selected into LI.
    SDValue Pred =
      getI32Imm(cast<ConstantSDNode>(N->getOperand(1))->getZExtValue());
    SDValue Ops[] = { Pred, N->getOperand(2), N->getOperand(3),
      N->getOperand(0), N->getOperand(4) };
    return CurDAG->SelectNodeTo(N, PPC::BCC, MVT::Other, Ops);
  }
  case ISD::BR_CC: {
    ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(1))->get();
    unsigned PCC = getPredicateForSetCC(CC);

    if (N->getOperand(2).getValueType() == MVT::i1) {
      unsigned Opc;
      bool Swap;
      switch (PCC) {
      default: llvm_unreachable("Unexpected Boolean-operand predicate");
      case PPC::PRED_LT: Opc = PPC::CRANDC; Swap = true;  break;
      case PPC::PRED_LE: Opc = PPC::CRORC;  Swap = true;  break;
      case PPC::PRED_EQ: Opc = PPC::CREQV;  Swap = false; break;
      case PPC::PRED_GE: Opc = PPC::CRORC;  Swap = false; break;
      case PPC::PRED_GT: Opc = PPC::CRANDC; Swap = false; break;
      case PPC::PRED_NE: Opc = PPC::CRXOR;  Swap = false; break;
      }

      SDValue BitComp(CurDAG->getMachineNode(Opc, dl, MVT::i1,
                                             N->getOperand(Swap ? 3 : 2),
                                             N->getOperand(Swap ? 2 : 3)), 0);
      return CurDAG->SelectNodeTo(N, PPC::BC, MVT::Other,
                                  BitComp, N->getOperand(4), N->getOperand(0));
    }

    SDValue CondCode = SelectCC(N->getOperand(2), N->getOperand(3), CC, dl);
    SDValue Ops[] = { getI32Imm(PCC), CondCode,
                        N->getOperand(4), N->getOperand(0) };
    return CurDAG->SelectNodeTo(N, PPC::BCC, MVT::Other, Ops);
  }
  case ISD::BRIND: {
    // FIXME: Should custom lower this.
    SDValue Chain = N->getOperand(0);
    SDValue Target = N->getOperand(1);
    unsigned Opc = Target.getValueType() == MVT::i32 ? PPC::MTCTR : PPC::MTCTR8;
    unsigned Reg = Target.getValueType() == MVT::i32 ? PPC::BCTR : PPC::BCTR8;
    Chain = SDValue(CurDAG->getMachineNode(Opc, dl, MVT::Glue, Target,
                                           Chain), 0);
    return CurDAG->SelectNodeTo(N, Reg, MVT::Other, Chain);
  }
  case PPCISD::TOC_ENTRY: {
    assert ((PPCSubTarget->isPPC64() || PPCSubTarget->isSVR4ABI()) &&
            "Only supported for 64-bit ABI and 32-bit SVR4");
    if (PPCSubTarget->isSVR4ABI() && !PPCSubTarget->isPPC64()) {
      SDValue GA = N->getOperand(0);
      return CurDAG->getMachineNode(PPC::LWZtoc, dl, MVT::i32, GA,
                                    N->getOperand(1));
    }

    // For medium and large code model, we generate two instructions as
    // described below.  Otherwise we allow SelectCodeCommon to handle this,
    // selecting one of LDtoc, LDtocJTI, LDtocCPT, and LDtocBA.
    CodeModel::Model CModel = TM.getCodeModel();
    if (CModel != CodeModel::Medium && CModel != CodeModel::Large)
      break;

    // The first source operand is a TargetGlobalAddress or a TargetJumpTable.
    // If it is an externally defined symbol, a symbol with common linkage,
    // a non-local function address, or a jump table address, or if we are
    // generating code for large code model, we generate:
    //   LDtocL(<ga:@sym>, ADDIStocHA(%X2, <ga:@sym>))
    // Otherwise we generate:
    //   ADDItocL(ADDIStocHA(%X2, <ga:@sym>), <ga:@sym>)
    SDValue GA = N->getOperand(0);
    SDValue TOCbase = N->getOperand(1);
    SDNode *Tmp = CurDAG->getMachineNode(PPC::ADDIStocHA, dl, MVT::i64,
                                        TOCbase, GA);

    if (isa<JumpTableSDNode>(GA) || isa<BlockAddressSDNode>(GA) ||
        CModel == CodeModel::Large)
      return CurDAG->getMachineNode(PPC::LDtocL, dl, MVT::i64, GA,
                                    SDValue(Tmp, 0));

    if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(GA)) {
      const GlobalValue *GValue = G->getGlobal();
      if ((GValue->getType()->getElementType()->isFunctionTy() &&
           (GValue->isDeclaration() || GValue->isWeakForLinker())) ||
          GValue->isDeclaration() || GValue->hasCommonLinkage() ||
          GValue->hasAvailableExternallyLinkage())
        return CurDAG->getMachineNode(PPC::LDtocL, dl, MVT::i64, GA,
                                      SDValue(Tmp, 0));
    }

    return CurDAG->getMachineNode(PPC::ADDItocL, dl, MVT::i64,
                                  SDValue(Tmp, 0), GA);
  }
  case PPCISD::PPC32_PICGOT: {
    // Generate a PIC-safe GOT reference.
    assert(!PPCSubTarget->isPPC64() && PPCSubTarget->isSVR4ABI() &&
      "PPCISD::PPC32_PICGOT is only supported for 32-bit SVR4");
    return CurDAG->SelectNodeTo(N, PPC::PPC32PICGOT, PPCLowering->getPointerTy(),  MVT::i32);
  }
  case PPCISD::VADD_SPLAT: {
    // This expands into one of three sequences, depending on whether
    // the first operand is odd or even, positive or negative.
    assert(isa<ConstantSDNode>(N->getOperand(0)) &&
           isa<ConstantSDNode>(N->getOperand(1)) &&
           "Invalid operand on VADD_SPLAT!");

    int Elt     = N->getConstantOperandVal(0);
    int EltSize = N->getConstantOperandVal(1);
    unsigned Opc1, Opc2, Opc3;
    EVT VT;

    if (EltSize == 1) {
      Opc1 = PPC::VSPLTISB;
      Opc2 = PPC::VADDUBM;
      Opc3 = PPC::VSUBUBM;
      VT = MVT::v16i8;
    } else if (EltSize == 2) {
      Opc1 = PPC::VSPLTISH;
      Opc2 = PPC::VADDUHM;
      Opc3 = PPC::VSUBUHM;
      VT = MVT::v8i16;
    } else {
      assert(EltSize == 4 && "Invalid element size on VADD_SPLAT!");
      Opc1 = PPC::VSPLTISW;
      Opc2 = PPC::VADDUWM;
      Opc3 = PPC::VSUBUWM;
      VT = MVT::v4i32;
    }

    if ((Elt & 1) == 0) {
      // Elt is even, in the range [-32,-18] + [16,30].
      //
      // Convert: VADD_SPLAT elt, size
      // Into:    tmp = VSPLTIS[BHW] elt
      //          VADDU[BHW]M tmp, tmp
      // Where:   [BHW] = B for size = 1, H for size = 2, W for size = 4
      SDValue EltVal = getI32Imm(Elt >> 1);
      SDNode *Tmp = CurDAG->getMachineNode(Opc1, dl, VT, EltVal);
      SDValue TmpVal = SDValue(Tmp, 0);
      return CurDAG->getMachineNode(Opc2, dl, VT, TmpVal, TmpVal);

    } else if (Elt > 0) {
      // Elt is odd and positive, in the range [17,31].
      //
      // Convert: VADD_SPLAT elt, size
      // Into:    tmp1 = VSPLTIS[BHW] elt-16
      //          tmp2 = VSPLTIS[BHW] -16
      //          VSUBU[BHW]M tmp1, tmp2
      SDValue EltVal = getI32Imm(Elt - 16);
      SDNode *Tmp1 = CurDAG->getMachineNode(Opc1, dl, VT, EltVal);
      EltVal = getI32Imm(-16);
      SDNode *Tmp2 = CurDAG->getMachineNode(Opc1, dl, VT, EltVal);
      return CurDAG->getMachineNode(Opc3, dl, VT, SDValue(Tmp1, 0),
                                    SDValue(Tmp2, 0));

    } else {
      // Elt is odd and negative, in the range [-31,-17].
      //
      // Convert: VADD_SPLAT elt, size
      // Into:    tmp1 = VSPLTIS[BHW] elt+16
      //          tmp2 = VSPLTIS[BHW] -16
      //          VADDU[BHW]M tmp1, tmp2
      SDValue EltVal = getI32Imm(Elt + 16);
      SDNode *Tmp1 = CurDAG->getMachineNode(Opc1, dl, VT, EltVal);
      EltVal = getI32Imm(-16);
      SDNode *Tmp2 = CurDAG->getMachineNode(Opc1, dl, VT, EltVal);
      return CurDAG->getMachineNode(Opc2, dl, VT, SDValue(Tmp1, 0),
                                    SDValue(Tmp2, 0));
    }
  }
  }

  return SelectCode(N);
}

// If the target supports the cmpb instruction, do the idiom recognition here.
// We don't do this as a DAG combine because we don't want to do it as nodes
// are being combined (because we might miss part of the eventual idiom). We
// don't want to do it during instruction selection because we want to reuse
// the logic for lowering the masking operations already part of the
// instruction selector.
SDValue PPCDAGToDAGISel::combineToCMPB(SDNode *N) {
  SDLoc dl(N);

  assert(N->getOpcode() == ISD::OR &&
         "Only OR nodes are supported for CMPB");

  SDValue Res;
  if (!PPCSubTarget->hasCMPB())
    return Res;

  if (N->getValueType(0) != MVT::i32 &&
      N->getValueType(0) != MVT::i64)
    return Res;

  EVT VT = N->getValueType(0);

  SDValue RHS, LHS;
  bool BytesFound[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
  uint64_t Mask = 0, Alt = 0;

  auto IsByteSelectCC = [this](SDValue O, unsigned &b,
                               uint64_t &Mask, uint64_t &Alt,
                               SDValue &LHS, SDValue &RHS) {
    if (O.getOpcode() != ISD::SELECT_CC)
      return false;
    ISD::CondCode CC = cast<CondCodeSDNode>(O.getOperand(4))->get();

    if (!isa<ConstantSDNode>(O.getOperand(2)) ||
        !isa<ConstantSDNode>(O.getOperand(3)))
      return false;

    uint64_t PM = O.getConstantOperandVal(2);
    uint64_t PAlt = O.getConstantOperandVal(3);
    for (b = 0; b < 8; ++b) {
      uint64_t Mask = UINT64_C(0xFF) << (8*b);
      if (PM && (PM & Mask) == PM && (PAlt & Mask) == PAlt)
        break;
    }

    if (b == 8)
      return false;
    Mask |= PM;
    Alt  |= PAlt;

    if (!isa<ConstantSDNode>(O.getOperand(1)) ||
        O.getConstantOperandVal(1) != 0) {
      SDValue Op0 = O.getOperand(0), Op1 = O.getOperand(1);
      if (Op0.getOpcode() == ISD::TRUNCATE)
        Op0 = Op0.getOperand(0);
      if (Op1.getOpcode() == ISD::TRUNCATE)
        Op1 = Op1.getOperand(0);

      if (Op0.getOpcode() == ISD::SRL && Op1.getOpcode() == ISD::SRL &&
          Op0.getOperand(1) == Op1.getOperand(1) && CC == ISD::SETEQ &&
          isa<ConstantSDNode>(Op0.getOperand(1))) {

        unsigned Bits = Op0.getValueType().getSizeInBits();
        if (b != Bits/8-1)
          return false;
        if (Op0.getConstantOperandVal(1) != Bits-8)
          return false;

        LHS = Op0.getOperand(0);
        RHS = Op1.getOperand(0);
        return true;
      }

      // When we have small integers (i16 to be specific), the form present
      // post-legalization uses SETULT in the SELECT_CC for the
      // higher-order byte, depending on the fact that the
      // even-higher-order bytes are known to all be zero, for example:
      //   select_cc (xor $lhs, $rhs), 256, 65280, 0, setult
      // (so when the second byte is the same, because all higher-order
      // bits from bytes 3 and 4 are known to be zero, the result of the
      // xor can be at most 255)
      if (Op0.getOpcode() == ISD::XOR && CC == ISD::SETULT &&
          isa<ConstantSDNode>(O.getOperand(1))) {

        uint64_t ULim = O.getConstantOperandVal(1);
        if (ULim != (UINT64_C(1) << b*8))
          return false;

        // Now we need to make sure that the upper bytes are known to be
        // zero.
        unsigned Bits = Op0.getValueType().getSizeInBits();
        if (!CurDAG->MaskedValueIsZero(Op0,
              APInt::getHighBitsSet(Bits, Bits - (b+1)*8)))
          return false;
        
        LHS = Op0.getOperand(0);
        RHS = Op0.getOperand(1);
        return true;
      }

      return false;
    }

    if (CC != ISD::SETEQ)
      return false;

    SDValue Op = O.getOperand(0);
    if (Op.getOpcode() == ISD::AND) {
      if (!isa<ConstantSDNode>(Op.getOperand(1)))
        return false;
      if (Op.getConstantOperandVal(1) != (UINT64_C(0xFF) << (8*b)))
        return false;

      SDValue XOR = Op.getOperand(0);
      if (XOR.getOpcode() == ISD::TRUNCATE)
        XOR = XOR.getOperand(0);
      if (XOR.getOpcode() != ISD::XOR)
        return false;

      LHS = XOR.getOperand(0);
      RHS = XOR.getOperand(1);
      return true;
    } else if (Op.getOpcode() == ISD::SRL) {
      if (!isa<ConstantSDNode>(Op.getOperand(1)))
        return false;
      unsigned Bits = Op.getValueType().getSizeInBits();
      if (b != Bits/8-1)
        return false;
      if (Op.getConstantOperandVal(1) != Bits-8)
        return false;

      SDValue XOR = Op.getOperand(0);
      if (XOR.getOpcode() == ISD::TRUNCATE)
        XOR = XOR.getOperand(0);
      if (XOR.getOpcode() != ISD::XOR)
        return false;

      LHS = XOR.getOperand(0);
      RHS = XOR.getOperand(1);
      return true;
    }

    return false;
  };

  SmallVector<SDValue, 8> Queue(1, SDValue(N, 0));
  while (!Queue.empty()) {
    SDValue V = Queue.pop_back_val();

    for (const SDValue &O : V.getNode()->ops()) {
      unsigned b;
      uint64_t M = 0, A = 0;
      SDValue OLHS, ORHS;
      if (O.getOpcode() == ISD::OR) {
        Queue.push_back(O);
      } else if (IsByteSelectCC(O, b, M, A, OLHS, ORHS)) {
        if (!LHS) {
          LHS = OLHS;
          RHS = ORHS;
          BytesFound[b] = true;
          Mask |= M;
          Alt  |= A;
        } else if ((LHS == ORHS && RHS == OLHS) ||
                   (RHS == ORHS && LHS == OLHS)) {
          BytesFound[b] = true;
          Mask |= M;
          Alt  |= A;
        } else {
          return Res;
        }
      } else {
        return Res;
      }
    }
  }

  unsigned LastB = 0, BCnt = 0;
  for (unsigned i = 0; i < 8; ++i)
    if (BytesFound[LastB]) {
      ++BCnt;
      LastB = i;
    }

  if (!LastB || BCnt < 2)
    return Res;

  // Because we'll be zero-extending the output anyway if don't have a specific
  // value for each input byte (via the Mask), we can 'anyext' the inputs.
  if (LHS.getValueType() != VT) {
    LHS = CurDAG->getAnyExtOrTrunc(LHS, dl, VT);
    RHS = CurDAG->getAnyExtOrTrunc(RHS, dl, VT);
  }

  Res = CurDAG->getNode(PPCISD::CMPB, dl, VT, LHS, RHS);

  bool NonTrivialMask = ((int64_t) Mask) != INT64_C(-1);
  if (NonTrivialMask && !Alt) {
    // Res = Mask & CMPB
    Res = CurDAG->getNode(ISD::AND, dl, VT, Res, CurDAG->getConstant(Mask, VT));
  } else if (Alt) {
    // Res = (CMPB & Mask) | (~CMPB & Alt)
    // Which, as suggested here:
    //   https://graphics.stanford.edu/~seander/bithacks.html#MaskedMerge
    // can be written as:
    // Res = Alt ^ ((Alt ^ Mask) & CMPB)
    // useful because the (Alt ^ Mask) can be pre-computed.
    Res = CurDAG->getNode(ISD::AND, dl, VT, Res,
                          CurDAG->getConstant(Mask ^ Alt, VT));
    Res = CurDAG->getNode(ISD::XOR, dl, VT, Res, CurDAG->getConstant(Alt, VT));
  }

  return Res;
}

void PPCDAGToDAGISel::PreprocessISelDAG() {
  SelectionDAG::allnodes_iterator Position(CurDAG->getRoot().getNode());
  ++Position;

  bool MadeChange = false;
  while (Position != CurDAG->allnodes_begin()) {
    SDNode *N = --Position;
    if (N->use_empty())
      continue;

    SDValue Res;
    switch (N->getOpcode()) {
    default: break;
    case ISD::OR:
      Res = combineToCMPB(N);
      break;
    }

    if (Res) {
      DEBUG(dbgs() << "PPC DAG preprocessing replacing:\nOld:    ");
      DEBUG(N->dump(CurDAG));
      DEBUG(dbgs() << "\nNew: ");
      DEBUG(Res.getNode()->dump(CurDAG));
      DEBUG(dbgs() << "\n");

      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), Res);
      MadeChange = true;
    }
  }

  if (MadeChange)
    CurDAG->RemoveDeadNodes();
}

/// PostprocessISelDAG - Perform some late peephole optimizations
/// on the DAG representation.
void PPCDAGToDAGISel::PostprocessISelDAG() {

  // Skip peepholes at -O0.
  if (TM.getOptLevel() == CodeGenOpt::None)
    return;

  PeepholePPC64();
  PeepholeCROps();
  PeepholePPC64ZExt();
}

// Check if all users of this node will become isel where the second operand
// is the constant zero. If this is so, and if we can negate the condition,
// then we can flip the true and false operands. This will allow the zero to
// be folded with the isel so that we don't need to materialize a register
// containing zero.
bool PPCDAGToDAGISel::AllUsersSelectZero(SDNode *N) {
  // If we're not using isel, then this does not matter.
  if (!PPCSubTarget->hasISEL())
    return false;

  for (SDNode::use_iterator UI = N->use_begin(), UE = N->use_end();
       UI != UE; ++UI) {
    SDNode *User = *UI;
    if (!User->isMachineOpcode())
      return false;
    if (User->getMachineOpcode() != PPC::SELECT_I4 &&
        User->getMachineOpcode() != PPC::SELECT_I8)
      return false;

    SDNode *Op2 = User->getOperand(2).getNode();
    if (!Op2->isMachineOpcode())
      return false;

    if (Op2->getMachineOpcode() != PPC::LI &&
        Op2->getMachineOpcode() != PPC::LI8)
      return false;

    ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op2->getOperand(0));
    if (!C)
      return false;

    if (!C->isNullValue())
      return false;
  }

  return true;
}

void PPCDAGToDAGISel::SwapAllSelectUsers(SDNode *N) {
  SmallVector<SDNode *, 4> ToReplace;
  for (SDNode::use_iterator UI = N->use_begin(), UE = N->use_end();
       UI != UE; ++UI) {
    SDNode *User = *UI;
    assert((User->getMachineOpcode() == PPC::SELECT_I4 ||
            User->getMachineOpcode() == PPC::SELECT_I8) &&
           "Must have all select users");
    ToReplace.push_back(User);
  }

  for (SmallVector<SDNode *, 4>::iterator UI = ToReplace.begin(),
       UE = ToReplace.end(); UI != UE; ++UI) {
    SDNode *User = *UI;
    SDNode *ResNode =
      CurDAG->getMachineNode(User->getMachineOpcode(), SDLoc(User),
                             User->getValueType(0), User->getOperand(0),
                             User->getOperand(2),
                             User->getOperand(1));

      DEBUG(dbgs() << "CR Peephole replacing:\nOld:    ");
      DEBUG(User->dump(CurDAG));
      DEBUG(dbgs() << "\nNew: ");
      DEBUG(ResNode->dump(CurDAG));
      DEBUG(dbgs() << "\n");

      ReplaceUses(User, ResNode);
  }
}

void PPCDAGToDAGISel::PeepholeCROps() {
  bool IsModified;
  do {
    IsModified = false;
    for (SelectionDAG::allnodes_iterator I = CurDAG->allnodes_begin(),
         E = CurDAG->allnodes_end(); I != E; ++I) {
      MachineSDNode *MachineNode = dyn_cast<MachineSDNode>(I);
      if (!MachineNode || MachineNode->use_empty())
        continue;
      SDNode *ResNode = MachineNode;

      bool Op1Set   = false, Op1Unset = false,
           Op1Not   = false,
           Op2Set   = false, Op2Unset = false,
           Op2Not   = false;

      unsigned Opcode = MachineNode->getMachineOpcode();
      switch (Opcode) {
      default: break;
      case PPC::CRAND:
      case PPC::CRNAND:
      case PPC::CROR:
      case PPC::CRXOR:
      case PPC::CRNOR:
      case PPC::CREQV:
      case PPC::CRANDC:
      case PPC::CRORC: {
        SDValue Op = MachineNode->getOperand(1);
        if (Op.isMachineOpcode()) {
          if (Op.getMachineOpcode() == PPC::CRSET)
            Op2Set = true;
          else if (Op.getMachineOpcode() == PPC::CRUNSET)
            Op2Unset = true;
          else if (Op.getMachineOpcode() == PPC::CRNOR &&
                   Op.getOperand(0) == Op.getOperand(1))
            Op2Not = true;
        }
        }  // fallthrough
      case PPC::BC:
      case PPC::BCn:
      case PPC::SELECT_I4:
      case PPC::SELECT_I8:
      case PPC::SELECT_F4:
      case PPC::SELECT_F8:
      case PPC::SELECT_VRRC:
      case PPC::SELECT_VSFRC:
      case PPC::SELECT_VSRC: {
        SDValue Op = MachineNode->getOperand(0);
        if (Op.isMachineOpcode()) {
          if (Op.getMachineOpcode() == PPC::CRSET)
            Op1Set = true;
          else if (Op.getMachineOpcode() == PPC::CRUNSET)
            Op1Unset = true;
          else if (Op.getMachineOpcode() == PPC::CRNOR &&
                   Op.getOperand(0) == Op.getOperand(1))
            Op1Not = true;
        }
        }
        break;
      }

      bool SelectSwap = false;
      switch (Opcode) {
      default: break;
      case PPC::CRAND:
        if (MachineNode->getOperand(0) == MachineNode->getOperand(1))
          // x & x = x
          ResNode = MachineNode->getOperand(0).getNode();
        else if (Op1Set)
          // 1 & y = y
          ResNode = MachineNode->getOperand(1).getNode();
        else if (Op2Set)
          // x & 1 = x
          ResNode = MachineNode->getOperand(0).getNode();
        else if (Op1Unset || Op2Unset)
          // x & 0 = 0 & y = 0
          ResNode = CurDAG->getMachineNode(PPC::CRUNSET, SDLoc(MachineNode),
                                           MVT::i1);
        else if (Op1Not)
          // ~x & y = andc(y, x)
          ResNode = CurDAG->getMachineNode(PPC::CRANDC, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(1),
                                           MachineNode->getOperand(0).
                                             getOperand(0));
        else if (Op2Not)
          // x & ~y = andc(x, y)
          ResNode = CurDAG->getMachineNode(PPC::CRANDC, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(1).
                                             getOperand(0));
        else if (AllUsersSelectZero(MachineNode))
          ResNode = CurDAG->getMachineNode(PPC::CRNAND, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(1)),
          SelectSwap = true;
        break;
      case PPC::CRNAND:
        if (MachineNode->getOperand(0) == MachineNode->getOperand(1))
          // nand(x, x) -> nor(x, x)
          ResNode = CurDAG->getMachineNode(PPC::CRNOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(0));
        else if (Op1Set)
          // nand(1, y) -> nor(y, y)
          ResNode = CurDAG->getMachineNode(PPC::CRNOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(1),
                                           MachineNode->getOperand(1));
        else if (Op2Set)
          // nand(x, 1) -> nor(x, x)
          ResNode = CurDAG->getMachineNode(PPC::CRNOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(0));
        else if (Op1Unset || Op2Unset)
          // nand(x, 0) = nand(0, y) = 1
          ResNode = CurDAG->getMachineNode(PPC::CRSET, SDLoc(MachineNode),
                                           MVT::i1);
        else if (Op1Not)
          // nand(~x, y) = ~(~x & y) = x | ~y = orc(x, y)
          ResNode = CurDAG->getMachineNode(PPC::CRORC, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0).
                                                      getOperand(0),
                                           MachineNode->getOperand(1));
        else if (Op2Not)
          // nand(x, ~y) = ~x | y = orc(y, x)
          ResNode = CurDAG->getMachineNode(PPC::CRORC, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(1).
                                                      getOperand(0),
                                           MachineNode->getOperand(0));
        else if (AllUsersSelectZero(MachineNode))
          ResNode = CurDAG->getMachineNode(PPC::CRAND, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(1)),
          SelectSwap = true;
        break;
      case PPC::CROR:
        if (MachineNode->getOperand(0) == MachineNode->getOperand(1))
          // x | x = x
          ResNode = MachineNode->getOperand(0).getNode();
        else if (Op1Set || Op2Set)
          // x | 1 = 1 | y = 1
          ResNode = CurDAG->getMachineNode(PPC::CRSET, SDLoc(MachineNode),
                                           MVT::i1);
        else if (Op1Unset)
          // 0 | y = y
          ResNode = MachineNode->getOperand(1).getNode();
        else if (Op2Unset)
          // x | 0 = x
          ResNode = MachineNode->getOperand(0).getNode();
        else if (Op1Not)
          // ~x | y = orc(y, x)
          ResNode = CurDAG->getMachineNode(PPC::CRORC, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(1),
                                           MachineNode->getOperand(0).
                                             getOperand(0));
        else if (Op2Not)
          // x | ~y = orc(x, y)
          ResNode = CurDAG->getMachineNode(PPC::CRORC, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(1).
                                             getOperand(0));
        else if (AllUsersSelectZero(MachineNode))
          ResNode = CurDAG->getMachineNode(PPC::CRNOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(1)),
          SelectSwap = true;
        break;
      case PPC::CRXOR:
        if (MachineNode->getOperand(0) == MachineNode->getOperand(1))
          // xor(x, x) = 0
          ResNode = CurDAG->getMachineNode(PPC::CRUNSET, SDLoc(MachineNode),
                                           MVT::i1);
        else if (Op1Set)
          // xor(1, y) -> nor(y, y)
          ResNode = CurDAG->getMachineNode(PPC::CRNOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(1),
                                           MachineNode->getOperand(1));
        else if (Op2Set)
          // xor(x, 1) -> nor(x, x)
          ResNode = CurDAG->getMachineNode(PPC::CRNOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(0));
        else if (Op1Unset)
          // xor(0, y) = y
          ResNode = MachineNode->getOperand(1).getNode();
        else if (Op2Unset)
          // xor(x, 0) = x
          ResNode = MachineNode->getOperand(0).getNode();
        else if (Op1Not)
          // xor(~x, y) = eqv(x, y)
          ResNode = CurDAG->getMachineNode(PPC::CREQV, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0).
                                                      getOperand(0),
                                           MachineNode->getOperand(1));
        else if (Op2Not)
          // xor(x, ~y) = eqv(x, y)
          ResNode = CurDAG->getMachineNode(PPC::CREQV, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(1).
                                             getOperand(0));
        else if (AllUsersSelectZero(MachineNode))
          ResNode = CurDAG->getMachineNode(PPC::CREQV, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(1)),
          SelectSwap = true;
        break;
      case PPC::CRNOR:
        if (Op1Set || Op2Set)
          // nor(1, y) -> 0
          ResNode = CurDAG->getMachineNode(PPC::CRUNSET, SDLoc(MachineNode),
                                           MVT::i1);
        else if (Op1Unset)
          // nor(0, y) = ~y -> nor(y, y)
          ResNode = CurDAG->getMachineNode(PPC::CRNOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(1),
                                           MachineNode->getOperand(1));
        else if (Op2Unset)
          // nor(x, 0) = ~x
          ResNode = CurDAG->getMachineNode(PPC::CRNOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(0));
        else if (Op1Not)
          // nor(~x, y) = andc(x, y)
          ResNode = CurDAG->getMachineNode(PPC::CRANDC, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0).
                                                      getOperand(0),
                                           MachineNode->getOperand(1));
        else if (Op2Not)
          // nor(x, ~y) = andc(y, x)
          ResNode = CurDAG->getMachineNode(PPC::CRANDC, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(1).
                                                      getOperand(0),
                                           MachineNode->getOperand(0));
        else if (AllUsersSelectZero(MachineNode))
          ResNode = CurDAG->getMachineNode(PPC::CROR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(1)),
          SelectSwap = true;
        break;
      case PPC::CREQV:
        if (MachineNode->getOperand(0) == MachineNode->getOperand(1))
          // eqv(x, x) = 1
          ResNode = CurDAG->getMachineNode(PPC::CRSET, SDLoc(MachineNode),
                                           MVT::i1);
        else if (Op1Set)
          // eqv(1, y) = y
          ResNode = MachineNode->getOperand(1).getNode();
        else if (Op2Set)
          // eqv(x, 1) = x
          ResNode = MachineNode->getOperand(0).getNode();
        else if (Op1Unset)
          // eqv(0, y) = ~y -> nor(y, y)
          ResNode = CurDAG->getMachineNode(PPC::CRNOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(1),
                                           MachineNode->getOperand(1));
        else if (Op2Unset)
          // eqv(x, 0) = ~x
          ResNode = CurDAG->getMachineNode(PPC::CRNOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(0));
        else if (Op1Not)
          // eqv(~x, y) = xor(x, y)
          ResNode = CurDAG->getMachineNode(PPC::CRXOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0).
                                                      getOperand(0),
                                           MachineNode->getOperand(1));
        else if (Op2Not)
          // eqv(x, ~y) = xor(x, y)
          ResNode = CurDAG->getMachineNode(PPC::CRXOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(1).
                                             getOperand(0));
        else if (AllUsersSelectZero(MachineNode))
          ResNode = CurDAG->getMachineNode(PPC::CRXOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(1)),
          SelectSwap = true;
        break;
      case PPC::CRANDC:
        if (MachineNode->getOperand(0) == MachineNode->getOperand(1))
          // andc(x, x) = 0
          ResNode = CurDAG->getMachineNode(PPC::CRUNSET, SDLoc(MachineNode),
                                           MVT::i1);
        else if (Op1Set)
          // andc(1, y) = ~y
          ResNode = CurDAG->getMachineNode(PPC::CRNOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(1),
                                           MachineNode->getOperand(1));
        else if (Op1Unset || Op2Set)
          // andc(0, y) = andc(x, 1) = 0
          ResNode = CurDAG->getMachineNode(PPC::CRUNSET, SDLoc(MachineNode),
                                           MVT::i1);
        else if (Op2Unset)
          // andc(x, 0) = x
          ResNode = MachineNode->getOperand(0).getNode();
        else if (Op1Not)
          // andc(~x, y) = ~(x | y) = nor(x, y)
          ResNode = CurDAG->getMachineNode(PPC::CRNOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0).
                                                      getOperand(0),
                                           MachineNode->getOperand(1));
        else if (Op2Not)
          // andc(x, ~y) = x & y
          ResNode = CurDAG->getMachineNode(PPC::CRAND, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(1).
                                             getOperand(0));
        else if (AllUsersSelectZero(MachineNode))
          ResNode = CurDAG->getMachineNode(PPC::CRORC, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(1),
                                           MachineNode->getOperand(0)),
          SelectSwap = true;
        break;
      case PPC::CRORC:
        if (MachineNode->getOperand(0) == MachineNode->getOperand(1))
          // orc(x, x) = 1
          ResNode = CurDAG->getMachineNode(PPC::CRSET, SDLoc(MachineNode),
                                           MVT::i1);
        else if (Op1Set || Op2Unset)
          // orc(1, y) = orc(x, 0) = 1
          ResNode = CurDAG->getMachineNode(PPC::CRSET, SDLoc(MachineNode),
                                           MVT::i1);
        else if (Op2Set)
          // orc(x, 1) = x
          ResNode = MachineNode->getOperand(0).getNode();
        else if (Op1Unset)
          // orc(0, y) = ~y
          ResNode = CurDAG->getMachineNode(PPC::CRNOR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(1),
                                           MachineNode->getOperand(1));
        else if (Op1Not)
          // orc(~x, y) = ~(x & y) = nand(x, y)
          ResNode = CurDAG->getMachineNode(PPC::CRNAND, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0).
                                                      getOperand(0),
                                           MachineNode->getOperand(1));
        else if (Op2Not)
          // orc(x, ~y) = x | y
          ResNode = CurDAG->getMachineNode(PPC::CROR, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(0),
                                           MachineNode->getOperand(1).
                                             getOperand(0));
        else if (AllUsersSelectZero(MachineNode))
          ResNode = CurDAG->getMachineNode(PPC::CRANDC, SDLoc(MachineNode),
                                           MVT::i1, MachineNode->getOperand(1),
                                           MachineNode->getOperand(0)),
          SelectSwap = true;
        break;
      case PPC::SELECT_I4:
      case PPC::SELECT_I8:
      case PPC::SELECT_F4:
      case PPC::SELECT_F8:
      case PPC::SELECT_VRRC:
      case PPC::SELECT_VSFRC:
      case PPC::SELECT_VSRC:
        if (Op1Set)
          ResNode = MachineNode->getOperand(1).getNode();
        else if (Op1Unset)
          ResNode = MachineNode->getOperand(2).getNode();
        else if (Op1Not)
          ResNode = CurDAG->getMachineNode(MachineNode->getMachineOpcode(),
                                           SDLoc(MachineNode),
                                           MachineNode->getValueType(0),
                                           MachineNode->getOperand(0).
                                             getOperand(0),
                                           MachineNode->getOperand(2),
                                           MachineNode->getOperand(1));
        break;
      case PPC::BC:
      case PPC::BCn:
        if (Op1Not)
          ResNode = CurDAG->getMachineNode(Opcode == PPC::BC ? PPC::BCn :
                                                               PPC::BC,
                                           SDLoc(MachineNode),
                                           MVT::Other,
                                           MachineNode->getOperand(0).
                                             getOperand(0),
                                           MachineNode->getOperand(1),
                                           MachineNode->getOperand(2));
        // FIXME: Handle Op1Set, Op1Unset here too.
        break;
      }

      // If we're inverting this node because it is used only by selects that
      // we'd like to swap, then swap the selects before the node replacement.
      if (SelectSwap)
        SwapAllSelectUsers(MachineNode);

      if (ResNode != MachineNode) {
        DEBUG(dbgs() << "CR Peephole replacing:\nOld:    ");
        DEBUG(MachineNode->dump(CurDAG));
        DEBUG(dbgs() << "\nNew: ");
        DEBUG(ResNode->dump(CurDAG));
        DEBUG(dbgs() << "\n");

        ReplaceUses(MachineNode, ResNode);
        IsModified = true;
      }
    }
    if (IsModified)
      CurDAG->RemoveDeadNodes();
  } while (IsModified);
}

// Gather the set of 32-bit operations that are known to have their
// higher-order 32 bits zero, where ToPromote contains all such operations.
static bool PeepholePPC64ZExtGather(SDValue Op32,
                                    SmallPtrSetImpl<SDNode *> &ToPromote) {
  if (!Op32.isMachineOpcode())
    return false;

  // First, check for the "frontier" instructions (those that will clear the
  // higher-order 32 bits.

  // For RLWINM and RLWNM, we need to make sure that the mask does not wrap
  // around. If it does not, then these instructions will clear the
  // higher-order bits.
  if ((Op32.getMachineOpcode() == PPC::RLWINM ||
       Op32.getMachineOpcode() == PPC::RLWNM) &&
      Op32.getConstantOperandVal(2) <= Op32.getConstantOperandVal(3)) {
    ToPromote.insert(Op32.getNode());
    return true;
  }

  // SLW and SRW always clear the higher-order bits.
  if (Op32.getMachineOpcode() == PPC::SLW ||
      Op32.getMachineOpcode() == PPC::SRW) {
    ToPromote.insert(Op32.getNode());
    return true;
  }

  // For LI and LIS, we need the immediate to be positive (so that it is not
  // sign extended).
  if (Op32.getMachineOpcode() == PPC::LI ||
      Op32.getMachineOpcode() == PPC::LIS) {
    if (!isUInt<15>(Op32.getConstantOperandVal(0)))
      return false;

    ToPromote.insert(Op32.getNode());
    return true;
  }

  // LHBRX and LWBRX always clear the higher-order bits.
  if (Op32.getMachineOpcode() == PPC::LHBRX ||
      Op32.getMachineOpcode() == PPC::LWBRX) {
    ToPromote.insert(Op32.getNode());
    return true;
  }

  // CNTLZW always produces a 64-bit value in [0,32], and so is zero extended.
  if (Op32.getMachineOpcode() == PPC::CNTLZW) {
    ToPromote.insert(Op32.getNode());
    return true;
  }

  // Next, check for those instructions we can look through.

  // Assuming the mask does not wrap around, then the higher-order bits are
  // taken directly from the first operand.
  if (Op32.getMachineOpcode() == PPC::RLWIMI &&
      Op32.getConstantOperandVal(3) <= Op32.getConstantOperandVal(4)) {
    SmallPtrSet<SDNode *, 16> ToPromote1;
    if (!PeepholePPC64ZExtGather(Op32.getOperand(0), ToPromote1))
      return false;

    ToPromote.insert(Op32.getNode());
    ToPromote.insert(ToPromote1.begin(), ToPromote1.end());
    return true;
  }

  // For OR, the higher-order bits are zero if that is true for both operands.
  // For SELECT_I4, the same is true (but the relevant operand numbers are
  // shifted by 1).
  if (Op32.getMachineOpcode() == PPC::OR ||
      Op32.getMachineOpcode() == PPC::SELECT_I4) {
    unsigned B = Op32.getMachineOpcode() == PPC::SELECT_I4 ? 1 : 0;
    SmallPtrSet<SDNode *, 16> ToPromote1;
    if (!PeepholePPC64ZExtGather(Op32.getOperand(B+0), ToPromote1))
      return false;
    if (!PeepholePPC64ZExtGather(Op32.getOperand(B+1), ToPromote1))
      return false;

    ToPromote.insert(Op32.getNode());
    ToPromote.insert(ToPromote1.begin(), ToPromote1.end());
    return true;
  }

  // For ORI and ORIS, we need the higher-order bits of the first operand to be
  // zero, and also for the constant to be positive (so that it is not sign
  // extended).
  if (Op32.getMachineOpcode() == PPC::ORI ||
      Op32.getMachineOpcode() == PPC::ORIS) {
    SmallPtrSet<SDNode *, 16> ToPromote1;
    if (!PeepholePPC64ZExtGather(Op32.getOperand(0), ToPromote1))
      return false;
    if (!isUInt<15>(Op32.getConstantOperandVal(1)))
      return false;

    ToPromote.insert(Op32.getNode());
    ToPromote.insert(ToPromote1.begin(), ToPromote1.end());
    return true;
  }

  // The higher-order bits of AND are zero if that is true for at least one of
  // the operands.
  if (Op32.getMachineOpcode() == PPC::AND) {
    SmallPtrSet<SDNode *, 16> ToPromote1, ToPromote2;
    bool Op0OK =
      PeepholePPC64ZExtGather(Op32.getOperand(0), ToPromote1);
    bool Op1OK =
      PeepholePPC64ZExtGather(Op32.getOperand(1), ToPromote2);
    if (!Op0OK && !Op1OK)
      return false;

    ToPromote.insert(Op32.getNode());

    if (Op0OK)
      ToPromote.insert(ToPromote1.begin(), ToPromote1.end());

    if (Op1OK)
      ToPromote.insert(ToPromote2.begin(), ToPromote2.end());

    return true;
  }

  // For ANDI and ANDIS, the higher-order bits are zero if either that is true
  // of the first operand, or if the second operand is positive (so that it is
  // not sign extended).
  if (Op32.getMachineOpcode() == PPC::ANDIo ||
      Op32.getMachineOpcode() == PPC::ANDISo) {
    SmallPtrSet<SDNode *, 16> ToPromote1;
    bool Op0OK =
      PeepholePPC64ZExtGather(Op32.getOperand(0), ToPromote1);
    bool Op1OK = isUInt<15>(Op32.getConstantOperandVal(1));
    if (!Op0OK && !Op1OK)
      return false;

    ToPromote.insert(Op32.getNode());

    if (Op0OK)
      ToPromote.insert(ToPromote1.begin(), ToPromote1.end());

    return true;
  }

  return false;
}

void PPCDAGToDAGISel::PeepholePPC64ZExt() {
  if (!PPCSubTarget->isPPC64())
    return;

  // When we zero-extend from i32 to i64, we use a pattern like this:
  // def : Pat<(i64 (zext i32:$in)),
  //           (RLDICL (INSERT_SUBREG (i64 (IMPLICIT_DEF)), $in, sub_32),
  //                   0, 32)>;
  // There are several 32-bit shift/rotate instructions, however, that will
  // clear the higher-order bits of their output, rendering the RLDICL
  // unnecessary. When that happens, we remove it here, and redefine the
  // relevant 32-bit operation to be a 64-bit operation.

  SelectionDAG::allnodes_iterator Position(CurDAG->getRoot().getNode());
  ++Position;

  bool MadeChange = false;
  while (Position != CurDAG->allnodes_begin()) {
    SDNode *N = --Position;
    // Skip dead nodes and any non-machine opcodes.
    if (N->use_empty() || !N->isMachineOpcode())
      continue;

    if (N->getMachineOpcode() != PPC::RLDICL)
      continue;

    if (N->getConstantOperandVal(1) != 0 ||
        N->getConstantOperandVal(2) != 32)
      continue;

    SDValue ISR = N->getOperand(0);
    if (!ISR.isMachineOpcode() ||
        ISR.getMachineOpcode() != TargetOpcode::INSERT_SUBREG)
      continue;

    if (!ISR.hasOneUse())
      continue;

    if (ISR.getConstantOperandVal(2) != PPC::sub_32)
      continue;

    SDValue IDef = ISR.getOperand(0);
    if (!IDef.isMachineOpcode() ||
        IDef.getMachineOpcode() != TargetOpcode::IMPLICIT_DEF)
      continue;

    // We now know that we're looking at a canonical i32 -> i64 zext. See if we
    // can get rid of it.

    SDValue Op32 = ISR->getOperand(1);
    if (!Op32.isMachineOpcode())
      continue;

    // There are some 32-bit instructions that always clear the high-order 32
    // bits, there are also some instructions (like AND) that we can look
    // through.
    SmallPtrSet<SDNode *, 16> ToPromote;
    if (!PeepholePPC64ZExtGather(Op32, ToPromote))
      continue;

    // If the ToPromote set contains nodes that have uses outside of the set
    // (except for the original INSERT_SUBREG), then abort the transformation.
    bool OutsideUse = false;
    for (SDNode *PN : ToPromote) {
      for (SDNode *UN : PN->uses()) {
        if (!ToPromote.count(UN) && UN != ISR.getNode()) {
          OutsideUse = true;
          break;
        }
      }

      if (OutsideUse)
        break;
    }
    if (OutsideUse)
      continue;

    MadeChange = true;

    // We now know that this zero extension can be removed by promoting to
    // nodes in ToPromote to 64-bit operations, where for operations in the
    // frontier of the set, we need to insert INSERT_SUBREGs for their
    // operands.
    for (SDNode *PN : ToPromote) {
      unsigned NewOpcode;
      switch (PN->getMachineOpcode()) {
      default:
        llvm_unreachable("Don't know the 64-bit variant of this instruction");
      case PPC::RLWINM:    NewOpcode = PPC::RLWINM8; break;
      case PPC::RLWNM:     NewOpcode = PPC::RLWNM8; break;
      case PPC::SLW:       NewOpcode = PPC::SLW8; break;
      case PPC::SRW:       NewOpcode = PPC::SRW8; break;
      case PPC::LI:        NewOpcode = PPC::LI8; break;
      case PPC::LIS:       NewOpcode = PPC::LIS8; break;
      case PPC::LHBRX:     NewOpcode = PPC::LHBRX8; break;
      case PPC::LWBRX:     NewOpcode = PPC::LWBRX8; break;
      case PPC::CNTLZW:    NewOpcode = PPC::CNTLZW8; break;
      case PPC::RLWIMI:    NewOpcode = PPC::RLWIMI8; break;
      case PPC::OR:        NewOpcode = PPC::OR8; break;
      case PPC::SELECT_I4: NewOpcode = PPC::SELECT_I8; break;
      case PPC::ORI:       NewOpcode = PPC::ORI8; break;
      case PPC::ORIS:      NewOpcode = PPC::ORIS8; break;
      case PPC::AND:       NewOpcode = PPC::AND8; break;
      case PPC::ANDIo:     NewOpcode = PPC::ANDIo8; break;
      case PPC::ANDISo:    NewOpcode = PPC::ANDISo8; break;
      }

      // Note: During the replacement process, the nodes will be in an
      // inconsistent state (some instructions will have operands with values
      // of the wrong type). Once done, however, everything should be right
      // again.

      SmallVector<SDValue, 4> Ops;
      for (const SDValue &V : PN->ops()) {
        if (!ToPromote.count(V.getNode()) && V.getValueType() == MVT::i32 &&
            !isa<ConstantSDNode>(V)) {
          SDValue ReplOpOps[] = { ISR.getOperand(0), V, ISR.getOperand(2) };
          SDNode *ReplOp =
            CurDAG->getMachineNode(TargetOpcode::INSERT_SUBREG, SDLoc(V),
                                   ISR.getNode()->getVTList(), ReplOpOps);
          Ops.push_back(SDValue(ReplOp, 0));
        } else {
          Ops.push_back(V);
        }
      }

      // Because all to-be-promoted nodes only have users that are other
      // promoted nodes (or the original INSERT_SUBREG), we can safely replace
      // the i32 result value type with i64.

      SmallVector<EVT, 2> NewVTs;
      SDVTList VTs = PN->getVTList();
      for (unsigned i = 0, ie = VTs.NumVTs; i != ie; ++i)
        if (VTs.VTs[i] == MVT::i32)
          NewVTs.push_back(MVT::i64);
        else
          NewVTs.push_back(VTs.VTs[i]);

      DEBUG(dbgs() << "PPC64 ZExt Peephole morphing:\nOld:    ");
      DEBUG(PN->dump(CurDAG));

      CurDAG->SelectNodeTo(PN, NewOpcode, CurDAG->getVTList(NewVTs), Ops);

      DEBUG(dbgs() << "\nNew: ");
      DEBUG(PN->dump(CurDAG));
      DEBUG(dbgs() << "\n");
    }

    // Now we replace the original zero extend and its associated INSERT_SUBREG
    // with the value feeding the INSERT_SUBREG (which has now been promoted to
    // return an i64).

    DEBUG(dbgs() << "PPC64 ZExt Peephole replacing:\nOld:    ");
    DEBUG(N->dump(CurDAG));
    DEBUG(dbgs() << "\nNew: ");
    DEBUG(Op32.getNode()->dump(CurDAG));
    DEBUG(dbgs() << "\n");

    ReplaceUses(N, Op32.getNode());
  }

  if (MadeChange)
    CurDAG->RemoveDeadNodes();
}

void PPCDAGToDAGISel::PeepholePPC64() {
  // These optimizations are currently supported only for 64-bit SVR4.
  if (PPCSubTarget->isDarwin() || !PPCSubTarget->isPPC64())
    return;

  SelectionDAG::allnodes_iterator Position(CurDAG->getRoot().getNode());
  ++Position;

  while (Position != CurDAG->allnodes_begin()) {
    SDNode *N = --Position;
    // Skip dead nodes and any non-machine opcodes.
    if (N->use_empty() || !N->isMachineOpcode())
      continue;

    unsigned FirstOp;
    unsigned StorageOpcode = N->getMachineOpcode();

    switch (StorageOpcode) {
    default: continue;

    case PPC::LBZ:
    case PPC::LBZ8:
    case PPC::LD:
    case PPC::LFD:
    case PPC::LFS:
    case PPC::LHA:
    case PPC::LHA8:
    case PPC::LHZ:
    case PPC::LHZ8:
    case PPC::LWA:
    case PPC::LWZ:
    case PPC::LWZ8:
      FirstOp = 0;
      break;

    case PPC::STB:
    case PPC::STB8:
    case PPC::STD:
    case PPC::STFD:
    case PPC::STFS:
    case PPC::STH:
    case PPC::STH8:
    case PPC::STW:
    case PPC::STW8:
      FirstOp = 1;
      break;
    }

    // If this is a load or store with a zero offset, we may be able to
    // fold an add-immediate into the memory operation.
    if (!isa<ConstantSDNode>(N->getOperand(FirstOp)) ||
        N->getConstantOperandVal(FirstOp) != 0)
      continue;

    SDValue Base = N->getOperand(FirstOp + 1);
    if (!Base.isMachineOpcode())
      continue;

    unsigned Flags = 0;
    bool ReplaceFlags = true;

    // When the feeding operation is an add-immediate of some sort,
    // determine whether we need to add relocation information to the
    // target flags on the immediate operand when we fold it into the
    // load instruction.
    //
    // For something like ADDItocL, the relocation information is
    // inferred from the opcode; when we process it in the AsmPrinter,
    // we add the necessary relocation there.  A load, though, can receive
    // relocation from various flavors of ADDIxxx, so we need to carry
    // the relocation information in the target flags.
    switch (Base.getMachineOpcode()) {
    default: continue;

    case PPC::ADDI8:
    case PPC::ADDI:
      // In some cases (such as TLS) the relocation information
      // is already in place on the operand, so copying the operand
      // is sufficient.
      ReplaceFlags = false;
      // For these cases, the immediate may not be divisible by 4, in
      // which case the fold is illegal for DS-form instructions.  (The
      // other cases provide aligned addresses and are always safe.)
      if ((StorageOpcode == PPC::LWA ||
           StorageOpcode == PPC::LD  ||
           StorageOpcode == PPC::STD) &&
          (!isa<ConstantSDNode>(Base.getOperand(1)) ||
           Base.getConstantOperandVal(1) % 4 != 0))
        continue;
      break;
    case PPC::ADDIdtprelL:
      Flags = PPCII::MO_DTPREL_LO;
      break;
    case PPC::ADDItlsldL:
      Flags = PPCII::MO_TLSLD_LO;
      break;
    case PPC::ADDItocL:
      Flags = PPCII::MO_TOC_LO;
      break;
    }

    // We found an opportunity.  Reverse the operands from the add
    // immediate and substitute them into the load or store.  If
    // needed, update the target flags for the immediate operand to
    // reflect the necessary relocation information.
    DEBUG(dbgs() << "Folding add-immediate into mem-op:\nBase:    ");
    DEBUG(Base->dump(CurDAG));
    DEBUG(dbgs() << "\nN: ");
    DEBUG(N->dump(CurDAG));
    DEBUG(dbgs() << "\n");

    SDValue ImmOpnd = Base.getOperand(1);

    // If the relocation information isn't already present on the
    // immediate operand, add it now.
    if (ReplaceFlags) {
      if (GlobalAddressSDNode *GA = dyn_cast<GlobalAddressSDNode>(ImmOpnd)) {
        SDLoc dl(GA);
        const GlobalValue *GV = GA->getGlobal();
        // We can't perform this optimization for data whose alignment
        // is insufficient for the instruction encoding.
        if (GV->getAlignment() < 4 &&
            (StorageOpcode == PPC::LD || StorageOpcode == PPC::STD ||
             StorageOpcode == PPC::LWA)) {
          DEBUG(dbgs() << "Rejected this candidate for alignment.\n\n");
          continue;
        }
        ImmOpnd = CurDAG->getTargetGlobalAddress(GV, dl, MVT::i64, 0, Flags);
      } else if (ConstantPoolSDNode *CP =
                 dyn_cast<ConstantPoolSDNode>(ImmOpnd)) {
        const Constant *C = CP->getConstVal();
        ImmOpnd = CurDAG->getTargetConstantPool(C, MVT::i64,
                                                CP->getAlignment(),
                                                0, Flags);
      }
    }

    if (FirstOp == 1) // Store
      (void)CurDAG->UpdateNodeOperands(N, N->getOperand(0), ImmOpnd,
                                       Base.getOperand(0), N->getOperand(3));
    else // Load
      (void)CurDAG->UpdateNodeOperands(N, ImmOpnd, Base.getOperand(0),
                                       N->getOperand(2));

    // The add-immediate may now be dead, in which case remove it.
    if (Base.getNode()->use_empty())
      CurDAG->RemoveDeadNode(Base.getNode());
  }
}


/// createPPCISelDag - This pass converts a legalized DAG into a
/// PowerPC-specific DAG, ready for instruction scheduling.
///
FunctionPass *llvm::createPPCISelDag(PPCTargetMachine &TM) {
  return new PPCDAGToDAGISel(TM);
}

static void initializePassOnce(PassRegistry &Registry) {
  const char *Name = "PowerPC DAG->DAG Pattern Instruction Selection";
  PassInfo *PI = new PassInfo(Name, "ppc-codegen", &SelectionDAGISel::ID,
                              nullptr, false, false);
  Registry.registerPass(*PI, true);
}

void llvm::initializePPCDAGToDAGISelPass(PassRegistry &Registry) {
  CALL_ONCE_INITIALIZATION(initializePassOnce);
}

