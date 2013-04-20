//===-- duetto-compiler.cpp - The Duetto JavaScript generator -------------===//
//
//	Copyright 2011-2012 Leaning Technlogies
//===----------------------------------------------------------------------===//

#include "LLVMContext.h"
#include "Module.h"
#include "PassManager.h"
#include "Pass.h"
#include "ADT/Triple.h"
#include "Support/IRReader.h"
#include "CodeGen/LinkAllAsmWriterComponents.h"
#include "CodeGen/LinkAllCodegenComponents.h"
#include "MC/SubtargetFeature.h"
#include "Support/CommandLine.h"
#include "Support/Debug.h"
#include "Support/FormattedStream.h"
#include "Support/ManagedStatic.h"
#include "Support/PluginLoader.h"
#include "Support/PrettyStackTrace.h"
#include "Support/ToolOutputFile.h"
#include "Support/Host.h"
#include "Support/Signals.h"
#include "Support/TargetRegistry.h"
#include "Support/TargetSelect.h"
#include "Target/TargetData.h"
#include "Target/TargetMachine.h"
#include "Transforms/Utils/Cloning.h"
#include "DerivedTypes.h"
#include "Support/IRBuilder.h"
#include "Duetto/Utils.h"
#include "Relooper.h"
#include <memory>
#include <map>
#include <iostream>
#include <stdio.h>
using namespace llvm;
using namespace std;

// General options for llc.  Other pass-specific options are specified
// within the corresponding llc passes, and target-specific options
// and back-end code generation options are specified with the target machine.
//
static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::init("-"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Output filename"), cl::value_desc("filename"));

// Determine optimization level.
static cl::opt<char>
OptLevel("O",
         cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
                  "(default = '-O2')"),
         cl::Prefix,
         cl::ZeroOrMore,
         cl::init(' '));

// GetFileNameRoot - Helper function to get the basename of a filename.
static inline std::string
GetFileNameRoot(const std::string &InputFilename) {
  std::string IFN = InputFilename;
  std::string outputFilename;
  int Len = IFN.length();
  if ((Len > 2) &&
      IFN[Len-3] == '.' &&
      ((IFN[Len-2] == 'b' && IFN[Len-1] == 'c') ||
       (IFN[Len-2] == 'l' && IFN[Len-1] == 'l'))) {
    outputFilename = std::string(IFN.begin(), IFN.end()-3); // s/.bc/.s/
  } else {
    outputFilename = IFN;
  }
  return outputFilename;
}

class JSWriter
{
private:
	Module* module;
	TargetData targetData;
	raw_fd_ostream& stream;
	uint32_t getIntFromValue(const Value* v) const;
	//std::set<const Value*> completeObjects;
	bool isValidTypeCast(const Value* cast, const Value* castOp, Type* src, Type* dst) const;
	bool isClientType(Type* t) const;
	bool isClientGlobal(const char* mangledName) const;
	bool isI32Type(Type* t) const;
	bool isComingFromAllocation(const Value* val) const;
	bool isComingFromAllocation(const Value* val, std::set<const PHINode*>& visitedPhis) const;
	bool isInlineable(const Instruction& I) const;
	bool isBitCast(const Value* v) const;
	bool isGEP(const Value* v) const;
	void compileTerminatorInstruction(const TerminatorInst& I);
	void compileTerminatorInstruction(const TerminatorInst& I,
			const std::map<const BasicBlock*, uint32_t>& blocksMap);
	bool compileInlineableInstruction(const Instruction& I);
	bool compileNotInlineableInstruction(const Instruction& I);
	void compilePHIOfBlockFromOtherBlock(const BasicBlock* to, const BasicBlock* from);
	const Type* compileRecursiveAccessToGEP(const Type* curType, const Use* it, const Use* const itE);
	void compilePredicate(CmpInst::Predicate p);
	void compileOperandForIntegerPredicate(const Value* v, CmpInst::Predicate p);
	void compileType(Type* t);
	bool isCompleteObject(const Value* val) const;
	bool isCompleteObject(const Value* val, std::set<const PHINode*>& visitedPhis) const;
	bool isCompleteArray(const Value* val) const;
	bool isCompleteArray(const Value* val, std::set<const PHINode*>& visitedPhis) const;
	/*
	 * \param v The pointer to dereference, it may be a regular pointer, a complete obj or a complete array
	 * \param offset An offset coming from code, which may be also NULL
	 * \param namedOffset An offset that will be added verbatim to the code
	 */
	void compileDereferencePointer(const Value* v, const Value* offset, const char* namedOffset);
	void compileFastGEPDereference(const Value* operand, const Use* idx_begin, const Use* idx_end);
	void compileGEP(const Value* val, const Use* it, const Use* const itE);
	const Type* compileObjectForPointerGEP(const Value* val, const Use* it, const Use* const itE);
	bool compileOffsetForPointerGEP(const Value* val, const Use* it, const Use* const itE, const Type* lastType);
	const Type* compileObjectForPointer(const Value* val);
	/*
	 * Returns true if anything is printed
	 */
	bool compileOffsetForPointer(const Value* val, const Type* lastType);
	Type* findRealType(const Value* v, std::set<const PHINode*>& visitedPhis) const;
	void compileCopy(const Value* dest, const Value* src, const Value* size);
	void compileCopyRecursive(const std::string& baseName, const Value* baseDest,
		const Value* baseSrc, const Type* currentType, const char* namedOffset);
	void compileReset(const Value* dest, uint8_t resetValue, const Value* size);
	void compileResetRecursive(const std::string& baseName, const Value* baseDest,
		uint8_t resetValue, const Type* currentType, const char* namedOffset);
	void compileAllocation(const Value* callV, const Value* size);
	void compileFree(const Value* obj);
	void printLLVMName(const StringRef& s) const;
	void printVarName(const Value* v);
	void handleBuiltinNamespace(const char* ident, User::const_op_iterator it,
			User::const_op_iterator itE);
	bool handleBuiltinCall(const char* ident, const Value* callV,
			User::const_op_iterator it, User::const_op_iterator itE);
	bool safeUsagesForNewedMemory(const Value* v) const;
	bool safeCallForNewedMemory(const CallInst* ci) const;
	uint32_t getUniqueIndexForValue(const Value* v);
	std::map<const Value*, uint32_t> unnamedValueMap;
	class DuettoRenderInterface: public RenderInterface
	{
	private:
		JSWriter* writer;
	public:
		DuettoRenderInterface(JSWriter* w):writer(w)
		{
		}
		void renderBlock(void* privateBlock);
		void renderIfBlockBegin(void* privateBlock, int branchId, bool first);
		void renderElseBlockBegin();
		void renderBlockEnd();
		void renderBlockPrologue(void* privateBlockTo, void* privateBlockFrom);
		void renderWhileBlockBegin();
		void renderWhileBlockBegin(int labelId);
		void renderDoBlockBegin();
		void renderDoBlockBegin(int labelId);
		void renderDoBlockEnd();
		void renderBreak();
		void renderBreak(int labelId);
		void renderContinue();
		void renderContinue(int labelId);
		void renderLabel(int labelId);
		void renderIfOnLabel(int labelId, bool first);
	};
public:
	JSWriter(Module* m, raw_fd_ostream& s):module(m),targetData(m),stream(s)
	{
	}
	void makeJS();
	void compileMethod(Function& F);
	void compileGlobal(GlobalVariable& G);
	void compileBB(BasicBlock& BB, const std::map<const BasicBlock*, uint32_t>& blocksMap);
	enum OperandFix{ OPERAND_NO_FIX = 0, OPERAND_EXPAND_COMPLETE_OBJECTS };
	void compileOperand(const Value* v, OperandFix fix = OPERAND_NO_FIX);
	void compileConstant(const Constant* c);
	void compileConstantExpr(const ConstantExpr* ce);
};

void JSWriter::handleBuiltinNamespace(const char* ident, User::const_op_iterator it,
			User::const_op_iterator itE)
{
	//Read the class name
	char* className;
	int classLen = strtol(ident,&className,10);
	assert(classLen!=0);
	ident = className + classLen;

	//Read the function name
	char* funcName;
	int funcNameLen=strtol(ident,&funcName,10);
	if(funcNameLen==0)
	{
		//This means that we are parsing a fuction which is not in a class
		//and we already parsed the function name
		funcName = className;
		funcNameLen = classLen;
		className = NULL;
		classLen = 0;
	}
	assert(funcNameLen!=0);

	//The first arg should be the object
	assert(it!=itE);
	if(strncmp(funcName,"get_",4)==0 && (itE-it)==1)
	{
		//Getter
		assert(className);
		compileOperand(*it);
		stream << ".";
		stream.write(funcName+4,funcNameLen-4);
	}
	else if(strncmp(funcName,"set_",4)==0 && (itE-it)==2)
	{
		//Setter
		assert(className);
		compileOperand(*it);
		++it;
		stream << ".";
		stream.write(funcName+4,funcNameLen-4);
		stream << " = ";
		compileOperand(*it);
	}
	else
	{
		//Regular call
		if(className)
		{
			compileOperand(*it);
			++it;
			stream << ".";
		}
		stream.write(funcName,funcNameLen);
		stream << "(";
		for(User::const_op_iterator cur=it;it!=itE;++it)
		{
			if(cur!=it)
				stream << ",";
			compileOperand(*it);
		}
		stream << ")";
	}
}

bool JSWriter::isBitCast(const Value* v) const
{
	if(BitCastInst::classof(v))
		return true;
	const ConstantExpr* ce=dyn_cast<const ConstantExpr>(v);
	if(ce && ce->getOpcode()==Instruction::BitCast)
		return true;
	return false;
}

bool JSWriter::isGEP(const Value* v) const
{
	if(GetElementPtrInst::classof(v))
		return true;
	const ConstantExpr* ce=dyn_cast<const ConstantExpr>(v);
	if(ce && ce->getOpcode()==Instruction::GetElementPtr)
		return true;
	return false;
}

void JSWriter::compileCopyRecursive(const std::string& baseName, const Value* baseDest,
		const Value* baseSrc, const Type* currentType, const char* namedOffset)
{
	switch(currentType->getTypeID())
	{
		case Type::IntegerTyID:
		case Type::DoubleTyID:
		case Type::PointerTyID:
		{
			compileDereferencePointer(baseDest, NULL, namedOffset);
			stream << baseName << " = ";
			compileDereferencePointer(baseSrc, NULL, namedOffset);
			stream << baseName << ";\n";
			break;
		}
		case Type::StructTyID:
		{
			const StructType* st=static_cast<const StructType*>(currentType);
			StructType::element_iterator E=st->element_begin();
			StructType::element_iterator EE=st->element_end();
			uint32_t offset=0;
			for(;E!=EE;++E)
			{
				char buf[16];
				snprintf(buf,16,".a%u",offset);
				compileCopyRecursive(baseName+buf, baseDest, baseSrc, *E, namedOffset);
				offset++;
			}
			break;
		}
		case Type::ArrayTyID:
		{
			const ArrayType* at=static_cast<const ArrayType*>(currentType);
			char buf[16];
			for(uint64_t i=0;i<at->getNumElements();i++)
			{
				snprintf(buf,16,"[%lu]",i);
				compileCopyRecursive(baseName+buf, baseDest, baseSrc, at->getElementType(), namedOffset);
			}
			break;
		}
		default:
			cerr << "Support type in copy ";
			currentType->dump();
			cerr << endl;
	}
}

