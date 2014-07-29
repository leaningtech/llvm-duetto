//===-- PointerAnalyzer.cpp - The Cheerp JavaScript generator -------------===//
//
//                     Cheerp: The C++ compiler for the Web
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2011-2014 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#include "llvm/Cheerp/PointerAnalyzer.h"
#include "llvm/Cheerp/Utility.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/FormattedStream.h"
#include <numeric>

using namespace llvm;

namespace cheerp {

struct KindOrUnknown
{
	enum {UNKNOWN = REGULAR+1};

	KindOrUnknown() : val(UNKNOWN){}
	KindOrUnknown(POINTER_KIND k) : val(k){}

	friend bool operator==(const KindOrUnknown & lhs, const KindOrUnknown & rhs)
	{
		return lhs.val == rhs.val;
	}

	friend bool operator!=(const KindOrUnknown & lhs, const KindOrUnknown & rhs)
	{
		return !(lhs==rhs);
	}

	explicit operator bool() const
	{
		return val!=UNKNOWN;
	}

	POINTER_KIND getValue() const
	{
		assert(val!=UNKNOWN);
		return POINTER_KIND(val);
	}

	unsigned val;
};

static KindOrUnknown operator||(const KindOrUnknown & lhs, const KindOrUnknown & rhs)
{
	// Unknown | CO = Unknown
	// Unknown | RE = RE
	// Unknown | Unknown = Unknown
	// CO | RE = RE
	// CO | CO = CO
	// RE | RE = RE
	
	// Handle 2,4, and 6
	if (lhs==REGULAR || rhs==REGULAR)
		return REGULAR;
	// Handle 1 and 3
	if ( !(lhs && rhs) )
		return {};
	return COMPLETE_OBJECT;
}

char PointerAnalyzer::ID = 0;

const char* PointerAnalyzer::getPassName() const
{
	return "CheerpPointerAnalyzer";
}

bool PointerAnalyzer::runOnModule(Module& M)
{
	for (const Function & F : M )
		if ( F.getReturnType()->isPointerTy() )
			getPointerKindForReturn(&F);

	return false;
}

struct PointerUsageVisitor
{
	typedef llvm::DenseSet< const llvm::Value* > visited_set_t;
	typedef PointerAnalyzer::ValueKindMap value_kind_map_t;

	PointerUsageVisitor( value_kind_map_t & cache ) : cachedValues(cache) {}

	KindOrUnknown visitValue(const Value* v);
	KindOrUnknown visitUse(const Use* U);
	KindOrUnknown visitReturn(const Function* F);
	bool visitByteLayoutChain ( const Value * v );
	POINTER_KIND getKindForType(Type*) const;

	KindOrUnknown visitAllUses(const Value* v)
	{
		KindOrUnknown result = COMPLETE_OBJECT;
		for(const Use& u : v->uses())
		{
			result = result || visitUse(&u);
			if (REGULAR==result)
				break;
		}
		return result;
	}

	Type * realType( const Value * v ) const
	{
		assert( v->getType()->isPointerTy() );
		if ( isBitCast(v) )
			v = cast<User>(v)->getOperand(0);
		return v->getType()->getPointerElementType();
	}

