//===-- CheerpWasmWriter.cpp - The Cheerp JavaScript generator ------------===//
//
//                     Cheerp: The C++ compiler for the Web
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2017-2020 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <limits>

#include "Relooper.h"
#include "CFGStackifier.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Cheerp/BuiltinInstructions.h"
#include "llvm/Cheerp/CommandLine.h"
#include "llvm/Cheerp/PHIHandler.h"
#include "llvm/Cheerp/NameGenerator.h"
#include "llvm/Cheerp/WasmWriter.h"
#include "llvm/Cheerp/Writer.h"
#include "llvm/IR/Type.h"

using namespace cheerp;
using namespace llvm;
using namespace std;

//#define WASM_DUMP_SECTIONS 1
//#define WASM_DUMP_SECTION_DATA 1
//#define WASM_DUMP_METHODS 1
//#define WASM_DUMP_METHOD_DATA 1
//#define STRESS_DEFERRED 1

static uint32_t COMPILE_METHOD_LIMIT = 100000;

enum BLOCK_TYPE { WHILE1 = 0, DO, SWITCH, CASE, LABEL_FOR_SWITCH, IF, LOOP };

class BlockType
{
public:
	BLOCK_TYPE type;
	uint32_t depth;
	int32_t label;

	BlockType(BLOCK_TYPE bt, uint32_t depth = 0, int32_t label = 0)
	 :

		type(bt),
		depth(depth),
		label(label)
	{}
};

BlockType* findSwitchBlockType(std::vector<BlockType>& blocks)
{
	for (size_t i = blocks.size(); i;)
	{
		BlockType* block = &blocks[--i];
		if (block->type == SWITCH)
			return block;
	}

	llvm_unreachable("switch render block not found");
}

namespace internal {

// The methods encodeSLEB128 and encodeULEB128 are identical to the ones in
// llvm/Support/LEB128.h, but require a parameter of type `WasmBuffer&`
// instead of the llvm output stream.
static inline void encodeSLEB128(int64_t Value, WasmBuffer& OS) {
	bool More;
	do {
		uint8_t Byte = Value & 0x7f;
		// NOTE: this assumes that this signed shift is an arithmetic right shift.
		Value >>= 7;
		More = !((((Value == 0) && ((Byte & 0x40) == 0)) ||
					((Value == -1) && ((Byte & 0x40) != 0))));
		if (More)
			Byte |= 0x80; // Mark this byte to show that more bytes will follow.
		OS << char(Byte);
	} while (More);
}

static inline void encodeULEB128(uint64_t Value, WasmBuffer& OS,
		unsigned Padding = 0) {
	do {
		uint8_t Byte = Value & 0x7f;
		Value >>= 7;
		if (Value != 0 || Padding != 0)
			Byte |= 0x80; // Mark this byte to show that more bytes will follow.
		OS << char(Byte);
	} while (Value != 0);

	// Pad with 0x80 and emit a null byte at the end.
	if (Padding != 0) {
		for (; Padding != 1; --Padding)
			OS << '\x80';
		OS << '\x00';
	}
}

static inline void encodeF32(float f, WasmBuffer& stream)
{
	stream.write(reinterpret_cast<const char*>(&f), sizeof(float));
}

static inline void encodeF64(double f, WasmBuffer& stream)
{
	stream.write(reinterpret_cast<const char*>(&f), sizeof(double));
}

static inline void encodeRegisterKind(Registerize::REGISTER_KIND regKind, WasmBuffer& stream)
{
	switch(regKind)
	{
		case Registerize::DOUBLE:
			encodeULEB128(0x7c, stream);
			break;
		case Registerize::FLOAT:
			encodeULEB128(0x7d, stream);
			break;
		case Registerize::INTEGER:
			encodeULEB128(0x7f, stream);
			break;
		case Registerize::OBJECT:
			encodeULEB128(0x6f, stream);
			break;
	}
}

static uint32_t getValType(const Type* t)
{
	if (t->isIntegerTy() || TypeSupport::isRawPointer(t, true))
		return 0x7f;
	else if (t->isFloatTy())
		return 0x7d;
	else if (t->isDoubleTy())
		return 0x7c;
	else if (t->isPointerTy())
		return 0x6f;
	else
	{
#ifndef NDEBUG
		llvm::errs() << "Unsupported type ";
		t->dump();
#endif
		llvm_unreachable("Unsuppored type");
	}
}

static void encodeValType(const Type* t, WasmBuffer& stream)
{
	encodeULEB128(getValType(t), stream);
}

static void encodeLiteralType(const Type* t, WasmBuffer& stream)
{
	if (t->isIntegerTy() || TypeSupport::isRawPointer(t, true))
		encodeULEB128(0x41, stream);
	else if(t->isFloatTy())
		encodeULEB128(0x43, stream);
	else if(t->isDoubleTy())
		encodeULEB128(0x44, stream);
	else
	{
#ifndef NDEBUG
		llvm::errs() << "Unsupported type: ";
		t->dump();
#endif
		llvm_unreachable("Unsuppored type");
	}
}

static void encodeOpcode(uint32_t opcode, const char* name,
		CheerpWasmWriter& writer, WasmBuffer& code)
{
	if (writer.mode == CheerpWasmWriter::WASM) {
		assert(opcode <= 255);
		code << char(opcode);
	} else {
		assert(writer.mode == CheerpWasmWriter::WAST);
		code << name << '\n';
	}
}

static void encodeS32Opcode(uint32_t opcode, const char* name,
		int32_t immediate, CheerpWasmWriter& writer, WasmBuffer& code)
{
	if (writer.mode == CheerpWasmWriter::WASM) {
		assert(opcode <= 255);
		code << char(opcode);
		encodeSLEB128(immediate, code);
	} else {
		assert(writer.mode == CheerpWasmWriter::WAST);
		code << name << ' ' << immediate << '\n';
	}
}

static void encodeU32Opcode(uint32_t opcode, const char* name,
		uint32_t immediate, CheerpWasmWriter& writer, WasmBuffer& code)
{
	if (writer.mode == CheerpWasmWriter::WASM) {
		assert(opcode <= 255);
		code << char(opcode);
		encodeULEB128(immediate, code);
	} else {
		assert(writer.mode == CheerpWasmWriter::WAST);
		code << name << ' ' << immediate << '\n';
	}
}

static void encodeU32U32Opcode(uint32_t opcode, const char* name,
		uint32_t i1, uint32_t i2, CheerpWasmWriter& writer, WasmBuffer& code)
{
	if (writer.mode == CheerpWasmWriter::WASM) {
		assert(opcode <= 255);
		code << char(opcode);
		encodeULEB128(i1, code);
		encodeULEB128(i2, code);
	} else {
		assert(writer.mode == CheerpWasmWriter::WAST);
		code << name << ' ' << i1 << ' ' << i2 << '\n';
	}
}

}

std::string string_to_hex(const std::string& input)
{
	static const char* const lut = "0123456789abcdef";
	size_t len = input.length();

	std::string output;
	output.reserve(2 * len);
	for (size_t i = 0; i < len; ++i)
	{
		const unsigned char c = input[i];
		output.push_back(lut[c >> 4]);
		output.push_back(lut[c & 15]);
		if ((i & 1) == 1 && (i + 1) < len)
			output.push_back(' ');
	}
	return output;
}

Section::Section(uint32_t sectionId, const char* sectionName, CheerpWasmWriter* writer)
	: writer(writer)
{
	if (writer->mode == CheerpWasmWriter::WASM) {
		std::ostringstream header;
		internal::encodeULEB128(sectionId, header);
		writer->stream << header.str();

		// Custom sections have a section name.
		if (!sectionId) {
			internal::encodeULEB128(strlen(sectionName), *this);
			(*this) << sectionName;
		}
	}
}
Section::~Section()
{
	std::string buf = str();
	if (writer->mode == CheerpWasmWriter::WASM) {
#if WASM_DUMP_SECTIONS
		uint64_t start = writer->stream.tell();
		fprintf(stderr, "%10s id=0x%x start=0x%08lx end=0x%08lx size=0x%08lx\n",
				sectionName, sectionId, start, start + buf.size(), buf.size());
#if WASM_DUMP_SECTION_DATA
		llvm::errs() << "section: " << string_to_hex(buf) << '\n';
#endif
#endif

		std::ostringstream prefix;
		internal::encodeULEB128(buf.size(), prefix);
		writer->stream << prefix.str();
	}
	writer->stream << buf;
}

enum ConditionRenderMode {
	NormalCondition = 0,
	InvertCondition
};

class CheerpWasmRenderInterface: public RenderInterface
{
private:
	CheerpWasmWriter* writer;
	WasmBuffer& code;
	std::vector<BlockType> blockTypes;
	uint32_t labelLocal;
	void renderCondition(const BasicBlock* B, const std::vector<int>& branchIds,
			ConditionRenderMode mode);
	void indent();
	int findIndexFromLabel(int labelId);
	int findClosestBlockIndex();
public:
	const BasicBlock* lastDepth0Block;
	CheerpWasmRenderInterface(CheerpWasmWriter* w, WasmBuffer& code, uint32_t labelLocal)
	 :
		writer(w),
		code(code),
		labelLocal(labelLocal),
		lastDepth0Block(nullptr)
	{ }
	void renderBlock(const BasicBlock* BB);
	void renderLabelForSwitch(int labelId);
	void renderSwitchOnLabel(IdShapeMap& idShapeMap);
	void renderCaseOnLabel(int labelId);
	void renderSwitchBlockBegin(const SwitchInst* switchInst, BlockBranchMap& branchesOut);
	void renderCaseBlockBegin(const BasicBlock* caseBlock, int branchId);
	void renderDefaultBlockBegin(bool empty = false);
	void renderIfBlockBegin(const BasicBlock* condBlock, int branchId, bool first, int labelId = 0);
	void renderIfBlockBegin(const BasicBlock* condBlock, const vector<int>& branchId, bool first, int labelId = 0);
	void renderElseBlockBegin();
	void renderIfBlockEnd(bool labelled = false);
	void renderBlockEnd(bool empty = false);
	void renderBlockPrologue(const BasicBlock* blockTo, const BasicBlock* blockFrom);
	void renderWhileBlockBegin();
	void renderWhileBlockBegin(int labelId);
	void renderDoBlockBegin();
	void renderDoBlockBegin(int labelId);
	void renderDoBlockEnd();
	void renderBlockBegin(int labelId);
	void renderBreak();
	void renderBreak(int labelId);
	void renderContinue();
	void renderContinue(int labelId);
	void renderLabel(int labelId);
	void renderIfOnLabel(int labelId, bool first);
};

void CheerpWasmRenderInterface::renderBlock(const BasicBlock* bb)
{
	if (blockTypes.empty())
		lastDepth0Block = bb;
	else
		lastDepth0Block = nullptr;

	writer->compileBB(code, *bb);

	if (!lastDepth0Block && isa<ReturnInst>(bb->getTerminator()))
	{
		writer->encodeInst(0x0f, "return", code);
	}
}

void CheerpWasmRenderInterface::indent()
{
	if (writer->mode == CheerpWasmWriter::WASM)
		return;

	for(uint32_t i=0;i<blockTypes.size();i++)
		code << "  ";
}

void CheerpWasmRenderInterface::renderCondition(const BasicBlock* bb,
		const std::vector<int>& branchIds,
		ConditionRenderMode mode)
{
	assert(!branchIds.empty());
	const TerminatorInst* term=bb->getTerminator();

	if(isa<BranchInst>(term))
	{
		assert(branchIds.size() == 1);
		int branchId = branchIds[0];
		(void)branchId;
		const BranchInst* bi=cast<BranchInst>(term);
		assert(bi->isConditional());
		//The second branch is the default
		assert(branchId==0);

		const Value* cond = bi->getCondition();
		bool canInvertCond = isa<Instruction>(cond) && writer->isInlineable(*cast<Instruction>(cond));

		if(canInvertCond && isa<ICmpInst>(cond))
		{
			const ICmpInst* ci = cast<ICmpInst>(cond);
			CmpInst::Predicate p = ci->getPredicate();
			if(mode == InvertCondition)
				p = CmpInst::getInversePredicate(p);
			// Optimize "if (a != 0)" to "if (a)" and "if (a == 0)" to "if (!a)".
			if ((p == CmpInst::ICMP_NE || p == CmpInst::ICMP_EQ) &&
					isa<Constant>(ci->getOperand(1)) &&
					cast<Constant>(ci->getOperand(1))->isNullValue())
			{
				if(ci->getOperand(0)->getType()->isPointerTy())
					writer->compileOperand(code, ci->getOperand(0));
				else if(ci->getOperand(0)->getType()->isIntegerTy(32))
					writer->compileSignedInteger(code, ci->getOperand(0), /*forComparison*/true);
				else
					writer->compileUnsignedInteger(code, ci->getOperand(0));
				if(p == CmpInst::ICMP_EQ)
					writer->encodeInst(0x45, "i32.eqz", code);
				return;
			}
			writer->compileICmp(*ci, p, code);
		}
		else if(canInvertCond && isa<FCmpInst>(cond))
		{
			const CmpInst* ci = cast<CmpInst>(cond);
			CmpInst::Predicate p = ci->getPredicate();
			if(mode == InvertCondition)
				p = CmpInst::getInversePredicate(p);
			writer->compileFCmp(ci->getOperand(0), ci->getOperand(1), p, code);
		}
		else
		{
			writer->compileOperand(code, bi->getCondition());
			if (mode == InvertCondition) {
				// Invert result
				writer->encodeInst(0x45, "i32.eqz", code);
			}
		}
	}
	else if(isa<SwitchInst>(term))
	{
		const SwitchInst* si=cast<SwitchInst>(term);
		bool first = true;
		for (int branchId: branchIds)
		{
			SwitchInst::ConstCaseIt it=si->case_begin();
			for(int i=1;i<branchId;i++)
				++it;
			const BasicBlock* dest=it->getCaseSuccessor();
			writer->compileOperand(code, si->getCondition());
			writer->compileOperand(code, it->getCaseValue());
			if(mode == InvertCondition)
				writer->encodeInst(0x47, "i32.ne", code);
			else
				writer->encodeInst(0x46, "i32.eq", code);
			//We found the destination, there may be more cases for the same
			//destination though
			for(++it;it!=si->case_end();++it)
			{
				if(it->getCaseSuccessor()==dest)
				{
					//Also add this condition
					writer->compileOperand(code, si->getCondition());
					writer->compileOperand(code, it->getCaseValue());
					if(mode == InvertCondition)
					{
						writer->encodeInst(0x47, "i32.ne", code);
						writer->encodeInst(0x71, "i32.and", code);
					}
					else
					{
						writer->encodeInst(0x46, "i32.eq", code);
						writer->encodeInst(0x72, "i32.or", code);
					}
				}
			}
			if (!first)
			{
				if(mode == InvertCondition)
					writer->encodeInst(0x71, "i32.and", code);
				else
					writer->encodeInst(0x72, "i32.or", code);
			}
			first = false;
		}
	}
	else
	{
#ifndef NDEBUG
		term->dump();
#endif
		llvm::report_fatal_error("Unsupported code found, please report a bug", false);
	}
}

void CheerpWasmRenderInterface::renderLabelForSwitch(int labelId)
{
	if (writer->mode == CheerpWasmWriter::WASM)
		writer->encodeU32Inst(0x02, "block", 0x40, code);
	else
		code << "block $" << labelId << '\n';
	blockTypes.emplace_back(LABEL_FOR_SWITCH, 1, labelId);
}

void CheerpWasmRenderInterface::renderSwitchOnLabel(IdShapeMap& idShapeMap)
{
	int64_t max = std::numeric_limits<int64_t>::min();
	int64_t min = std::numeric_limits<int64_t>::max();
	for (auto iter = idShapeMap.begin(); iter != idShapeMap.end(); iter++)
	{
		int64_t curr = iter->first;
		max = std::max(max, curr);
		min = std::min(min, curr);
	}

	// There should be at least one case.
	uint32_t depth = max - min + 1;
	assert(depth >= 1);

	// Fill the jump table. By default, jump to the first block. This block
	// will do nothing.
	std::vector<uint32_t> table;
	table.assign(depth, 0);
	uint32_t blockIndex = 1;

	for (auto iter = idShapeMap.begin(); iter != idShapeMap.end(); iter++) {
		table.at(iter->first - min) = blockIndex;
		blockIndex++;
	}

	for (uint32_t i = 0; i < idShapeMap.size() + 1; i++)
		writer->encodeU32Inst(0x02, "block", 0x40, code);

	// Wrap the br_table instruction in its own block
	writer->encodeU32Inst(0x02, "block", 0x40, code);
	writer->encodeU32Inst(0x20, "get_local", labelLocal, code);
	if (min != 0)
	{
		writer->encodeS32Inst(0x41, "i32.const", min, code);
		writer->encodeInst(0x6b, "i32.sub", code);
	}

	writer->encodeBranchTable(code, table, 0);

	writer->encodeInst(0x0b, "end", code);

	// The first block does not do anything, and breaks out of the switch.
	writer->encodeU32Inst(0x0c, "br", idShapeMap.size(), code);
	writer->encodeInst(0x0b, "end", code);

	blockTypes.emplace_back(SWITCH, 0);
	blockTypes.emplace_back(CASE, idShapeMap.size());
}

void CheerpWasmRenderInterface::renderCaseOnLabel(int)
{
	BlockType prevBlock = blockTypes.back();
	(void)prevBlock;
	assert(prevBlock.type == CASE && prevBlock.depth > 0);
}

uint32_t findBlockInBranchesOutMap(const BasicBlock* dest, BlockBranchMap& branchesOut)
{
	int i = 0;
	for (auto it = branchesOut.begin(); it != branchesOut.end(); ++it) {
		if (it->first->llvmBlock == dest)
			return i;
		// Do not count the default block. The default block will be rendered
		// at the end by relooper.
		if (it->second->branchId == -1)
			continue;
		i++;
	}

	llvm_unreachable("destination not found in branches out");
}