void JSWriter::compileReset(const Value* castedDest, uint8_t resetValue, const Value* size)
{
	//First of all dump the bitcast
	assert(isBitCast(castedDest));
	const Value* dest=static_cast<const BitCastInst*>(castedDest)->getOperand(0);
	assert(dest->getType()->isPointerTy());
	const PointerType* pointedType = static_cast<const PointerType*>(dest->getType());
	stream << '{';
	compileResetRecursive("", dest, resetValue, pointedType->getElementType(), NULL);
	stream << '}';
}

void JSWriter::compileResetRecursive(const std::string& baseName, const Value* baseDest,
		uint8_t resetValue, const Type* currentType, const char* namedOffset)
{
	switch(currentType->getTypeID())
	{
		case Type::IntegerTyID:
		{
			compileDereferencePointer(baseDest, NULL, namedOffset);
			assert(resetValue == 0 || resetValue == 0xff);
			if(resetValue == 0)
				stream << baseName << " = 0";
			else if(resetValue == 0xff)
				stream << baseName << " = 0xffffffff";
			stream << ";\n";
			break;
		}
		case Type::DoubleTyID:
		{
			compileDereferencePointer(baseDest, NULL, namedOffset);
			assert(resetValue == 0);
			stream << baseName << " = 0;\n";
			break;
		}
		case Type::PointerTyID:
		{
			compileDereferencePointer(baseDest, NULL, namedOffset);
			assert(resetValue == 0);
			stream << baseName << " = null;\n";
			break;
		}
		case Type::StructTyID:
		{
			const StructType* st=static_cast<const StructType*>(currentType);
			StructType::element_iterator E=st->element_begin();
			StructType::element_iterator EE=st->element_end();
			uint32_t offset=0;
			for(;E!=EE;++E)
			{
				char buf[16];
				snprintf(buf,16,".a%u",offset);
				compileResetRecursive(baseName+buf, baseDest, resetValue, *E, namedOffset);
				offset++;
			}
			break;
		}
		case Type::ArrayTyID:
		{
			const ArrayType* at=static_cast<const ArrayType*>(currentType);
			char buf[16];
			for(uint64_t i=0;i<at->getNumElements();i++)
			{
				snprintf(buf,16,"[%lu]",i);
				compileResetRecursive(baseName+buf, baseDest, resetValue, at->getElementType(), namedOffset);
			}
			break;
		}
		default:
			cerr << "Support type in reset ";
			currentType->dump();
			cerr << endl;
	}
}

Type* JSWriter::findRealType(const Value* v, std::set<const PHINode*>& visitedPhis) const
{
	if(isBitCast(v))
		return static_cast<const User*>(v)->getOperand(0)->getType();

	const PHINode* newPHI=dyn_cast<const PHINode>(v);
	if(newPHI)
 	{
		if(visitedPhis.count(newPHI))
		{
			//Assume true, if needed it will become false later on
			return NULL;
		}
		visitedPhis.insert(newPHI);
		assert(newPHI->getNumIncomingValues()>=1);
		Type* ret=findRealType(newPHI->getIncomingValue(0),visitedPhis);
		for(unsigned i=1;i<newPHI->getNumIncomingValues();i++)
		{
			Type* t=findRealType(newPHI->getIncomingValue(i),visitedPhis);
			if(ret==NULL)
				ret=t;
			else
				assert(ret==t);
		}
		visitedPhis.erase(newPHI);
		return ret;
 	}
	return v->getType();
}

void JSWriter::compileCopy(const Value* dest, const Value* src, const Value* size)
{
	//Find out the real type of the copied object
	std::set<const PHINode*> visitedPhis;
	Type* destType=findRealType(dest,visitedPhis);
	visitedPhis.clear();
	Type* srcType=findRealType(src,visitedPhis);
	assert(destType==srcType);
	assert(destType->isPointerTy());

	Type* pointedType = static_cast<PointerType*>(destType)->getElementType();
	uint32_t typeSize = targetData.getTypeAllocSize(pointedType);
	if(ConstantInt::classof(size))
	{
		uint32_t allocatedSize = getIntFromValue(size);
		assert((allocatedSize % typeSize) == 0);
		uint32_t numElem = allocatedSize/typeSize;
		assert(numElem>0);
		//The first element is always copied directly, to support complete objects
		compileCopyRecursive("", dest, src, pointedType,NULL);
		//The rest is compiled using a for loop, or native TypedArray set operator
		if(numElem==1)
			return;

		if(pointedType->isIntegerTy(8) || pointedType->isIntegerTy(16) ||
				pointedType->isIntegerTy(32) || pointedType->isDoubleTy())
		{
			const Type* lastTypeDest=compileObjectForPointer(dest);
			stream << ".set(";
			const Type* lastTypeSrc=compileObjectForPointer(src);
			//We need to get a subview of the source
			stream << ".subarray(";
			bool notFirst=compileOffsetForPointer(src,lastTypeSrc);
			if(!notFirst)
				stream << '0';
			stream << ',';
			notFirst=compileOffsetForPointer(src,lastTypeSrc);
			if(notFirst)
				stream << '+';
			stream << numElem << "),";
			notFirst=compileOffsetForPointer(dest,lastTypeDest);
			if(!notFirst)
				stream << '0';
			stream << ')';
		}
		else
		{
			stream << "for(var __i__=1;__i__<" << numElem << ";__i__++) {\n";
			compileCopyRecursive("", dest, src, pointedType,"__i__");
			stream << "}\n";
		}
	}
	else
	{
		//TODO: See if we should support complete objects for dynamic sizes
		//TODO: Remove division for size 1
		if(pointedType->isIntegerTy(8) || pointedType->isIntegerTy(16) ||
				pointedType->isIntegerTy(32) || pointedType->isDoubleTy())
		{
			const Type* lastTypeDest=compileObjectForPointer(dest);
			stream << ".set(";
			const Type* lastTypeSrc=compileObjectForPointer(src);
			//We need to get a subview of the source
			stream << ".subarray(";
			bool notFirst=compileOffsetForPointer(src,lastTypeSrc);
			if(!notFirst)
				stream << '0';
			stream << ',';
			notFirst=compileOffsetForPointer(src,lastTypeSrc);
			if(notFirst)
				stream << '+';
			stream << '(';
			compileOperand(size);
			stream << '/' << typeSize << ")),";
			notFirst=compileOffsetForPointer(dest,lastTypeDest);
			if(!notFirst)
				stream << '0';
			stream << ')';
		}
		else
		{
			stream << "for(var __i__=0;__i__<(";
			compileOperand(size);
			stream << '/' << typeSize << ");__i__++) {\n";
			compileCopyRecursive("", dest, src, pointedType,"__i__");
			stream << "}\n";
		}
	}
}

void JSWriter::compileAllocation(const Value* callV, const Value* size)
{
	//Find out if this is casted to something
	Value::const_use_iterator it=callV->use_begin();
	Value::const_use_iterator itE=callV->use_end();
	const Type* castedType = NULL;
	for(;it!=itE;++it)
	{
		if(!BitCastInst::classof(*it))
			continue;
		if(castedType == NULL)
			castedType = it->getType();
		else
		{
			//Make sure this is not casted to more than a type
			assert(castedType == it->getType());
		}
	}

	//If there are no casts, use i8* from the call itself
	if(castedType==0)
		castedType = callV->getType();

	assert(castedType->isPointerTy());
	Type* t=static_cast<const PointerType*>(castedType)->getElementType();
	//For numerical types, create typed arrays
	if(t->isIntegerTy() || t->isDoubleTy())
	{
		stream << "new ";
		if(t->isIntegerTy(8))
			stream << "Int8Array";
		else if(t->isIntegerTy(16))
			stream << "Int16Array";
		else if(t->isIntegerTy(32))
			stream << "Int32Array";
		else if(t->isDoubleTy())
			stream << "Float64Array";
		stream << '(';
		//Use the size in bytes
		compileOperand(size);
		stream << ')';
	}
	else
	{
		uint32_t typeSize = targetData.getTypeAllocSize(t);
		if(ConstantInt::classof(size))
		{
			uint32_t allocatedSize = getIntFromValue(size);
			assert((allocatedSize % typeSize) == 0);
			uint32_t numElem = allocatedSize/typeSize;
			stream << '[';
			for(uint64_t i=0;i<numElem;i++)
			{
				compileType(t);
				if((i+1)<numElem)
					stream << ",";
			}
			stream << ']';
		}
		else
		{
			//TODO: Initialize the values
			stream << "new Array(";
			compileOperand(size);
			stream << '/' << typeSize;
			stream << ')';
		}
	}
}

void JSWriter::compileFree(const Value* obj)
{
	//Best effort
	compileOperand(obj);
	stream << "=null";
}

bool JSWriter::handleBuiltinCall(const char* ident, const Value* callV,
			User::const_op_iterator it, User::const_op_iterator itE)
{
	if(strcmp(ident,"_ZN6client5ArrayixEi")==0 ||
		strcmp(ident,"_ZNK6client6ObjectcvdEv")==0)
	{
		//Do not touch method that are implemented in native JS code
		return false;
	}
	else if(strncmp(ident,"_ZN6client6Client",17)==0)
	{
		//Handle getters in Client
		const char* rest=ident+17;
		char* functionName;
		int functionNameLen=strtol(rest,&functionName,10);
		assert(strncmp(functionName,"get_",4)==0);
		stream.write(functionName+4,functionNameLen-4);
		return true;
	}
	else if(strncmp(ident,"_ZN6client",10)==0)
	{
		handleBuiltinNamespace(ident+10,it,itE);
		return true;
	}
	else if(strncmp(ident,"_ZNK6client",11)==0)
	{
		handleBuiltinNamespace(ident+11,it,itE);
		return true;
	}
	else if(strncmp(ident,"default_duettoCreateBuiltin_",28)==0)
	{
		assert(it==itE);
		//Default handling of builtin constructors
		stream << "new " << (ident+28) << "()";
		return true;
	}
	else if(strncmp(ident,"llvm.memcpy",11)==0)
	{
		compileCopy(*(it), *(it+1), *(it+2));
		return true;
	}
	else if(strncmp(ident,"llvm.memset",11)==0)
	{
		uint32_t resetVal = getIntFromValue(*(it+1));
		compileReset(*(it), resetVal, (*it+2));
		return true;
	}
	else if(strcmp(ident,"malloc")==0 ||
		strcmp(ident,"_Znaj")==0 ||
		strcmp(ident,"_Znwj")==0)
	{
		compileAllocation(callV, *it);
		return true;
	}
	else if(strcmp(ident,"free")==0 ||
		strcmp(ident,"_ZdlPv")==0 ||
		strcmp(ident,"_ZdaPv")==0)
	{
		compileFree(*it);
		return true;
	}
	return false;
}

