//===-- LinearMemoryHelper.cpp - The Cheerp JavaScript generator ----------===//
//
//                     Cheerp: The C++ compiler for the Web
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2017 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#include "llvm/Cheerp/LinearMemoryHelper.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Module.h"
#include "llvm/Cheerp/Utility.h"

using namespace cheerp;
using namespace llvm;

void LinearMemoryHelper::compileConstantAsBytes(const Constant* c, bool asmjs, ByteListener* listener, int32_t offset) const
{
	const auto& targetData = *module.getDataLayout();
	if(const ConstantDataSequential* CD = dyn_cast<ConstantDataSequential>(c))
	{
		assert(offset==0);
		for(uint32_t i=0;i<CD->getNumElements();i++)
			compileConstantAsBytes(CD->getElementAsConstant(i), asmjs, listener);
	}
	else if(const UndefValue* U = dyn_cast<UndefValue>(c))
	{
		assert(offset==0);
		uint32_t size = targetData.getTypeAllocSize(U->getType());
		for (uint32_t i = 0; i < size; i++)
			listener->addByte(0);
	}
	else if(isa<ConstantArray>(c) || isa<ConstantStruct>(c))
	{
		assert(offset==0);
		for(uint32_t i=0;i<c->getNumOperands();i++)
			compileConstantAsBytes(cast<Constant>(c->getOperand(i)), asmjs, listener);
	}
	else if(const ConstantFP* f=dyn_cast<ConstantFP>(c))
	{
		assert(offset==0);
		const APFloat& flt = f->getValueAPF();
		const APInt& integerRepresentation = flt.bitcastToAPInt();
		uint64_t val = integerRepresentation.getLimitedValue();
		uint32_t bitWidth = integerRepresentation.getBitWidth();
		for(uint32_t i=0;i<bitWidth;i+=8)
			listener->addByte((val>>i)&255);
	}
	else if(const ConstantInt* i=dyn_cast<ConstantInt>(c))
	{
		const APInt& integerRepresentation = i->getValue();
		uint64_t val = integerRepresentation.getLimitedValue() + offset;
		uint32_t bitWidth = integerRepresentation.getBitWidth();
		for(uint32_t i=0;i<bitWidth;i+=8)
			listener->addByte((val>>i)&255);
	}
	else if(const ConstantAggregateZero* Z = dyn_cast<ConstantAggregateZero>(c))
	{
		uint32_t size = targetData.getTypeAllocSize(Z->getType());
		for (uint32_t i = 0; i < size; i++)
			listener->addByte(0);
	}
	else if(isBitCast(c))
	{
		compileConstantAsBytes(cast<Constant>(cast<User>(c)->getOperand(0)), asmjs, listener);
	}
	else if(isa<Function>(c) || isa<GlobalVariable>(c) || isa<ConstantExpr>(c))
	{
		listener->addRunTimeBytes(c);
	}
	else if(dyn_cast<ConstantPointerNull>(c))
	{
		listener->addByte(0);
		listener->addByte(0);
		listener->addByte(0);
		listener->addByte(0);
	}
	else if (asmjs)
	{
		if(const Function* F = dyn_cast<Function>(c))
		{
			assert(offset==0);
			if (!functionAddresses.count(F))
			{
				llvm::errs() << "function not in table: " << F->getName() <<"\n";
				llvm::report_fatal_error("please report a bug");
			}
			uint32_t addr = getFunctionAddress(F);
			for(uint32_t i=0;i<32;i+=8)
				listener->addByte((addr>>i)&255);
		}
		else if(isa<ConstantExpr>(c))
		{
			const ConstantExpr* ce = cast<ConstantExpr>(c);
			switch(ce->getOpcode())
			{
				case Instruction::GetElementPtr:
				{
					Type* curTy = ce->getOperand(0)->getType();
					SmallVector< const Value *, 8 > indices ( std::next(ce->op_begin()), ce->op_end() );
					for (uint32_t i=0; i<indices.size(); i++)
					{
						uint32_t index = cast<ConstantInt>(indices[i])->getZExtValue();
						if (StructType* ST = dyn_cast<StructType>(curTy))
						{
							const StructLayout* SL = targetData.getStructLayout( ST );
							offset += SL->getElementOffset(index);
							curTy = ST->getElementType(index);
						}
						else
						{
							offset += index*targetData.getTypeAllocSize(curTy->getSequentialElementType());
							curTy = curTy->getSequentialElementType();
						}
					}
					compileConstantAsBytes(ce->getOperand(0), asmjs, listener, offset);
					break;
				}
				case Instruction::IntToPtr:
				case Instruction::BitCast:
				{
					compileConstantAsBytes(ce->getOperand(0), asmjs, listener, offset);
					break;
				}
				default:
					// TODO: It could make sense to emit the right number of zeroes anyway
					llvm::errs() << "warning: Unsupported constant expr in asm.js module :" << ce->getOpcodeName() << '\n';
			}
		}
		else if(isa<GlobalVariable>(c))
		{
			const GlobalVariable* g = cast<GlobalVariable>(c);
			if (globalAddresses.count(g) != 1)
			{
				llvm::errs() << "global variable not found:" << g->getName() << "\n";
				llvm::report_fatal_error("please report a bug");
			}
			uint32_t val = globalAddresses.at(g)+offset;
			for(uint32_t i=0;i<32;i+=8)
				listener->addByte((val>>i)&255);
		}
		else
		{
			// TODO: It could make sense to emit the right number of zeroes anyway
			llvm::errs() << "Unsupported constant type for bytes in asm.js module :";
			c->getType()->dump();
		}
	}
	else
	{
		// TODO: It could make sense to emit the right number of zeroes anyway
		llvm::errs() << "Unsupported constant type for bytes ";
		c->dump();
	}
}