	value_kind_map_t & cachedValues;
	visited_set_t closedset;
};

bool PointerUsageVisitor::visitByteLayoutChain( const Value * p )
{
	if ( getKindForType(p->getType()->getPointerElementType()) == BYTE_LAYOUT && visitValue(p) != COMPLETE_OBJECT)
		return true;
	if ( isGEP(p))
	{
		const User* u = cast<User>(p);
		// We need to find out if the base element or any element accessed by the GEP is byte layout
		if (visitByteLayoutChain(u->getOperand(0)))
			return true;
		Type* curType = u->getOperand(0)->getType();
		for (uint32_t i=1;i<u->getNumOperands();i++)
		{
			if (StructType* ST = dyn_cast<StructType>(curType))
			{
				if (ST->hasByteLayout())
					return true;
				uint32_t index = cast<ConstantInt>( u->getOperand(i) )->getZExtValue();
				curType = ST->getElementType(index);
			}
			else
			{
				// This case also handles the first index
				curType = curType->getSequentialElementType();
			}
		}
		return false;
	}

	if ( isBitCast(p))
	{
		const User* u = cast<User>(p);
		if (TypeSupport::hasByteLayout(u->getOperand(0)->getType()->getPointerElementType()))
			return true;
		if (visitByteLayoutChain(u->getOperand(0)))
			return true;
		return false;
	}

	return false;
}

KindOrUnknown PointerUsageVisitor::visitValue(const Value* p)
{
	if( cachedValues.count(p) )
		return cachedValues[p];

	if(!closedset.insert(p).second)
		return {};

	auto CacheAndReturn = [&](KindOrUnknown k)
	{
		closedset.erase(p);
		if (k)
			cachedValues.insert( std::make_pair(p, k.getValue() ) );
		return k;
	};

	llvm::Type * type = realType(p);

	if ( const IntrinsicInst * intrinsic = dyn_cast<IntrinsicInst>(p) )
	{
		switch ( intrinsic->getIntrinsicID() )
		{
		case Intrinsic::cheerp_downcast:
		case Intrinsic::cheerp_upcast_collapsed:
		case Intrinsic::cheerp_cast_user:
			break;
		case Intrinsic::cheerp_allocate:
			break;
		case Intrinsic::cheerp_pointer_base:
		case Intrinsic::cheerp_create_closure:
		case Intrinsic::cheerp_make_complete_object:
			if(visitAllUses(p) != COMPLETE_OBJECT)
			{
				llvm::errs() << "Result of " << intrinsic->getName() << " used as REGULAR: " << *p << "\n";
				llvm::report_fatal_error("Unsupported code found, please report a bug", false);
			}
			return CacheAndReturn(COMPLETE_OBJECT);
		case Intrinsic::memmove:
		case Intrinsic::memcpy:
		case Intrinsic::memset:
			return CacheAndReturn(visitValue(intrinsic->getArgOperand(0)));
		case Intrinsic::cheerp_pointer_offset:
		case Intrinsic::invariant_start:
			return CacheAndReturn(visitValue(intrinsic->getArgOperand(1)));
		case Intrinsic::invariant_end:
		case Intrinsic::vastart:
		case Intrinsic::vaend:
		case Intrinsic::flt_rounds:
		default:
			SmallString<128> str("Unreachable code in cheerp::PointerAnalyzer::visitValue, unhandled intrinsic: ");
			str+=intrinsic->getCalledFunction()->getName();
			llvm::report_fatal_error(str,false);
		}
	}

	if(getKindForType(type) == COMPLETE_OBJECT)
	{
		return CacheAndReturn(COMPLETE_OBJECT);
	}

	if(TypeSupport::isImmutableType(type))
	{
		return CacheAndReturn(REGULAR);
	}

	if(isa<LoadInst>(p))
		return CacheAndReturn(REGULAR);

	if(const Argument* arg = dyn_cast<Argument>(p))
	{
		if(arg->getParent()->hasAddressTaken())
			return CacheAndReturn(REGULAR);
	}

	// TODO this is not really necessary,
	// but we need to modify the writer so that CallInst and InvokeInst
	// perform a demotion in place.
	if(ImmutableCallSite cs = p)
		return CacheAndReturn(visitReturn(cs.getCalledFunction()));

	return CacheAndReturn(visitAllUses(p));
}

KindOrUnknown PointerUsageVisitor::visitUse(const Use* U)
{
	const User * p = U->getUser();
	if ( isGEP(p) )
	{
		const Constant * constOffset = dyn_cast<Constant>( p->getOperand(1) );
		
		if ( constOffset && constOffset->isNullValue() )
		{
			if ( p->getNumOperands() == 2 )
				return visitValue( p );
			return COMPLETE_OBJECT;
		}
		
		return REGULAR;
	}

	if ( isa<StoreInst>(p) && U->getOperandNo() == 0 )
		return REGULAR;

	if ( isa<PtrToIntInst>(p) || ( isa<ConstantExpr>(p) && cast<ConstantExpr>(p)->getOpcode() == Instruction::PtrToInt) )
		return REGULAR;

	if ( const IntrinsicInst * intrinsic = dyn_cast<IntrinsicInst>(p) )
	{
		switch ( intrinsic->getIntrinsicID() )
		{
		case Intrinsic::memmove:
		case Intrinsic::memcpy:
		case Intrinsic::memset:
			return REGULAR;
		case Intrinsic::invariant_start:
		case Intrinsic::invariant_end:
		case Intrinsic::vastart:
		case Intrinsic::vaend:
		case Intrinsic::lifetime_start:
		case Intrinsic::lifetime_end:
		case Intrinsic::cheerp_element_distance:
			return COMPLETE_OBJECT;
		case Intrinsic::cheerp_downcast:
		case Intrinsic::cheerp_upcast_collapsed:
		case Intrinsic::cheerp_cast_user:
			return visitValue( p );
		case Intrinsic::cheerp_pointer_base:
		case Intrinsic::cheerp_pointer_offset:
			return REGULAR;
		case Intrinsic::cheerp_create_closure:
			assert( U->getOperandNo() == 1 );
			if ( const Function * f = dyn_cast<Function>(p->getOperand(0) ) )
			{
				return visitValue( f->arg_begin() );
			}
			else
				llvm::report_fatal_error("Unreachable code in cheerp::PointerAnalyzer::visitUse, cheerp_create_closure");
		case Intrinsic::cheerp_make_complete_object:
			return COMPLETE_OBJECT;
		case Intrinsic::flt_rounds:
		case Intrinsic::cheerp_allocate:
		default:
			SmallString<128> str("Unreachable code in cheerp::PointerAnalyzer::visitUse, unhandled intrinsic: ");
			str+=intrinsic->getCalledFunction()->getName();
			llvm::report_fatal_error(str,false);
		}
		return REGULAR;
	}

	if ( ImmutableCallSite cs = p )
	{
		if ( cs.isCallee(U) )
			return COMPLETE_OBJECT;

		const Function * calledFunction = cs.getCalledFunction();
		if ( !calledFunction )
			return REGULAR;

		unsigned argNo = cs.getArgumentNo(U);

		if ( argNo >= calledFunction->arg_size() )
		{
			// Passed as a variadic argument
			return REGULAR;
		}

		Function::const_arg_iterator arg = calledFunction->arg_begin();
		std::advance(arg, argNo);

		return visitValue( arg );
	}

	if ( const ReturnInst * ret = dyn_cast<ReturnInst>(p) )
	{
		return visitReturn( ret->getParent()->getParent() );
	}

	// Bitcasts from byte layout types require COMPLETE_OBJECT, and generate BYTE_LAYOUT
	if(isBitCast(p))
	{
		if (TypeSupport::hasByteLayout(p->getOperand(0)->getType()->getPointerElementType()))
			return COMPLETE_OBJECT;
		else
			return visitValue( p );
	}

	if(isa<SelectInst> (p) || isa <PHINode>(p))
		return visitValue(p);

	if ( isa<Constant>(p) )
		return REGULAR;

	return COMPLETE_OBJECT;
}

KindOrUnknown PointerUsageVisitor::visitReturn(const Function* F)
{
	if(!F)
		return REGULAR;

	/**
	 * Note:
	 * we can not use F as the cache key here,
	 * since F is a pointer to function which might be used elsewhere.
	 * Hence we store the entry basic block.
	 */

	if(cachedValues.count(F->begin()))
		return cachedValues[F->begin()];

	if(!closedset.insert(F).second)
		return {};

	auto CacheAndReturn = [&](KindOrUnknown k) 
	{ 
		closedset.erase(F); 
		if (k)
			cachedValues.insert( std::make_pair(F->begin(), k.getValue() ) );
		return k;
	};

	Type* returnPointedType = F->getReturnType()->getPointerElementType();

	if(getKindForType(returnPointedType)==COMPLETE_OBJECT)
		return CacheAndReturn(COMPLETE_OBJECT);

	if(TypeSupport::isImmutableType(returnPointedType))
		return CacheAndReturn(REGULAR);

	if(F->hasAddressTaken())
		return CacheAndReturn(REGULAR);

	KindOrUnknown result = COMPLETE_OBJECT;
	for(const Use& u : F->uses())
	{
		ImmutableCallSite cs = u.getUser();
		if(cs && cs.isCallee(&u))
			result = result || visitAllUses(cs.getInstruction());

		if (REGULAR==result)
			break;
	}
	return CacheAndReturn(result);
}

POINTER_KIND PointerUsageVisitor::getKindForType(Type * tp) const
{
	if ( tp->isFunctionTy() ||
		TypeSupport::isClientType( tp ) )
		return COMPLETE_OBJECT;

	if ( TypeSupport::hasByteLayout( tp ) )
		return BYTE_LAYOUT;

	return REGULAR;
}

struct TimerGuard
{
	TimerGuard(Timer & timer) : timer(timer)
	{
		timer.startTimer();
	}
	~TimerGuard()
	{
		timer.stopTimer();
	}