void JSWriter::compilePredicate(CmpInst::Predicate p)
{
	switch(p)
	{
		case CmpInst::FCMP_OEQ:
		case CmpInst::ICMP_EQ:
			stream << " === ";
			break;
		case CmpInst::FCMP_UNE: //The undordered case correspond to the usual JS operator
					//See ECMA-262, Section 11.9.6
		case CmpInst::ICMP_NE:
			stream << " !== ";
			break;
		case CmpInst::FCMP_OGT: //TODO: fix this, if an operand is NaN LLVM expects false,
		case CmpInst::FCMP_UGT:	//but JS returns undefined. Adding ==true after the whole expression
					//should work
		case CmpInst::ICMP_SGT:
		case CmpInst::ICMP_UGT: //TODO: To support unsigned we need to add casts around the ops
			stream << " > ";
			break;
		case CmpInst::FCMP_UGE:
		case CmpInst::FCMP_OGE:
		case CmpInst::ICMP_SGE:
		case CmpInst::ICMP_UGE:
			stream << " >= ";
			break;
		case CmpInst::FCMP_OLT: //TODO: fix this, if an operand is NaN LLVM expects false,
		case CmpInst::FCMP_ULT:	//but JS returns undefined. Adding ==true after the whole expression
					//should work
		case CmpInst::ICMP_SLT:
		case CmpInst::ICMP_ULT: //TODO: To support unsigned we need to add casts around the ops
			stream << " < ";
			break;
		case CmpInst::FCMP_ULE:
		case CmpInst::FCMP_OLE:
		case CmpInst::ICMP_SLE:
		case CmpInst::ICMP_ULE:
			stream << " <= ";
			break;
		default:
			cerr << "Support predicate " << p << endl;
	}
}

void JSWriter::compileOperandForIntegerPredicate(const Value* v, CmpInst::Predicate p)
{
	if(CmpInst::isUnsigned(p))
	{
		stream << "(";
		compileOperand(v);
		stream << ">>>0)";
	}
	else
	{
		stream << "(";
		compileOperand(v);
		stream << ">>0)";
	}
}

void JSWriter::printLLVMName(const StringRef& s) const
{
	const char* data=s.data();
	//Add an '_' to skip reserved names
	stream.write("_",1);
	for(uint32_t i=0;i<s.size();i++)
	{
		if(data[i]=='.' || data[i]=='-')
			stream.write("_",1);
		else
			stream.write(data+i,1);
	}
}

bool JSWriter::isCompleteArray(const Value* v) const
{
	std::set<const PHINode*> visitedPhis;
	return isCompleteArray(v, visitedPhis);
}

bool JSWriter::isCompleteArray(const Value* v, std::set<const PHINode*>& visitedPhis) const
{
	assert(v->getType()->isPointerTy());
	if(isComingFromAllocation(v))
		return true;
	if(ConstantPointerNull::classof(v))
	{
		//null can be considered a complete array as well as an object
		//dereferencing it is undefined anyway
		return true;
	}
	//Follow bitcasts
	if(isBitCast(v))
	{
		const User* bi=static_cast<const User*>(v);
		return isCompleteArray(bi->getOperand(0), visitedPhis);
	}
	//Follow PHIs
	const PHINode* newPHI=dyn_cast<const PHINode>(v);
	if(newPHI)
	{
		if(visitedPhis.count(newPHI))
		{
			//Assume true, if needed it will become false later on
			return true;
		}
		visitedPhis.insert(newPHI);
		for(unsigned i=0;i<newPHI->getNumIncomingValues();i++)
		{
			if(!isCompleteArray(newPHI->getIncomingValue(i),visitedPhis))
			{
				visitedPhis.erase(newPHI);
				return false;
			}
		}
		visitedPhis.erase(newPHI);
		return true;
	}
	return false;
}

bool JSWriter::isCompleteObject(const Value* v) const
{
	std::set<const PHINode*> visitedPhis;
	return isCompleteObject(v, visitedPhis);
}

bool JSWriter::isCompleteObject(const Value* v, std::set<const PHINode*>& visitedPhis) const
{
	assert(v->getType()->isPointerTy());
	if(AllocaInst::classof(v))
		return true;
	if(GlobalVariable::classof(v))
		return true;
	if(ConstantPointerNull::classof(v))
	{
		//null can be considered a complete object
		return true;
	}
	if(isCompleteArray(v, visitedPhis))
		return true;
	//Follow bitcasts
	if(isBitCast(v))
	{
		//stream << "IS BITCAST\n";
		const User* bi=static_cast<const User*>(v);
		return isCompleteObject(bi->getOperand(0), visitedPhis);
	}
	const PHINode* newPHI=dyn_cast<const PHINode>(v);
	if(newPHI)
	{
		if(visitedPhis.count(newPHI))
		{
			//Assume true, if needed it will become false later on
			return true;
		}
		visitedPhis.insert(newPHI);
		for(unsigned i=0;i<newPHI->getNumIncomingValues();i++)
		{
			if(!isCompleteObject(newPHI->getIncomingValue(i),visitedPhis))
			{
				visitedPhis.erase(newPHI);
				return false;
			}
		}
		visitedPhis.erase(newPHI);
		return true;
	}
	return false;
}

void JSWriter::compileDereferencePointer(const Value* v, const Value* offset, const char* namedOffset = NULL)
{
	assert(v->getType()->isPointerTy());
	bool isArray = isCompleteArray(v);
	bool isObj = isCompleteObject(v);
	bool isOffsetConstantZero = false;
	if(offset==NULL || (ConstantInt::classof(offset) && getIntFromValue(offset)==0))
		isOffsetConstantZero = true;

	const Type* lastType=compileObjectForPointer(v);
	if(isObj && !isArray)
	{
		assert(isOffsetConstantZero);
		assert(namedOffset==NULL);
		return;
	}
	stream << '[';
	if(isArray)
	{
		bool notFirst=false;
		if(namedOffset)
		{
			stream << namedOffset;
			notFirst = true;
		}

		if(isOffsetConstantZero)
		{
			if(!notFirst)
				stream << '0';
		}
		else
		{
			if(notFirst)
				stream << '+';
			compileOperand(offset);
		}
	}
	else
	{
		bool notFirst=compileOffsetForPointer(v, lastType);
		if(!isOffsetConstantZero)
		{
			if(notFirst)
				stream << '+';
			compileOperand(offset);
			notFirst = true;
		}
		if(namedOffset)
		{
			if(notFirst)
				stream << '+';
			stream << namedOffset;
			notFirst = true;
		}
		if(!notFirst)
			stream << '0';
	}
	stream << ']';
}

uint32_t JSWriter::getIntFromValue(const Value* v) const
{
	assert(ConstantInt::classof(v));
	const ConstantInt* i=cast<const ConstantInt>(v);
	return i->getZExtValue();
}

const Type* JSWriter::compileRecursiveAccessToGEP(const Type* curType, const Use* it, const Use* const itE)
{
	//Before this the base name has been already printed
	if(it==itE)
		return curType;
	const Type* subType = NULL;
	if(curType->isStructTy())
	{
		const StructType* st=static_cast<const StructType*>(curType);
		//Special handling for constant offsets
		assert(ConstantInt::classof(*it));
		uint32_t elementIndex = getIntFromValue(*it);
		stream << ".a" << elementIndex;
		subType = st->getElementType(elementIndex);
	}
	else if(curType->isArrayTy())
	{
		const ArrayType* at=static_cast<const ArrayType*>(curType);
		stream << '[';
		//Special handling for constant offsets
		if(ConstantInt::classof(*it))
		{
			uint32_t elementIndex = getIntFromValue(*it);
			stream << elementIndex;
		}
		else
			compileOperand(*it);
		stream << ']';
		subType = at->getElementType();
	}
	else
		assert(false);
	return compileRecursiveAccessToGEP(subType, ++it, itE);
}

bool JSWriter::isClientType(Type* t) const
{
	return (t->isStructTy() && cast<StructType>(t)->hasName() &&
		strncmp(t->getStructName().data(), "class.client::", 14)==0);
}

bool JSWriter::safeUsagesForNewedMemory(const Value* v) const
{
	Value::const_use_iterator it=v->use_begin();
	Value::const_use_iterator itE=v->use_end();
	for(;it!=itE;++it)
	{
		const PHINode* p=dyn_cast<const PHINode>(*it);
		//If the usage is a PHI node, recursively check its usages
		if(p)
		{
			if(!safeUsagesForNewedMemory(p))
				return false;
		}
		else
		{
			const CallInst* ci=dyn_cast<const CallInst>(*it);
			if(!safeCallForNewedMemory(ci))
				return false;
		}
	}
	return true;
}

bool JSWriter::safeCallForNewedMemory(const CallInst* ci) const
{
	//We allow the unsafe cast to i8* only
	//if the usage is memcpy, memset, free or delete
	//or one of the lifetime/invariant intrinsics
	return (ci && (ci->getCalledFunction()->getName()=="llvm.memcpy.p0i8.p0i8.i32" ||
		ci->getCalledFunction()->getName()=="llvm.memset.p0i8.i32" ||
		ci->getCalledFunction()->getName()=="llvm.memset.p0i8.i64" ||
		ci->getCalledFunction()->getName()=="llvm.memmove.p0i8.p0i8.i32" ||
		ci->getCalledFunction()->getName()=="free" ||
		ci->getCalledFunction()->getName()=="_ZdaPv" ||
		ci->getCalledFunction()->getName()=="_ZdlPv" ||
		ci->getCalledFunction()->getName()=="llvm.lifetime.start" ||
		ci->getCalledFunction()->getName()=="llvm.lifetime.end" ||
		ci->getCalledFunction()->getName()=="llvm.invariant.start" ||
		ci->getCalledFunction()->getName()=="llvm.invariant.end" ||
		//Allow unsafe casts for a limited number of functions that accepts callback args
		//TODO: find a nicer approach for this
		ci->getCalledFunction()->getName()=="__cxa_atexit"));
}

bool JSWriter::isComingFromAllocation(const Value* val) const
{
	std::set<const PHINode*> visitedPhis;
	return isComingFromAllocation(val, visitedPhis);
}

