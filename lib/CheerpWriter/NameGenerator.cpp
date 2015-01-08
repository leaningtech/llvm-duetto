//===-- Opcodes.cpp - The Cheerp JavaScript generator ---------------------===//
//
//                     Cheerp: The C++ compiler for the Web
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2014 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringExtras.h"
#include "llvm/Cheerp/NameGenerator.h"
#include "llvm/Cheerp/GlobalDepsAnalyzer.h"
#include "llvm/Cheerp/Utility.h"
#include "llvm/IR/Function.h"
#include <functional>
#include <set>

using namespace llvm;

namespace cheerp {

/**
 * Note: this describes the valid *C++* identifiers.
 * 
 * Iterate over all the valid JS identifier is much more complicated , because JS uses unicode.
 * Reference for valid JS identifiers:
*  http://stackoverflow.com/questions/1661197/valid-characters-for-javascript-variable-names
*/
struct JSSymbols
{
	enum : char {first_symbol = 'a' };
	static char next( char c )
	{
		return  c == '_' ? 'a' :
			c == 'z' ? 'A' :
			c == 'Z' ? '0' :
			c == '9' ? '_' :
			++c;
	}

	template< class String >
	static bool is_valid( String & s )
	{
		// Can not be empty
		if ( s.empty() ) return false;
		
		// Can not start with a digit
		if ( std::isdigit(s.front() ) )
		{
			std::fill( s.begin(), s.end(), '_' );
			return false;
		}
		
		// Can not be made only of '_';
		if ( std::all_of( s.begin(), s.end(), [](char c) { return c == '_'; }) )
			return false;
		
		// Check for reserved keywords
		if ( is_reserved_name(s) )
			return false;
		
		// "null" is used by cheerp internally
		if ( s == "null" )
			return false;
		
		// In the rare case that a name is longer than 4 characters, it need to start with an underscore
		// just to be safe.
		if ( s.size() > 4 && s.front() != '_' )
		{
			std::fill( s.begin(), s.end(), '_' );
			return false;
		}
		
		// Check for labels generated by the relooper
		if ( s.front() == 'L' && std::all_of( s.begin()+1, s.end(), ::isdigit ) )
		{
			std::fill( s.begin()+1,s.end(),'9');
			return false;
		}

		return true;
	}