void CheerpWasmRenderInterface::renderSwitchBlockBegin(const SwitchInst* si, BlockBranchMap& branchesOut)
{
	assert(si->getNumCases());

	uint32_t bitWidth = si->getCondition()->getType()->getIntegerBitWidth();

	auto getCaseValue = [](const ConstantInt* c, uint32_t bitWidth) -> int64_t
	{
		return bitWidth == 32 ? c->getSExtValue() : c->getZExtValue();
	};

	llvm::BasicBlock* defaultDest = si->getDefaultDest();
	int64_t max = std::numeric_limits<int64_t>::min();
	int64_t min = std::numeric_limits<int64_t>::max();
	for (auto& c: si->cases())
	{
		if (c.getCaseSuccessor() == defaultDest)
			continue;
		int64_t curr = getCaseValue(c.getCaseValue(), bitWidth);
		max = std::max(max, curr);
		min = std::min(min, curr);
	}

	// There should be at least one default case and zero or more cases.
	uint32_t depth = max - min + 1;
	assert(depth >= 1);

	// Fill the jump table.
	std::vector<uint32_t> table;
	table.assign(depth, numeric_limits<uint32_t>::max());

	std::unordered_map<const llvm::BasicBlock*, uint32_t> blockIndexMap;
	uint32_t caseBlocks = 0;

	auto it = si->case_begin();
	auto itE = si->case_begin();
	for (;it != itE; ++it)
	{
		const BasicBlock* dest = it->getCaseSuccessor();
		if (dest == defaultDest)
			continue;
		const auto& found = blockIndexMap.find(dest);

		if (found == blockIndexMap.end())
		{
			// Use the block index from the Relooper branches list. Otherwise,
			// it is possible that the Relooper branches list does not match
			// with the order of the LLVM Basic Blocks.
			uint32_t blockIndex = findBlockInBranchesOutMap(dest, branchesOut);
			blockIndexMap.emplace(dest, blockIndex);
			table.at(getCaseValue(it->getCaseValue(), bitWidth) - min) = blockIndex;
			assert(blockIndex != numeric_limits<uint32_t>::max());

			// Add cases that have the same destination
			auto it_next = it;
			for (++it_next; it_next != si->case_end(); ++it_next)
			{
				if (it_next->getCaseSuccessor() != dest)
					continue;

				table.at(getCaseValue(it_next->getCaseValue(), bitWidth) - min) = blockIndex;
			}

			caseBlocks++;
		}
	}

	// Elements that are not set, will jump to the default block.
	std::replace(table.begin(), table.end(), numeric_limits<uint32_t>::max(), caseBlocks);

	// Print the case blocks and the default block.
	for (uint32_t i = 0; i < caseBlocks + 1; i++)
		writer->encodeU32Inst(0x02, "block", 0x40, code);

	// Wrap the br_table instruction in its own block.
	writer->encodeU32Inst(0x02, "block", 0x40, code);
	writer->compileOperand(code, si->getCondition());
	if (min != 0)
	{
		writer->encodeS32Inst(0x41, "i32.const", min, code);
		writer->encodeInst(0x6b, "i32.sub", code);
	}
	if (bitWidth != 32 && CheerpWriter::needsUnsignedTruncation(si->getCondition(), /*asmjs*/true))
	{
		assert(bitWidth < 32);
		writer->encodeS32Inst(0x41, "i32.const", getMaskForBitWidth(bitWidth), code);
		writer->encodeInst(0x71, "i32.and", code);
	}

	// Print the case labels and the default label.
	writer->encodeBranchTable(code, table, caseBlocks);

	writer->encodeInst(0x0b, "end", code);

	blockTypes.emplace_back(SWITCH, 0);
	blockTypes.emplace_back(CASE, caseBlocks + 1);
}

void CheerpWasmRenderInterface::renderCaseBlockBegin(const BasicBlock*, int branchId)
{
	BlockType prevBlock = blockTypes.back();
	(void)prevBlock;
	assert(prevBlock.type == CASE && prevBlock.depth > 0);
}

void CheerpWasmRenderInterface::renderDefaultBlockBegin(bool)
{
	renderCaseBlockBegin(nullptr, 0);
}

void CheerpWasmRenderInterface::renderIfBlockBegin(const BasicBlock* bb, int branchId, bool first, int labelId)
{
	if(!first)
	{
		indent();
		writer->encodeInst(0x05, "else", code);
	}
	// The condition goes first
	renderCondition(bb, {branchId}, NormalCondition);
	indent();
	writer->encodeU32Inst(0x04, "if", 0x40, code);
	if(first)
	{
		blockTypes.emplace_back(IF, 1, labelId);
	}
	else
	{
		assert(blockTypes.back().type == IF);
		blockTypes.back().depth += 1;
	}
}

void CheerpWasmRenderInterface::renderIfBlockBegin(const BasicBlock* bb, const std::vector<int>& skipBranchIds, bool first, int labelId)
{
	if(!first)
	{
		indent();
		writer->encodeInst(0x05, "else", code);
	}
	// The condition goes first
	renderCondition(bb, skipBranchIds, InvertCondition);
	indent();
	writer->encodeU32Inst(0x04, "if", 0x40, code);

	if(first)
	{
		blockTypes.emplace_back(IF, 1, labelId);
	}
	else
	{
		assert(blockTypes.back().type == IF);
		blockTypes.back().depth += 1;
	}
}

void CheerpWasmRenderInterface::renderElseBlockBegin()
{
	assert(!blockTypes.empty());
	assert(blockTypes.back().type == IF);

	indent();
	writer->encodeInst(0x05, "else", code);
}

void CheerpWasmRenderInterface::renderIfBlockEnd(bool)
{
	assert(!blockTypes.empty());
	BlockType block = blockTypes.back();
	blockTypes.pop_back();
	assert(block.type == IF);

	for(uint32_t i = 0; i < block.depth; i++)
	{
		indent();
		writer->encodeInst(0x0b, "end", code);
	}
}

void CheerpWasmRenderInterface::renderBlockEnd(bool)
{
	assert(!blockTypes.empty());
	BlockType block = blockTypes.back();
	blockTypes.pop_back();

	if(block.type == WHILE1)
	{
		writer->encodeU32Inst(0x0c, "br", 1, code);
		writer->encodeInst(0x0b, "end", code);
		writer->encodeInst(0x0b, "end", code);
	}
	else if (block.type == CASE)
	{
		if (--block.depth > 0)
			blockTypes.push_back(block);
		writer->encodeInst(0x0b, "end", code);
	}
	else if(block.type == IF || block.type == DO)
	{
		for(uint32_t i = 0; i < block.depth; i++)
		{
			indent();
			writer->encodeInst(0x0b, "end", code);
		}
	}
	else if (block.type == SWITCH)
	{
		assert(block.depth == 0);
		if (!blockTypes.empty() && blockTypes.back().type == LABEL_FOR_SWITCH) {
			blockTypes.pop_back();
			writer->encodeInst(0x0b, "end", code);
		}
	}
	else
	{
		assert(false);
	}
}

void CheerpWasmRenderInterface::renderBlockPrologue(const BasicBlock* bbTo, const BasicBlock* bbFrom)
{
	writer->compilePHIOfBlockFromOtherBlock(code, bbTo, bbFrom);
}

void CheerpWasmRenderInterface::renderWhileBlockBegin()
{
	// Wrap a block in a loop so that:
	// br 1 -> break
	// br 2 -> continue
	indent();
	writer->encodeU32Inst(0x03, "loop", 0x40, code);
	indent();
	writer->encodeU32Inst(0x02, "block", 0x40, code);

	blockTypes.emplace_back(WHILE1, 2);
}

void CheerpWasmRenderInterface::renderWhileBlockBegin(int blockLabel)
{
	// Wrap a block in a loop so that:
	// br 1 -> break
	// br 2 -> continue
	indent();

	if (writer->mode == CheerpWasmWriter::WASM)
		writer->encodeU32Inst(0x03, "loop", 0x40, code);
	else
		code << "loop $c" << blockLabel << "\n";

	indent();

	if (writer->mode == CheerpWasmWriter::WASM)
		writer->encodeU32Inst(0x02, "block", 0x40, code);
	else
		code << "block $" << blockLabel << "\n";

	blockTypes.emplace_back(WHILE1, 2, blockLabel);
}

void CheerpWasmRenderInterface::renderDoBlockBegin()
{
	indent();
	writer->encodeU32Inst(0x02, "block", 0x40, code);
	blockTypes.emplace_back(DO, 1);
}

void CheerpWasmRenderInterface::renderDoBlockBegin(int blockLabel)
{
	indent();

	if (writer->mode == CheerpWasmWriter::WASM)
		writer->encodeU32Inst(0x02, "block", 0x40, code);
	else
		code << "block $" << blockLabel << "\n";

	blockTypes.emplace_back(DO, 1, blockLabel);
}

void CheerpWasmRenderInterface::renderDoBlockEnd()
{
	assert(!blockTypes.empty());
	assert(blockTypes.back().type == DO);
	blockTypes.pop_back();

	indent();
	writer->encodeInst(0x0b, "end", code);
}

void CheerpWasmRenderInterface::renderBlockBegin(int labelId)
{
	renderDoBlockBegin(labelId);
}

int CheerpWasmRenderInterface::findClosestBlockIndex()
{
	uint32_t breakIndex = 0;
	for (uint32_t i = 0; i < blockTypes.size(); i++)
	{
		BLOCK_TYPE bt = blockTypes[blockTypes.size() - i - 1].type;
		breakIndex += blockTypes[blockTypes.size() - i - 1].depth;
		if (bt == WHILE1)
			breakIndex -= 1;
		if (bt == DO || bt == WHILE1 || bt == SWITCH || bt == LOOP)
			break;
	}
	assert(breakIndex > 0);
	return breakIndex - 1;
}

int CheerpWasmRenderInterface::findIndexFromLabel(int labelId)
{
	uint32_t breakIndex = 0;
	uint32_t i = 0;
	BLOCK_TYPE bt;
	assert(!blockTypes.empty());
	for (; i < blockTypes.size(); i++)
	{
		BlockType& block = blockTypes[blockTypes.size() - i - 1];
		bt = block.type;

		breakIndex += block.depth;

		if (block.label == labelId)
			break;
	}
	if (bt == WHILE1)
		breakIndex -= 1;
	assert(i < blockTypes.size() && "cannot find labelId in block types");
	return breakIndex - 1;
}

void CheerpWasmRenderInterface::renderBreak()
{
	BlockType block = blockTypes.back();
	if (block.type == CASE)
	{
		assert(block.depth > 0);
		writer->encodeU32Inst(0x0c, "br", block.depth - 1, code);
	}
	else
	{
		writer->encodeU32Inst(0x0c, "br", findClosestBlockIndex(), code);
	}
}

void CheerpWasmRenderInterface::renderBreak(int labelId)
{
	int breakIndex = findIndexFromLabel(labelId);
	writer->encodeU32Inst(0x0c, "br", breakIndex, code);
}

void CheerpWasmRenderInterface::renderContinue()
{
	// Find the last loop's block
	uint32_t breakIndex = 0;
	for (uint32_t i = 0; i < blockTypes.size(); i++)
	{
		BLOCK_TYPE bt = blockTypes[blockTypes.size() - i - 1].type;

		breakIndex += blockTypes[blockTypes.size() - i - 1].depth;

		if (bt == WHILE1 || bt == LOOP)
			break;
	}
	writer->encodeU32Inst(0x0c, "br", breakIndex - 1, code);
}

void CheerpWasmRenderInterface::renderContinue(int labelId)
{
	uint32_t breakIndex = 0;
	uint32_t i = 0;
	for (; i < blockTypes.size(); i++)
	{
		BlockType& block = blockTypes[blockTypes.size() - i - 1];

		breakIndex += block.depth;

		if (block.label == labelId)
			break;
	}
	assert(i < blockTypes.size() && "cannot find labelId in block types");
	writer->encodeU32Inst(0x0c, "br", breakIndex - 1, code);
}

void CheerpWasmRenderInterface::renderLabel(int labelId)
{
	writer->encodeS32Inst(0x41, "i32.const", labelId, code);
	writer->encodeU32Inst(0x21, "set_local", labelLocal, code);
}

void CheerpWasmRenderInterface::renderIfOnLabel(int labelId, bool first)
{
	// TODO: Use first to optimize dispatch
	writer->encodeS32Inst(0x41, "i32.const", labelId, code);
	writer->encodeU32Inst(0x20, "get_local", labelLocal, code);
	writer->encodeInst(0x46, "i32.eq", code);
	indent();
	writer->encodeU32Inst(0x04, "if", 0x40, code);
	blockTypes.emplace_back(IF, 1);
}

void CheerpWasmWriter::encodeInst(uint32_t opcode, const char* name, WasmBuffer& code)
{
	internal::encodeOpcode(opcode, name, *this, code);
}

uint32_t CheerpWasmWriter::findDepth(const Value* v) const
{
	// Must be an instruction
	const Instruction* I = dyn_cast<Instruction>(v);
	if(!I)
		return -1;
	if(isInlineable(*I))
	{
		if(I->getNumOperands() < 1)
			return -1;
		uint32_t res = findDepth(I->getOperand(0));
		if(I->isCommutative())
		{
			assert(I->getNumOperands() == 2);
			res = std::min(res, findDepth(I->getOperand(1)));
		}
		return res;
	}
	else
	{
		return teeLocals.findDepth(v);
	}
}

void CheerpWasmWriter::filterNop(std::string& buf) const
{
	assert(buf.back() == 0x0b);
	nopLocations.push_back(buf.size());
	std::sort(nopLocations.begin(), nopLocations.end());
	uint32_t nopIndex = 0;
	uint32_t old = 0;
	uint32_t curr = 0;
	while (old < buf.size())
	{
		if (nopLocations[nopIndex] <= old)
		{
			while (buf[old] == 0x01)
			{
				++old;
			}
			++nopIndex;
			continue;
		}
		buf[curr] = buf[old];
		++curr;
		++old;
	}
	while (curr < buf.size())
		buf.pop_back();
	assert(buf.back() == 0x0b);
}