bool JSWriter::isComingFromAllocation(const Value* val, std::set<const PHINode*>& visitedPhis) const
{
	const CallInst* newCall=dyn_cast<const CallInst>(val);
	if(newCall && newCall->getCalledFunction())
	{
		return newCall->getCalledFunction()->getName()=="_Znwj"
			|| newCall->getCalledFunction()->getName()=="_Znaj"
			|| newCall->getCalledFunction()->getName()=="realloc"
			|| newCall->getCalledFunction()->getName()=="malloc";
	}
	//Try invoke as well
	const InvokeInst* newInvoke=dyn_cast<const InvokeInst>(val);
	if(newInvoke && newInvoke->getCalledFunction())
	{
		//TODO: Disable throw in new, it's nonsense in JS context
		return newInvoke->getCalledFunction()->getName()=="_Znwj"
			|| newInvoke->getCalledFunction()->getName()=="_Znaj"
			|| newInvoke->getCalledFunction()->getName()=="realloc"
			|| newInvoke->getCalledFunction()->getName()=="malloc";
	}
	const PHINode* newPHI=dyn_cast<const PHINode>(val);
	if(newPHI)
	{
		if(visitedPhis.count(newPHI))
		{
			//Assume true, if needed it will become false later on
			return true;
		}
		visitedPhis.insert(newPHI);
		for(unsigned i=0;i<newPHI->getNumIncomingValues();i++)
		{
			if(!isComingFromAllocation(newPHI->getIncomingValue(i),visitedPhis))
			{
				visitedPhis.erase(newPHI);
				return false;
			}
		}
		visitedPhis.erase(newPHI);
		return true;
	}
	return false;
}

bool JSWriter::isValidTypeCast(const Value* castI, const Value* castOp, Type* srcPtr, Type* dstPtr) const
{
	//Only pointer casts are possible anyway
	assert(srcPtr->isPointerTy() && dstPtr->isPointerTy());
	Type* src=cast<PointerType>(srcPtr)->getElementType();
	Type* dst=cast<PointerType>(dstPtr)->getElementType();
	//Conversion between client objects is free
	if(isClientType(src) && isClientType(dst))
		return true;
	//Conversion between any function pointer are ok
	if(src->isFunctionTy() && dst->isFunctionTy())
		return true;
	if(dst->isIntegerTy(8))
	{
		if(safeUsagesForNewedMemory(castI))
			return true;
	}
	//Support getting functions back from the Vtable
	if(src->isPointerTy() && dst->isPointerTy())
	{
		Type* innerSrc=cast<PointerType>(src)->getElementType();
		Type* innerDst=cast<PointerType>(dst)->getElementType();
		if(innerSrc->isIntegerTy(8) || innerDst->isFunctionTy())
		{
			const GetElementPtrInst* gep=dyn_cast<const GetElementPtrInst>(castOp);
			if(gep)
				assert(false);
			const ConstantExpr* constGep=dyn_cast<const ConstantExpr>(castOp);
			if(constGep && constGep->getOpcode()==Instruction::GetElementPtr)
			{
				const Value* sourceVal = constGep->getOperand(0);
				if(sourceVal->hasName() &&
					strncmp(sourceVal->getName().data(),"_ZTV",4)==0)
				{
					//This casts ultimately comes from a VTable, it's ok
					return true;
				}
			}
		}
		if(innerSrc->isFunctionTy() && innerDst->isFunctionTy())
			return true;
	}
	//Also allow the unsafe cast from i8* only when casting from new, malloc
	//NOTE: The fresh memory may be passed uncasted to memset to zero new memory
	//NOTE: The fresh memory may be passed uncasted to memcpy (it optimizes another cast to i8*)
	//NOTE: The fresh memory may be passed uncasted to icmp to test against null
	if(src->isIntegerTy(8))
	{
		bool comesFromNew = isComingFromAllocation(castOp);
		bool allowedRawUsages = true;
		Value::const_use_iterator it=castOp->use_begin();
		Value::const_use_iterator itE=castOp->use_end();
		for(;it!=itE;++it)
		{
			//Check that the other use is a memset or an icmp
			if((*it)==castI)
				continue;
			const CallInst* ci=dyn_cast<const CallInst>(*it);
			if(!(ICmpInst::classof(*it) || safeCallForNewedMemory(ci)))
				allowedRawUsages = false;
		}
		if(comesFromNew && allowedRawUsages)
			return true;
	}
	castI->dump();
	cerr << endl;
	src->dump();
	cerr << endl;
	dst->dump();
	cerr << endl;
	return false;
}

void JSWriter::compileConstantExpr(const ConstantExpr* ce)
{
	switch(ce->getOpcode())
	{
		case Instruction::GetElementPtr:
		{
			assert(ce->getNumOperands()>=3);
			Value* base = ce->getOperand(0);
			assert(GlobalVariable::classof(base));
			//GlobalVariables never changes, they are assumed to be
			//in the form { d: [<objPointed>], o: 0 }
			assert(cast<GlobalVariable>(base)->hasInitializer());
			//TODO: Support external global variables
			//NOTE: the first dereference must be 0, they point to a single object
			Value* first=ce->getOperand(1);
			assert(getIntFromValue(first)==0);
			compileGEP(base, ce->op_begin()+1, ce->op_end()-1);
			break;
		}
		case Instruction::BitCast:
		{
			assert(ce->getNumOperands()==1);
			Value* val=ce->getOperand(0);
			Type* dst=ce->getType();
			Type* src=val->getType();
			//Special case guard variables, they are defined as 64bit,
			//but only the first byte is specified and probably used
			//Guard variables are identified by their mangling prefix
			if(val->hasName() && strncmp("_ZGV",val->getName().data(),4)==0)
			{
				compileOperand(val);
				break;
			}
			assert(isValidTypeCast(ce, val, src, dst));
			compileOperand(val);
			break;
		}
		default:
			cerr << "Unsupported constant expr " << ce->getOpcodeName() << endl;
	}
}

bool JSWriter::isClientGlobal(const char* mangledName) const
{
	return strncmp(mangledName,"_ZN6client",10)==0;
}

void JSWriter::compileConstant(const Constant* c)
{
	if(ConstantExpr::classof(c))
		compileConstantExpr(cast<ConstantExpr>(c));
	else if(ConstantDataSequential::classof(c))
	{
		const ConstantDataSequential* d=cast<const ConstantDataSequential>(c);
		Type* t=d->getElementType();
		if(t->isIntegerTy(8))
			stream << "Int8Array";
		else if(t->isIntegerTy(16))
			stream << "Int16Array";
		else if(t->isIntegerTy(32))
			stream << "Int32Array";
		else if(t->isDoubleTy())
			stream << "Float64Array";
		else
			assert(false);
		stream << "([";
		for(uint32_t i=0;i<d->getNumElements();i++)
		{
			compileConstant(d->getElementAsConstant(i));
			if((i+1)<d->getNumElements())
				stream << ",";
		}
		stream << "])";
	}
	else if(ConstantArray::classof(c))
	{
		const ConstantArray* d=cast<const ConstantArray>(c);
		stream << '[';
		assert(d->getType()->getNumElements() == d->getNumOperands());
		for(uint32_t i=0;i<d->getNumOperands();i++)
		{
			compileConstant(d->getOperand(i));
			if((i+1)<d->getNumOperands())
				stream << ",";
		}
		stream << ']';
	}
	else if(ConstantStruct::classof(c))
	{
		const ConstantStruct* d=cast<const ConstantStruct>(c);
		stream << '{';
		assert(d->getType()->getNumElements() == d->getNumOperands());
		for(uint32_t i=0;i<d->getNumOperands();i++)
		{
			stream << 'a' << i << ':';
			compileConstant(d->getOperand(i));
			if((i+1)<d->getNumOperands())
				stream << ",";
		}
		stream << '}';
	}
	else if(ConstantFP::classof(c))
	{
		const ConstantFP* f=cast<const ConstantFP>(c);
		//Must compare pointers, it seems
		if(&f->getValueAPF().getSemantics()==&APFloat::IEEEsingle)
			stream << f->getValueAPF().convertToFloat();
		else if(&f->getValueAPF().getSemantics()==&APFloat::IEEEdouble)
			stream << f->getValueAPF().convertToDouble();
		else
			assert(false);
	}
	else if(ConstantInt::classof(c))
	{
		const ConstantInt* i=cast<const ConstantInt>(c);
		//TODO: Restore when 64bit are forbidden by the frontend
		//assert(i->getBitWidth()<=32);
		stream << i->getSExtValue();
	}
	else if(Function::classof(c))
	{
		assert(c->hasName());
		//printLLVMName already add '_' to the name
		printLLVMName(c->getName());
	}
	else if(ConstantPointerNull::classof(c))
	{
		stream << "null";
	}
	else if(UndefValue::classof(c))
	{
		stream << "undefined";
	}
	else if(GlobalValue::classof(c))
	{
		assert(c->hasName());
		//Check if this is a client global value, if so skip mangling
		const char* mangledName = c->getName().data();
		if(isClientGlobal(mangledName))
		{
			//Client value
			char* objName;
			int nameLen=strtol(mangledName+10,&objName,10);
			stream.write(objName, nameLen);
		}
		else
			printLLVMName(c->getName());
	}
	else if(ConstantAggregateZero::classof(c))
	{
		compileType(c->getType());
	}
	else if(c->hasName())
	{
		printLLVMName(c->getName());
	}
	else
	{
		cerr << "Unsupported constant type ";
		c->dump();
		stream << "null";
	}
}

void JSWriter::compileOperand(const Value* v, OperandFix fix)
{
	//First deal with complete objects
	if(v->getType()->isPointerTy() && fix==OPERAND_EXPAND_COMPLETE_OBJECTS && isCompleteObject(v))
	{
		//Synthetize a pointer just in time
		if(isCompleteArray(v))
		{
			stream << "{ d: ";
			compileOperand(v, OPERAND_NO_FIX);
			stream << ", o: 0}";
			return;
		}
		else //if(isCompleteObject(v))
		{
			stream << "{ d: [";
			compileOperand(v, OPERAND_NO_FIX);
			stream << "], o: 0}";
			return;
		}
	}

	const Constant* c=dyn_cast<const Constant>(v);
	if(c)
		compileConstant(c);
	else if(dyn_cast<Instruction>(v))
	{
		const Instruction* it=cast<const Instruction>(v);
		if(isInlineable(*it))
			compileInlineableInstruction(*cast<Instruction>(v));
		else
			printVarName(it);
	}
	else if(v->hasName())
		printLLVMName(v->getName());
	else
	{
		cerr << "No name for value ";
		v->dump();
	}
}

void JSWriter::compileType(Type* t)
{
	switch(t->getTypeID())
	{
		case Type::IntegerTyID:
		{
			//IntegerType* it=static_cast<IntegerType*>(t);
			//We only really have 32bit integers.
			//We will allow anything shorter.
			//NOTE: Only bit operations are allowed on shorter types
			//this is enforced on a per-operation basis
			//TODO: put assertion back
			//assert(it->getBitWidth()<=32);
			//Print out a '0'. To let the engine know this is an integer
			stream << '0';
			break;
		}
		case Type::FloatTyID:
		case Type::DoubleTyID:
		{
			stream << '0';
			break;
		}
		case Type::StructTyID:
		{
			stream << "{ ";
			StructType* st=static_cast<StructType*>(t);
			StructType::element_iterator E=st->element_begin();
			StructType::element_iterator EE=st->element_end();
			uint32_t offset=0;
			for(;E!=EE;++E)
			{
				if(offset!=0)
					stream << ", ";
				stream << "a" << offset << ": ";
				compileType(*E);
				offset++;
			}
			stream << " }";
			break;
		}
		case Type::PointerTyID:
			stream << "null";
			break;
		case Type::ArrayTyID:
		{
			ArrayType* at=static_cast<ArrayType*>(t);
			stream << '[';
			for(uint64_t i=0;i<at->getNumElements();i++)
			{
				compileType(at->getElementType());
				if((i+1)<at->getNumElements())
					stream << ",";
			}
			stream << ']';
			break;
		}
		default:
			cerr << "Support type ";
			t->dump();
			cerr << endl;
	}
}