	template< class String >
	static bool is_reserved_name( String& s)
	{
		const char* reserved_names[] = {
			"byte",
			"case",
			"char",
			"do",
			"else",
			"enum",
			"for",
			"goto",
			"if",
			"in",
			"int",
			"let",
			"new",
			"this",
			"top",
			"try",
			"var",
			"void",
			"with"
		};
		return std::binary_search(reserved_names, reserved_names+(sizeof(reserved_names)/sizeof(const char*)), s);
	}
};

NameGenerator::NameGenerator(const Module& M, const GlobalDepsAnalyzer& gda, const Registerize& r,
				const PointerAnalyzer& PA, bool makeReadableNames):registerize(r), PA(PA)
{
	if ( makeReadableNames )
		generateReadableNames(M, gda);
	else
		generateCompressedNames(M, gda);
}

llvm::StringRef NameGenerator::getNameForEdge(const llvm::Value* v) const
{
	assert(!edgeContext.isNull());
	if (const Instruction* I=dyn_cast<Instruction>(v))
	{
		auto it=edgeNamemap.find(InstOnEdge(edgeContext.fromBB, edgeContext.toBB, registerize.getRegisterId(I)));
		if (it!=edgeNamemap.end())
			return it->second;
	}
	return namemap.at(v);
}

SmallString< 4 > NameGenerator::filterLLVMName(StringRef s, bool isGlobalName)
{
	SmallString< 4 > ans;
	ans.reserve( s.size() + 1 );

	//Add an '_' or 'L' to skip reserved names
	ans.push_back( isGlobalName ? '_' : 'L' );

	for ( char c : s )
	{
		//We need to escape invalid chars
		switch(c)
		{
			case '.':
				ans.append("$p");
				break;
			case '-':
				ans.append("$m");
				break;
			case ':':
				ans.append("$c");
				break;
			case '<':
				ans.append("$l");
				break;
			case '>':
				ans.append("$r");
				break;
			case ' ':
				ans.append("$s");
				break;
			default:
				ans.push_back(c);
		}
	}

	return ans;
}

void NameGenerator::generateCompressedNames(const Module& M, const GlobalDepsAnalyzer& gda)
{
	typedef std::pair<unsigned, const Value *> useValuePair;
	typedef std::pair<unsigned, std::vector<const Value *> > useValuesPair;
	typedef std::vector<useValuesPair> useValuesVec;
	typedef std::pair<unsigned, std::vector<InstOnEdge> > useInstsOnEdgePair;
	typedef std::vector<useInstsOnEdgePair> useInstsOnEdgeVec;
        
	// Class to handle giving names to temporary variables needed for recursively dependent PHIs
	class CompressedPHIHandler: public EndOfBlockPHIHandler
	{
	public:
		CompressedPHIHandler(const BasicBlock* f, const BasicBlock* t, NameGenerator& n, useInstsOnEdgeVec& a ):
			fromBB(f), toBB(t), namegen(n), allTmpPHIs(a), nextIndex(0)
		{
		}
	private:
		const BasicBlock* fromBB;
		const BasicBlock* toBB;
		NameGenerator& namegen;
		useInstsOnEdgeVec& allTmpPHIs;
		uint32_t nextIndex;
		void handleRecursivePHIDependency(const Instruction* phi) override
		{
			uint32_t regId=namegen.registerize.getRegisterId(phi);
			// We don't know exactly how many times the tmpphi is going to be used in this edge
			// but assume 1. We increment the usage count for the first not already used tmpphi
			// and add the InstOnEdge to its list
			if (nextIndex >= allTmpPHIs.size())
				allTmpPHIs.resize(nextIndex+1);
			allTmpPHIs[nextIndex].first++;
			allTmpPHIs[nextIndex].second.emplace_back(InstOnEdge(fromBB, toBB, regId));
			nextIndex++;
		}
		void handlePHI(const Instruction* phi, const Value* incoming) override
		{
			// Nothing to do here, we have already given names to all PHIs
		}
	};
	/**
	 * Collect the local values.
	 * 
	 * We sort them by uses, then store together those in the same position.
	 * i.e. allLocalValues[0].second will contain all the most used local values
	 * for each function, and allLocalValues[0].first will be the sum of the uses
	 * of all those local values.
	 */
        
	useValuesVec allLocalValues;
	useInstsOnEdgeVec allTmpPHIs;
        
	/**
	 * Sort the global values by number of uses
	 */
	std::set< useValuePair, std::greater< useValuePair > > allGlobalValues;

	for (const Function & f : M.getFunctionList() )
	{
		unsigned nUses = f.getNumUses();

		if ( f.getName() == "_Z7webMainv" )
			++nUses; // We explicitly invoke the webmain

		// Constructors are also explicitly invoked
		if ( std::find(gda.constructors().begin(), gda.constructors().end(), &f ) != gda.constructors().end() )
			++nUses;

		allGlobalValues.emplace( nUses, &f );

		/**
		 * TODO, some cheerp-internals functions are actually generated even with an empty IR.
		 * They should be considered here.
		 */
		if ( f.empty() ) 
			continue;

		// Local values are all stored in registers
		useValuesVec thisFunctionLocals;

		// Insert all the instructions
		for (const BasicBlock & bb : f)
		{
			for (const Instruction & I : bb)
			{
				if ( needsName(I, PA) )
				{
					uint32_t registerId = registerize.getRegisterId(&I);
					if (registerId >= thisFunctionLocals.size())
						thisFunctionLocals.resize(registerId+1);
					useValuesPair& regData = thisFunctionLocals[registerId];
					// Add the uses for this instruction to the total count for the register
					regData.first+=I.getNumUses();
					// Add the instruction itself to the list of istructions
					regData.second.push_back(&I);
				}
			}
			// Handle the special names required for the edges between blocks
			const TerminatorInst* term=bb.getTerminator();
			for(uint32_t i=0;i<term->getNumSuccessors();i++)
			{
				const BasicBlock* succBB=term->getSuccessor(i);
				CompressedPHIHandler(&bb, succBB, *this, allTmpPHIs).runOnEdge(registerize, &bb, succBB);
			}
		}

		uint32_t currentArgPos=thisFunctionLocals.size();
		thisFunctionLocals.resize(currentArgPos+f.arg_size());
		// Insert the arguments
		for ( auto arg_it = f.arg_begin(); arg_it != f.arg_end(); ++arg_it, currentArgPos++ )
		{
			thisFunctionLocals[currentArgPos].first = f.getNumUses();
			thisFunctionLocals[currentArgPos].second.push_back( arg_it );
		}

		// Resize allLocalValues so that we have empty useValuesPair at the end of the container
		if ( thisFunctionLocals.size() > allLocalValues.size() )
			allLocalValues.resize( thisFunctionLocals.size() );

		std::sort(thisFunctionLocals.begin(),thisFunctionLocals.end());
		auto dst_it = allLocalValues.begin();

		for (auto src_it = thisFunctionLocals.rbegin(); src_it != thisFunctionLocals.rend(); ++src_it, ++dst_it )
		{
			dst_it->first += src_it->first;
			dst_it->second.insert(dst_it->second.end(), src_it->second.begin(), src_it->second.end());
		}
	}
        
	assert( std::is_sorted( 
		allLocalValues.rbegin(), 
		allLocalValues.rend(),
		[] (const useValuesPair & lhs, const useValuesPair & rhs) { return lhs.first < rhs.first; } 
		) );

	for ( const GlobalValue & GV : M.getGlobalList() )
	{
		if ( isa<GlobalVariable>(GV) && TypeSupport::isClientGlobal(&GV) )
		{
			demangler_iterator dmg( GV.getName() );
			assert(*dmg == "client");
			
			namemap.emplace( &GV, *(++dmg) );
			
			continue;
		}

		allGlobalValues.emplace( GV.getNumUses(), &GV );
	}

	/**
	 * Now generate the names and fill the namemap.
	 * 
	 * Note that there will never be a global with the same name as a local.
	 * This is suboptimal, since in theory we could check out for the uses
	 * of the global inside the function.
	 */
	name_iterator<JSSymbols> name_it;
	
	// We need to iterate over allGlobalValues, allLocalValues and allTmpPHIs
	// at the same time incrementing selectively only one of the iterators
	
	std::set< useValuePair >::const_iterator global_it = allGlobalValues.begin();
	useValuesVec::const_iterator local_it = allLocalValues.begin();
	useInstsOnEdgeVec::const_iterator tmpphi_it = allTmpPHIs.begin();

	bool globalsFinished = global_it == allGlobalValues.end();
	bool localsFinished = local_it == allLocalValues.end();
	bool tmpPHIsFinished = tmpphi_it == allTmpPHIs.end();
	for ( ; !globalsFinished || !localsFinished || !tmpPHIsFinished; ++name_it )
	{
		if ( !globalsFinished &&
			(localsFinished || global_it->first >= local_it->first) &&
			(tmpPHIsFinished || global_it->first >= tmpphi_it->first))
		{
			// Assign this name to a global value
			namemap.emplace( global_it->second, *name_it );
			++global_it;
		}
		else if ( !localsFinished &&
			(globalsFinished || local_it->first >= global_it->first) &&
			(tmpPHIsFinished || local_it->first >= tmpphi_it->first))
		{
			// Assign this name to all the local values
			for ( const Value * v : local_it->second )
				namemap.emplace( v, *name_it );
			
			++local_it;
		}
		else
		{
			// Assign this name to all the tmpphis
			for ( const InstOnEdge& i : tmpphi_it->second )
				edgeNamemap.emplace( i, StringRef(*name_it));
			
			++tmpphi_it;
		}
		globalsFinished = global_it == allGlobalValues.end();
		localsFinished = local_it == allLocalValues.end();
		tmpPHIsFinished = tmpphi_it == allTmpPHIs.end();
	}
}

void NameGenerator::generateReadableNames(const Module& M, const GlobalDepsAnalyzer& gda)
{
	// Class to handle giving names to temporary variables needed for recursively dependent PHIs
	class ReadablePHIHandler: public EndOfBlockPHIHandler
	{
	public:
		ReadablePHIHandler(const BasicBlock* f, const BasicBlock* t, NameGenerator& n ):
			fromBB(f), toBB(t), namegen(n), nextIndex(0)
		{
		}
	private:
		const BasicBlock* fromBB;
		const BasicBlock* toBB;
		NameGenerator& namegen;
		uint32_t nextIndex;
		void handleRecursivePHIDependency(const Instruction* phi) override
		{
			uint32_t regId=namegen.registerize.getRegisterId(phi);
			namegen.edgeNamemap.emplace(InstOnEdge(fromBB, toBB, regId),
							StringRef( "tmpphi" + std::to_string(nextIndex++)));
		}
		void handlePHI(const Instruction* phi, const Value* incoming) override
		{
			// Nothing to do here, we have already given names to all PHIs
		}
	};
	for (const Function & f : M.getFunctionList() )
	{
		// Temporary mapping between registers and names
		// NOTE: We only store references to names in the namemap
		std::unordered_map<uint32_t, llvm::SmallString<4>& > regmap;

		for (const BasicBlock & bb : f)
		{
			for (const Instruction & I : bb)
			{
				if ( needsName(I, PA) )
				{
					uint32_t registerId = registerize.getRegisterId(&I);
					// If the register already has a name, use it
					// Otherwise assign one as good as possible and assign it to the register as well
					auto regNameIt = regmap.find(registerId);
					if(regNameIt != regmap.end())
						namemap.emplace( &I, regNameIt->second );
					else if ( I.hasName() )
					{
						auto it=namemap.emplace( &I, filterLLVMName(I.getName(), false) ).first;
						regmap.emplace( registerId, it->second );
					}
					else
					{
						auto it=namemap.emplace( &I,
							StringRef( "tmp" + std::to_string(registerId) ) ).first;
						regmap.emplace( registerId, it->second );
					}
				}
			}
			// Handle the special names required for the edges between blocks
			const TerminatorInst* term=bb.getTerminator();
			for(uint32_t i=0;i<term->getNumSuccessors();i++)
			{
				const BasicBlock* succBB=term->getSuccessor(i);
				ReadablePHIHandler(&bb, succBB, *this).runOnEdge(registerize, &bb, succBB);
			}
		}

		unsigned argCounter = 0;
		for ( auto arg_it = f.arg_begin(); arg_it != f.arg_end(); ++arg_it )
			if ( arg_it->hasName() )
				namemap.emplace( arg_it, filterLLVMName(arg_it->getName(), false) );
			else
				namemap.emplace( arg_it, StringRef( "arg" + std::to_string(argCounter++) ) );
			
		namemap.emplace( &f, filterLLVMName( f.getName(), true ) );
	}

	for (const GlobalVariable & GV : M.getGlobalList() )
		if (TypeSupport::isClientGlobal(&GV) )
		{
			demangler_iterator dmg( GV.getName() );
			assert(*dmg == "client");
			
			namemap.emplace( &GV, *(++dmg) );
			
		}
		else
			namemap.emplace( &GV, filterLLVMName( GV.getName(), true ) );
}

bool NameGenerator::needsName(const Instruction & I, const PointerAnalyzer& PA) const
{
	return !isInlineable(I, PA) && !I.getType()->isVoidTy() && !I.use_empty();
}

}