	Timer & timer;
};

void PointerAnalyzer::prefetch(const Module& m) const
{
#ifndef NDEBUG
	Timer t( "prefetch", timerGroup);
	TimerGuard guard(t);
#endif //NDEBUG

	for(const Function & F : m)
	{
		for(const BasicBlock & BB : F)
		{
			for(auto it=BB.rbegin();it != BB.rend();++it)
				if(it->getType()->isPointerTy())
					getPointerKind(&(*it));
		}
		if(F.getReturnType()->isPointerTy())
			getPointerKindForReturn(&F);
	}
}

POINTER_KIND PointerAnalyzer::getPointerKind(const Value* p) const
{
#ifndef NDEBUG
	TimerGuard guard(gpkTimer);
#endif //NDEBUG
	if (PointerUsageVisitor(cache).visitByteLayoutChain(p))
	{
		return cache.insert( std::make_pair(p, BYTE_LAYOUT) ).first->second;
	}
	KindOrUnknown k = PointerUsageVisitor(cache).visitValue(p);

	//If all the uses are unknown no use is REGULAR, we can return CO
	if (!k)
	{
		return cache.insert( std::make_pair(p, COMPLETE_OBJECT) ).first->second;
	}
	return k.getValue();
}

POINTER_KIND PointerAnalyzer::getPointerKindForReturn(const Function* F) const
{
#ifndef NDEBUG
	TimerGuard guard(gpkfrTimer);
#endif //NDEBUG
	KindOrUnknown k = PointerUsageVisitor(cache).visitReturn(F);

	//If all the uses are unknown no use is REGULAR, we can return CO
	if (!k)
	{
		return cache.insert( std::make_pair(F->begin(), COMPLETE_OBJECT) ).first->second;
	}
	return k.getValue();
}

POINTER_KIND PointerAnalyzer::getPointerKindForType(Type* tp) const
{
	return PointerUsageVisitor(cache).getKindForType(tp);
}

void PointerAnalyzer::invalidate(const Value * v)
{
	if ( cache.erase(v) )
	{
		const User * u = dyn_cast<User>(v);

		// Go on and invalidate all the operands
		if ( u && u->getType()->isPointerTy() )
		{
			for ( const Use & U : u->operands() )
			{
				if ( U->getType()->isPointerTy() )
					invalidate(U.get());
			}
		}

		// If v is a function invalidate also all its call and arguments
		if ( const Function * F = dyn_cast<Function>(u) )
		{
			for ( const Argument & arg : F->getArgumentList() )
				if (arg.getType()->isPointerTy())
					invalidate(&arg);

			for ( const Use & U : F->uses() )
				if ( ImmutableCallSite cs = U.getUser() )
					invalidate( cs.getInstruction() );
		}
	}
}

#ifndef NDEBUG

void PointerAnalyzer::dumpPointer(const Value* v, bool dumpOwnerFunc) const
{
	llvm::formatted_raw_ostream fmt( llvm::errs() );

	fmt.changeColor( llvm::raw_ostream::RED, false, false );
	v->printAsOperand( fmt );
	fmt.resetColor();

	if (dumpOwnerFunc)
	{
		if ( const Instruction * I = dyn_cast<Instruction>(v) )
			fmt << " in function: " << I->getParent()->getParent()->getName();
		else if ( const Argument * A = dyn_cast<Argument>(v) )
			fmt << " arg of function: " << A->getParent()->getName();
	}

	if (v->getType()->isPointerTy())
	{
		fmt.PadToColumn(92);
		switch (getPointerKind(v))
		{
			case COMPLETE_OBJECT: fmt << "COMPLETE_OBJECT"; break;
			case REGULAR: fmt << "REGULAR"; break;
		}
		fmt.PadToColumn(112) << (TypeSupport::isImmutableType( v->getType()->getPointerElementType() ) ? "true" : "false" );
	}
	else
		fmt << " is not a pointer";
	fmt << '\n';
}

void dumpAllPointers(const Function & F, const PointerAnalyzer & analyzer)
{
	llvm::errs() << "Function: " << F.getName();
	if ( F.hasAddressTaken() )
		llvm::errs() << " (with address taken)";
	if ( F.getReturnType()->isPointerTy() )
	{
		llvm::errs() << " [";
		switch (analyzer.getPointerKindForReturn(&F))
		{
			case COMPLETE_OBJECT: llvm::errs() << "COMPLETE_OBJECT"; break;
			case REGULAR: llvm::errs() << "REGULAR"; break;
		}
		llvm::errs() << ']';
	}

	llvm::errs() << "\n";

	for ( const Argument & arg : F.getArgumentList() )
		analyzer.dumpPointer(&arg, false);

	for ( const BasicBlock & BB : F )
	{
		for ( const Instruction & I : BB )
		{
			if ( I.getType()->isPointerTy() )
				analyzer.dumpPointer(&I, false);
		}
	}
	llvm::errs() << "\n";
}

void writePointerDumpHeader()
{
	llvm::formatted_raw_ostream fmt( llvm::errs() );
	fmt.PadToColumn(0) << "Name";
	fmt.PadToColumn(92) << "Kind";
	fmt.PadToColumn(112) << "UsageFlags";
	fmt.PadToColumn(132) << "UsageFlagsComplete";
	fmt.PadToColumn(152) << "IsImmutable";
	fmt << '\n';
}

#endif //NDEBUG

}