uint32_t JSWriter::getUniqueIndexForValue(const Value* v)
{
	std::map<const Value*,uint32_t>::iterator it=unnamedValueMap.find(v);
	if(it==unnamedValueMap.end())
		it=unnamedValueMap.insert(make_pair(v, unnamedValueMap.size())).first;
	return it->second;
}

void JSWriter::printVarName(const Value* val)
{
	if(val->hasName())
		printLLVMName(val->getName());
	else
		stream << "tmp" << getUniqueIndexForValue(val);
}

void JSWriter::compilePHIOfBlockFromOtherBlock(const BasicBlock* to, const BasicBlock* from)
{
	BasicBlock::const_iterator I=to->begin();
	BasicBlock::const_iterator IE=to->end();
	for(;I!=IE;++I)
	{
		const PHINode* phi=dyn_cast<const PHINode>(I);
		if(phi==NULL)
			continue;
		const Value* val=phi->getIncomingValueForBlock(from);
		//TODO: verify that 'var' works
		stream << "var ";
		printVarName(phi);
		stream << " = ";
		//Fix complete object pointers if needed
		OperandFix fix=OPERAND_NO_FIX;
		if(val->getType()->isPointerTy() && !isCompleteObject(phi))
			fix = OPERAND_EXPAND_COMPLETE_OBJECTS;
		compileOperand(val, fix);
		stream << ";\n";
	}
}

/*
 * This method is fragile, each opcode must handle the phis in the correct place
 */
void JSWriter::compileTerminatorInstruction(const TerminatorInst& I)
{
	switch(I.getOpcode())
	{
		case Instruction::Ret:
		{
			const ReturnInst& ri=static_cast<const ReturnInst&>(I);
			assert(I.getNumSuccessors()==0);
			Value* retVal = ri.getReturnValue();
			stream << "return ";
			if(retVal)
				compileOperand(retVal);
			stream << ";\n";
			break;
		}
		case Instruction::Invoke:
		{
			const InvokeInst& ci=static_cast<const InvokeInst&>(I);
			//TODO: Support unwind
			//For now, pretend it's a regular call
			if(ci.getCalledFunction())
			{
				//Direct call
				const char* funcName=ci.getCalledFunction()->getName().data();
				if(handleBuiltinCall(funcName,&ci,ci.op_begin(),ci.op_begin()+ci.getNumArgOperands()))
				{
					stream << ";\n";
					//Only consider the normal successor for PHIs here
					//For each successor output the variables for the phi nodes
					compilePHIOfBlockFromOtherBlock(ci.getNormalDest(), I.getParent());
					break;
				}
				else
					stream << '_' << funcName;
			}
			else
			{
				//Indirect call
				compileOperand(ci.getCalledValue());
			}

			stream << '(';
			for(uint32_t i=0;i<ci.getNumArgOperands();i++)
			{
				if(i!=0)
					stream << ", ";
				compileOperand(ci.getArgOperand(i), OPERAND_EXPAND_COMPLETE_OBJECTS);
			}
			stream << ");\n";
			//Only consider the normal successor for PHIs here
			//For each successor output the variables for the phi nodes
			compilePHIOfBlockFromOtherBlock(ci.getNormalDest(), I.getParent());
			break;
		}
		case Instruction::Resume:
		{
			//TODO: support exceptions
			break;
		}
		case Instruction::Br:
		case Instruction::Switch:
			break;
		default:
			stream << "alert('Unsupported code');\n";
			cerr << "\tImplement terminator inst " << I.getOpcodeName() << endl;
			break;
	}
}

void JSWriter::compileTerminatorInstruction(const TerminatorInst& I,
		const std::map<const BasicBlock*, uint32_t>& blocksMap)
{
	compileTerminatorInstruction(I);
	switch(I.getOpcode())
	{
		case Instruction::Ret:
			break;
		case Instruction::Invoke:
		{
			//TODO: Support unwind
			const InvokeInst& ci=static_cast<const InvokeInst&>(I);
			stream << "__block = " << blocksMap.find(ci.getNormalDest())->second << ";\n";
			break;
		}
		case Instruction::Resume:
		{
			//TODO: support exceptions
			break;
		}
		case Instruction::Br:
		{
			//For each successor output the variables for the phi nodes
			const BranchInst& bi=static_cast<const BranchInst&>(I);
			if(bi.isUnconditional())
			{
				//Generate the PHIs
				compilePHIOfBlockFromOtherBlock(bi.getSuccessor(0), I.getParent());
				stream << "__block = " << blocksMap.find(bi.getSuccessor(0))->second << ";\n";
			}
			else
			{
				//In each branch generate the right PHIs
				stream << "if( ";
				compileOperand(bi.getCondition());
				stream << ") { ";
				compilePHIOfBlockFromOtherBlock(bi.getSuccessor(0), I.getParent());
				stream << "__block = " << blocksMap.find(bi.getSuccessor(0))->second <<
					"; } else {";
				compilePHIOfBlockFromOtherBlock(bi.getSuccessor(1), I.getParent());
				stream << "__block = " << blocksMap.find(bi.getSuccessor(1))->second <<
					";}\n";
			}
			break;
		}
		case Instruction::Switch:
		{
			//Create a JS switch
			const SwitchInst& si=static_cast<const SwitchInst&>(I);
			stream << "switch (";
			compileOperand(si.getCondition());
			stream << "){";
			SwitchInst::ConstCaseIt it=si.case_begin();
			for(;it!=si.case_end();++it)
			{
				stream << "case ";
				compileConstant(it.getCaseValue());
				stream << ":\n__block = " << blocksMap.find(it.getCaseSuccessor())->second <<
					"; break;";
			}
			assert(si.getDefaultDest());
			stream << "default:\n__block = " << blocksMap.find(si.getDefaultDest())->second <<
				";";
			stream << "}\n";
			break;
		}
		default:
			stream << "alert('Unsupported code');\n";
			cerr << "\tImplement terminator inst " << I.getOpcodeName() << endl;
			break;
	}
}

bool JSWriter::compileNotInlineableInstruction(const Instruction& I)
{
	switch(I.getOpcode())
	{
		case Instruction::Alloca:
		{
			const AllocaInst& ai=static_cast<const AllocaInst&>(I);
			//Alloca returns complete objects, not pointers
			compileType(ai.getAllocatedType());
			return true;
		}
		case Instruction::Call:
		{
			const CallInst& ci=static_cast<const CallInst&>(I);
			if(ci.getCalledFunction())
			{
				//Direct call
				const char* funcName=ci.getCalledFunction()->getName().data();
				if(handleBuiltinCall(funcName,&ci,ci.op_begin(),ci.op_begin()+ci.getNumArgOperands()))
					return true;
				stream << '_' << funcName;
			}
			else
			{
				//Indirect call
				compileOperand(ci.getCalledValue());
			}
			stream << '(';
			for(uint32_t i=0;i<ci.getNumArgOperands();i++)
			{
				if(i!=0)
					stream << ", ";
				compileOperand(ci.getArgOperand(i), OPERAND_EXPAND_COMPLETE_OBJECTS);
			}
			stream << ")";
			return true;
		}
		case Instruction::LandingPad:
		{
			//TODO: Support exceptions
			stream << " alert('Exceptions not supported')";
			//Do not continue block
			return false;
		}
		case Instruction::InsertValue:
		{
			const InsertValueInst& ivi=static_cast<const InsertValueInst&>(I);
			const Value* aggr=ivi.getAggregateOperand();
			Type* t=aggr->getType();
			assert(t->isStructTy());
			if(UndefValue::classof(aggr))
			{
				//We have to assemble the type object from scratch
				compileType(t);
				stream << ";\n";
				//Also assign the element
				assert(ivi.getNumIndices()==1);
				//Find the offset to the pointed element
				assert(ivi.hasName());
				printLLVMName(ivi.getName());
			}
			else
			{
				//Optimize for the assembly of the aggregate values
				assert(aggr->hasOneUse());
				assert(aggr->hasName());
				printLLVMName(aggr->getName());
			}
			uint32_t offset=ivi.getIndices()[0];
			stream << ".a" << offset << " = ";
			compileOperand(ivi.getInsertedValueOperand());
			return true;
		}
		case Instruction::Load:
		{
			const LoadInst& li=static_cast<const LoadInst&>(I);
			const Value* ptrOp=li.getPointerOperand();
			stream << "(";
			//If the ptrOp has a single use and it'a a GEP
			//we can optimize it
			if(GetElementPtrInst::classof(ptrOp))
			{
				const GetElementPtrInst& gep=static_cast<const GetElementPtrInst&>(*ptrOp);
				compileFastGEPDereference(gep.getOperand(0), gep.idx_begin(), gep.idx_end());
			}
			else if((ConstantExpr::classof(ptrOp)) &&
					cast<ConstantExpr>(ptrOp)->getOpcode()==Instruction::GetElementPtr)
			{
				const ConstantExpr& cgep=static_cast<const ConstantExpr&>(*ptrOp);
				compileFastGEPDereference(cgep.getOperand(0), cgep.op_begin()+1, cgep.op_end());
			}
			else
				compileDereferencePointer(ptrOp, NULL);
			stream << ")";
			return true;
		}
		case Instruction::Store:
		{
			const StoreInst& si=static_cast<const StoreInst&>(I);
			const Value* ptrOp=si.getPointerOperand();
			const Value* valOp=si.getValueOperand();
			//If the ptrOp is a GEP we can optimize the access
			if(GetElementPtrInst::classof(ptrOp))
			{
				const GetElementPtrInst& gep=static_cast<const GetElementPtrInst&>(*ptrOp);
				compileFastGEPDereference(gep.getOperand(0), gep.idx_begin(), gep.idx_end());
			}
			else if((ConstantExpr::classof(ptrOp)) &&
					cast<ConstantExpr>(ptrOp)->getOpcode()==Instruction::GetElementPtr)
			{
				const ConstantExpr& cgep=static_cast<const ConstantExpr&>(*ptrOp);
				compileFastGEPDereference(cgep.getOperand(0), cgep.op_begin()+1, cgep.op_end());
			}
			else
				compileDereferencePointer(ptrOp, NULL);
			stream << " = ";
			compileOperand(valOp, OPERAND_EXPAND_COMPLETE_OBJECTS);
			return true;
		}
		default:
			return compileInlineableInstruction(I);
	}
}

bool JSWriter::isI32Type(Type* t) const
{
	//TODO: To compile bullet 64 bit integers
	//are needed. We don't want to support them
	//but for first round support we will cheat
	//as emscripten does and allow them
	return t->isIntegerTy();// && static_cast<IntegerType*>(t)->getBitWidth()==32;
}