const llvm::Value* LinearMemoryHelper::compileGEP(const llvm::Value* p, GepListener* listener) const
{
	const auto& targetData = *module.getDataLayout();
	int64_t constPart = 0;
	while ( isBitCast(p) || isGEP(p) )
	{
		const User * u = cast<User>(p);
		if (isGEP(p))
		{
			Type* curType = u->getOperand(0)->getType();
			SmallVector< const Value *, 8 > indices ( std::next(u->op_begin()), u->op_end() );
			for (uint32_t i=0; i<indices.size(); i++)
			{
				if (StructType* ST = dyn_cast<StructType>(curType))
				{
					uint32_t index = cast<ConstantInt>( indices[i] )->getZExtValue();
					const StructLayout* SL = targetData.getStructLayout( ST );
					curType = ST->getElementType(index);
					uint32_t offset =  SL->getElementOffset(index);
					constPart += offset;
				}
				else
				{
					curType = curType->getSequentialElementType();
					uint32_t size = targetData.getTypeAllocSize(curType);
					if (const ConstantInt* idx = dyn_cast<ConstantInt>(indices[i]))
					{
						constPart += idx->getSExtValue()*size;
					}
					else
					{
						listener->addValue(indices[i], size);
					}
				}
			}
		}
		p = u->getOperand(0);
		continue;
	}
	if (constPart != 0)
		listener->addConst(constPart);
	return p;
}


void LinearMemoryHelper::addGlobals()
{
	const auto& targetData = *module.getDataLayout();
	for (const auto& G: module.globals())
	{
		if (G.getSection() != StringRef("asmjs")) continue;

		Type* ty = G.getType();
		uint32_t size = targetData.getTypeAllocSize(ty->getPointerElementType());
		// Ensure the right alignment for the type
		uint32_t alignment = TypeSupport::getAlignmentAsmJS(targetData, ty->getPointerElementType());
		// The following is correct if alignment is a power of 2 (which it should be)
		heapStart = (heapStart + alignment - 1) & ~(alignment - 1);
		globalAddresses.emplace(&G, heapStart);
		heapStart += size;
	}
}

static std::string getFunctionTableName(const FunctionType* ft)
{
	std::string table_name;
	Type* ret = ft->getReturnType();
	if (ret->isVoidTy())
	{
		table_name += 'v';
	}
	else if (ret->isIntegerTy() || ret->isPointerTy())
	{
		table_name += 'i';
	}
	else if (ret->isFloatingPointTy())
	{
		table_name += 'f';
	}
	for (const auto& param : ft->params())
	{
		if (param->isIntegerTy() || param->isPointerTy())
		{
			table_name += 'i';
		}
		else if (param->isFloatingPointTy())
		{
			table_name += 'f';
		}
	}
	return table_name;
}
void LinearMemoryHelper::addFunctions()
{
	// Build the function tables first
	for (const auto& F: module.functions())
	{
		if (F.getSection() != StringRef("asmjs") || !F.hasAddressTaken())
			continue;
		const FunctionType* fTy = F.getFunctionType();
		auto it = functionTables.find(fTy);
		if (it == functionTables.end())
		{
			it = functionTables.emplace(fTy,FunctionTableInfo()).first;
			it->second.name = getFunctionTableName(fTy);
		}
		it->second.functions.push_back(&F);
	}
	// Then assign addresses
	uint32_t offset = 0;
	for (const auto& FT: functionTables)
	{
		if (mode == FunctionAddressMode::AsmJS)
		{
			offset = (offset+1)<<16;
		}
		else
		{
			offset += FT.second.functions.size();
		}
		uint32_t addr = 0;
		for (const auto F: FT.second.functions)
		{
			functionAddresses.emplace(F, addr+offset);
			addr++;
		}
	}
}

void LinearMemoryHelper::addHeapStart()
{
	GlobalVariable* heapStartVar = module.getNamedGlobal("_heapStart");

	if (heapStartVar)
	{
		// Align to 8 bytes
		heapStart = (heapStart + 7) & ~7;
		ConstantInt* addr = ConstantInt::get(IntegerType::getInt32Ty(module.getContext()), heapStart, false);
		Constant* heapInit = ConstantExpr::getIntToPtr(addr, heapStartVar->getType()->getElementType(), false);
		heapStartVar->setInitializer(heapInit);
		heapStartVar->setSection("asmjs");
	}
}

uint32_t LinearMemoryHelper::getGlobalVariableAddress(const GlobalVariable* G) const
{
	assert(globalAddresses.count(G));
	return globalAddresses.find(G)->second;
}
uint32_t LinearMemoryHelper::getFunctionAddress(const llvm::Function* F) const
{
	assert(functionAddresses.count(F));
	return functionAddresses.find(F)->second;
}
bool LinearMemoryHelper::functionHasAddress(const llvm::Function* F) const
{
	return functionAddresses.count(F);
}
uint32_t LinearMemoryHelper::getFunctionAddressMask(const llvm::FunctionType* Fty) const
{
	auto t = functionTables.find(Fty);
	assert (t != functionTables.end());
	uint32_t mask = t->second.functions.size();
	uint32_t next_power_of_2 = 1;
	while(next_power_of_2 < mask)
		next_power_of_2 <<= 1;
	mask = next_power_of_2 - 1;
	return mask;

}
