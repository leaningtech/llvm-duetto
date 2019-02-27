#include "llvm/Transforms/Utils/LowerSwitch.h"
#include  "llvm/Cheerp/CFGPasses.h"

using namespace llvm;

namespace {

class CheerpLowerSwitch: public LowerSwitch {
public:
	const char *getPassName() const override {
		return "CheerpLowerSwitch";
	}
	bool runOnFunction(Function &F) override;
	static char ID;
private:
	bool keepSwitch(const SwitchInst* si);
};

}

bool CheerpLowerSwitch::keepSwitch(const SwitchInst* si)
{
	// At least 3 successors
	if (si->getNumSuccessors() < 3)
		return false;
	//In asm.js cases values must be in the range [-2^31,2^31),
	//and the difference between the biggest and the smaller must be < 2^31
	int64_t max = std::numeric_limits<int64_t>::min();
	int64_t min = std::numeric_limits<int64_t>::max();
	for (auto& c: si->cases())
	{
		int64_t curr = c.getCaseValue()->getSExtValue();
		max = std::max(max,curr);
		min = std::min(min,curr);
	}
	if (min >= std::numeric_limits<int32_t>::min() &&
		max <= std::numeric_limits<int32_t>::max() && 
		//NOTE: this number is the maximum allowed by V8 for wasm's br_table,
		// it is not defined in the spec
		max-min <= 32 * 1024 &&
		// Avoid extremely big and extremely sparse tables, require at least 3% fill rate
		(max-min <= 100 || si->getNumCases() * 100 >= 3 * (max-min)))
	{
		return true;
	}
	return false;
}

bool CheerpLowerSwitch::runOnFunction(Function& F)
{
	bool Changed = false;

	SmallPtrSet<BasicBlock*, 8> DeleteList;
	for (Function::iterator I = F.begin(), E = F.end(); I != E; )
	{
		BasicBlock *Cur = I++; // Advance over block so we don't traverse new blocks

		if (SwitchInst *SI = dyn_cast<SwitchInst>(Cur->getTerminator()))
		{
			if (!keepSwitch(SI))
			{
				Changed = true;
				processSwitchInst(SI, DeleteList);
			}
		}
	}

	for (BasicBlock* BB: DeleteList)
		DeleteDeadBlock(BB);

	return Changed;
}

char CheerpLowerSwitch::ID = 0;

namespace llvm {

FunctionPass* createCheerpLowerSwitchPass()
{
	return new CheerpLowerSwitch();
}

}

INITIALIZE_PASS_BEGIN(CheerpLowerSwitch, "CheerpLowerSwitch", "Lower switches too sparse or big into if/else branch chains",
                      false, false)
INITIALIZE_PASS_END(CheerpLowerSwitch, "CheerpLowerSwitch", "Lower switches too sparse or big into if/else branch chains",
                    false, false)