void JSWriter::compileFastGEPDereference(const Value* operand, const Use* idx_begin, const Use* idx_end)
{
	assert(idx_begin!=idx_end);
	compileObjectForPointerGEP(operand, idx_begin, idx_end);
}

const Type* JSWriter::compileObjectForPointer(const Value* val)
{
	if(isGEP(val))
	{
		const User* gep=static_cast<const User*>(val);
		GetElementPtrInst::const_op_iterator it=gep->op_begin()+1;
		//We compile as usual till the last level
		GetElementPtrInst::const_op_iterator itE=gep->op_end()-1;
		return compileObjectForPointerGEP(gep->getOperand(0), it, itE);
	}
	else if(isBitCast(val))
	{
		const User* b=static_cast<const User*>(val);
		return compileObjectForPointer(b->getOperand(0));
	}
	else if(isCompleteObject(val))
	{
		compileOperand(val);
		return NULL;
	}
	else
	{
		compileOperand(val);
		stream << ".d";
		return NULL;
	}
}

bool JSWriter::compileOffsetForPointer(const Value* val, const Type* lastType)
{
	if(isGEP(val))
	{
		const User* gep=static_cast<const User*>(val);
		GetElementPtrInst::const_op_iterator it=gep->op_begin()+1;
		//We compile as usual till the last level
		GetElementPtrInst::const_op_iterator itE=gep->op_end()-1;
		return compileOffsetForPointerGEP(gep->getOperand(0), it, itE, lastType);
	}
	else if(isBitCast(val))
	{
		const User* b=static_cast<const User*>(val);
		return compileOffsetForPointer(b->getOperand(0), lastType);
	}
	else if(isCompleteObject(val))
		return false;
	else
	{
		compileOperand(val);
		stream << ".o";
		return true;
	}
}

const Type* JSWriter::compileObjectForPointerGEP(const Value* val, const Use* it, const Use* const itE)
{
	Type* t=val->getType();
	assert(t->isPointerTy());
	PointerType* ptrT=static_cast<PointerType*>(t);
	if(it==itE)
	{
		//Same level access, we are just computing another pointer from this pointer
		compileObjectForPointer(val);
		return ptrT->getElementType();
	}
	else
	{
		//First dereference the pointer
		compileDereferencePointer(val, *it);
		return compileRecursiveAccessToGEP(ptrT->getElementType(), ++it, itE);
	}
}

bool JSWriter::compileOffsetForPointerGEP(const Value* val, const Use* it, const Use* const itE, const Type* lastType)
{
	if(it==itE)
	{
		//Same level access, we are just computing another pointer from this pointer
		bool notFirst=compileOffsetForPointer(val, lastType);
		//Compute the offset
		if(ConstantInt::classof(*itE))
		{
			uint32_t firstElement = getIntFromValue(*itE);
			if(firstElement==0)
				return notFirst;
			if(notFirst)
				stream << '+';
			stream << firstElement;
		}
		else
		{
			if(notFirst)
				stream << '+';
			compileOperand(*itE);
		}
	}
	else
	{
		//Now add the offset for the desired element
		if(ConstantInt::classof(*itE))
		{
			uint32_t elementIndex = getIntFromValue(*itE);
			if(elementIndex == 0 && !lastType->isStructTy())
				return false;
			if(lastType->isStructTy())
				stream << "\"a";
			stream << elementIndex;
			if(lastType->isStructTy())
				stream << '"';
		}
		else
		{
			assert(!lastType->isStructTy());
			compileOperand(*itE);
		}
	}
	//TODO: Skip some useless offsets when possible
	return true;
}

void JSWriter::compileGEP(const Value* val, const Use* it, const Use* const itE)
{
	Type* t=val->getType();
	assert(t->isPointerTy());
	stream << "{ d: ";
	const Type* lastType=compileObjectForPointerGEP(val, it, itE);
	stream << ", o: ";
	bool notFirst=compileOffsetForPointerGEP(val, it, itE,lastType);
	if(!notFirst)
		stream << '0';
	stream << '}';
}

/*
 * This can be used for both named instructions and inlined ones
 * NOTE: Call, Ret, Invoke are NEVER inlined
 */