void CheerpWasmWriter::encodeBinOp(const llvm::Instruction& I, WasmBuffer& code)
{
	switch (I.getOpcode()) {
		case Instruction::URem:
		case Instruction::UDiv:
			compileUnsignedInteger(code, I.getOperand(0));
			compileUnsignedInteger(code, I.getOperand(1));
			break;
		case Instruction::SRem:
		case Instruction::SDiv:
			compileSignedInteger(code, I.getOperand(0), /*forComparison*/ false);
			compileSignedInteger(code, I.getOperand(1), /*forComparison*/ false);
			break;
		case Instruction::LShr:
			compileUnsignedInteger(code, I.getOperand(0));
			compileOperand(code, I.getOperand(1));
			break;
		case Instruction::AShr:
			compileSignedInteger(code, I.getOperand(0), /*forComparison*/ false);
			compileOperand(code, I.getOperand(1));
			break;
		case Instruction::FSub:
			if (I.getOperand(0) == ConstantFP::getZeroValueForNegation(I.getOperand(0)->getType()))
			{
				//Wasm has an operator negate on floating point
				//(-0.0) - something -> neg(something)
				//Note that this transformation is safe only for negative zero
				compileOperand(code, I.getOperand(1));
				const Type* t = I.getType();
				if (t->isFloatTy())
					encodeInst(0x8c, "f32.neg", code);
				else if (t->isDoubleTy())
					encodeInst(0x9a, "f64.neg", code);
				//We just encoded the operation, so now we can return
				return;
			}
			else
			{
				compileOperand(code, I.getOperand(0));
				compileOperand(code, I.getOperand(1));
				break;
			}
		default:
			if(I.isCommutative())
			{
				// Favor tee_local from the current candidate's stack
				if (findDepth(I.getOperand(0)) > findDepth(I.getOperand(1)))
				{
					compileOperand(code, I.getOperand(1));
					compileOperand(code, I.getOperand(0));
					// Go out of the switch
					break;
				}
				// Fallthrough
			}
			compileOperand(code, I.getOperand(0));
			compileOperand(code, I.getOperand(1));
			break;
	}

	const Type* t = I.getType();
	switch (I.getOpcode())
	{
#define BINOPI(Ty, name, i32, i64) \
		case Instruction::Ty: \
		{ \
			assert(t->isIntegerTy() || t->isPointerTy()); \
			encodeInst(i32, "i32."#name, code); \
			return; \
		}
		BINOPI( Add,   add, 0x6a, 0x7c)
		BINOPI( Sub,   sub, 0x6b, 0x7d)
		BINOPI( Mul,   mul, 0x6c, 0x7e)
		BINOPI(SDiv, div_s, 0x6d, 0x7f)
		BINOPI(UDiv, div_u, 0x6e, 0x80)
		BINOPI(SRem, rem_s, 0x6f, 0x81)
		BINOPI(URem, rem_u, 0x70, 0x82)
		BINOPI( And,   and, 0x71, 0x83)
		BINOPI(  Or,    or, 0x72, 0x84)
		BINOPI( Xor,   xor, 0x73, 0x85)
		BINOPI( Shl,   shl, 0x74, 0x86)
		BINOPI(AShr, shr_s, 0x75, 0x87)
		BINOPI(LShr, shr_u, 0x76, 0x88)
#undef BINOPI

#define BINOPF(Ty, name, f32, f64) \
		case Instruction::Ty: \
		{ \
			if (t->isFloatTy()) { \
				encodeInst(f32, "f32."#name, code); \
				return; \
			} \
			if (t->isDoubleTy()) { \
				encodeInst(f64, "f64."#name, code); \
				return; \
			} \
			break; \
		}
		BINOPF(FAdd,   add, 0x92, 0xa0)
		BINOPF(FSub,   sub, 0x93, 0xa1)
		BINOPF(FMul,   mul, 0x94, 0xa2)
		BINOPF(FDiv,   div, 0x95, 0xa3)
#undef BINOPF
		default:
		{
#ifndef NDEBUG
			I.dump();
#endif
			llvm_unreachable("unknown binop instruction");
		}
	}

#ifndef NDEBUG
	I.dump();
#endif
	llvm_unreachable("unknown type for binop instruction");
}

void CheerpWasmWriter::encodeS32Inst(uint32_t opcode, const char* name, int32_t immediate, WasmBuffer& code)
{
	internal::encodeS32Opcode(opcode, name, immediate, *this, code);
}

void CheerpWasmWriter::encodeU32Inst(uint32_t opcode, const char* name, uint32_t immediate, WasmBuffer& code)
{
	if (mode == CheerpWasmWriter::WAST) {
		// Do not print the immediate for some opcodes when mode is set to
		// wast. Wast doesn't need the immediate, while wasm does.
		switch(opcode) {
			case 0x02: // "block"
			case 0x03: // "loop"
			case 0x04: // "if"
				internal::encodeOpcode(opcode, name, *this, code);
				return;
			default:
				break;
		}
	}
	internal::encodeU32Opcode(opcode, name, immediate, *this, code);
}

void CheerpWasmWriter::encodeU32U32Inst(uint32_t opcode, const char* name, uint32_t i1, uint32_t i2, WasmBuffer& code)
{
	if (mode == CheerpWasmWriter::WAST) {
		// Do not print the immediates for some opcodes when mode is set to
		// wast. Wast doesn't need the immediate, while wasm does.
		switch(opcode) {
			case 0x28: // "i32.load"
			case 0x2a: // "f32.load"
			case 0x2b: // "f64.load"
			case 0x2c: // "i32.load8_s"
			case 0x2d: // "i32.load8_u"
			case 0x2e: // "i32.load16_s"
			case 0x2f: // "i32.load16_u"
			case 0x36: // "i32.store"
			case 0x38: // "f32.store"
			case 0x39: // "f64.store"
			case 0x3a: // "i32.store8"
			case 0x3b: // "i32.store16"
				code << name;
				if (i2)
					code << " offset=" << i2;
				if (i1)
					code << " align=" << (1 << i1);
				code << '\n';
				return;
			default:
				break;
		}
	}
	internal::encodeU32U32Opcode(opcode, name, i1, i2, *this, code);
}

void CheerpWasmWriter::encodePredicate(const llvm::Type* ty, const llvm::CmpInst::Predicate predicate, WasmBuffer& code)
{
	// TODO add i64 support.
	assert(ty->isIntegerTy() || ty->isPointerTy());
	switch(predicate)
	{
#define PREDICATE(Ty, name, opcode) \
		case CmpInst::ICMP_##Ty: \
			encodeInst(opcode, "i32."#name, code); \
			break;
		PREDICATE( EQ,   eq, 0x46);
		PREDICATE( NE,   ne, 0x47);
		PREDICATE(SLT, lt_s, 0x48);
		PREDICATE(ULT, lt_u, 0x49);
		PREDICATE(SGT, gt_s, 0x4a);
		PREDICATE(UGT, gt_u, 0x4b);
		PREDICATE(SLE, le_s, 0x4c);
		PREDICATE(ULE, le_u, 0x4d);
		PREDICATE(SGE, ge_s, 0x4e);
		PREDICATE(UGE, ge_u, 0x4f);
#undef PREDICATE
		default:
			llvm::errs() << "Handle predicate " << predicate << "\n";
			llvm_unreachable("unknown predicate");
	}
}

void CheerpWasmWriter::encodeLoad(const llvm::Type* ty, uint32_t offset,
		WasmBuffer& code, bool signExtend)
{
	if(ty->isIntegerTy())
	{
		uint32_t bitWidth = ty->getIntegerBitWidth();
		if(bitWidth == 1)
			bitWidth = 8;

		// TODO add support for i64.
		switch (bitWidth)
		{
			// Currently assume unsigned, like Cheerp. We may optimize
			// this be looking at a following sext or zext instruction.
			case 8:
				encodeU32U32Inst(signExtend ? 0x2c : 0x2d, signExtend ? "i32.load8_s" : "i32.load8_u", 0x0, offset, code);
				break;
			case 16:
				encodeU32U32Inst(signExtend ? 0x2e : 0x2f, signExtend ? "i32.load16_s" : "i32.load16_u", 0x1, offset, code);
				break;
			case 32:
				encodeU32U32Inst(0x28, "i32.load", 0x2, offset, code);
				break;
			default:
				llvm::errs() << "bit width: " << bitWidth << '\n';
				llvm_unreachable("unknown integer bit width");
		}
	} else {
		if (ty->isFloatTy())
			encodeU32U32Inst(0x2a, "f32.load", 0x2, offset, code);
		else if (ty->isDoubleTy())
			encodeU32U32Inst(0x2b, "f64.load", 0x3, offset, code);
		else
			encodeU32U32Inst(0x28, "i32.load", 0x2, offset, code);
	}
}

void CheerpWasmWriter::encodeWasmIntrinsic(WasmBuffer& code, const llvm::Function* F)
{
	const auto& builtin = TypedBuiltinInstr::getMathTypedBuiltin(*F);

	assert(TypedBuiltinInstr::isValidWasmMathBuiltin(builtin) && "Only proper Wasm builtin can be emitted");

	encodeInst(TypedBuiltinInstr::opcodeWasmBuiltin(builtin),
			TypedBuiltinInstr::nameWasmBuiltin(builtin),
			code);
}

//Return whether explicit assigment to the phi is needed
//Also insert the relevant instruction into getLocalDone when needed
bool CheerpWasmWriter::requiresExplicitAssigment(const Instruction* phi, const Value* incoming)
{
	const Instruction* incomingInst=getUniqueIncomingInst(incoming, PA);
	if(!incomingInst)
		return true;
	assert(!isInlineable(*incomingInst));
	const bool isSameRegister = (registerize.getRegisterId(phi, EdgeContext::emptyContext())==registerize.getRegisterId(incomingInst, edgeContext));
	if (isSameRegister)
		getLocalDone.insert(incomingInst);
	return !isSameRegister;
}

void CheerpWasmWriter::compilePHIOfBlockFromOtherBlock(WasmBuffer& code, const BasicBlock* to, const BasicBlock* from)
{
	class WriterPHIHandler: public PHIHandlerUsingStack
	{
	public:
		WriterPHIHandler(CheerpWasmWriter& w, WasmBuffer& c, const BasicBlock* from)
			:PHIHandlerUsingStack(w.PA),writer(w), code(c), fromBB(from)
		{
		}
		~WriterPHIHandler()
		{
		}
	private:
		CheerpWasmWriter& writer;
		WasmBuffer& code;
		const BasicBlock* fromBB;
		void handlePHIStackGroup(const std::vector<const llvm::PHINode*>& phiToHandle) override
		{
			std::vector<std::pair<const Value*, std::vector<const llvm::PHINode*>>> toProcessOrdered;
			std::map<const Value*, std::vector<const llvm::PHINode*>> toProcessMap;
			for (auto& phi : phiToHandle)
			{
				const Value* incoming = phi->getIncomingValueForBlock(fromBB);
				// We can avoid assignment from the same register if no pointer kind conversion is required
				if(!writer.requiresExplicitAssigment(phi, incoming))
					continue;
				// We can leave undefined values undefined
				if (isa<UndefValue>(incoming))
					continue;

				if (toProcessMap.count(incoming) == 0)
					toProcessOrdered.push_back({incoming,{}});

				toProcessMap[incoming].push_back(phi);
			}

			//TODO: remove me
			writer.teeLocals.instructionStart(code);

			//Note that any process order works, as long as it's deterministic
			//So reordering for leaving on the stack whatever is needed also works
			for (auto& pair : toProcessOrdered)
			{
				// 1) Put the value on the stack
				writer.compileOperand(code, pair.first);
				pair.second = std::move(toProcessMap[pair.first]);
			}

			writer.teeLocals.removeConsumed();

			while (!toProcessOrdered.empty())
			{
				const Value* incoming = toProcessOrdered.back().first;
				const auto& phiVector  = toProcessOrdered.back().second;

				for (const PHINode* phi : phiVector)
				{
					// 2) Save the value in the phi
					uint32_t reg = writer.registerize.getRegisterId(phi, EdgeContext::emptyContext());
					uint32_t local = writer.localMap.at(reg);
					if (phi == phiVector.back())
					{
						if (toProcessOrdered.size() == 1)
							writer.teeLocals.addCandidate(incoming, /*isInstructionAssigment*/false, local, code.tellp());
						writer.encodeU32Inst(0x21, "set_local", local, code);
					}
					else
						writer.encodeU32Inst(0x22, "tee_local", local, code);
				}
				toProcessOrdered.pop_back();
			}
			writer.teeLocals.instructionStart(code);
		}
	};

	WriterPHIHandler(*this, code, from).runOnEdge(registerize, from, to);
}

const char* CheerpWasmWriter::getTypeString(const Type* t)
{
	if (t->isIntegerTy() || TypeSupport::isRawPointer(t, true))
		return "i32";
	else if(t->isFloatTy())
		return "f32";
	else if(t->isDoubleTy())
		return "f64";
	else if(t->isPointerTy())
		return "anyref";
	else
	{
#ifndef NDEBUG
		llvm::errs() << "Unsupported type ";
		t->dump();
#endif
		llvm_unreachable("Unsuppored type");
	}
}

void CheerpWasmWriter::compileGEP(WasmBuffer& code, const llvm::User* gep_inst, bool standalone)
{
	const auto I = dyn_cast<Instruction>(gep_inst);
	if (I && !isInlineable(*I)) {
		if (!standalone) {
			compileGetLocal(code, I);
			return;
		}
	}

	WasmGepWriter gepWriter(*this, code);
	const llvm::Value *p = linearHelper.compileGEP(gep_inst, &gepWriter, &PA);
	if(const GlobalVariable* GV = dyn_cast<GlobalVariable>(p))
		gepWriter.addConst(linearHelper.getGlobalVariableAddress(GV));
	else if(!isa<ConstantPointerNull>(p))
		gepWriter.addValue(p, 1);
	gepWriter.compileValues(/*useConstPart*/true);
}

void CheerpWasmWriter::encodeBranchTable(WasmBuffer& code, std::vector<uint32_t> table, int32_t defaultBlock)
{
	if (mode == CheerpWasmWriter::WASM) {
		encodeInst(0x0e, "br_table", code);
		internal::encodeULEB128(table.size(), code);
		for (auto label : table)
			internal::encodeULEB128(label, code);
		internal::encodeULEB128(defaultBlock, code);
	} else {
		code << "br_table";
		for (auto label : table)
			code << " " << label;
		code << " " << defaultBlock << "\n";
	}
}

void CheerpWasmWriter::compileSignedInteger(WasmBuffer& code, const llvm::Value* v, bool forComparison)
{
	uint32_t shiftAmount = 32-v->getType()->getIntegerBitWidth();
	if(const ConstantInt* C = dyn_cast<ConstantInt>(v))
	{
		int32_t value = C->getSExtValue();
		if(forComparison)
			value <<= shiftAmount;
		encodeS32Inst(0x41, "i32.const", value, code);
		return;
	}

	compileOperand(code, v);

	if (shiftAmount == 0)
		return;

	if (forComparison)
	{
		// When comparing two signed values we can avoid the right shift
		encodeS32Inst(0x41, "i32.const", shiftAmount, code);
		encodeInst(0x74, "i32.shl", code);
	}
	else
	{
		encodeS32Inst(0x41, "i32.const", shiftAmount, code);
		encodeInst(0x74, "i32.shl", code);
		encodeS32Inst(0x41, "i32.const", shiftAmount, code);
		encodeInst(0x75, "i32.shr_s", code);
	}
}

void CheerpWasmWriter::compileUnsignedInteger(WasmBuffer& code, const llvm::Value* v)
{
	if(const ConstantInt* C = dyn_cast<ConstantInt>(v))
	{
		encodeS32Inst(0x41, "i32.const", C->getZExtValue(), code);
		return;
	}

	compileOperand(code, v);

	uint32_t initialSize = v->getType()->getIntegerBitWidth();
	if(initialSize != 32 && CheerpWriter::needsUnsignedTruncation(v, /*asmjs*/true))
	{
		encodeS32Inst(0x41, "i32.const", getMaskForBitWidth(initialSize), code);
		encodeInst(0x71, "i32.and", code);
	}
}

void CheerpWasmWriter::compileConstantExpr(WasmBuffer& code, const ConstantExpr* ce)
{
	switch(ce->getOpcode())
	{
		case Instruction::Add:
		{
			compileOperand(code, ce->getOperand(0));
			compileOperand(code, ce->getOperand(1));
			encodeInst(0x6a, "i32.add", code);
			break;
		}
		case Instruction::And:
		{
			compileOperand(code, ce->getOperand(0));
			compileOperand(code, ce->getOperand(1));
			encodeInst(0x71, "i32.and", code);
			break;
		}
		case Instruction::Or:
		{
			compileOperand(code, ce->getOperand(0));
			compileOperand(code, ce->getOperand(1));
			encodeInst(0x73, "i32.or", code);
			break;
		}
		case Instruction::Sub:
		{
			compileOperand(code, ce->getOperand(0));
			compileOperand(code, ce->getOperand(1));
			encodeInst(0x6b, "i32.sub", code);
			break;
		}
		case Instruction::GetElementPtr:
		{
			compileGEP(code, ce);
			break;
		}
		case Instruction::BitCast:
		{
			assert(ce->getOperand(0)->getType()->isPointerTy());
			compileOperand(code, ce->getOperand(0));
			break;
		}
		case Instruction::IntToPtr:
		{
			compileOperand(code, ce->getOperand(0));
			break;
		}
		case Instruction::ICmp:
		{
			CmpInst::Predicate p = (CmpInst::Predicate)ce->getPredicate();
			compileICmp(ce->getOperand(0), ce->getOperand(1), p, code);
			break;
		}
		case Instruction::PtrToInt:
		{
			compileOperand(code, ce->getOperand(0));
			break;
		}
		case Instruction::Select:
		{
			compileOperand(code, ce->getOperand(1));
			compileOperand(code, ce->getOperand(2));
			compileCondition(code, ce->getOperand(0), /*booleanInvert*/false);
			encodeInst(0x1b, "select", code);
			break;
		}
		default:
			encodeInst(0x00, "unreachable", code);
			llvm::errs() << "warning: Unsupported constant expr " << ce->getOpcodeName() << '\n';
	}
}

void CheerpWasmWriter::compileFloatToText(WasmBuffer& code, const APFloat& f, uint32_t precision)
{
	if(f.isInfinity())
	{
		if(f.isNegative())
			code << '-';
		code << "inf";
	}
	else if(f.isNaN())
	{
		code << "nan";
	}
	else
	{
		char buf[40];
		// TODO: Figure out the right amount of hexdigits
		unsigned charCount = f.convertToHexString(buf, precision, false, APFloat::roundingMode::rmNearestTiesToEven);
		(void)charCount;
		assert(charCount < 40);
		code << buf;
	}
}

void CheerpWasmWriter::compileConstant(WasmBuffer& code, const Constant* c, bool forGlobalInit)
{
	if(const ConstantExpr* CE = dyn_cast<ConstantExpr>(c))
	{
		compileConstantExpr(code, CE);
	}
	else if(const ConstantInt* i=dyn_cast<ConstantInt>(c))
	{
		assert(i->getType()->isIntegerTy() && i->getBitWidth() <= 64);
		if (i->getBitWidth() == 64) {
			assert(i->getSExtValue() <= INT32_MAX);
			assert(i->getSExtValue() >= INT32_MIN);
			encodeS32Inst(0x41, "i32.const", i->getSExtValue(), code);
		} else if (i->getBitWidth() == 32)
			encodeS32Inst(0x41, "i32.const", i->getSExtValue(), code);
		else
			encodeS32Inst(0x41, "i32.const", i->getZExtValue(), code);
	}
	else if(const ConstantFP* f=dyn_cast<ConstantFP>(c))
	{
		if (mode == CheerpWasmWriter::WASM) {
			internal::encodeLiteralType(c->getType(), code);
			if (c->getType()->isDoubleTy()) {
				internal::encodeF64(f->getValueAPF().convertToDouble(), code);
			} else {
				assert(c->getType()->isFloatTy());
				internal::encodeF32(f->getValueAPF().convertToFloat(), code);
			}
		} else {
			// TODO: use encodeInst() and friends.
			code << getTypeString(f->getType()) << ".const ";
			compileFloatToText(code, f->getValueAPF(), f->getType()->isFloatTy() ? 8 : 16);
			code << '\n';
		}
	}
	else if(const GlobalVariable* GV = dyn_cast<GlobalVariable>(c))
	{
		uint32_t address = linearHelper.getGlobalVariableAddress(GV);
		encodeS32Inst(0x41, "i32.const", address, code);
	}
	else if(isa<ConstantPointerNull>(c))
	{
		encodeS32Inst(0x41, "i32.const", 0, code);
	}
	else if(isa<Function>(c))
	{
		const Function* F = cast<Function>(c);
		if (linearHelper.functionHasAddress(F))
		{
			uint32_t addr = linearHelper.getFunctionAddress(F);
			if (!addr)
				llvm::errs() << "function name: " << c->getName() << '\n';
			assert(addr && "function address is zero (aka nullptr conflict)");
			encodeS32Inst(0x41, "i32.const", addr, code);
		}
		else
		{
			// When dealing with indirectly used undefined functions forward them to the null function
			// TODO: This improve the robustness of the compiler, but it might generate unexpected behavor
			//       if the address is ever explicitly compared to 0
			assert(F->empty());
			encodeS32Inst(0x41, "i32.const", 0, code);
		}
	}
	else if (isa<UndefValue>(c))
	{
		if (mode == CheerpWasmWriter::WASM) {
			// Encode a literal f64, f32 or i32 zero as the return value.
			internal::encodeLiteralType(c->getType(), code);
			if (c->getType()->isDoubleTy()) {
				internal::encodeF64(0., code);
			} else if (c->getType()->isFloatTy()) {
				internal::encodeF32(0.f, code);
			} else {
				internal::encodeSLEB128(0, code);
			}
		} else {
			code << getTypeString(c->getType()) << ".const 0\n";
		}
	}
	else
	{
#ifndef NDEBUG
		c->dump();
#endif
		llvm::report_fatal_error("Cannot handle this constant");
	}
}

void CheerpWasmWriter::compileGetLocal(WasmBuffer& code, const llvm::Instruction* I)
{
	compileInstructionAndSet(code, *I);
	if (hasPutTeeLocalOnStack(code, I))
	{
		//Successfully find a candidate to transform in tee local
		return;
	}
	uint32_t idx = registerize.getRegisterId(I, edgeContext);
	uint32_t localId = localMap.at(idx);
	getLocalDone.insert(I);
	encodeU32Inst(0x20, "get_local", localId, code);
}

void CheerpWasmWriter::compileOperand(WasmBuffer& code, const llvm::Value* v)
{
	if(const Constant* c=dyn_cast<Constant>(v))
	{
		auto it = globalizedConstants.find(c);
		if(it != globalizedConstants.end())
			encodeU32Inst(0x23, "get_global", it->second.first, code);
		else
			compileConstant(code, c, /*forGlobalInit*/false);
	}
	else if(const Instruction* it=dyn_cast<Instruction>(v))
	{
		if(isInlineable(*it)) {
			compileInlineInstruction(code, *it);
		} else {
			compileGetLocal(code, it);
		}
	}
	else if(const Argument* arg=dyn_cast<Argument>(v))
	{
		uint32_t local = arg->getArgNo();
		encodeU32Inst(0x20, "get_local", local, code);
	}
	else
	{
#ifndef NDEBUG
		v->dump();
#endif
		assert(false);
	}
}

const char* CheerpWasmWriter::getIntegerPredicate(llvm::CmpInst::Predicate p)
{
	switch(p)
	{
		case CmpInst::ICMP_EQ:
			return "eq";
		case CmpInst::ICMP_NE:
			return "ne";
		case CmpInst::ICMP_SGE:
			return "ge_s";
		case CmpInst::ICMP_SGT:
			return "gt_s";
		case CmpInst::ICMP_SLE:
			return "le_s";
		case CmpInst::ICMP_SLT:
			return "lt_s";
		case CmpInst::ICMP_UGE:
			return "ge_u";
		case CmpInst::ICMP_UGT:
			return "gt_u";
		case CmpInst::ICMP_ULE:
			return "le_u";
		case CmpInst::ICMP_ULT:
			return "lt_u";
		default:
			llvm::errs() << "Handle predicate " << p << "\n";
			break;
	}
	return "";
}

bool CheerpWasmWriter::isSignedLoad(const Value* V) const
{
	const LoadInst* LI = dyn_cast<LoadInst>(V);
	if(!LI)
		return false;
	if(GlobalVariable* ptrGV = dyn_cast<GlobalVariable>(LI->getOperand(0)))
	{
		auto it = globalizedGlobalsIDs.find(ptrGV);
		if(it != globalizedGlobalsIDs.end())
			return false;
	}
	for(const User* U: LI->users())
	{
		const Instruction* userI = cast<Instruction>(U);
		if(userI->getOpcode() == Instruction::SExt)
			continue;
		else if(userI->getOpcode() == Instruction::ICmp && cast<ICmpInst>(userI)->isSigned())
			continue;
		else
			return false;
	}
	return true;
}

void CheerpWasmWriter::compileICmp(const Value* op0, const Value* op1, const CmpInst::Predicate p,
		WasmBuffer& code)
{
	bool useEqz = false;
	if(p == CmpInst::ICMP_EQ)
	{
		// Move the constant on op1 to simplify the logic below
		if(isa<Constant>(op0))
			std::swap(op0, op1);
		if(isa<Constant>(op1) && cast<Constant>(op1)->isNullValue())
			useEqz = true;
	}
	if(op0->getType()->isPointerTy())
	{
		compileOperand(code, op0);
		if(useEqz)
		{
			encodeInst(0x45, "i32.eqz", code);
			return;
		}
		compileOperand(code, op1);
	}
	else if(CmpInst::isSigned(p))
	{
		bool isOp0Signed = isSignedLoad(op0);
		bool isOp1Signed = isSignedLoad(op1);
		// Only use the "forComparison" trick if neither operands are signed loads
		bool useForComparison = !isOp0Signed && !isOp1Signed;
		if(isOp0Signed)
			compileOperand(code, op0);
		else
			compileSignedInteger(code, op0, useForComparison);
		if(isOp1Signed)
			compileOperand(code, op1);
		else
			compileSignedInteger(code, op1, useForComparison);
	}
	else if (CmpInst::isUnsigned(p) || !op0->getType()->isIntegerTy(32))
	{
		compileUnsignedInteger(code, op0);
		if(useEqz)
		{
			encodeInst(0x45, "i32.eqz", code);
			return;
		}
		compileUnsignedInteger(code, op1);
	}
	else
	{
		compileSignedInteger(code, op0, true);
		if(useEqz)
		{
			encodeInst(0x45, "i32.eqz", code);
			return;
		}
		compileSignedInteger(code, op1, true);
	}
	encodePredicate(op0->getType(), p, code);
}

void CheerpWasmWriter::compileICmp(const ICmpInst& ci, const CmpInst::Predicate p,
		WasmBuffer& code)
{
	compileICmp(ci.getOperand(0), ci.getOperand(1), p, code);
}

void CheerpWasmWriter::compileFCmp(const Value* lhs, const Value* rhs, CmpInst::Predicate p, WasmBuffer& code)
{
	if (p == CmpInst::FCMP_ORD)
	{
		Type* ty = lhs->getType();
		assert(ty->isDoubleTy() || ty->isFloatTy());
		assert(ty == rhs->getType());

		// Check if both operands are equal to itself. A nan-value is
		// never equal to itself. Use a logical and operator for the
		// resulting comparison.
		compileOperand(code, lhs);
		compileOperand(code, lhs);
		if (ty->isDoubleTy())
			encodeInst(0x61, "f64.eq", code);
		else
			encodeInst(0x5b, "f32.eq", code);

		compileOperand(code, rhs);
		compileOperand(code, rhs);
		if (ty->isDoubleTy())
			encodeInst(0x61, "f64.eq", code);
		else
			encodeInst(0x5b, "f32.eq", code);

		encodeInst(0x71, "i32.and", code);
	} else if (p == CmpInst::FCMP_UNO) {
		Type* ty = lhs->getType();
		assert(ty->isDoubleTy() || ty->isFloatTy());
		assert(ty == rhs->getType());

		// Check if at least one operand is not equal to itself.
		// A nan-value is never equal to itself. Use a logical
		// or operator for the resulting comparison.
		compileOperand(code, lhs);
		compileOperand(code, lhs);
		if (ty->isDoubleTy())
			encodeInst(0x62, "f64.ne", code);
		else
			encodeInst(0x5c, "f32.ne", code);

		compileOperand(code, rhs);
		compileOperand(code, rhs);
		if (ty->isDoubleTy())
			encodeInst(0x62, "f64.ne", code);
		else
			encodeInst(0x5c, "f32.ne", code);

		encodeInst(0x73, "i32.or", code);
	} else {
		compileOperand(code, lhs);
		compileOperand(code, rhs);
		Type* ty = lhs->getType();
		assert(ty->isDoubleTy() || ty->isFloatTy());
		// It is much more efficient to invert the predicate if we need to check for unorderedness
		bool invertForUnordered = CmpInst::isUnordered(p);
		if(invertForUnordered)
			p = CmpInst::getInversePredicate(p);
		assert(!CmpInst::isUnordered(p));
		switch(p)
		{
#define PREDICATE(Ty, name, f32, f64) \
			case CmpInst::FCMP_O##Ty: \
				if (ty->isDoubleTy()) \
					encodeInst(f64, "f64."#name, code); \
				else \
					encodeInst(f32, "f32."#name, code); \
				break;
			PREDICATE(EQ, eq, 0x5b, 0x61)
			PREDICATE(NE, ne, 0x5c, 0x62)
			PREDICATE(LT, lt, 0x5d, 0x63)
			PREDICATE(GT, gt, 0x5e, 0x64)
			PREDICATE(LE, le, 0x5f, 0x65)
			PREDICATE(GE, ge, 0x60, 0x66)
#undef PREDICATE
			default:
				llvm::errs() << "Handle predicate " << p << "\n";
				break;
		}
		if(invertForUnordered)
		{
			// Invert result
			encodeInst(0x45, "i32.eqz", code);
		}
	}
}

void CheerpWasmWriter::compileDowncast(WasmBuffer& code, ImmutableCallSite callV)
{
	assert(callV.arg_size() == 2);
	assert(callV.getCalledFunction()->getIntrinsicID() == Intrinsic::cheerp_downcast ||
		callV.getCalledFunction()->getIntrinsicID() == Intrinsic::cheerp_virtualcast);

	const Value* src = callV.getArgument(0);
	const Value* offset = callV.getArgument(1);

	Type* t = src->getType()->getPointerElementType();

	compileOperand(code, src);

	if(!TypeSupport::isClientType(t) &&
			(!isa<ConstantInt>(offset) || !cast<ConstantInt>(offset)->isNullValue()))
	{
		compileOperand(code, offset);
		encodeInst(0x6a, "i32.add", code);
	}
}

uint32_t CheerpWasmWriter::compileLoadStorePointer(WasmBuffer& code, const Value* ptrOp)
{
	uint32_t offset = 0;
	if(isa<Instruction>(ptrOp) && isInlineable(*cast<Instruction>(ptrOp))) {
		// Calling compileGEP is safe on any instruction
		WasmGepWriter gepWriter(*this, code);
		auto p = linearHelper.compileGEP(ptrOp, &gepWriter, &PA);
		if(const GlobalVariable* GV = dyn_cast<GlobalVariable>(p))
			gepWriter.addConst(linearHelper.getGlobalVariableAddress(GV));
		else
			gepWriter.addValue(p, 1);
		// The immediate offset of a load instruction is an unsigned
		// 32-bit integer. Negative immediate offsets are not supported.
		// So let compileValues deal with the value
		bool negativeConstPart = gepWriter.constPart < 0;
		bool firstOperand = gepWriter.compileValues(/*useConstPart*/negativeConstPart);
		if (!negativeConstPart) {
			// There must be something on the stack
			if (firstOperand)
				encodeS32Inst(0x41, "i32.const", 0, code);
			offset += gepWriter.constPart;
		}
	} else {
		const Constant* C = dyn_cast<Constant>(ptrOp);
		if (C && !globalizedConstants.count(C))
		{
			struct AddrListener: public LinearMemoryHelper::ByteListener
			{
				uint32_t addr;
				uint32_t off;
				AddrListener():addr(0),off(0)
				{
				}
				void addByte(uint8_t b) override
				{
					addr |= b << off;
					off += 8;
				}
			};
			AddrListener addrListener;
			linearHelper.compileConstantAsBytes(C, /* asmjs */ true, &addrListener);
			encodeS32Inst(0x41, "i32.const", 0, code);
			offset = addrListener.addr;
		}
		else
		{
			compileOperand(code, ptrOp);
		}
	}
	return offset;
}

void CheerpWasmWriter::compileLoad(WasmBuffer& code, const LoadInst& li, bool signExtend)
{
	const Value* ptrOp=li.getPointerOperand();
	// 1) The pointer
	uint32_t offset = compileLoadStorePointer(code, ptrOp);
	// 2) Load
	encodeLoad(li.getType(), offset, code, signExtend);
}

bool CheerpWasmWriter::compileInstruction(WasmBuffer& code, const Instruction& I)
{
	switch(I.getOpcode())
	{
		case Instruction::GetElementPtr:
		{
			compileGEP(code, &I, true);
			break;
		}
		default:
			return compileInlineInstruction(code, I);
	}
	return false;
}

bool CheerpWasmWriter::isTailCall(const CallInst& ci) const
{
	if(!WasmReturnCalls || !ci.isTailCall())
		return false;
	const Instruction* nextI = ci.getNextNode();
	// The next inst must be a return
	if(!isa<ReturnInst>(nextI))
		return false;
	// Both call and return are void
	if(currentFun->getReturnType()->isVoidTy())
		return ci.getType()->isVoidTy();
	// The return uses the call
	return nextI->getOperand(0) == &ci;
}

bool CheerpWasmWriter::isReturnPartOfTailCall(const Instruction& ti) const
{
	const BasicBlock* BB = ti.getParent();
	// Make sure this return is not the first instruction of the block
	if(&*BB->begin() == &ti)
		return false;
	const Instruction* TermPrev = ti.getPrevNode();
	// Make sure the previous instruction is a call
	if(!isa<CallInst>(TermPrev))
		return false;
	return isTailCall(*cast<CallInst>(TermPrev));
}

void CheerpWasmWriter::checkAndSanitizeDependencies(InstructionToDependenciesMap& dependencies) const
{
	for (auto& pair : dependencies)
	{
		assert(pair.first->getParent() == currentBB);
		pair.second.erase(pair.first);
		for (const auto& I : pair.second)
		{
			assert(!isInlineable(*I));
			assert(I->getParent() == currentBB);
		}
	}
}

void CheerpWasmWriter::flushMemoryDependencies(WasmBuffer& code, const Instruction& I)
{
	const bool needsSubStack = teeLocals.needsSubStack(code);
	if (needsSubStack)
		teeLocals.addIndentation(code);
	for (const auto& x : memoryDependencies[&I])
		compileInstructionAndSet(code, *x);
	if (needsSubStack)
		teeLocals.decreaseIndentation(code, false);
}

void CheerpWasmWriter::flushSetLocalDependencies(WasmBuffer& code, const Instruction& I)
{
	const bool needsSubStack = teeLocals.needsSubStack(code);
	if (needsSubStack)
		teeLocals.addIndentation(code);
	for (const auto& x : localsDependencies[&I])
		compileInstructionAndSet(code, *x);
	if (needsSubStack)
		teeLocals.decreaseIndentation(code, false);
}

bool CheerpWasmWriter::compileInlineInstruction(WasmBuffer& code, const Instruction& I)
{
	switch(I.getOpcode())
	{
		case Instruction::Alloca:
		{
			llvm::report_fatal_error("Allocas in wasm should be removed in the AllocaLowering pass. This is a bug");
		}
		case Instruction::Add:
		case Instruction::And:
		case Instruction::AShr:
		case Instruction::LShr:
		case Instruction::Mul:
		case Instruction::Or:
		case Instruction::Shl:
		case Instruction::Sub:
		case Instruction::SDiv:
		case Instruction::UDiv:
		case Instruction::SRem:
		case Instruction::URem:
		case Instruction::Xor:
		case Instruction::FAdd:
		case Instruction::FDiv:
		case Instruction::FMul:
		case Instruction::FSub:
		{
			encodeBinOp(I, code);
			break;
		}
		case Instruction::BitCast:
		{
			assert(I.getType()->isPointerTy());
			compileOperand(code, I.getOperand(0));
			break;
		}
		case Instruction::Br:
			break;
		case Instruction::VAArg:
		{
			const VAArgInst& vi=cast<VAArgInst>(I);

			// Load the current argument
			compileOperand(code, vi.getPointerOperand());
			encodeU32U32Inst(0x28, "i32.load", 0x2, 0x0, code);
			encodeLoad(vi.getType(), 0, code, /*signExtend*/false);

			// Move varargs pointer to next argument
			compileOperand(code, vi.getPointerOperand());
			compileOperand(code, vi.getPointerOperand());
			encodeU32U32Inst(0x28, "i32.load", 0x2, 0x0, code);
			encodeS32Inst(0x41, "i32.const", 8, code);
			encodeInst(0x6a, "i32.add", code);
			encodeU32U32Inst(0x36, "i32.store", 0x2, 0x0, code);
			break;
		}
		case Instruction::Call:
		{
			const CallInst& ci = cast<CallInst>(I);
			const Function * calledFunc = ci.getCalledFunction();
			const Value * calledValue = ci.getCalledValue();
			const PointerType* pTy = cast<PointerType>(calledValue->getType());
			const FunctionType* fTy = cast<FunctionType>(pTy->getElementType());
			assert(!ci.isInlineAsm());
			// NOTE: If 'useTailCall' the code _must_ use return_call or insert a return
			//       Returns are not otherwise added in such cases
			const bool useTailCall = isTailCall(ci);

			if (calledFunc)
			{
				unsigned intrinsicId = calledFunc->getIntrinsicID();
				switch (intrinsicId)
				{
					case Intrinsic::trap:
					{
						encodeInst(0x00, "unreachable", code);
						// NOTE: No point in adding a return even if 'useTailCall' is true
						return true;
					}
					case Intrinsic::stacksave:
					{
						encodeU32Inst(0x23, "get_global", stackTopGlobal, code);
						if(useTailCall)
						{
							encodeInst(0x0f, "return", code);
							return true;
						}
						return false;
					}
					case Intrinsic::stackrestore:
					{
						compileOperand(code, ci.getOperand(0));
						encodeU32Inst(0x24, "set_global", stackTopGlobal, code);
						if(useTailCall)
							encodeInst(0x0f, "return", code);
						return true;
					}
					case Intrinsic::vastart:
					{
						llvm::report_fatal_error("Vastart in wasm should be removed in the AllocaLowering pass. This is a bug");
					}
					case Intrinsic::vacopy:
					{
						compileOperand(code, ci.getOperand(0));
						compileOperand(code, ci.getOperand(1));
						encodeU32U32Inst(0x28, "i32.load", 0x2, 0x0, code);
						encodeU32U32Inst(0x36, "i32.store", 0x2, 0x0, code);
						if(useTailCall)
							encodeInst(0x0f, "return", code);
						return true;
					}
					case Intrinsic::vaend:
					{
						// Do nothing.
						if(useTailCall)
							encodeInst(0x0f, "return", code);
						return true;
					}
					case Intrinsic::cheerp_downcast:
					case Intrinsic::cheerp_virtualcast:
					{
						compileDowncast(code, &ci);
						if(useTailCall)
						{
							encodeInst(0x0f, "return", code);
							return true;
						}
						return false;
					}
					case Intrinsic::cheerp_downcast_current:
					{
						compileOperand(code, ci.getOperand(0));
						if(useTailCall)
						{
							encodeInst(0x0f, "return", code);
							return true;
						}
						return false;
					}
					case Intrinsic::cheerp_upcast_collapsed:
					{
						compileOperand(code, ci.getOperand(0));
						if(useTailCall)
						{
							encodeInst(0x0f, "return", code);
							return true;
						}
						return false;
					}
					case Intrinsic::cheerp_cast_user:
					{
						if(ci.use_empty())
							return true;
						compileOperand(code, ci.getOperand(0));
						// NOTE: If there are no uses this cannot be a tail-call (the user would have been the return)
						if(useTailCall)
						{
							encodeInst(0x0f, "return", code);
							return true;
						}
						return false;
					}
					case Intrinsic::cheerp_grow_memory:
					{
						compileOperand(code, ci.getOperand(0));
						if(useWasmLoader)
						{
							uint32_t importedId = linearHelper.getBuiltinId(BuiltinInstr::BUILTIN::GROW_MEM);
							if(useTailCall)
							{
								encodeU32Inst(0x12, "return_call", importedId, code);
								return true;
							}
							else
								encodeU32Inst(0x10, "call", importedId, code);
						}
						else
						{
							encodeS32Inst(0x40, "grow_memory", 0, code);
							if(useTailCall)
							{
								encodeInst(0x0f, "return", code);
								return true;
							}
						}
						return false;
					}
					case Intrinsic::flt_rounds:
					{
						// Rounding mode 1: nearest
						encodeS32Inst(0x41, "i32.const", 1, code);
						if(useTailCall)
						{
							encodeInst(0x0f, "return", code);
							return true;
						}
						return false;
					}
					case Intrinsic::invariant_start:
					{
						//TODO: Try to optimize using this, for now just pass the second arg
						if (ci.use_empty())
							return true;

						compileOperand(code, ci.getOperand(1));
						// NOTE: If there are no uses this cannot be a tail-call (the user would have been the return)
						if(useTailCall)
						{
							encodeInst(0x0f, "return", code);
							return true;
						}
						return false;
					}
					case Intrinsic::invariant_end:
					{
						// Do nothing.
						if(useTailCall)
							encodeInst(0x0f, "return", code);
						return true;
					}
					case Intrinsic::memmove:
					{
						compileOperand(code, ci.op_begin()->get());
						compileOperand(code, (ci.op_begin() + 1)->get());
						compileOperand(code, (ci.op_begin() + 2)->get());
						llvm::Function* f = module.getFunction("memmove");
						uint32_t functionId = linearHelper.getFunctionIds().at(f);
						encodeU32Inst(0x10, "call", functionId, code);
						encodeInst(0x1a, "drop", code);
						// NOTE: Cannot tail call, the return type is different
						if(useTailCall)
							encodeInst(0x0f, "return", code);
						return true;
					}
					case Intrinsic::memcpy:
					{
						compileOperand(code, ci.op_begin()->get());
						compileOperand(code, (ci.op_begin() + 1)->get());
						compileOperand(code, (ci.op_begin() + 2)->get());
						llvm::Function* f = module.getFunction("memcpy");
						uint32_t functionId = linearHelper.getFunctionIds().at(f);
						encodeU32Inst(0x10, "call", functionId, code);
						encodeInst(0x1a, "drop", code);
						// NOTE: Cannot tail call, the return type is different
						if(useTailCall)
							encodeInst(0x0f, "return", code);
						return true;
					}
					case Intrinsic::memset:
					{
						compileOperand(code, ci.op_begin()->get());
						compileOperand(code, (ci.op_begin() + 1)->get());
						compileOperand(code, (ci.op_begin() + 2)->get());
						llvm::Function* f = module.getFunction("memset");
						uint32_t functionId = linearHelper.getFunctionIds().at(f);
						encodeU32Inst(0x10, "call", functionId, code);
						encodeInst(0x1a, "drop", code);
						// NOTE: Cannot tail call, the return type is different
						if(useTailCall)
							encodeInst(0x0f, "return", code);
						return true;
					}
					case Intrinsic::cheerp_allocate:
					case Intrinsic::cheerp_allocate_array:
					{
						calledFunc = module.getFunction("malloc");
						if (!calledFunc)
							llvm::report_fatal_error("missing malloc definition");
						break;
					}
					case Intrinsic::cheerp_reallocate:
					{
						calledFunc = module.getFunction("realloc");
						if (!calledFunc)
							llvm::report_fatal_error("missing realloc definition");
						break;
					}
					case Intrinsic::cheerp_deallocate:
					{
						calledFunc = module.getFunction("free");
						if (!calledFunc)
							llvm::report_fatal_error("missing free definition");
						break;
					}
					case Intrinsic::ctlz:
					case Intrinsic::fabs:
					case Intrinsic::ceil:
					case Intrinsic::floor:
					case Intrinsic::trunc:
					case Intrinsic::minnum:
					case Intrinsic::maxnum:
					case Intrinsic::copysign:
					{
						//Handled below inside if (isWasmIntrinsic(calledFunction))
						break;
					}
					case Intrinsic::cos:
					case Intrinsic::exp:
					case Intrinsic::log:
					case Intrinsic::pow:
					case Intrinsic::sin:
					{
						if (globalDeps.getMathMode() != GlobalDepsAnalyzer::WASM_BUILTINS)
						{
							// Handled below
							break;
						}
					}
					default:
					{
						unsigned intrinsic = calledFunc->getIntrinsicID();
#ifndef NDEBUG
						if (intrinsic != Intrinsic::not_intrinsic)
							ci.dump();
#endif
						assert(intrinsic == Intrinsic::not_intrinsic);
					}
					break;
				}

				if (globalDeps.getMathMode() == GlobalDepsAnalyzer::WASM_BUILTINS)
				{
					StringRef ident = calledFunc->getName();
					BuiltinInstr::BUILTIN b = BuiltinInstr::BUILTIN::NONE;
					if(ident=="acos" || ident=="acosf")
					{
						b = BuiltinInstr::BUILTIN::ACOS_F;
					}
					else if(ident=="asin" || ident=="asinf")
					{
						b = BuiltinInstr::BUILTIN::ASIN_F;
					}
					else if(ident=="atan" || ident=="atanf")
					{
						b = BuiltinInstr::BUILTIN::ATAN_F;
					}
					else if(ident=="atan2" || ident=="atan2f")
					{
						b = BuiltinInstr::BUILTIN::ATAN2_F;
					}
					else if(ident=="cos" || ident=="cosf" || intrinsicId==Intrinsic::cos)
					{
						b = BuiltinInstr::BUILTIN::COS_F;
					}
					else if(ident=="exp" || ident=="expf" || intrinsicId==Intrinsic::exp)
					{
						b = BuiltinInstr::BUILTIN::EXP_F;
					}
					else if(ident=="log" || ident=="logf" || intrinsicId==Intrinsic::log)
					{
						b = BuiltinInstr::BUILTIN::LOG_F;
					}
					else if(ident=="pow" || ident=="powf" || intrinsicId==Intrinsic::pow)
					{
						b = BuiltinInstr::BUILTIN::POW_F;
					}
					else if(ident=="sin" || ident=="sinf" || intrinsicId==Intrinsic::sin)
					{
						b = BuiltinInstr::BUILTIN::SIN_F;
					}
					else if(ident=="tan" || ident=="tanf")
					{
						b = BuiltinInstr::BUILTIN::TAN_F;
					}

					if (b == BuiltinInstr::BUILTIN::SIN_F ||
						b == BuiltinInstr::BUILTIN::COS_F)
						b = BuiltinInstr::BUILTIN::NONE;

					if(b != BuiltinInstr::BUILTIN::NONE)
					{
						// We will use a builtin, do float conversion if needed
						bool floatType = calledFunc->getReturnType()->isFloatTy();
						for (auto op = ci.op_begin(); op != ci.op_begin() + fTy->getNumParams(); ++op)
						{
							compileOperand(code, op->get());
							if(floatType)
								encodeInst(0xbb, "f64.promote/f32", code);
						}
						uint32_t importedId = linearHelper.getBuiltinId(b);
						assert(importedId);
						encodeU32Inst(0x10, "call", importedId, code);
						if(floatType)
							encodeInst(0xb6, "f32.demote/f64", code);
						// TODO: We could tail call if the type matches
						if(useTailCall)
						{
							encodeInst(0x0f, "return", code);
							return true;
						}
						return false;
					}
				}
			}

			//This corrections is needed basically for ctlz / cttz since they have an extra parameters to be ignored
			const unsigned int numUsedParameters = fTy->getNumParams() - TypedBuiltinInstr::numExtraParameters(calledFunc);
			for (auto op = ci.op_begin();
					op != ci.op_begin() + numUsedParameters; ++op)
			{
				compileOperand(code, op->get());
			}

			if (calledFunc)
			{
				if (TypedBuiltinInstr::isWasmIntrinsic(calledFunc))
				{
					encodeWasmIntrinsic(code, calledFunc);
					if(useTailCall)
						encodeInst(0x0f, "return", code);
					return useTailCall;
				}
				else if (linearHelper.getFunctionIds().count(calledFunc))
				{
					uint32_t functionId = linearHelper.getFunctionIds().at(calledFunc);
					if (functionId < COMPILE_METHOD_LIMIT) {
						if(useTailCall)
							encodeU32Inst(0x12, "return_call", functionId, code);
						else
							encodeU32Inst(0x10, "call", functionId, code);
					} else {
						encodeInst(0x00, "unreachable", code);
					}
				}
				else
				{
					llvm::errs() << "warning: Undefined function " << calledFunc->getName() << " called\n";
					encodeInst(0x00, "unreachable", code);
					return true;
				}
			}
			else
			{
				if (linearHelper.getFunctionTables().count(fTy))
				{
					const auto& table = linearHelper.getFunctionTables().at(fTy);
					compileOperand(code, calledValue);
					if (mode == CheerpWasmWriter::WASM) {
						if(useTailCall)
							encodeU32U32Inst(0x13, "return_call_indirect", table.typeIndex, 0, code);
						else
							encodeU32U32Inst(0x11, "call_indirect", table.typeIndex, 0, code);
					} else {
						//code << "call_indirect $vt_" << table.name << '\n';
						code << "call_indirect " << table.typeIndex << '\n';
					}
				}
				else
				{
					encodeInst(0x00, "unreachable", code);
					return true;
				}
			}

			if(ci.getType()->isVoidTy())
				return true;
			break;
		}
		case Instruction::FCmp:
		{
			const CmpInst& ci = cast<CmpInst>(I);
			compileFCmp(ci.getOperand(0), ci.getOperand(1), ci.getPredicate(), code);
			break;
		}
		case Instruction::FRem:
		{
			// No FRem in wasm, implement manually
			// frem x, y -> fsub (x, fmul( ftrunc ( fdiv (x, y) ), y ) )
			compileOperand(code, I.getOperand(0));
			compileOperand(code, I.getOperand(0));
			compileOperand(code, I.getOperand(1));
#define BINOPF(name, f32, f64) \
			if (I.getType()->isFloatTy()) { \
				encodeInst(f32, "f32."#name, code); \
			} \
			else if (I.getType()->isDoubleTy()) { \
				encodeInst(f64, "f64."#name, code); \
			} else { \
				assert(false); \
			}
			BINOPF(  div, 0x95, 0xa3)
			BINOPF(trunc, 0x8f, 0x9d)
			compileOperand(code, I.getOperand(1));
			BINOPF(  mul, 0x94, 0xa2)
			BINOPF(  sub, 0x93, 0xa1)
#undef BINOPF
			break;
		}
		case Instruction::GetElementPtr:
		{
			compileGEP(code, &I);
			break;
		}
		case Instruction::ICmp:
		{
			const ICmpInst& ci = cast<ICmpInst>(I);
			ICmpInst::Predicate p = ci.getPredicate();
			compileICmp(ci, p, code);
			break;
		}
		case Instruction::Load:
		{
			const LoadInst& li = cast<LoadInst>(I);
			if(GlobalVariable* ptrGV = dyn_cast<GlobalVariable>(li.getOperand(0)))
			{
				auto it = globalizedGlobalsIDs.find(ptrGV);
				if(it != globalizedGlobalsIDs.end())
				{
					// We can encode this as a get_global
					encodeU32Inst(0x23, "get_global", it->second, code);
					break;
				}
			}
			compileLoad(code, li, /*signExtend*/isSignedLoad(&li));
			break;
		}
		case Instruction::PtrToInt:
		{
			compileOperand(code, I.getOperand(0));
			break;
		}
		case Instruction::Store:
		{
			const StoreInst& si = cast<StoreInst>(I);
			const Value* ptrOp=si.getPointerOperand();
			const Value* valOp=si.getValueOperand();
			if(const GlobalVariable* ptrGV = dyn_cast<GlobalVariable>(ptrOp))
			{
				auto it = globalizedGlobalsIDs.find(ptrGV);
				if(it != globalizedGlobalsIDs.end())
				{
					// We can encode this as a set_global
					compileOperand(code, valOp);
					encodeU32Inst(0x24, "set_global", it->second, code);
					break;
				}
			}
			// 1) The pointer
			uint32_t offset = compileLoadStorePointer(code, ptrOp);
			// Special case writing 0 to floats/double
			if(valOp->getType()->isFloatingPointTy() && isa<Constant>(valOp) && cast<Constant>(valOp)->isNullValue())
			{
				if(valOp->getType()->isFloatTy())
				{
					encodeS32Inst(0x41, "i32.const", 0, code);
					encodeU32U32Inst(0x36, "i32.store", 0x2, offset, code);
				}
				else
				{
					assert(valOp->getType()->isDoubleTy());
					encodeS32Inst(0x42, "i64.const", 0, code);
					encodeU32U32Inst(0x37, "i64.store", 0x3, offset, code);
				}
				break;
			}
			// 2) The value
			compileOperand(code, valOp);
			// 3) Store
			// When storing values with size less than 32-bit we need to truncate them
			if(valOp->getType()->isIntegerTy())
			{
				uint32_t bitWidth = valOp->getType()->getIntegerBitWidth();
				if(bitWidth == 1)
					bitWidth = 8;

				// TODO add support for i64.
				switch (bitWidth)
				{
					case 8:
						encodeU32U32Inst(0x3a, "i32.store8", 0x0, offset, code);
						break;
					case 16:
						encodeU32U32Inst(0x3b, "i32.store16", 0x1, offset, code);
						break;
					case 32:
						encodeU32U32Inst(0x36, "i32.store", 0x2, offset, code);
						break;
					default:
						llvm::errs() << "bit width: " << bitWidth << '\n';
						llvm_unreachable("unknown integer bit width");
				}
			} else {
				if (valOp->getType()->isFloatTy())
					encodeU32U32Inst(0x38, "f32.store", 0x2, offset, code);
				else if (valOp->getType()->isDoubleTy())
					encodeU32U32Inst(0x39, "f64.store", 0x3, offset, code);
				else
					encodeU32U32Inst(0x36, "i32.store", 0x2, offset, code);
			}
			break;
		}
		case Instruction::Switch:
			break;
		case Instruction::Trunc:
		{
			compileOperand(code, I.getOperand(0));
			break;
		}
		case Instruction::Ret:
		{
			const ReturnInst& ri = cast<ReturnInst>(I);
			Value* retVal = ri.getReturnValue();
			if(retVal)
			{
				// NOTE: If the retValue is inlineable we must render it here
				//       If 'isReturnPartOfTailCall' return true then retVal must be a CallInst
				//       so blindly casting it to Instruction is safe
				if(isReturnPartOfTailCall(ri) && !isInlineable(*cast<Instruction>(retVal)))
					break;
				compileOperand(code, I.getOperand(0));
			}
			break;
		}
		case Instruction::Select:
		{
			const SelectInst& si = cast<SelectInst>(I);
			compileOperand(code, si.getTrueValue());
			compileOperand(code, si.getFalseValue());
			compileCondition(code, si.getCondition(), /*booleanInvert*/false);
			encodeInst(0x1b, "select", code);
			break;
		}
		case Instruction::SExt:
		{
			const Value* op = I.getOperand(0);
			compileOperand(code, op);
			if(!isSignedLoad(op))
			{
				uint32_t bitWidth = I.getOperand(0)->getType()->getIntegerBitWidth();
				encodeS32Inst(0x41, "i32.const", 32-bitWidth, code);
				encodeInst(0x74, "i32.shl", code);
				encodeS32Inst(0x41, "i32.const", 32-bitWidth, code);
				encodeInst(0x75, "i32.shr_s", code);
			}
			break;
		}
		case Instruction::FPToSI:
		{
			// TODO: add support for i64.
			// Wasm opcodes traps on invalid values, we need to do an explicit check if requested
			if(!AvoidWasmTraps)
			{
				compileOperand(code, I.getOperand(0));
				if(I.getOperand(0)->getType()->isFloatTy())
					encodeInst(0xa8, "i32.trunc_s/f32", code);
				else
					encodeInst(0xaa, "i32.trunc_s/f64", code);
			}
			else if (I.getOperand(0)->getType()->isFloatTy())
			{
				compileOperand(code, I.getOperand(0));
				encodeInst(0x8b, "f32.abs", code);
				encodeInst(0x43, "f32.const", code);
				internal::encodeF32(0x80000000, code);
				// Use LT here, we are using the first invalid positive integer as the limit value
				encodeInst(0x5d, "f32.lt", code);
				encodeU32Inst(0x04, "if", 0x7f, code);
				compileOperand(code, I.getOperand(0));
				encodeInst(0xa8, "i32.trunc_s/f32", code);
				encodeInst(0x05, "else", code);
				// We excluded the valid INT32_MIN in the range above, but in the undefined case we use it unconditionally
				encodeS32Inst(0x41, "i32.const", INT32_MIN, code);
				encodeInst(0x0b, "end", code);
			}
			else
			{
				compileOperand(code, I.getOperand(0));
				encodeInst(0x99, "f64.abs", code);
				encodeInst(0x43, "f32.const", code);
				internal::encodeF32(0x80000000, code);
				encodeInst(0xbb, "f64.promote/f32", code);
				// Use LT here, we are using the first invalid positive integer as the limit value
				encodeInst(0x63, "f64.lt", code);
				encodeU32Inst(0x04, "if", 0x7f, code);
				compileOperand(code, I.getOperand(0));
				encodeInst(0xaa, "i32.trunc_s/f64", code);
				encodeInst(0x05, "else", code);
				// We excluded the valid INT32_MIN in the range above, but in the undefined case we use it unconditionally
				encodeS32Inst(0x41, "i32.const", INT32_MIN, code);
				encodeInst(0x0b, "end", code);
			}
			break;
		}
		case Instruction::FPToUI:
		{
			// TODO: add support for i64.
			// Wasm opcodes traps on invalid values, we need to do an explicit check if requested
			if(!AvoidWasmTraps)
			{
				compileOperand(code, I.getOperand(0));
				if(I.getOperand(0)->getType()->isFloatTy())
					encodeInst(0xa9, "i32.trunc_u/f32", code);
				else
					encodeInst(0xab, "i32.trunc_u/f64", code);
			}
			else if (I.getOperand(0)->getType()->isFloatTy())
			{
				compileOperand(code, I.getOperand(0));
				encodeInst(0x43, "f32.const", code);
				internal::encodeF32(0x100000000LL, code);
				// Use LT here, we are using the first invalid positive integer as the limit value
				encodeInst(0x5d, "f32.lt", code);
				// Also compare against 0
				compileOperand(code, I.getOperand(0));
				encodeInst(0x43, "f32.const", code);
				internal::encodeF32(0, code);
				encodeInst(0x60, "f32.ge", code);
				encodeInst(0x71, "i32.and", code);
				encodeU32Inst(0x04, "if", 0x7f, code);
				compileOperand(code, I.getOperand(0));
				encodeInst(0xa9, "i32.trunc_u/f32", code);
				encodeInst(0x05, "else", code);
				encodeS32Inst(0x41, "i32.const", 0, code);
				encodeInst(0x0b, "end", code);
			}
			else
			{
				compileOperand(code, I.getOperand(0));
				encodeInst(0x43, "f32.const", code);
				internal::encodeF32(0x100000000LL, code);
				encodeInst(0xbb, "f64.promote/f32", code);
				// Use LT here, we are using the first invalid positive integer as the limit value
				encodeInst(0x63, "f64.lt", code);
				// Also compare against 0
				compileOperand(code, I.getOperand(0));
				encodeInst(0x43, "f32.const", code);
				internal::encodeF32(0, code);
				encodeInst(0xbb, "f64.promote/f32", code);
				encodeInst(0x66, "f64.ge", code);
				encodeInst(0x71, "i32.and", code);
				encodeU32Inst(0x04, "if", 0x7f, code);
				compileOperand(code, I.getOperand(0));
				encodeInst(0xab, "i32.trunc_u/f64", code);
				encodeInst(0x05, "else", code);
				encodeS32Inst(0x41, "i32.const", 0, code);
				encodeInst(0x0b, "end", code);
			}
			break;
		}
		case Instruction::SIToFP:
		{
			assert(I.getOperand(0)->getType()->isIntegerTy());
			compileOperand(code, I.getOperand(0));
			uint32_t bitWidth = I.getOperand(0)->getType()->getIntegerBitWidth();
			if(bitWidth != 32)
			{
				// Sign extend
				encodeS32Inst(0x41, "i32.const", 32-bitWidth, code);
				encodeInst(0x74, "i32.shl", code);
				encodeS32Inst(0x41, "i32.const", 32-bitWidth, code);
				encodeInst(0x75, "i32.shr_s", code);
			}
			// TODO: add support for i64.
			if (I.getType()->isDoubleTy()) {
				encodeInst(0xb7, "f64.convert_s/i32", code);
			} else {
				assert(I.getType()->isFloatTy());
				encodeInst(0xb2, "f32.convert_s/i32", code);
			}
			break;
		}
		case Instruction::UIToFP:
		{
			assert(I.getOperand(0)->getType()->isIntegerTy());
			compileOperand(code, I.getOperand(0));
			uint32_t bitWidth = I.getOperand(0)->getType()->getIntegerBitWidth();
			if(bitWidth != 32)
			{
				encodeS32Inst(0x41, "i32.const", getMaskForBitWidth(bitWidth), code);
				encodeInst(0x71, "i32.and", code);
			}
			// TODO: add support for i64.
			if (I.getType()->isDoubleTy()) {
				encodeInst(0xb8, "f64.convert_u/i32", code);
			} else {
				assert(I.getType()->isFloatTy());
				encodeInst(0xb3, "f32.convert_u/i32", code);
			}
			break;
		}
		case Instruction::FPTrunc:
		{
			assert(I.getType()->isFloatTy());
			assert(I.getOperand(0)->getType()->isDoubleTy());
			compileOperand(code, I.getOperand(0));
			encodeInst(0xb6, "f32.demote/f64", code);
			break;
		}
		case Instruction::FPExt:
		{
			assert(I.getType()->isDoubleTy());
			assert(I.getOperand(0)->getType()->isFloatTy());
			compileOperand(code, I.getOperand(0));
			encodeInst(0xbb, "f64.promote/f32", code);
			break;
		}
		case Instruction::ZExt:
		{
			compileUnsignedInteger(code, I.getOperand(0));
			break;
		}
		case Instruction::IntToPtr:
		{
			compileOperand(code, I.getOperand(0));
			break;
		}
		case Instruction::Unreachable:
		{
			encodeInst(0x00, "unreachable", code);
			break;
		}
		default:
		{
#ifndef NDEBUG
			I.dump();
#endif
			llvm::errs() << "\tImplement inst " << I.getOpcodeName() << '\n';
		}
	}
	return false;
}

void CheerpWasmWriter::compileInstructionAndSet(WasmBuffer& code, const llvm::Instruction& I)
{
	if (compiled.count(&I) || I.getParent() != currentBB)
		return;
	if (isa<PHINode>(&I) || isInlineable(I))
		return;
	if(const IntrinsicInst* II=dyn_cast<IntrinsicInst>(&I))
	{
		//Skip some kind of intrinsics
		if(II->getIntrinsicID()==Intrinsic::lifetime_start ||
			II->getIntrinsicID()==Intrinsic::lifetime_end ||
			II->getIntrinsicID()==Intrinsic::dbg_declare ||
			II->getIntrinsicID()==Intrinsic::dbg_value ||
			II->getIntrinsicID()==Intrinsic::assume)
		{
			return;
		}
	}

	const bool needsSubStack = teeLocals.needsSubStack(code);

	if (needsSubStack)
		teeLocals.addIndentation(code);

	auto lastUsedCandidate = teeLocals.lastUsed();

	flushMemoryDependencies(code, I);

	assert(compiled.count(&I) == 0);
	compiled.insert(&I);
	const bool ret = compileInstruction(code, I);

	flushSetLocalDependencies(code, I);

	teeLocals.removeConsumed(lastUsedCandidate);

	if (needsSubStack)
		teeLocals.decreaseIndentation(code, /*performCheck*/false);

	if(!ret && !I.getType()->isVoidTy())
	{
		if(I.use_empty()) {
			encodeInst(0x1a, "drop", code);
		} else {
			uint32_t reg = registerize.getRegisterId(&I, edgeContext);
			uint32_t local = localMap.at(reg);
			teeLocals.addCandidate(&I, /*isInstructionAssigment*/true, local, code.tellp());
			encodeU32Inst(0x21, "set_local", local, code);
		}
	}
	teeLocals.instructionStart(code);
}

bool CheerpWasmWriter::shouldDefer(const llvm::Instruction* I) const
{
	// Figure out if we can defer this instruction for a gain
	// Must have a user in the same BB (this also automatically deals with instruction without users
	bool hasUserInSameBlock = false;
	for(const User* u: I->users())
	{
		if(cast<Instruction>(u)->getParent() == currentBB)
		{
			hasUserInSameBlock = true;
			break;
		}
	}
	return !hasUserInSameBlock;
}

void CheerpWasmWriter::compileBB(WasmBuffer& code, const BasicBlock& BB)
{
	assert(localsDependencies.empty());
	assert(memoryDependencies.empty());
	assert(!currentBB);
	currentBB = &BB;
	assert(deferred.empty());
	BasicBlock::const_iterator I=BB.begin();
	BasicBlock::const_iterator IE=BB.end();
	const llvm::Instruction* lastStoreLike = nullptr;
	std::vector<const llvm::Instruction*> instructionsLoadLike;
	llvm::DenseMap<uint32_t, std::vector<const Instruction*>> getLocalFromRegister;
	llvm::DenseMap<uint32_t, const Instruction*> lastAssignedToRegister;

	for(;I!=IE;++I)
	{
		//Calculate dependencies for each get local in a tree of inlineable instructions
		if(I->getOpcode()!=Instruction::PHI)
		{
			std::vector<const llvm::Instruction*> queue;
			queue.push_back(&*I);
			while (!queue.empty())
			{
				const llvm::Instruction* curr = queue.back();
				queue.pop_back();
				for (const auto& op : curr->operands())
				{
					if (!isa<Instruction>(op))
						continue;
					const llvm::Instruction* next = cast<Instruction>(op);
					if (registerize.hasRegister(next))
					{
						const uint32_t ID = registerize.getRegisterId(next, edgeContext);
						if (lastAssignedToRegister.count(ID))
							localsDependencies[&*I].insert(lastAssignedToRegister[ID]);
						getLocalFromRegister[ID].push_back(&*I);
					}
					else
						queue.push_back(next);
				}
			}
		}

		//Calculate dependencies for a setLocal, that is all getLocal done on the previously set setLocal for the same ID
		//Note that this HAS to be performed also for PHI
		if (registerize.hasRegister(&*I))
		{
			assert(!isInlineable(*I));

			const uint32_t ID = registerize.getRegisterId(&*I, edgeContext);

			std::vector<const llvm::Instruction*> queue(getLocalFromRegister[ID].begin(), getLocalFromRegister[ID].end());
			while (!queue.empty())
			{
				const llvm::Instruction* curr = queue.back();
				queue.pop_back();
				if (!isInlineable(*curr))
					localsDependencies[&*I].insert(curr);
				else
				{
					for (const User* User : curr->users())
					{
						const llvm::Instruction* next = cast<Instruction>(User);
						if (!isa<PHINode>(next) && next->getParent() == currentBB)
							queue.push_back(next);
					}
				}
			}
			getLocalFromRegister[ID].clear();

			lastAssignedToRegister[ID] = &*I;
		}

		if(I->getOpcode()==Instruction::PHI)
		{
			//Phis are manually handled
			continue;
		}
		if(const IntrinsicInst* II=dyn_cast<IntrinsicInst>(&(*I)))
		{
			//Skip some kind of intrinsics
			if(II->getIntrinsicID()==Intrinsic::lifetime_start ||
				II->getIntrinsicID()==Intrinsic::lifetime_end ||
				II->getIntrinsicID()==Intrinsic::dbg_declare ||
				II->getIntrinsicID()==Intrinsic::dbg_value ||
				II->getIntrinsicID()==Intrinsic::assume)
			{
				continue;
			}
		}

		// Display file and line markers in WAST for debugging purposes
		const llvm::DebugLoc& debugLoc = I->getDebugLoc();
		if (debugLoc && mode == CheerpWasmWriter::WAST) {
			MDNode* file = debugLoc.getScope();
			assert(file);
			assert(file->getNumOperands()>=2);
			MDNode* fileNamePath = cast<MDNode>(file->getOperand(1));
			assert(fileNamePath->getNumOperands()==2);
			StringRef fileName = cast<MDString>(fileNamePath->getOperand(0))->getString();
			uint32_t currentLine = debugLoc.getLine();
			code << ";; " << fileName.str() << ":" << currentLine << "\n";
		}

//		if(I->isTerminator() || !I->use_empty() || I->mayHaveSideEffects())
		if (!isInlineable(*I))
		{
			deferred.push_back(&*I);

			bool mayHaveSideEffects = I->mayHaveSideEffects();
			bool mayReadFromMemory = I->mayReadFromMemory();
			std::vector<const Instruction*> queue;
			for (auto & op : I->operands())
				if (isa<Instruction>(op))
					queue.push_back(cast<Instruction>(op));
			while (!queue.empty())
			{
				const Instruction* curr = queue.back();
				queue.pop_back();
				if (!isInlineable(*curr))
					continue;
				if (curr->mayReadFromMemory())
					mayReadFromMemory = true;
				if (curr->mayHaveSideEffects())
					mayHaveSideEffects = true;
				for (auto & op : curr->operands())
					if (isa<Instruction>(op))
						queue.push_back(cast<Instruction>(op));
			}

			if (mayHaveSideEffects)
			{
				assert(isInlineable(*I) == false);
				if (lastStoreLike)
					memoryDependencies[&*I].insert(lastStoreLike);
				lastStoreLike = &*I;
				for (auto& x: instructionsLoadLike)
					memoryDependencies[&*I].insert(x);
				instructionsLoadLike.clear();
			}
			else if (mayReadFromMemory)
			{
				instructionsLoadLike.push_back(&*I);
				if (lastStoreLike)
					memoryDependencies[&*I].insert(lastStoreLike);
			}
		}
	}

	checkAndSanitizeDependencies(memoryDependencies);
	checkAndSanitizeDependencies(localsDependencies);

#ifdef STRESS_DEFERRED
	reverse(deferred.begin(), deferred.end()-1);
#endif
	renderDeferred(code, deferred);

	deferred.clear();
	currentBB = nullptr;
	localsDependencies.clear();
	memoryDependencies.clear();
}

void CheerpWasmWriter::renderDeferred(WasmBuffer& code, const vector<const llvm::Instruction*>& deferred)
{
	for (const llvm::Instruction* I : deferred)
	{
		if (shouldDefer(I))
			compileInstructionAndSet(code, *I);
	}
	for (const llvm::Instruction* I : deferred)
		compileInstructionAndSet(code, *I);
}

void CheerpWasmWriter::compileMethodLocals(WasmBuffer& code, const vector<int>& locals)
{
	if (mode == CheerpWasmWriter::WASM) {
		uint32_t groups = (uint32_t) locals.at(Registerize::INTEGER) > 0;
		groups += (uint32_t) locals.at(Registerize::DOUBLE) > 0;
		groups += (uint32_t) locals.at(Registerize::FLOAT) > 0;
		groups += (uint32_t) locals.at(Registerize::OBJECT) > 0;

		// Local declarations are compressed into a vector whose entries
		// consist of:
		//
		//   - a u32 `count',
		//   - a `ValType',
		//
		// denoting `count' locals of the same `ValType'.
		internal::encodeULEB128(groups, code);

		if (locals.at(Registerize::INTEGER)) {
			internal::encodeULEB128(locals.at(Registerize::INTEGER), code);
			internal::encodeRegisterKind(Registerize::INTEGER, code);
		}

		if (locals.at(Registerize::DOUBLE)) {
			internal::encodeULEB128(locals.at(Registerize::DOUBLE), code);
			internal::encodeRegisterKind(Registerize::DOUBLE, code);
		}

		if (locals.at(Registerize::FLOAT)) {
			internal::encodeULEB128(locals.at(Registerize::FLOAT), code);
			internal::encodeRegisterKind(Registerize::FLOAT, code);
		}

		if (locals.at(Registerize::OBJECT)) {
			internal::encodeULEB128(locals.at(Registerize::OBJECT), code);
			internal::encodeRegisterKind(Registerize::OBJECT, code);
		}
	} else {
		code << "(local";

		if (locals.at(Registerize::INTEGER)) {
			for (int i = 0; i < locals.at(Registerize::INTEGER); i++)
				code << " i32";
		}

		if (locals.at(Registerize::DOUBLE)) {
			for (int i = 0; i < locals.at(Registerize::DOUBLE); i++)
				code << " f64";
		}

		if (locals.at(Registerize::FLOAT)) {
			for (int i = 0; i < locals.at(Registerize::FLOAT); i++)
				code << " f32";
		}

		if (locals.at(Registerize::OBJECT)) {
			for (int i = 0; i < locals.at(Registerize::OBJECT); i++)
				code << " anyref";
		}

		code << ")\n";
	}
}

void CheerpWasmWriter::compileMethodParams(WasmBuffer& code, const FunctionType* fTy)
{
	uint32_t numArgs = fTy->getNumParams();
	if (mode == CheerpWasmWriter::WASM)
	{
		internal::encodeULEB128(numArgs, code);

		for(uint32_t i = 0; i < numArgs; i++)
			internal::encodeValType(fTy->getParamType(i), code);
	}
	else if(fTy->getNumParams())
	{
		assert(mode == CheerpWasmWriter::WAST);
		code << "(param";
		for(uint32_t i = 0; i < numArgs; i++)
			code << ' ' << getTypeString(fTy->getParamType(i));
		code << ')';
	}
}

void CheerpWasmWriter::compileMethodResult(WasmBuffer& code, const Type* ty)
{
	if (mode == CheerpWasmWriter::WASM)
	{
		if (ty->isVoidTy())
		{
			internal::encodeULEB128(0, code);
		}
		else
		{
			internal::encodeULEB128(1, code);
			internal::encodeValType(ty, code);
		}
	}
	else if(!ty->isVoidTy())
	{
		assert(mode == CheerpWasmWriter::WAST);
		code << "(result " << getTypeString(ty) << ')';
	}
}

void CheerpWasmWriter::compileCondition(WasmBuffer& code, const llvm::Value* cond, bool booleanInvert)
{
	bool canInvertCond = isa<Instruction>(cond) && isInlineable(*cast<Instruction>(cond));

	if(canInvertCond && isa<ICmpInst>(cond))
	{
		const ICmpInst* ci = cast<ICmpInst>(cond);
		CmpInst::Predicate p = ci->getPredicate();
		if(booleanInvert)
			p = CmpInst::getInversePredicate(p);
		Value* op0 = ci->getOperand(0);
		Value* op1 = ci->getOperand(1);
		if(ci->isCommutative() && isa<Constant>(op0))
		{
			// Move the constant on op1 to simplify the logic below
			std::swap(op0, op1);
		}
		// Optimize "if (a != 0)" to "if (a)" and "if (a == 0)" to "if (!a)".
		if ((p == CmpInst::ICMP_NE || p == CmpInst::ICMP_EQ) &&
				isa<Constant>(op1) &&
				cast<Constant>(op1)->isNullValue())
		{
			if(op0->getType()->isPointerTy())
				compileOperand(code, op0);
			else if(op0->getType()->isIntegerTy(32))
				compileSignedInteger(code, op0, /*forComparison*/true);
			else
				compileUnsignedInteger(code, op0);
			if(p == CmpInst::ICMP_EQ)
				encodeInst(0x45, "i32.eqz", code);
			teeLocals.removeConsumed();
			return;
		}
		compileICmp(op0, op1, p, code);
	}
	else if(canInvertCond && isa<FCmpInst>(cond))
	{
		const CmpInst* ci = cast<CmpInst>(cond);
		CmpInst::Predicate p = ci->getPredicate();
		if(booleanInvert)
			p = CmpInst::getInversePredicate(p);
		compileFCmp(ci->getOperand(0), ci->getOperand(1), p, code);
	}
	else
	{
		compileOperand(code, cond);
		if (booleanInvert) {
			// Invert result
			encodeInst(0x45, "i32.eqz", code);
		}
	}
	teeLocals.removeConsumed();
}

void CheerpWasmWriter::compileBranchTable(WasmBuffer& code, const llvm::SwitchInst* si,
	const std::vector<std::pair<int, int>>& cases)
{
	assert(si->getNumCases());

	uint32_t bitWidth = si->getCondition()->getType()->getIntegerBitWidth();

	auto getCaseValue = [](const ConstantInt* c, uint32_t bitWidth) -> int64_t
	{
		return bitWidth == 32 ? c->getSExtValue() : c->getZExtValue();
	};

	llvm::BasicBlock* defaultDest = si->getDefaultDest();
	int64_t max = std::numeric_limits<int64_t>::min();
	int64_t min = std::numeric_limits<int64_t>::max();
	for (auto& c: si->cases())
	{
		if (c.getCaseSuccessor() == defaultDest)
			continue;
		int64_t curr = getCaseValue(c.getCaseValue(), bitWidth);
		max = std::max(max, curr);
		min = std::min(min, curr);
	}

	// There should be at least one default case and zero or more cases.
	uint32_t depth = max - min + 1;
	assert(depth >= 1);

	// Fill the jump table.
	std::vector<uint32_t> table;
	table.assign(depth, numeric_limits<uint32_t>::max());
	uint32_t defaultIdx = numeric_limits<uint32_t>::max();

	for (auto c: cases)
	{
		if (c.first == 0)
			defaultIdx = c.second;
		else
		{
			// The value to match for case `i` has index `2*i`
			auto cv = cast<ConstantInt>(si->getOperand(2*c.first));
			table.at(getCaseValue(cv, bitWidth) - min) = c.second;
		}
	}

	// Elements that are not set, will jump to the default block.
	std::replace(table.begin(), table.end(), numeric_limits<uint32_t>::max(),
		defaultIdx);

	// Print the condition
	compileOperand(code, si->getCondition());
	if (min != 0)
	{
		encodeS32Inst(0x41, "i32.const", min, code);
		encodeInst(0x6b, "i32.sub", code);
	}
	if (bitWidth != 32 && CheerpWriter::needsUnsignedTruncation(si->getCondition(), /*asmjs*/true))
	{
		assert(bitWidth < 32);
		encodeS32Inst(0x41, "i32.const", getMaskForBitWidth(bitWidth), code);
		encodeInst(0x71, "i32.and", code);
	}

	// Print the case labels and the default label.
	encodeBranchTable(code, table, defaultIdx);
}

const BasicBlock* CheerpWasmWriter::compileTokens(WasmBuffer& code,
	const TokenList& Tokens)
{
	std::vector<const Token*> ScopeStack;
	const BasicBlock* lastDepth0Block = nullptr;
	auto indent = [&]()
	{
		if (mode == CheerpWasmWriter::WASM)
			return;

		for(uint32_t i=0;i<ScopeStack.size();i++)
			code << "  ";
	};
	auto getDepth = [&](const Token* Scope)
	{
		Scope = Scope->getKind() == Token::TK_Loop ? Scope : Scope->getMatch();
		auto it = std::find(ScopeStack.rbegin(), ScopeStack.rend(), Scope);
		assert(it != ScopeStack.rend());
		return std::distance(ScopeStack.rbegin(), it);
	};

	for (TokenList::const_iterator it = Tokens.begin(), ie = Tokens.end(); it != ie; ++it)
	{
		const Token& T = *it;
		teeLocals.instructionStart(code);
		switch (T.getKind())
		{
			case Token::TK_BasicBlock:
			{
				if (ScopeStack.empty())
					lastDepth0Block = T.getBB();
				else
					lastDepth0Block = nullptr;
				compileBB(code, *T.getBB());
				if(!lastDepth0Block)
				{
					const BasicBlock* BB = T.getBB();
					const Instruction* Term = BB->getTerminator();
					if (isa<ReturnInst>(Term) && !isReturnPartOfTailCall(*Term))
						encodeInst(0x0f, "return", code);
				}
				break;
			}
			case Token::TK_Loop:
			{
				teeLocals.addIndentation(code);
				indent();
				encodeU32Inst(0x03, "loop", 0x40, code);
				ScopeStack.push_back(&T);
				break;
			}
			case Token::TK_Block:
			{
				teeLocals.addIndentation(code);
				indent();
				encodeU32Inst(0x02, "block", 0x40, code);
				ScopeStack.emplace_back(&T);
				break;
			}
			case Token::TK_Condition:
			{
				const BranchInst* bi=cast<BranchInst>(T.getBB()->getTerminator());
				assert(bi->isConditional());
				compileCondition(code, bi->getCondition(), /*booleanInvert*/false);
				break;
			}
			case Token::TK_BrIf:
			case Token::TK_BrIfNot:
			{
				bool IfNot = T.getKind() == Token::TK_BrIfNot;
				// The condition goes first
				const BranchInst* bi=cast<BranchInst>(T.getBB()->getTerminator());
				assert(bi->isConditional());
				compileCondition(code, bi->getCondition(), IfNot);
				const int Depth = getDepth(T.getMatch());
				teeLocals.clearTopmostCandidates(code, Depth+1);
				encodeU32Inst(0x0d, "br_if", Depth, code);
				break;
			}
			case Token::TK_If:
			case Token::TK_IfNot:
			{
				bool IfNot = T.getKind() == Token::TK_IfNot;
				// The condition goes first
				const BranchInst* bi=cast<BranchInst>(T.getBB()->getTerminator());
				assert(bi->isConditional());
				compileCondition(code, bi->getCondition(), IfNot);
				teeLocals.addIndentation(code);
				indent();
				encodeU32Inst(0x04, "if", 0x40, code);
				ScopeStack.push_back(&T);
				break;
			}
			case Token::TK_Else:
			{
				teeLocals.decreaseIndentation(code);
				teeLocals.addIndentation(code);
				indent();
				encodeInst(0x05, "else", code);
				break;
			}
			case Token::TK_Branch:
			{
				const int Depth = getDepth(T.getMatch());
				teeLocals.clearTopmostCandidates(code, Depth+1);
				encodeU32Inst(0x0c, "br", Depth, code);
				break;
			}
			case Token::TK_End:
			{
				teeLocals.decreaseIndentation(code);
				ScopeStack.pop_back();
				indent();
				encodeInst(0x0b, "end", code);
				break;
			}
			case Token::TK_Prologue:
			{
				const BasicBlock* To = T.getBB()->getTerminator()->getSuccessor(T.getId());
				compilePHIOfBlockFromOtherBlock(code, To, T.getBB());
				break;
			}
			case Token::TK_Switch:
			{
				std::vector<std::pair<int, int>> Cases;
				const SwitchInst* si = cast<SwitchInst>(T.getBB()->getTerminator());
				it++;
				while(it->getKind() != Token::TK_End)
				{
					assert(it->getKind()==Token::TK_Case);
					std::vector<int> ids;
					while(it->getKind() == Token::TK_Case)
					{
						ids.push_back(it->getId());
						it++;
					}
					assert(it->getKind() == Token::TK_Branch);
					int Depth = getDepth(it->getMatch());
					for (int id: ids)
						Cases.push_back(std::make_pair(id, Depth));
					it++;
				}
				compileBranchTable(code, si, Cases);
				break;
			}
			case Token::TK_Case:
				report_fatal_error("Case token found outside of switch block");
				break;
			case Token::TK_Invalid:
				report_fatal_error("Invalid token found");
				break;
		}
	}
	return lastDepth0Block;
}
void CheerpWasmWriter::compileMethod(WasmBuffer& code, const Function& F)
{
	assert(!F.empty());
	currentFun = &F;

	if (mode == CheerpWasmWriter::WAST)
	{
		code << "(func $" << F.getName().str();

		// TODO: We should not export them all
		code << " (export \"" << NameGenerator::filterLLVMName(F.getName(),
					NameGenerator::NAME_FILTER_MODE::GLOBAL).str().str() << "\")";

		compileMethodParams(code, F.getFunctionType());
		compileMethodResult(code, F.getReturnType());

		code << '\n';
	}

	uint32_t numArgs = F.arg_size();
	const llvm::BasicBlock* lastDepth0Block = nullptr;

	Relooper* rl = nullptr;
	bool needsLabel = false;

	if (F.size() != 1 && useCfgLegacy) {
		rl = CheerpWriter::runRelooperOnFunction(F, PA, registerize);
		needsLabel = rl->needsLabel();
	}

	const std::vector<Registerize::RegisterInfo>& regsInfo = registerize.getRegistersForFunction(&F);
	uint32_t localCount = regsInfo.size() + (int)needsLabel;

	vector<int> locals(4, 0);
	localMap.assign(localCount, 0);
	uint32_t reg = 0;

	// Make lookup table for registers to locals.
	for(const Registerize::RegisterInfo& regInfo: regsInfo)
	{
		assert(!regInfo.needsSecondaryName);

		// Save the current local index
		localMap.at(reg) = numArgs + locals.at((int)regInfo.regKind);
		locals.at((int)regInfo.regKind)++;
		reg++;
	}

	if (needsLabel) {
		localMap.at(reg) = numArgs + locals.at((int)Registerize::INTEGER);
		locals.at((int)Registerize::INTEGER)++;
	}

	// Add offset of other local groups to local lookup table.  Since INTEGER
	// is the first group, the local for label does not require an offset.
	reg = 0;
	for(const Registerize::RegisterInfo& regInfo: regsInfo)
	{
		uint32_t offset = 0;
		switch (regInfo.regKind) {
			case Registerize::INTEGER:
				break;
			case Registerize::DOUBLE:
				offset += locals.at((int)Registerize::INTEGER);
				break;
			case Registerize::FLOAT:
				offset += locals.at((int)Registerize::INTEGER);
				offset += locals.at((int)Registerize::DOUBLE);
				break;
			case Registerize::OBJECT:
				offset += locals.at((int)Registerize::INTEGER);
				offset += locals.at((int)Registerize::DOUBLE);
				offset += locals.at((int)Registerize::FLOAT);
				break;
		}
		localMap[reg++] += offset;
	}

	compileMethodLocals(code, locals);

	teeLocals.performInitialization(code);

	if (F.size() == 1)
	{
		compileBB(code, *F.begin());
		lastDepth0Block = &(*F.begin());
	}
	else
	{
		const std::vector<Registerize::RegisterInfo>& regsInfo = registerize.getRegistersForFunction(&F);
		uint32_t numRegs = regsInfo.size();

		// label is the very last local
		uint32_t labelLocal = needsLabel ? localMap[numRegs] : 0;
		if (useCfgLegacy)
		{
			CheerpWasmRenderInterface ri(this, code, labelLocal);
			rl->Render(&ri);
			lastDepth0Block = ri.lastDepth0Block;
		}
		else
		{
			DominatorTree &DT = pass.getAnalysis<DominatorTreeWrapperPass>(const_cast<Function&>(F)).getDomTree();
			LoopInfo &LI = pass.getAnalysis<LoopInfoWrapperPass>(const_cast<Function&>(F)).getLoopInfo();
			CFGStackifier CN(F, LI, DT, registerize, PA, CFGStackifier::Wasm);
			lastDepth0Block = compileTokens(code, CN.Tokens);
		}
	}

	if (!useCfgLegacy)
	{
		checkImplicitedAssignedPhi(F);
		generateNOP(code);
	}

	getLocalDone.clear();
	teeLocals.clear(code);
	compiled.clear();

	// A function has to terminate with a return value when the return type is
	// not void.
	if (!lastDepth0Block || (!isa<ReturnInst>(lastDepth0Block->getTerminator()) && !isa<UnreachableInst>(lastDepth0Block->getTerminator())))
	{
		if(!F.getReturnType()->isVoidTy())
		{
			if (mode == CheerpWasmWriter::WASM) {
				// Encode a literal f64, f32 or i32 zero as the return value.
				internal::encodeLiteralType(F.getReturnType(), code);
				if (F.getReturnType()->isDoubleTy()) {
					internal::encodeF64(0., code);
				} else if (F.getReturnType()->isFloatTy()) {
					internal::encodeF32(0.f, code);
				} else {
					internal::encodeSLEB128(0, code);
				}
			} else {
				code << getTypeString(F.getReturnType()) << ".const 0\n";
			}
		}
	}

	if (mode == CheerpWasmWriter::WASM) {
		// Encode the end of the method.
		internal::encodeULEB128(0x0b, code);
	} else {
		assert(mode == CheerpWasmWriter::WAST);
		code << ")\n";
	}
}

//The call to requiresExplicitAssigment has the side effect of perfomring bookkeeping on the implicited assigned instructions
void CheerpWasmWriter::checkImplicitedAssignedPhi(const llvm::Function& F)
{
	for (const BasicBlock& BB : F)
	{
		for (const Instruction& I : BB)
		{
			if (!isa<PHINode>(I))
				break;
			const PHINode& phi = cast<PHINode>(I);
			for (uint32_t index = 0; index < phi.getNumIncomingValues(); index++)
				requiresExplicitAssigment(&phi, phi.getIncomingValue(index));
		}
	}
}

void CheerpWasmWriter::generateNOP(WasmBuffer& code)
{
	for (const TeeLocals::LocalInserted& localInserted : teeLocals.getLocalInserted())
	{
		const Instruction* I = localInserted.I;
		if (getLocalDone.count(I))
			continue;
		putNOP(code, localInserted.localId, localInserted.bufferOffset, teeLocals.isValueUsed(I));
	}
}

void CheerpWasmWriter::compileTypeSection()
{
	if (linearHelper.getFunctionTypes().empty())
		return;

	Section section(0x01, "Type", this);

	if (mode == CheerpWasmWriter::WASM)
	{
		// Encode number of entries in the type section.
		internal::encodeULEB128(linearHelper.getFunctionTypes().size(), section);

		// Define function type variables
		for (const auto& fTy : linearHelper.getFunctionTypes())
		{
			internal::encodeULEB128(0x60, section);
			compileMethodParams(section, fTy);
			compileMethodResult(section, fTy->getReturnType());
		}
	} else {
		// Define function type variables
		for (const auto& fTy : linearHelper.getFunctionTypes())
		{
			section << "(type " << "$vt_" << linearHelper.getFunctionTableName(fTy) << " (func ";
			compileMethodParams(section, fTy);
			compileMethodResult(section, fTy->getReturnType());
			section << "))\n";
		}
	}
}

void CheerpWasmWriter::compileImport(WasmBuffer& code, StringRef funcName, FunctionType* fTy)
{
	assert(useWasmLoader);

	std::string fieldName = funcName;

	if (mode == CheerpWasmWriter::WASM) {
		// Encode the module name.
		std::string moduleName = "i";
		internal::encodeULEB128(moduleName.size(), code);
		code.write(moduleName.data(), moduleName.size());

		// Encode the field name.
		internal::encodeULEB128(fieldName.size(), code);
		code.write(fieldName.data(), fieldName.size());

		// Encode kind as 'Function' (= 0).
		internal::encodeULEB128(0x00, code);

		// Encode type index of function signature.
		const auto& found = linearHelper.getFunctionTypeIndices().find(fTy);
		assert(found != linearHelper.getFunctionTypeIndices().end());
		internal::encodeULEB128(found->second, code);
	} else {
		code << "(func (import \"i\" \"";
		code.write(fieldName.data(), fieldName.size());
		code << "\")";
		uint32_t numArgs = fTy->getNumParams();
		if(numArgs)
		{
			code << "(param";
			for(uint32_t i = 0; i < numArgs; i++)
				code << ' ' << getTypeString(fTy->getParamType(i));
			code << ')';
		}
		if(!fTy->getReturnType()->isVoidTy())
			code << "(result " << getTypeString(fTy->getReturnType()) << ')';
		code << ")\n";
	}
}

void CheerpWasmWriter::compileImportSection()
{
	// Count imported builtins
	uint32_t importedBuiltins = 0;
	for(uint32_t i=0;i<BuiltinInstr::numGenericBuiltins();i++)
	{
		if(globalDeps.needsBuiltin(BuiltinInstr::BUILTIN(i)))
			importedBuiltins++;
	}

	uint32_t importedTotal = importedBuiltins + globalDeps.asmJSImports().size();

	if (importedTotal == 0 || !useWasmLoader)
		return;

	Section section(0x02, "Import", this);

	if (mode == CheerpWasmWriter::WASM) {
		// Encode number of entries in the import section.
		internal::encodeULEB128(importedTotal, section);
	}

	for (const Function* F : globalDeps.asmJSImports())
		compileImport(section, namegen.getName(F), F->getFunctionType());

	Type* f64 = Type::getDoubleTy(module.getContext());
	Type* i32 = Type::getInt32Ty(module.getContext());
	Type* f64_1[] = { f64 };
	Type* f64_2[] = { f64, f64 };
	Type* i32_1[] = { i32 };
	FunctionType* f64_f64_1 = FunctionType::get(f64, f64_1, false);
	FunctionType* f64_f64_2 = FunctionType::get(f64, f64_2, false);
	FunctionType* i32_i32_1 = FunctionType::get(i32, i32_1, false);
	if(globalDeps.needsBuiltin(BuiltinInstr::BUILTIN::ACOS_F))
		compileImport(section, namegen.getBuiltinName(NameGenerator::ACOS), f64_f64_1);
	if(globalDeps.needsBuiltin(BuiltinInstr::BUILTIN::ASIN_F))
		compileImport(section, namegen.getBuiltinName(NameGenerator::ASIN), f64_f64_1);
	if(globalDeps.needsBuiltin(BuiltinInstr::BUILTIN::ATAN_F))
		compileImport(section, namegen.getBuiltinName(NameGenerator::ATAN), f64_f64_1);
	if(globalDeps.needsBuiltin(BuiltinInstr::BUILTIN::ATAN2_F))
		compileImport(section, namegen.getBuiltinName(NameGenerator::ATAN2), f64_f64_2);
	if(globalDeps.needsBuiltin(BuiltinInstr::BUILTIN::COS_F))
		compileImport(section, namegen.getBuiltinName(NameGenerator::COS), f64_f64_1);
	if(globalDeps.needsBuiltin(BuiltinInstr::BUILTIN::EXP_F))
		compileImport(section, namegen.getBuiltinName(NameGenerator::EXP), f64_f64_1);
	if(globalDeps.needsBuiltin(BuiltinInstr::BUILTIN::LOG_F))
		compileImport(section, namegen.getBuiltinName(NameGenerator::LOG), f64_f64_1);
	if(globalDeps.needsBuiltin(BuiltinInstr::BUILTIN::POW_F))
		compileImport(section, namegen.getBuiltinName(NameGenerator::POW), f64_f64_2);
	if(globalDeps.needsBuiltin(BuiltinInstr::BUILTIN::SIN_F))
		compileImport(section, namegen.getBuiltinName(NameGenerator::SIN), f64_f64_1);
	if(globalDeps.needsBuiltin(BuiltinInstr::BUILTIN::TAN_F))
		compileImport(section, namegen.getBuiltinName(NameGenerator::TAN), f64_f64_1);
	if(globalDeps.needsBuiltin(BuiltinInstr::BUILTIN::GROW_MEM))
		compileImport(section, namegen.getBuiltinName(NameGenerator::GROW_MEM), i32_i32_1);
}

void CheerpWasmWriter::compileFunctionSection()
{
	if (linearHelper.getFunctionTypes().empty() || mode != CheerpWasmWriter::WASM)
		return;

	Section section(0x03, "Function", this);

	uint32_t count = linearHelper.functions().size();
	count = std::min(count, COMPILE_METHOD_LIMIT); // TODO

	// Encode number of entries in the function section.
	internal::encodeULEB128(count, section);

	// Define function type ids
	size_t i = 0;
	for (const Function* F : linearHelper.functions()) {
		const FunctionType* fTy = F->getFunctionType();
		const auto& found = linearHelper.getFunctionTypeIndices().find(fTy);
		assert(found != linearHelper.getFunctionTypeIndices().end());
		assert(found->second < linearHelper.getFunctionTypes().size());
		internal::encodeULEB128(found->second, section);

		if (++i >= COMPILE_METHOD_LIMIT)
			break; // TODO
	}
}


void CheerpWasmWriter::compileTableSection()
{
	if (linearHelper.getFunctionTables().empty())
		return;

	uint32_t count = 0;
	for (const auto& table : linearHelper.getFunctionTables())
		count += table.second.functions.size();
	count = std::min(count, COMPILE_METHOD_LIMIT); // TODO

	Section section(0x04, "Table", this);

	if (mode == CheerpWasmWriter::WASM) {
		// Encode number of function tables in the table section.
		internal::encodeULEB128(1, section);

		// Encode element type 'anyfunc'.
		internal::encodeULEB128(0x70, section);

		// Encode function tables in the table section.
		// Use a 'limit' (= 0x00) with only a maximum value.
		internal::encodeULEB128(0x00, section);
		internal::encodeULEB128(count, section);
	} else {
		assert(mode == CheerpWasmWriter::WAST);
		section << "(table anyfunc (elem";
		size_t j = 0;
		for (const FunctionType* fTy: linearHelper.getFunctionTableOrder()) {
			const auto table = linearHelper.getFunctionTables().find(fTy);
			for (const auto& F : table->second.functions) {
				section << " $" << F->getName().str();
				if (++j == COMPILE_METHOD_LIMIT)
					break; // TODO
			}
			if (j == COMPILE_METHOD_LIMIT)
				break; // TODO
		}
		section << "))\n";

	}
}

CheerpWasmWriter::GLOBAL_CONSTANT_ENCODING CheerpWasmWriter::shouldEncodeConstantAsGlobal(const Constant* C, uint32_t useCount, uint32_t getGlobalCost)
{
	assert(useCount > 1);
	if(const ConstantFP* CF = dyn_cast<ConstantFP>(C))
	{
		const uint32_t costAsLiteral = C->getType()->isDoubleTy() ? 9 : 5;
		// 1 (type) + costAsLiteral + 1 (end byte)
		const uint32_t globalInitCost = 2 + costAsLiteral;
		const uint32_t globalUsesCost = globalInitCost + getGlobalCost * useCount;
		uint32_t directUsesCost = costAsLiteral * useCount;
		if(globalUsesCost < directUsesCost)
			return FULL;
		else
			return NONE;
	}
	else
	{
		// We don't try to globalize integer constants as that has a negative performance impact
		return NONE;
	}
}

void CheerpWasmWriter::compileMemoryAndGlobalSection()
{
	// Define the memory for the module in WasmPage units. The heap size is
	// defined in MiB and the wasm page size is 64 KiB. Thus, the wasm heap
	// max size parameter is defined as: heapSize << 20 >> 16 = heapSize << 4.
	uint32_t maxMemory = heapSize << 4;
	uint32_t minMemory = (linearHelper.getHeapStart() + 65535) >> 16;

	// TODO use WasmPage variable instead of hardcoded '1>>16'.
	assert(WasmPage == 64 * 1024);
	
	if (noGrowMemory)
		minMemory = maxMemory;

	{
		Section section(0x05, "Memory", this);

		if (mode == CheerpWasmWriter::WASM) {
			internal::encodeULEB128(1, section);
			// from the spec:
			//limits ::= 0x00 n:u32          => {min n, max e, unshared}
			//           0x01 n:u32 m:u32    => {min n, max m, unshared}
			//           0x03 n:u32 m:u32    => {min n, max m, shared}
			// We use 0x01 and 0x03 only for now
			int memType = sharedMemory ? 0x03 : 0x01;
			internal::encodeULEB128(memType, section);
			// Encode minimum and maximum memory parameters.
			internal::encodeULEB128(minMemory, section);
			internal::encodeULEB128(maxMemory, section);
		} else {
			section << "(memory (export \"" << namegen.getBuiltinName(NameGenerator::MEMORY).str() << "\") " << minMemory << ' ' << maxMemory;
			if (sharedMemory)
				section << " shared";
			section << ")\n";
		}
	}

	// Temporary map for the globalized constants. We update the global one at the end, to avoid
	// global constants referencing each other
	std::unordered_map<const llvm::Constant*, std::pair<uint32_t, GLOBAL_CONSTANT_ENCODING>> globalizedConstantsTmp;
	std::unordered_map<const llvm::Constant*, uint32_t> orderOfInsertion;
	const LinearMemoryHelper::GlobalUsageMap& globalizedGlobalsUsage(linearHelper.getGlobalizedGlobalUsage());

	for (auto G : linearHelper.globals())
	{
		if (globalizedGlobalsUsage.count(G))
			orderOfInsertion[G] = orderOfInsertion.size();
	}
	// Gather all constants used multiple times, we want to encode those in the global section
	for (const Function* F: linearHelper.functions())
	{
		for(const BasicBlock& BB: *F)
		{
			for(const Instruction& I: BB)
			{
				// Heuristic: Avoid globalizing values which will be anyway encoded as a load/store offset
				// Fully constant GEPs will be a ConstantExpr, so when we see a GEP there are 2 possibilities
				// 1) A constant base pointer and at least a variable index: On a load/store the indexes will
				//    be computed on the stack, while the base pointer will be the offset
				// 2) A variable base pointer: Indexes will be accumulated in a total value, no reason for globalization
				// NOTE: We skip all GEP instructionsa here, assuming a load/store follows. This may not be necessarily the case.
				if(I.getOpcode() == Instruction::GetElementPtr)
					continue;
				for(Value* V: I.operands())
				{
					Constant* C = dyn_cast<Constant>(V);
					if(!C)
						continue;
					if(isa<Function>(C) || isa<ConstantPointerNull>(C))
						continue;
					if(isa<GlobalVariable>(C) && globalizedGlobalsUsage.count(cast<GlobalVariable>(C)))
					{
						// The whole global is globalized, there is no point in globalizing the address
						continue;
					}
					globalizedConstantsTmp[C].first++;
					if (orderOfInsertion.count(C) == 0)
						orderOfInsertion[C] = orderOfInsertion.size();
				}
			}
		}
	}
	// We need to order the constants by use count
	struct GlobalConstant
	{
		const Constant* C;
		uint32_t useCount;
		GLOBAL_CONSTANT_ENCODING encoding;
		uint32_t insertionIndex;
		// NOTE: We want to have the high use counts first
		bool operator<(const GlobalConstant& rhs) const
		{
			// We need to fully order these to keep the output consistent
			// So we use insertionIndex as tie-breaker
			if (useCount != rhs.useCount)
				return useCount > rhs.useCount;
			return insertionIndex < rhs.insertionIndex;
		}
	};
	std::vector<GlobalConstant> orderedConstants;
	// Remove single use constants right away
	auto it = globalizedConstantsTmp.begin();
	auto itEnd = globalizedConstantsTmp.end();
	while (it != itEnd)
	{
		if(it->second.first == 1)
			it = globalizedConstantsTmp.erase(it);
		else
		{
			orderedConstants.push_back(GlobalConstant{it->first, it->second.first, it->second.second, orderOfInsertion.at(it->first)});
			++it;
		}
	}
	for(auto& it: globalizedGlobalsUsage)
	{
		orderedConstants.push_back(GlobalConstant{it.first, it.second, GLOBAL, orderOfInsertion.at(it.first)});
	}

	std::sort(orderedConstants.begin(), orderedConstants.end());

	// Assign global ids
	uint32_t globalId = 1;
	for(uint32_t i=0;i<orderedConstants.size();i++)
	{
		GlobalConstant& GC = orderedConstants[i];
		if(GC.encoding == GLOBAL)
		{
			auto it = globalizedGlobalsUsage.find(cast<GlobalVariable>(GC.C));
			assert(it != globalizedGlobalsUsage.end());
			globalizedGlobalsIDs[it->first] = globalId++;
			continue;
		}
		// TODO: We need al helper function for this
		// NOTE: It is not the same as getIntEncodingLength since the global id is unsigned
		uint32_t getGlobalCost = 0;
		if(globalId < (1<<7))
			getGlobalCost = 2;
		else if(globalId < (1<<14))
			getGlobalCost = 3;
		else
			getGlobalCost = 4;
		GLOBAL_CONSTANT_ENCODING encoding = shouldEncodeConstantAsGlobal(GC.C, GC.useCount, getGlobalCost);
		GC.encoding = encoding;
		auto it = globalizedConstantsTmp.find(GC.C);
		if(encoding == NONE)
		{
			// Remove this constant from the map
			// Leave it in the vector, but skip it later
			globalizedConstantsTmp.erase(it);
		}
		else
		{
			it->second.first = globalId++;
			it->second.second = encoding;
		}
	}

	{
		Section section(0x06, "Global", this);

		// Start the stack from the end of default memory
		stackTopGlobal = usedGlobals++;
		uint32_t stackTop = linearHelper.getStackStart();

		if (mode == CheerpWasmWriter::WASM) {
			// There is the stack and the globalized constants
			internal::encodeULEB128(1 + globalizedConstantsTmp.size() + globalizedGlobalsIDs.size(), section);
			// The global has type i32 (0x7f) and is mutable (0x01).
			internal::encodeULEB128(0x7f, section);
			internal::encodeULEB128(0x01, section);
			// The global value is a 'i32.const' literal.
			internal::encodeLiteralType(Type::getInt32Ty(Ctx), section);
			internal::encodeSLEB128(stackTop, section);
			// Encode the end of the instruction sequence.
			internal::encodeULEB128(0x0b, section);
			// Render globals in reverse order
			for(auto it = orderedConstants.begin(); it != orderedConstants.end(); ++it)
			{
				const Constant* C = it->C;
				if(it->encoding == NONE)
					continue;
				else if(it->encoding == GLOBAL)
				{
					const GlobalVariable* GV = cast<GlobalVariable>(C);
					internal::encodeULEB128(internal::getValType(GV->getValueType()), section);
					// Mutable -> 1
					internal::encodeULEB128(0x01, section);
					assert(GV->hasInitializer());
					compileConstant(section, GV->getInitializer(), /*forGlobalInit*/true);
					internal::encodeULEB128(0x0b, section);
					continue;
				}
				// Constant type
				uint32_t valType = 0;
				switch(it->encoding)
				{
					case FULL:
						valType = internal::getValType(C->getType());
						break;
					default:
						assert(false);
				}
				internal::encodeULEB128(valType, section);
				// Immutable -> 0
				internal::encodeULEB128(0x00, section);
				switch(it->encoding)
				{
					case FULL:
					{
						compileConstant(section, C, /*forGlobalInit*/true);
						break;
					}
					default:
						assert(false);
				}
				internal::encodeULEB128(0x0b, section);
			}
		} else {
			section << "(global (mut i32) (i32.const " << stackTop << "))\n";
			for(auto it = orderedConstants.begin(); it != orderedConstants.end(); ++it)
			{
				const Constant* C = it->C;
				if(it->encoding == NONE)
					continue;
				const char* strType = nullptr;
				switch(it->encoding)
				{
					case FULL:
						strType = getTypeString(C->getType());
						break;
					default:
						assert(false);
				}
				stream << "(global " << strType << " (";
				// Compile the constant expression
				switch(it->encoding)
				{
					case FULL:
					{
						compileConstant(section, C, /*forGlobalInit*/true);
						break;
					}
					default:
						assert(false);
				}
				stream << "))\n";
			}
		}
	}
	globalizedConstants = std::move(globalizedConstantsTmp);
}

void CheerpWasmWriter::compileExportSection()
{
	if (mode == CheerpWasmWriter::WAST)
		return;

	Section section(0x07, "Export", this);
	std::vector<const llvm::Function*> exports;

	// Export the webMain symbol, if defined.
	const llvm::Function* entry = globalDeps.getEntryPoint();
	if(entry && entry->getSection() == StringRef("asmjs")) {
		assert(globalDeps.asmJSExports().find(entry) == globalDeps.asmJSExports().end());
		exports.push_back(entry);
	}

	// Add the static constructors
	for (const Function * F : globalDeps.constructors() )
	{
		if (F->getSection() == StringRef("asmjs"))
			exports.push_back(F);
	}
	// Add the list of asmjs-exported functions.
	exports.insert(exports.end(), globalDeps.asmJSExports().begin(),
			globalDeps.asmJSExports().end());

	// We export the memory unconditionally, but may also need to export the table
	uint32_t extraExports = 1;
	if(exportedTable)
		extraExports = 2;
	internal::encodeULEB128(exports.size() + extraExports, section);

	// Encode the memory.
	StringRef name = namegen.getBuiltinName(NameGenerator::MEMORY);
	internal::encodeULEB128(name.size(), section);
	section.write(name.data(), name.size());
	internal::encodeULEB128(0x02, section);
	internal::encodeULEB128(0, section);

	if(exportedTable)
	{
		// Encode the table
		StringRef name = "tbl";
		internal::encodeULEB128(name.size(), section);
		section.write(name.data(), name.size());
		internal::encodeULEB128(0x01, section);
		internal::encodeULEB128(0, section);
	}

	for (const llvm::Function* F : exports) {
		// Encode the method name.
		name = namegen.getName(F);

		internal::encodeULEB128(name.size(), section);
		section.write(name.data(), name.size());

		// Encode the function index (where '0x00' means that this export is a
		// function).
		internal::encodeULEB128(0x00, section);
		internal::encodeULEB128(linearHelper.getFunctionIds().find(F)->second, section);
	}
}

void CheerpWasmWriter::compileStartSection()
{
	// It's not possible to run constructors that depend on asmjs / generic js
	// code, since the heap can only be accessed after the module has been
	// initialised. Therefore, we disable the start section when a wasm loader
	// is generated.
	if (useWasmLoader)
		return;

	// Experimental entry point for wasm code
	llvm::Function* entry = module.getFunction("_start");
	if(!entry)
		return;

	uint32_t functionId = linearHelper.getFunctionIds().at(entry);
	if (functionId >= COMPILE_METHOD_LIMIT)
		return;

	Section section(0x08, "Start", this);

	if (mode == CheerpWasmWriter::WASM) {
		internal::encodeULEB128(functionId, section);
	} else {
		section << "(start " << functionId << ")\n";
	}
}

void CheerpWasmWriter::compileElementSection()
{
	if (mode == CheerpWasmWriter::WAST)
		return;
	if (linearHelper.getFunctionTables().empty())
		return;

	Section section(0x09, "Element", this);

	// There is only one element segment.
	internal::encodeULEB128(1, section);

	// The table index is 0 in the MVP.
	internal::encodeULEB128(0, section);

	// The offset into memory, which is the address.
	int32_t offset = 0;
	internal::encodeLiteralType(Type::getInt32Ty(Ctx), section);
	internal::encodeSLEB128(offset, section);
	// Encode the end of the instruction sequence.
	internal::encodeULEB128(0x0b, section);

	// Encode the sequence of function indices.
	std::ostringstream elem;
	size_t count = 0;
	for (const FunctionType* fTy: linearHelper.getFunctionTableOrder()) {
		const auto table = linearHelper.getFunctionTables().find(fTy);
		for (const auto& F : table->second.functions) {
			uint32_t idx = linearHelper.getFunctionIds().at(F);
			internal::encodeULEB128(idx, elem);
			count++;
		}
	}
	std::string buf = elem.str();
	internal::encodeULEB128(count, section);
	section << buf;
}

void CheerpWasmWriter::compileCodeSection()
{
	Section section(0x0a, "Code", this);

	if (mode == CheerpWasmWriter::WASM) {
		// Encode the number of methods in the code section.
		uint32_t count = linearHelper.functions().size();
		count = std::min(count, COMPILE_METHOD_LIMIT);
		internal::encodeULEB128(count, section);
#if WASM_DUMP_METHODS
		llvm::errs() << "method count: " << count << '\n';
#endif
	}

	size_t i = 0;

	for (const Function* F: linearHelper.functions())
	{
		if (mode == CheerpWasmWriter::WASM) {
			std::ostringstream method;
#if WASM_DUMP_METHODS
			llvm::errs() << i << " method name: " << F->getName() << '\n';
#endif
			compileMethod(method, *F);
			std::string buf = method.str();

			filterNop(buf);
			nopLocations.clear();

#if WASM_DUMP_METHOD_DATA
			llvm::errs() << "method length: " << buf.size() << '\n';
			llvm::errs() << "method: " << string_to_hex(buf) << '\n';
#endif
			internal::encodeULEB128(buf.size(), section);
			section << buf;
		} else {
			compileMethod(section, *F);
		}
		if (++i == COMPILE_METHOD_LIMIT)
			break; // TODO
	}
}

void CheerpWasmWriter::encodeDataSectionChunk(WasmBuffer& data, uint32_t address, const std::string& buf)
{
	if (mode == CheerpWasmWriter::WASM) {
		// In the current version of WebAssembly, at most one memory is
		// allowed in a module. Consequently, the only valid memidx is 0.
		internal::encodeULEB128(0, data);
		// The offset into memory, which is the address
		internal::encodeLiteralType(Type::getInt32Ty(Ctx), data);
		internal::encodeSLEB128(address, data);
		// Encode the end of the instruction sequence.
		internal::encodeULEB128(0x0b, data);
		// Prefix the number of bytes to the bytes vector.
		internal::encodeULEB128(buf.size(), data);
		data.write(buf.data(), buf.size());
	} else {
		data << "(data (i32.const " << address << ") \"" << buf << "\")\n";
	}
}

uint32_t CheerpWasmWriter::encodeDataSectionChunks(WasmBuffer& data, uint32_t address, const std::string& buf)
{
	// Split data section buffer into chunks based on 6 (or more) zero bytes.
	uint32_t chunks = 0;
	size_t cur = 0, last = 0, end = 0;
	std::string delimiter("\0\0\0\0\0\0\0", 6);
	while ((cur = buf.find(delimiter, last)) != std::string::npos) {
		std::string chunk = buf.substr(last, cur - last);
		assert(chunk.size() == cur - last);
		assert(address + last > end);
		encodeDataSectionChunk(data, address + last, chunk);
		chunks++;

		end = address + last + chunk.size();

		// Skip the delimiter and all consecutive zero bytes.
		last = cur + delimiter.length();
		for (; last < buf.size() && buf[last] == 0; last++);
	}

	// If the buffer ends with zero bytes (last == buf.size()), an empty chunk
	// will be encoded. This should not happen, and is prevented by stripping
	// leading and trailing zeros from the buffer when this function is called.
	assert(last < buf.size());
	encodeDataSectionChunk(data, address + last, buf.substr(last));

	return chunks + 1;
}

void CheerpWasmWriter::compileDataSection()
{
	Section section(0x0b, "Data", this);

	std::ostringstream data;
	uint32_t count = 0;

	auto globals = linearHelper.addressableGlobals();
	for (auto g = globals.begin(), e = globals.end(); g != e; ++g)
	{
		const GlobalVariable* GV = *g;

		// Skip global variables that are zero-initialised.
		if (!linearHelper.hasNonZeroInitialiser(GV))
			continue;
		const Constant* init = GV->getInitializer();

		uint32_t address = linearHelper.getGlobalVariableAddress(GV);

		// Concatenate global variables into one big binary blob. This
		// optimization omits the data section item header, and that will save
		// a minimum of 5 bytes per global variable.
		std::ostringstream bytes;
		WasmBytesWriter bytesWriter(bytes, *this);

		for (; g != e; ++g) {
			GV = *g;

			// Do not concatenate global variables that have no initialiser or
			// are zero-initialised.
			if (!linearHelper.hasNonZeroInitialiser(GV))
				break;
			init = GV->getInitializer();

			// Determine amount of padding bytes necessary for the alignment.
			long written = bytes.tellp();
			uint32_t nextAddress = linearHelper.getGlobalVariableAddress(GV);
			uint32_t padding = nextAddress - (address + written);
			for (uint32_t i = 0; i < padding; i++)
				bytes << (char)0;

			linearHelper.compileConstantAsBytes(init,/* asmjs */ true, &bytesWriter);
		}

		std::string buf = bytes.str();

		// Strip leading and trailing zeros.
		size_t pos = 0, len = buf.size();
		for (unsigned i = 0; i < buf.size() && !buf[i]; i++) {
			pos++;
			len--;
		}
		for (unsigned i = buf.size(); i > 0 && !buf[--i];)
			len--;
		buf = buf.substr(pos, len);
		assert(len > 0 && "found a zero-initialised variable");

		address += pos;

		count += encodeDataSectionChunks(data, address, buf);

		// Break the outer loop when the last global variable is concatenated.
		// Without this check, the outer loop will increment `g` as well, which
		// will cause the condition `g != e` to pass, resulting in an
		// out-of-bounds access on the iterator.
		if (g == e)
			break;
	}

	if (mode == CheerpWasmWriter::WASM)
		internal::encodeULEB128(count, section);

	std::string buf = data.str();
	section.write(buf.data(), buf.size());
}

void CheerpWasmWriter::compileNameSection()
{
	if (mode != CheerpWasmWriter::WASM)
		return;

	assert(prettyCode);
	Section section(0x00, "name", this);

	// Assign names to functions
	{
		std::ostringstream data;
		uint32_t count = linearHelper.functions().size();
		internal::encodeULEB128(count, data);

		for (const Function* F : linearHelper.functions())
		{
			uint32_t functionId = linearHelper.getFunctionIds().at(F);
			internal::encodeULEB128(functionId, data);
			internal::encodeULEB128(F->getName().size(), data);
			data << F->getName().str();
		}

		std::string buf = data.str();

		internal::encodeULEB128(0x01, section);
		internal::encodeULEB128(buf.size(), section);
		section.write(buf.data(), buf.size());
	}
}

void CheerpWasmWriter::compileModule()
{
	if (mode == CheerpWasmWriter::WAST) {
		stream << "(module\n";
	} else {
		assert(mode == CheerpWasmWriter::WASM);
		std::ostringstream code;

		// Magic number for wasm.
		internal::encodeULEB128(0x00, code);
		internal::encodeULEB128(0x61, code);
		internal::encodeULEB128(0x73, code);
		internal::encodeULEB128(0x6D, code);
		// Version number.
		internal::encodeULEB128(0x01, code);
		internal::encodeULEB128(0x00, code);
		internal::encodeULEB128(0x00, code);
		internal::encodeULEB128(0x00, code);

		stream << code.str();
	}

	compileTypeSection();

	compileImportSection();

	compileFunctionSection();

	compileTableSection();

	compileMemoryAndGlobalSection();

	compileExportSection();

	compileStartSection();

	compileElementSection();

	compileCodeSection();

	compileDataSection();

	if (prettyCode) {
		compileNameSection();
	}
	
	if (mode == CheerpWasmWriter::WAST) {
		stream << ')';
	}
}

void CheerpWasmWriter::makeWasm()
{
	compileModule();
}

void CheerpWasmWriter::WasmBytesWriter::addByte(uint8_t byte)
{
	if (writer.mode == CheerpWasmWriter::WASM) {
		code.write(reinterpret_cast<char*>(&byte), 1);
	} else {
		char buf[4];
		snprintf(buf, 4, "\\%02x", byte);
		code << buf;
	}
}

void CheerpWasmWriter::WasmGepWriter::addValue(const llvm::Value* v, uint32_t size)
{
	addedValues.emplace_back(v, size);
}

void CheerpWasmWriter::WasmGepWriter::subValue(const llvm::Value* v, uint32_t size)
{
	subbedValues.emplace_back(v, size);
}

void CheerpWasmWriter::WasmGepWriter::compileValue(const llvm::Value* v, uint32_t size) const
{
	writer.compileOperand(code, v);
	if (size > 1)
	{
		if (isPowerOf2_32(size))
		{
			writer.encodeS32Inst(0x41, "i32.const", Log2_32(size), code);
			writer.encodeInst(0x74, "i32.shl", code);
		}
		else
		{
			writer.encodeS32Inst(0x41, "i32.const", size, code);
			writer.encodeInst(0x6c, "i32.mul", code);
		}
	}
}

bool CheerpWasmWriter::WasmGepWriter::compileValues(bool useConstPart) const
{
	bool first = true;
	for(auto& it: addedValues)
	{
		compileValue(it.first, it.second);
		if(!first)
			writer.encodeInst(0x6a, "i32.add", code);
		first = false;
	}
	if(useConstPart && constPart != 0)
	{
		writer.encodeS32Inst(0x41, "i32.const", constPart, code);
		if(!first)
			writer.encodeInst(0x6a, "i32.add", code);
		first = false;
	}
	if(subbedValues.empty())
		return first;
	// To deal with subtracted values we need at least a value
	if(first)
		writer.encodeS32Inst(0x41, "i32.const", 0, code);
	for(auto& it: subbedValues)
	{
		compileValue(it.first, it.second);
		writer.encodeInst(0x6b, "i32.sub", code);
	}
	return false;
}

void CheerpWasmWriter::WasmGepWriter::addConst(int64_t v)
{
	assert(v);
	// Just make sure that the constant part of the offset is not too big
	// TODO: maybe use i64.const here instead of crashing
	assert(v>=std::numeric_limits<int32_t>::min());
	assert(v<=std::numeric_limits<int32_t>::max());

	constPart += v;
}