bool JSWriter::compileInlineableInstruction(const Instruction& I)
{
	//Check if there are any special metadata
	if(I.getMetadata("duetto.downcast.ignore"))
		return true;
	if(I.getMetadata("duetto.downcast"))
	{
		//Downcast will be handled with an access to the base table
		stream << "alert('support downcast')";
		return true;
	}
	switch(I.getOpcode())
	{
		case Instruction::BitCast:
		{
			const BitCastInst& bi=static_cast<const BitCastInst&>(I);
			Type* srcPtr=bi.getSrcTy();
			Type* dstPtr=bi.getDestTy();

			bool isCollapsedUpcast = I.getMetadata("duetto.upcast.collapsed")!=NULL;
			assert(isCollapsedUpcast || isValidTypeCast(&bi, bi.getOperand(0), srcPtr, dstPtr));
			//Collapsed upcast are ok as well by compiling the operand
			compileOperand(bi.getOperand(0));
			return true;
		}
		case Instruction::FPToSI:
		{
			const CastInst& ci=static_cast<const CastInst&>(I);
			//Check that the in and out types are sane
			//Type* srcT = ci.getSrcTy();
			Type* dstT = ci.getDestTy();
			//TODO: Restore type check, now relax it
			//assert(srcT->isDoubleTy());
			assert(isI32Type(dstT));

			stream << "(";
			compileOperand(ci.getOperand(0));
			//Seems to be the fastest way
			//http://jsperf.com/math-floor-vs-math-round-vs-parseint/33
			stream << " >> 0)";
			return true;
		}
		case Instruction::FPToUI:
		{
			const CastInst& ci=static_cast<const CastInst&>(I);
			//Check that the in and out types are sane
			Type* srcT = ci.getSrcTy();
			Type* dstT = ci.getDestTy();
			assert(srcT->isDoubleTy());
			assert(isI32Type(dstT));

			stream << "(";
			compileOperand(ci.getOperand(0));
			//Cast to signed anyway
			//ECMA-262 guarantees that (a >> 0) >>> 0
			//is the same as (a >>> 0)
			stream << " >> 0)";
			return true;
		}
		case Instruction::SIToFP:
		{
			const CastInst& ci=static_cast<const CastInst&>(I);
			//Check that the in and out types are sane
			Type* srcT = ci.getSrcTy();
			Type* dstT = ci.getDestTy();
			assert(isI32Type(srcT));
			assert(dstT->isDoubleTy());
			//It's a NOP, values are logically FP anyway in JS
			compileOperand(ci.getOperand(0));
			return true;
		}
		case Instruction::UIToFP:
		{
			const CastInst& ci=static_cast<const CastInst&>(I);
			//Check that the in and out types are sane
			Type* srcT = ci.getSrcTy();
			//Type* dstT = ci.getDestTy();
			assert(isI32Type(srcT));
			//assert(dstT->isDoubleTy());
			//We need to cast to unsigned before
			stream << "(";
			compileOperand(ci.getOperand(0));
			stream << " >>> 0)";
			return true;
		}
		case Instruction::GetElementPtr:
		{
			const GetElementPtrInst& gep=static_cast<const GetElementPtrInst&>(I);
			Type* t=gep.getOperand(0)->getType();
			assert(t->isPointerTy());
			PointerType* ptrT=static_cast<PointerType*>(t);
			if(isClientType(ptrT->getElementType()))
			{
				//Client objects are just passed through
				compileOperand(gep.getOperand(0));
			}
			else
			{
				const Value* val=gep.getOperand(0);
				GetElementPtrInst::const_op_iterator it=gep.idx_begin();
				//We compile as usual till the last level
				GetElementPtrInst::const_op_iterator itE=gep.idx_end()-1;
				compileGEP(val, it, itE);
			}
			return true;
		}
		case Instruction::Add:
		{
			//Integer addition
			assert(I.getNumOperands()==2);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(isI32Type(I.getOperand(1)->getType()));
			assert(isI32Type(I.getType()));
			stream << "((";
			compileOperand(I.getOperand(0));
			stream << " + ";
			compileOperand(I.getOperand(1));
			stream << ") >> 0)";
			return true;
		}
		case Instruction::FAdd:
		{
			//Double addition
			assert(I.getNumOperands()==2);
			//assert(I.getOperand(0)->getType()->isDoubleTy());
			//assert(I.getOperand(1)->getType()->isDoubleTy());
			//assert(I.getType()->isDoubleTy());
			stream << "(";
			compileOperand(I.getOperand(0));
			stream << " + ";
			compileOperand(I.getOperand(1));
			stream << ")";
			return true;
		}
		case Instruction::Sub:
		{
			//Integer subtraction
			//TODO: optimize negation
			assert(I.getNumOperands()==2);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(isI32Type(I.getOperand(1)->getType()));
			assert(isI32Type(I.getType()));
			stream << "((";
			compileOperand(I.getOperand(0));
			stream << " - ";
			compileOperand(I.getOperand(1));
			stream << ") >> 0)";
			return true;
		}
		case Instruction::FSub:
		{
			//Double subtraction
			//TODO: optimize negation
			assert(I.getNumOperands()==2);
			assert(I.getOperand(0)->getType()->isDoubleTy());
			assert(I.getOperand(1)->getType()->isDoubleTy());
			assert(I.getType()->isDoubleTy());
			stream << "(";
			compileOperand(I.getOperand(0));
			stream << " - ";
			compileOperand(I.getOperand(1));
			stream << ")";
			return true;
		}
		case Instruction::ZExt:
		{
			const BitCastInst& bi=static_cast<const BitCastInst&>(I);
			Type* src=bi.getSrcTy();
			Type* dst=bi.getDestTy();
			assert(src->isIntegerTy() && dst->isIntegerTy());
			IntegerType* srcI=static_cast<IntegerType*>(src);
			IntegerType* dstI=static_cast<IntegerType*>(dst);
			//TODO: put asserts back
			//assert(srcI->getBitWidth()<=32);
			//assert(dstI->getBitWidth()<=32);
			assert(srcI->getBitWidth()<=dstI->getBitWidth());
			//The operation is a NOP
			compileOperand(bi.getOperand(0));
			return true;
		}
		case Instruction::SDiv:
		{
			//Integer signed division
			assert(I.getNumOperands()==2);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(isI32Type(I.getOperand(1)->getType()));
			assert(isI32Type(I.getType()));
			stream << "(((";
			compileOperand(I.getOperand(0));
			stream << ">> 0) / (";
			compileOperand(I.getOperand(1));
			stream << ">> 0)) >> 0)";
			return true;
		}
		case Instruction::UDiv:
		{
			//Integer unsigned division
			assert(I.getNumOperands()==2);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(isI32Type(I.getOperand(1)->getType()));
			assert(isI32Type(I.getType()));
			stream << "(((";
			compileOperand(I.getOperand(0));
			stream << ">>> 0) / (";
			compileOperand(I.getOperand(1));
			stream << ">>> 0)) >>> 0)";
			return true;
		}
		case Instruction::SRem:
		{
			//Integer signed remainder
			assert(I.getNumOperands()==2);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(isI32Type(I.getOperand(1)->getType()));
			assert(isI32Type(I.getType()));
			stream << "(((";
			compileOperand(I.getOperand(0));
			stream << ">> 0) % (";
			compileOperand(I.getOperand(1));
			stream << ">> 0)) >> 0)";
			return true;
		}
		case Instruction::URem:
		{
			//Integer unsigned division
			assert(I.getNumOperands()==2);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(isI32Type(I.getOperand(1)->getType()));
			assert(isI32Type(I.getType()));
			stream << "(((";
			compileOperand(I.getOperand(0));
			stream << ">>> 0) % (";
			compileOperand(I.getOperand(1));
			stream << ">>> 0)) >>> 0)";
			return true;
		}
		case Instruction::FDiv:
		{
			//Double multiplication
			assert(I.getNumOperands()==2);
			//assert(I.getOperand(0)->getType()->isDoubleTy());
			//assert(I.getOperand(1)->getType()->isDoubleTy());
			//assert(I.getType()->isDoubleTy());
			stream << "(";
			compileOperand(I.getOperand(0));
			stream << " / ";
			compileOperand(I.getOperand(1));
			stream << ")";
			return true;
		}
		case Instruction::Mul:
		{
			//Integer signed multiplication
			assert(I.getNumOperands()==2);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(isI32Type(I.getOperand(1)->getType()));
			assert(isI32Type(I.getType()));
			stream << "((";
			compileOperand(I.getOperand(0));
			stream << " * ";
			compileOperand(I.getOperand(1));
			stream << ") >> 0)";
			return true;
		}
		case Instruction::FMul:
		{
			//Double multiplication
			assert(I.getNumOperands()==2);
			//assert(I.getOperand(0)->getType()->isDoubleTy());
			//assert(I.getOperand(1)->getType()->isDoubleTy());
			//assert(I.getType()->isDoubleTy());
			stream << "(";
			compileOperand(I.getOperand(0));
			stream << " * ";
			compileOperand(I.getOperand(1));
			stream << ")";
			return true;
		}
		case Instruction::ICmp:
		{
			//Integer comparison
			const CmpInst& ci=static_cast<const CmpInst&>(I);
			assert(ci.getNumOperands()==2);
			//Check that the operation is JS safe
			switch(ci.getPredicate())
			{
				case CmpInst::ICMP_EQ:
				case CmpInst::ICMP_NE:
					//Those are safe in any size
					break;
				default:
					break;
					/*assert(isI32Type(ci.getOperand(0)->getType()));
					assert(isI32Type(ci.getOperand(1)->getType()));*/
			}
			assert(ci.getOperand(0)->getType()==ci.getOperand(1)->getType());
			stream << "(";
			if(ci.getOperand(0)->getType()->isPointerTy())
			{
				//Comparison on pointers is only valid
				//for the same base!
				const Type* lastType1=compileObjectForPointer(ci.getOperand(0));
				stream << "===";
				const Type* lastType2=compileObjectForPointer(ci.getOperand(1));
				if(!isCompleteObject(ci.getOperand(0)) ||
					!isCompleteObject(ci.getOperand(1)))
				{
					stream << " && ";
					bool notFirst=compileOffsetForPointer(ci.getOperand(0),lastType1);
					if(!notFirst)
						stream << '0';
					compilePredicate(ci.getPredicate());
					notFirst=compileOffsetForPointer(ci.getOperand(1),lastType2);
					if(!notFirst)
						stream << '0';
				}
			}
			else
			{
				compileOperandForIntegerPredicate(ci.getOperand(0),ci.getPredicate());
				compilePredicate(ci.getPredicate());
				compileOperandForIntegerPredicate(ci.getOperand(1),ci.getPredicate());
			}
			stream << ")";
			return true;
		}
		case Instruction::FCmp:
		{
			//Integer comparison
			const CmpInst& ci=static_cast<const CmpInst&>(I);
			assert(ci.getNumOperands()==2);
			//Check that the operation is JS safe
			//TOTO: Verify that any float type is ok
			/*switch(ci.getPredicate())
			{
				default:
					assert(ci.getOperand(0)->getType()->isDoubleTy());
					assert(ci.getOperand(1)->getType()->isDoubleTy());
			}*/
			stream << "(";
			compileOperand(ci.getOperand(0));
			compilePredicate(ci.getPredicate());
			compileOperand(ci.getOperand(1));
			stream << ")";
			return true;
		}
		case Instruction::And:
		{
			//Integer logical and
			assert(I.getNumOperands()==2);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(isI32Type(I.getOperand(1)->getType()));
			assert(isI32Type(I.getType()));
			//No need to apply the >> operator. The result is an integer by spec
			stream << '(';
			compileOperand(I.getOperand(0));
			stream << " & ";
			compileOperand(I.getOperand(1));
			stream << ')';
			return true;
		}
		case Instruction::LShr:
		{
			//Integer logical shift right
			assert(I.getNumOperands()==2);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(isI32Type(I.getOperand(1)->getType()));
			assert(isI32Type(I.getType()));
			//No need to apply the >> operator. The result is an integer by spec
			stream << '(';
			compileOperand(I.getOperand(0));
			stream << " >>> ";
			compileOperand(I.getOperand(1));
			stream << ')';
			return true;
		}
		case Instruction::AShr:
		{
			//Integer logical shift right
			assert(I.getNumOperands()==2);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(isI32Type(I.getOperand(1)->getType()));
			assert(isI32Type(I.getType()));
			//No need to apply the >> operator. The result is an integer by spec
			stream << '(';
			compileOperand(I.getOperand(0));
			stream << " >> ";
			compileOperand(I.getOperand(1));
			stream << ')';
			return true;
		}
		case Instruction::Shl:
		{
			//Integer shift left
			assert(I.getNumOperands()==2);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(isI32Type(I.getOperand(1)->getType()));
			assert(isI32Type(I.getType()));
			//No need to apply the >> operator. The result is an integer by spec
			stream << '(';
			compileOperand(I.getOperand(0));
			stream << " << ";
			compileOperand(I.getOperand(1));
			stream << ')';
			return true;
		}
		case Instruction::Or:
		{
			//Integer logical or
			assert(I.getNumOperands()==2);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(isI32Type(I.getOperand(1)->getType()));
			assert(isI32Type(I.getType()));
			//No need to apply the >> operator. The result is an integer by spec
			stream << '(';
			compileOperand(I.getOperand(0));
			stream << " | ";
			compileOperand(I.getOperand(1));
			stream << ')';
			return true;
		}
		case Instruction::Xor:
		{
			//Integer logical or
			assert(I.getNumOperands()==2);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(isI32Type(I.getOperand(1)->getType()));
			assert(isI32Type(I.getType()));
			//No need to apply the >> operator. The result is an integer by spec
			stream << '(';
			compileOperand(I.getOperand(0));
			stream << " ^ ";
			compileOperand(I.getOperand(1));
			stream << ')';
			return true;
		}
		case Instruction::Trunc:
		{
			//Well, ideally this should not be used since, since it's a waste of bit to
			//use integers less than 32 bit wide. Still we can support it
			assert(I.getNumOperands()==1);
			assert(isI32Type(I.getOperand(0)->getType()));
			assert(I.getType()->isIntegerTy());
			uint32_t finalSize = I.getType()->getIntegerBitWidth();
			assert(finalSize < 32);
			uint32_t mask = (1 << finalSize) - 1;
			stream << '(';
			compileOperand(I.getOperand(0));
			stream << " & " << mask << ')';
			return true;
		}
		case Instruction::SExt:
		{
			//We can use a couple of shift to make this work
			assert(I.getNumOperands()==1);
			assert(I.getOperand(0)->getType()->isIntegerTy());
			uint32_t initialSize = I.getOperand(0)->getType()->getIntegerBitWidth();
			assert(initialSize < 32);
			assert(I.getType()->isIntegerTy());
			assert(I.getType()->getIntegerBitWidth()<=32);
			//We anyway have to use 32 bits for sign extension to work
			uint32_t shiftAmount = 32-initialSize;
			stream << "((";
			compileOperand(I.getOperand(0));
			stream << "<<" << shiftAmount << ")>>" << shiftAmount << ')';
			return true;
		}
		case Instruction::Select:
		{
			const SelectInst& si=static_cast<const SelectInst&>(I);
			stream << "(";
			compileOperand(si.getCondition());
			stream << "?";
			compileOperand(si.getTrueValue());
			stream << ":";
			compileOperand(si.getFalseValue());
			stream << ")";
			return true;
		}
		case Instruction::ExtractValue:
		{
			const ExtractValueInst& evi=static_cast<const ExtractValueInst&>(I);
			const Value* aggr=evi.getAggregateOperand();
			Type* t=aggr->getType();
			assert(t->isStructTy());
			assert(!UndefValue::classof(aggr));

			printVarName(aggr);

			uint32_t offset=evi.getIndices()[0];
			stream << ".a" << offset;
			return true;
		}
		default:
			stream << "alert('Unsupported code');\n";
			cerr << "\tImplement inst " << I.getOpcodeName() << endl;
			return false;
	}
}

bool JSWriter::isInlineable(const Instruction& I) const
{
	//Special case GEPs. They should always be inline since creating the object is really slow
	if(I.getOpcode()==Instruction::GetElementPtr)
		return true;
	//Beside a few cases, instructions with a single use may be inlined
	//TODO: Find out a better heuristic for inlining, it's seems that computing
	//may be faster even on more than 1 use
	if(I.hasOneUse())
	{
		//Inlining a variable used by a PHI it's unsafe
		//When the phi's are computed the result
		//correctness may depend on the order they are
		//computed. A single use is guaranteed
		if(PHINode::classof(I.use_back()))
			return false;
		//A few opcodes needs to be executed anyway as they
		//do not operated on registers
		switch(I.getOpcode())
		{
			case Instruction::Call:
			case Instruction::Invoke:
			case Instruction::Ret:
			case Instruction::LandingPad:
			case Instruction::PHI:
			case Instruction::Load:
			case Instruction::Store:
			case Instruction::InsertValue:
			case Instruction::Resume:
			case Instruction::Br:
			case Instruction::Alloca:
			case Instruction::Switch:
			case Instruction::Unreachable:
				return false;
			case Instruction::Add:
			case Instruction::Sub:
			case Instruction::Mul:
			case Instruction::And:
			case Instruction::Or:
			case Instruction::Xor:
			case Instruction::Trunc:
			case Instruction::FPToSI:
			case Instruction::SIToFP:
			case Instruction::SDiv:
			case Instruction::SRem:
			case Instruction::Shl:
			case Instruction::AShr:
			case Instruction::LShr:
			case Instruction::BitCast:
			case Instruction::FAdd:
			case Instruction::FDiv:
			case Instruction::FSub:
			case Instruction::FPTrunc:
			case Instruction::FPExt:
			case Instruction::FMul:
			case Instruction::FCmp:
			case Instruction::ICmp:
			case Instruction::ZExt:
			case Instruction::SExt:
			case Instruction::Select:
			case Instruction::ExtractValue:
			case Instruction::URem:
			case Instruction::UDiv:
			case Instruction::UIToFP:
			case Instruction::FPToUI:
				return true;
			default:
				cerr << "Is " << I.getOpcodeName() << " inlineable?" << endl;
				assert(false);
		}
	}
	return false;
}

void JSWriter::compileBB(BasicBlock& BB, const std::map<const BasicBlock*, uint32_t>& blocksMap)
{
	BasicBlock::iterator I=BB.begin();
	BasicBlock::iterator IE=BB.end();
	for(;I!=IE;++I)
	{
		if(isInlineable(*I))
			continue;
		if(I->getOpcode()==Instruction::PHI) //Phys are manually handled
			continue;
		if(I->getType()->getTypeID()!=Type::VoidTyID)
		{
			stream << "var ";
			printVarName(&(*I));
			stream << " = ";
		}
		if(I->isTerminator())
		{
			//TODO: Keep support for both relooper and swicth generation
			compileTerminatorInstruction(*dyn_cast<TerminatorInst>(I));
			//compileTerminatorInstruction(*dyn_cast<TerminatorInst>(I), blocksMap);
		}
		else
		{
			bool ret=compileNotInlineableInstruction(*I);
			stream << ";\n";
			if(ret==false)
			{
				//Stop basic block compilation
				return;
			}
		}
	}
	//At the end of the block
}

void JSWriter::DuettoRenderInterface::renderBlock(void* privateBlock)
{
	BasicBlock* bb=(BasicBlock*)privateBlock;
	std::map<const BasicBlock*, uint32_t> blocksMap;
	writer->compileBB(*bb, blocksMap);
}

void JSWriter::DuettoRenderInterface::renderIfBlockBegin(void* privateBlock, int branchId, bool first)
{
	BasicBlock* bb=(BasicBlock*)privateBlock;
	if(!first)
		writer->stream << "} else ";
	TerminatorInst* term=bb->getTerminator();
	writer->stream << "if (";
	if(BranchInst::classof(term))
	{
		const BranchInst* bi=cast<const BranchInst>(term);
		assert(bi->isConditional());
		//The second branch is the default
		assert(branchId==0);
		writer->compileOperand(bi->getCondition());
	}
	else if(SwitchInst::classof(term))
	{
		const SwitchInst* si=cast<const SwitchInst>(term);
		writer->compileOperand(si->getCondition());
		writer->stream << " === ";
		assert(branchId > 0);
		SwitchInst::ConstCaseIt it=si->case_begin();
		for(int i=1;i<branchId;i++)
			++it;
		writer->compileConstant(it.getCaseValue());
	}
	else
	{
		term->dump();
		assert(false);
	}
	writer->stream << ") {\n";
}

void JSWriter::DuettoRenderInterface::renderElseBlockBegin()
{
	writer->stream << "} else {\n";
}

void JSWriter::DuettoRenderInterface::renderBlockEnd()
{
	writer->stream << "}\n";
}

void JSWriter::DuettoRenderInterface::renderBlockPrologue(void* privateBlockTo,void* privateBlockFrom)
{
	BasicBlock* bbTo=(BasicBlock*)privateBlockTo;
	BasicBlock* bbFrom=(BasicBlock*)privateBlockFrom;
	writer->compilePHIOfBlockFromOtherBlock(bbTo, bbFrom);
}

void JSWriter::DuettoRenderInterface::renderWhileBlockBegin()
{
	writer->stream << "while(1) {\n";
}

void JSWriter::DuettoRenderInterface::renderWhileBlockBegin(int blockLabel)
{
	writer->stream << 'L' << blockLabel << ':';
	renderWhileBlockBegin();
}

void JSWriter::DuettoRenderInterface::renderDoBlockBegin()
{
	writer->stream << "do {\n";
}

void JSWriter::DuettoRenderInterface::renderDoBlockBegin(int blockLabel)
{
	writer->stream << 'L' << blockLabel << ':';
	renderDoBlockBegin();
}

void JSWriter::DuettoRenderInterface::renderDoBlockEnd()
{
	writer->stream << "} while(0);\n";
}

void JSWriter::DuettoRenderInterface::renderBreak()
{
	writer->stream << "break;\n";
}

void JSWriter::DuettoRenderInterface::renderBreak(int labelId)
{
	writer->stream << "break L" << labelId << ";\n";
}

void JSWriter::DuettoRenderInterface::renderContinue()
{
	writer->stream << "continue;\n";
}

void JSWriter::DuettoRenderInterface::renderContinue(int labelId)
{
	writer->stream << "continue L" << labelId << "\n";
}

void JSWriter::DuettoRenderInterface::renderLabel(int labelId)
{
	writer->stream << "label = " << labelId << ";\n";
}

void JSWriter::DuettoRenderInterface::renderIfOnLabel(int labelId, bool first)
{
	if(first==false)
		writer->stream << "else ";
	writer->stream << "if (label === " << labelId << ") {\n";
}

void JSWriter::compileMethod(Function& F)
{
	if(F.empty())
		return;
	std::cerr << (string)F.getName() << std::endl;
	stream << "function _" << F.getName().data() << "(";
	const Function::const_arg_iterator A=F.arg_begin();
	const Function::const_arg_iterator AE=F.arg_end();
	for(Function::const_arg_iterator curArg=A;curArg!=AE;++curArg)
	{
		if(curArg!=A)
			stream << ", ";
		if(curArg->hasName())
			printLLVMName(curArg->getName());
		else
			stream << "arg" << curArg->getArgNo();
	}
	stream << ") {\n";
	std::map<const BasicBlock*, uint32_t> blocksMap;
	if(F.size()==1)
		compileBB(*F.begin(), blocksMap);
	else
	{
		stream << "var label = 0;\n";
		//TODO: Support exceptions
		Function::iterator B=F.begin();
		Function::iterator BE=F.end();
		//First run, create the corresponding relooper blocks
		std::map<const BasicBlock*, /*relooper::*/Block*> relooperMap;
		for(;B!=BE;++B)
		{
			if(B->isLandingPad())
				continue;
			Block* rlBlock = new Block(&(*B));
			relooperMap.insert(make_pair(&(*B),rlBlock));
		}

		B=F.begin();
		BE=F.end();
		//Second run, add the branches
		for(;B!=BE;++B)
		{
			if(B->isLandingPad())
				continue;
			const TerminatorInst* term=B->getTerminator();
			uint32_t defaultBranchId=-1;
			//Find out which branch id is the default
			if(BranchInst::classof(term))
			{
				const BranchInst* bi=cast<const BranchInst>(term);
				if(bi->isUnconditional())
					defaultBranchId = 0;
				else
					defaultBranchId = 1;
			}
			else if(SwitchInst::classof(term))
			{
				const SwitchInst* si=cast<const SwitchInst>(term);
				assert(si->getDefaultDest()==si->getSuccessor(0));
				defaultBranchId = 0;
			}
			else if(InvokeInst::classof(term))
			{
				const InvokeInst* ii=cast<const InvokeInst>(term);
				assert(ii->getNormalDest()==ii->getSuccessor(0));
				defaultBranchId = 0;
			}
			else if(term->getNumSuccessors())
			{
				//Only a problem if there are successors
				term->dump();
				assert(false);
			}

			for(uint32_t i=0;i<term->getNumSuccessors();i++)
			{
				if(term->getSuccessor(i)->isLandingPad())
					continue;
				Block* target=relooperMap[term->getSuccessor(i)];
				//Use -1 for the default target
				relooperMap[&(*B)]->AddBranchTo(target, (i==defaultBranchId)?-1:i);
			}
		}

		B=F.begin();
		BE=F.end();
		//Third run, add the block to the relooper and run it
		Relooper* rl=new Relooper();
		for(;B!=BE;++B)
		{
			if(B->isLandingPad())
				continue;
			rl->AddBlock(relooperMap[&(*B)]);
		}
		rl->Calculate(relooperMap[&F.getEntryBlock()]);
		RenderInterface* ri=new DuettoRenderInterface(this);
		rl->Render(ri);
/*		//Build a map from basicblocks to ids
		uint32_t blockId=0;
		for(;B!=BE;++B)
		{
			blocksMap.insert(make_pair(&(*B), blockId));
			blockId++;
		}
		//Rest the iterator
		B=F.begin();

		//Create an emscripten style switch for now
		stream << "var __block=0;\nwhile(true){\nswitch(__block){\n";
		for(;B!=BE;++B)
		{
			stream << "case " << blocksMap.find(&(*B))->second << ":\n";
			compileBB(*B, blocksMap);
			stream << "break;\n";
		}
		stream << "}}\n";*/
	}

	stream << "}\n";
}

void JSWriter::compileGlobal(GlobalVariable& G)
{
	assert(G.hasName());
	if(isClientGlobal(G.getName().data()))
	{
		//Global objects in the client namespace are only
		//placeholders for JS calls
		return;
	}
	stream  << "var ";
	printLLVMName(G.getName());
	if(G.hasInitializer())
	{
		stream << " = ";
		compileConstant(G.getInitializer());
	}
	stream << ";\n";
}

void JSWriter::makeJS()
{
	//Header: output a guard variable to to set the environment
	stream << "var __duetto_compiler = true;\n";
	//Output all the globals
	Module::global_iterator G=module->global_begin();
	Module::global_iterator GE=module->global_end();
	for(; G != GE; ++G)
	{
		compileGlobal(*G);
	}
	Module::iterator F=module->begin();
	Module::iterator FE=module->end();
	for (; F != FE; ++F)
	{
		DuettoUtils::rewriteNativeObjectsConstructors(*module, *F);
		compileMethod(*F);
	}
	//Invoke the webMain function
	stream << "__Z7webMainv();\n";
}

// main - Entry point for the duetto double target compiler.
//
int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  // Enable debug stream buffering.
  EnableDebugBuffering = true;

  LLVMContext &Context = getGlobalContext();
  llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

  // Initialize targets first, so that --version shows registered targets.
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  // Register the target printer for --version.
  cl::AddExtraVersionPrinter(TargetRegistry::printRegisteredTargetsForVersion);

  cl::ParseCommandLineOptions(argc, argv, "llvm system compiler\n");

  // Load the module to be compiled...
  SMDiagnostic Err;
  std::auto_ptr<Module> M;

  M.reset(ParseIRFile(InputFilename, Err, Context));
  if (M.get() == 0) {
    Err.print(argv[0], errs());
    return 1;
  }
  Module* clientMod = M.get();

  std::string JSOutputFilename = GetFileNameRoot(InputFilename) + ".js";

  std::string errorInfo;
  raw_fd_ostream JSOut(JSOutputFilename.c_str(),errorInfo,sys::fs::F_None);
  if(!errorInfo.empty())
  {
	std::cerr << errorInfo << std::endl;
	return -1;
  }

  JSWriter writer(clientMod, JSOut);
  writer.makeJS();

  JSOut.close();

  return 0;
}
