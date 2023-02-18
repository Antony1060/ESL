#include "compiler.h"
#include "../ErrorHandling/errorHandler.h"
#include <unordered_set>
#include <iostream>
#include "../Includes/fmt/format.h"
#include "../codegen/valueHelpersInline.cpp"

using namespace compileCore;
using namespace object;
using namespace valueHelpers;

#ifdef COMPILER_USE_LONG_INSTRUCTION
#define SHORT_CONSTANT_LIMIT 0
#else
#define SHORT_CONSTANT_LIMIT UINT8_MAX
#endif

//only checks the closest loop/switch, since any break, continue or advance is going to break out of that loop/switch
#define CHECK_SCOPE_FOR_LOOP (current->scopeWithLoop.size() > 0 && local.depth <= current->scopeWithLoop.back())
#define CHECK_SCOPE_FOR_SWITCH (current->scopeWithSwitch.size() > 0 && local.depth <= current->scopeWithSwitch.back())

CurrentChunkInfo::CurrentChunkInfo(CurrentChunkInfo* _enclosing, FuncType _type) : enclosing(_enclosing), type(_type) {
	upvalues = std::array<Upvalue, UPVAL_MAX>();
	hasReturnStmt = false;
	hasCapturedLocals = false;
	localCount = 0;
	scopeDepth = 0;
	line = 0;
	//first slot is claimed for function name
	Local* local = &locals[localCount++];
	local->depth = 0;
	if (type == FuncType::TYPE_CONSTRUCTOR || type == FuncType::TYPE_METHOD) {
		local->name = "this";
	}
	else {
		local->name = "";
	}
	chunk = Chunk();
	func = new ObjFunc();
}

Compiler::Compiler(vector<CSLModule*>& _units) {
	current = new CurrentChunkInfo(nullptr, FuncType::TYPE_SCRIPT);
	currentClass = nullptr;
	curUnitIndex = 0;
	curGlobalIndex = 0;
	units = _units;
    nativeFuncs = runtime::createNativeFuncs();
    nativeFuncNames = runtime::createNativeNameTable(nativeFuncs);

	for (CSLModule* unit : units) {
		curUnit = unit;
		sourceFiles.push_back(unit->file);
		for (const auto decl : unit->topDeclarations) {
			globals.emplace_back(decl->getName().getLexeme(), encodeNil());
            definedGlobals.push_back(false);
		}
		for (int i = 0; i < unit->stmts.size(); i++) {
			//doing this here so that even if a error is detected, we go on and possibly catch other(valid) errors
			try {
				unit->stmts[i]->accept(this);
			}
			catch (CompilerException e) {
                // Do nothing, only used for unwinding the stack
			}
		}
		curGlobalIndex = globals.size();
		curUnitIndex++;
	}
    mainBlockFunc = endFuncDecl();
    mainBlockFunc->name = "script";
	memory::gc.collect(this);
	for (CSLModule* unit : units) delete unit;
}

void Compiler::visitAssignmentExpr(AST::AssignmentExpr* expr) {
	//rhs of the expression is on the top of the stack and stays there, since assignment is an expression
	expr->value->accept(this);
	namedVar(expr->name, true);
}

void Compiler::visitSetExpr(AST::SetExpr* expr) {
	//different behaviour for '[' and '.'
	updateLine(expr->accessor);

	switch (expr->accessor.type) {
	case TokenType::LEFT_BRACKET: {
		//allows for things like object["field" + "name"]
		expr->value->accept(this);
		expr->callee->accept(this);
		expr->field->accept(this);
		emitByte(+OpCode::SET);
		break;
	}
	case TokenType::DOT: {
		//the "." is always followed by a field name as a string, emitting a constant speeds things up and avoids unnecessary stack manipulation
		expr->value->accept(this);
		expr->callee->accept(this);
		uint16_t name = identifierConstant(dynamic_cast<AST::LiteralExpr*>(expr->field.get())->token);
		if (name <= SHORT_CONSTANT_LIMIT) emitBytes(+OpCode::SET_PROPERTY, name);
		else emitByteAnd16Bit(+OpCode::SET_PROPERTY_LONG, name);
		break;
	}
	}
}

void Compiler::visitConditionalExpr(AST::ConditionalExpr* expr) {
	//compile condition and emit a jump over then branch if the condition is false
	expr->condition->accept(this);
	int thenJump = emitJump(+OpCode::JUMP_IF_FALSE_POP);
	expr->thenBranch->accept(this);
	//prevents fallthrough to else branch
	int elseJump = emitJump(+OpCode::JUMP);
	patchJump(thenJump);
	//only emit code if else branch exists, since its optional
	if (expr->elseBranch) expr->elseBranch->accept(this);
	patchJump(elseJump);
}

void Compiler::visitBinaryExpr(AST::BinaryExpr* expr) {
	updateLine(expr->op);

	expr->left->accept(this);
	if (expr->op.type == TokenType::OR) {
		//if the left side is true, we know that the whole expression will eval to true
		int jump = emitJump(+OpCode::JUMP_IF_TRUE);
		//pop the left side and eval the right side, right side result becomes the result of the whole expression
		emitByte(+OpCode::POP);
		expr->right->accept(this);
		patchJump(jump);//left side of the expression becomes the result of the whole expression
		return;
	}
	else if (expr->op.type == TokenType::AND) {
		//at this point we have the left side of the expression on the stack, and if it's false we skip to the end
		//since we know the whole expression will evaluate to false
		int jump = emitJump(+OpCode::JUMP_IF_FALSE);
		//if the left side is true, we pop it and then push the right side to the stack, and the result of right side becomes the result of
		//whole expression
		emitByte(+OpCode::POP);
		expr->right->accept(this);
		patchJump(jump);
		return;
	}

	uint8_t op = 0;
	switch (expr->op.type) {
		//take in double or string(in case of add)
        case TokenType::PLUS:	op = +OpCode::ADD; break;
        case TokenType::MINUS:	op = +OpCode::SUBTRACT; break;
        case TokenType::SLASH:	op = +OpCode::DIVIDE; break;
        case TokenType::STAR:	op = +OpCode::MULTIPLY; break;
		//for these operators, a check is preformed to confirm both numbers are integers, not decimals
        case TokenType::PERCENTAGE:		op = +OpCode::MOD; break;
        case TokenType::BITSHIFT_LEFT:	op = +OpCode::BITSHIFT_LEFT; break;
        case TokenType::BITSHIFT_RIGHT:	op = +OpCode::BITSHIFT_RIGHT; break;
        case TokenType::BITWISE_AND:	op = +OpCode::BITWISE_AND; break;
        case TokenType::BITWISE_OR:		op = +OpCode::BITWISE_OR; break;
        case TokenType::BITWISE_XOR:	op = +OpCode::BITWISE_XOR; break;
		//these return bools and use an epsilon value when comparing
        case TokenType::EQUAL_EQUAL:	 op = +OpCode::EQUAL; break;
        case TokenType::BANG_EQUAL:		 op = +OpCode::NOT_EQUAL; break;
        case TokenType::GREATER:		 op = +OpCode::GREATER; break;
        case TokenType::GREATER_EQUAL:	 op = +OpCode::GREATER_EQUAL; break;
        case TokenType::LESS:			 op = +OpCode::LESS; break;
        case TokenType::LESS_EQUAL:		 op = +OpCode::LESS_EQUAL; break;
	}
	expr->right->accept(this);
	emitByte(op);
}

void Compiler::visitUnaryExpr(AST::UnaryExpr* expr) {
	updateLine(expr->op);
	//incrementing and decrementing a variable or an object field is optimized using INCREMENT opcode
	//the value from a variable is fetched, incremented/decremented and put into back into the variable in a single dispatch iteration
	if (expr->op.type == TokenType::INCREMENT || expr->op.type == TokenType::DECREMENT) {
		int arg = -1;
		//type definition and arg size
		//0: local(8bit index), 1: upvalue(8bit index), 2: global(8bit constant), 3: global(16bit constant)
		//4: dot access(8bit constant), 5: dot access(16bit constant), 6: field access(none, field is compiled to stack)
		byte type = 0;
		if (expr->right->type == AST::ASTType::LITERAL) {
			//if a variable is being incremented, first get what kind of variable it is(local, upvalue or global)
			//also get argument(local: stack position, upvalue: upval position in func, global: name constant index)
			AST::LiteralExpr* left = dynamic_cast<AST::LiteralExpr*>(expr->right.get());

			updateLine(left->token);
			arg = resolveLocal(left->token);
			if (arg != -1) type = 0;
			else if ((arg = resolveUpvalue(current, left->token)) != -1) type = 1;
			else if((arg = resolveGlobal(left->token, true)) != -1){
                if(arg == -2) error(left->token, fmt::format("Trying to access variable '{}' before it's initialized.", left->token.getLexeme()));
				if(!definedGlobals[arg]) error(left->token, fmt::format("Use of undefined variable '{}'.", left->token.getLexeme()));
				type = arg > SHORT_CONSTANT_LIMIT ? 3 : 2;
			}else error(left->token, fmt::format("Variable '{}' isn't declared.", left->token.getLexeme()));
		}
		else if (expr->right->type == AST::ASTType::FIELD_ACCESS) {
			//if a field is being incremented, compile the object, and then if it's not a dot access also compile the field
			AST::FieldAccessExpr* left = dynamic_cast<AST::FieldAccessExpr*>(expr->right.get());
			updateLine(left->accessor);
			left->callee->accept(this);

			if (left->accessor.type == TokenType::DOT) {
				arg = identifierConstant(dynamic_cast<AST::LiteralExpr*>(left->field.get())->token);
				type = arg > SHORT_CONSTANT_LIMIT ? 5 : 4;
			}
			else {
				left->field->accept(this);
				type = 6;
			}
		}
		else error(expr->op, "Left side is not incrementable.");

		//0b00000001: increment or decrement
		//0b00000010: prefix or postfix increment/decrement
		//0b00011100: type
		byte args = 0;
		args = (expr->op.type == TokenType::INCREMENT ? 1 : 0) |
			((expr->isPrefix ? 1 : 0) << 1) |
			(type << 2);
		emitBytes(+OpCode::INCREMENT, args);

		if (arg != -1) arg > SHORT_CONSTANT_LIMIT ? emit16Bit(arg) : emitByte(arg);

		return;
	}
	//regular unary operations
	if (expr->isPrefix) {
		expr->right->accept(this);
		switch (expr->op.type) {
		case TokenType::MINUS: emitByte(+OpCode::NEGATE); break;
		case TokenType::BANG: emitByte(+OpCode::NOT); break;
		case TokenType::TILDA: emitByte(+OpCode::BIN_NOT); break;
		}
	}
}

void Compiler::visitArrayLiteralExpr(AST::ArrayLiteralExpr* expr) {
	//we need all of the array member values to be on the stack prior to executing "OP_CREATE_ARRAY"
	//compiling members in reverse order because we add to the array by popping from the stack
    for(auto mem : expr->members){
        mem->accept(this);
    }
	emitBytes(+OpCode::CREATE_ARRAY, expr->members.size());
}

void Compiler::visitCallExpr(AST::CallExpr* expr) {
	//invoking is field access + call, when the compiler recognizes this pattern it optimizes
	if (invoke(expr)) return;
	//todo: tail recursion optimization
	expr->callee->accept(this);
	for (AST::ASTNodePtr arg : expr->args) {
		arg->accept(this);
	}
	emitBytes(+OpCode::CALL, expr->args.size());

}

void Compiler::visitFieldAccessExpr(AST::FieldAccessExpr* expr) {
	updateLine(expr->accessor);

	expr->callee->accept(this);

	switch (expr->accessor.type) {
	//array[index] or object["propertyAsString"]
	case TokenType::LEFT_BRACKET: {
		expr->field->accept(this);
		emitByte(+OpCode::GET);
		break;
	}
	//object.property, we can optimize since we know the string in advance
	case TokenType::DOT:
		uint16_t name = identifierConstant(dynamic_cast<AST::LiteralExpr*>(expr->field.get())->token);
		if (name <= SHORT_CONSTANT_LIMIT) emitBytes(+OpCode::GET_PROPERTY, name);
		else emitByteAnd16Bit(+OpCode::GET_PROPERTY_LONG, name);
		break;
	}
}

void Compiler::visitStructLiteralExpr(AST::StructLiteral* expr) {
	vector<int> constants;

	bool isLong = false;
	//for each field, compile it and get the constant of the field name
	for (AST::StructEntry entry : expr->fields) {
		entry.expr->accept(this);
		updateLine(entry.name);
		uint16_t num = identifierConstant(entry.name);
		if (num > SHORT_CONSTANT_LIMIT) isLong = true;
		constants.push_back(num);
	}
	//since the amount of fields is variable, we emit the number of fields follwed by constants for each field
	//constants are emitted in reverse order, because we get the values by popping them from stack(reverse order from which they were pushed)
	if (!isLong) {
		emitBytes(+OpCode::CREATE_STRUCT, constants.size());

		for (int i = constants.size() - 1; i >= 0; i--) emitByte(constants[i]);
	}
	else {
		emitBytes(+OpCode::CREATE_STRUCT_LONG, constants.size());

		for (int i = constants.size() - 1; i >= 0; i--) emit16Bit(constants[i]);
	}
}

void Compiler::visitSuperExpr(AST::SuperExpr* expr) {
	int name = identifierConstant(expr->methodName);
	if (currentClass == nullptr) {
		error(expr->methodName, "Can't use 'super' outside of a class.");
	}
	else if (!currentClass->superclass) {
		error(expr->methodName, "Can't use 'super' in a class with no superclass.");
	}
	// We use a synthetic token since 'this' is defined if we're currently compiling a class method
	namedVar(syntheticToken("this"), false);
    emitConstant(Value(currentClass->superclass));
	if (name <= SHORT_CONSTANT_LIMIT) emitBytes(+OpCode::GET_SUPER, name);
	else emitByteAnd16Bit(+OpCode::GET_SUPER_LONG, name);
}

void Compiler::visitLiteralExpr(AST::LiteralExpr* expr) {
	updateLine(expr->token);

	switch (expr->token.type) {
	case TokenType::NUMBER: {
        string num = expr->token.getLexeme();
        double val = std::stod(num);
        if (isInt(val) && val <= SHORT_CONSTANT_LIMIT) { emitBytes(+OpCode::LOAD_INT, val); }
        else { emitConstant(encodeNumber(val)); }
		break;
	}
	case TokenType::TRUE: emitByte(+OpCode::TRUE); break;
	case TokenType::FALSE: emitByte(+OpCode::FALSE); break;
	case TokenType::NIL: emitByte(+OpCode::NIL); break;
	case TokenType::STRING: {
		//this gets rid of quotes, ""Hello world""->"Hello world"
		string temp = expr->token.getLexeme();
		temp.erase(0, 1);
		temp.erase(temp.size() - 1, 1);
		emitConstant(encodeObj(new ObjString(temp)));
		break;
	}

	case TokenType::THIS: {
		if (currentClass == nullptr) {
			error(expr->token, "Can't use keyword 'this' outside of a class.");
			break;
		}
		//'this' get implicitly defined by the compiler
		namedVar(expr->token, false);
		break;
	}
	case TokenType::IDENTIFIER: {
		namedVar(expr->token, false);
		break;
	}
	}
}

void Compiler::visitFuncLiteral(AST::FuncLiteral* expr) {
	//creating a new compilerInfo sets us up with a clean slate for writing bytecode, the enclosing functions info
	//is stored in parserCurrent->enclosing
	current = new CurrentChunkInfo(current, FuncType::TYPE_FUNC);
	//no need for a endScope, since returning from the function discards the entire callstack
	beginScope();
	//we define the args as locals, when the function is called, the args will be sitting on the stack in order
	//we just assign those positions to each arg
	for (Token& var : expr->args) {
		updateLine(var);
		uint8_t constant = parseVar(var);
		defineVar(constant);
	}
    for(auto stmt : expr->body->statements){
        stmt->accept(this);
    }
	current->func->arity = expr->args.size();
	current->func->name = "Anonymous function";
	//have to do this here since endFuncDecl() deletes the compilerInfo
	std::array<Upvalue, UPVAL_MAX> upvals = current->upvalues;
	ObjFunc* func = endFuncDecl();

	//if there are no upvalues captured, compile the function enclosed in a closure and we're done
	if (func->upvalueCount == 0) {
		emitConstant(encodeObj(new ObjClosure(func)));
		return;
	}

	uint16_t constant = makeConstant(encodeObj(func));
	if (constant <= SHORT_CONSTANT_LIMIT) emitBytes(+OpCode::CLOSURE, constant);
	else emitByteAnd16Bit(+OpCode::CLOSURE_LONG, constant);
	//if this function does capture any upvalues, we emit the code for getting them,
	//when we execute "OP_CLOSURE" we will check to see how many upvalues the function captures by going directly to the func->upvalueCount
	for (int i = 0; i < func->upvalueCount; i++) {
		emitByte(upvals[i].isLocal ? 1 : 0);
		emitByte(upvals[i].index);
	}
}

void Compiler::visitModuleAccessExpr(AST::ModuleAccessExpr* expr) {
	uint16_t arg = resolveModuleVariable(expr->moduleName, expr->ident);

	if (arg > SHORT_CONSTANT_LIMIT) {
		emitByteAnd16Bit(+OpCode::GET_GLOBAL_LONG, arg);
		return;
	}
	emitBytes(+OpCode::GET_GLOBAL, arg);
}

// This shouldn't ever be visited as every macro should be expanded before compilation
void Compiler::visitMacroExpr(AST::MacroExpr* expr) {
    error("Non-expanded macro encountered during compilation.");
}

void Compiler::visitAsyncExpr(AST::AsyncExpr* expr) {
	updateLine(expr->token);
	expr->callee->accept(this);
	for (AST::ASTNodePtr arg : expr->args) {
		arg->accept(this);
	}
	emitBytes(+OpCode::LAUNCH_ASYNC, expr->args.size());
}

void Compiler::visitAwaitExpr(AST::AwaitExpr* expr) {
	updateLine(expr->token);
	expr->expr->accept(this);
	emitByte(+OpCode::AWAIT);
}

void Compiler::visitVarDecl(AST::VarDecl* decl) {
	// If this is a global, we get a index into global array, if it's a local, it returns a dummy 0
	uint16_t global = parseVar(decl->name);
	// Compile the right side of the declaration, if there is no right side, the variable is initialized as nil
	AST::ASTNodePtr expr = decl->value;
	if (expr == nullptr) {
		emitByte(+OpCode::NIL);
	}
	else {
		expr->accept(this);
	}
	// If it's a local, do nothing the slot that the compiled value is at becomes a local var
	defineVar(global);
    // Put the value into the global array if we're in global scope
    if(current->scopeDepth > 0) return;
    if(global <= SHORT_CONSTANT_LIMIT) emitBytes(+OpCode::SET_GLOBAL, global);
    else emitByteAnd16Bit(+OpCode::SET_GLOBAL_LONG, global);
    emitByte(+OpCode::POP);
}

void Compiler::visitFuncDecl(AST::FuncDecl* decl) {
	uint16_t index = parseVar(decl->getName());
    // Defining the function here to allow for recursion
    defineVar(index);
	//creating a new compilerInfo sets us up with a clean slate for writing bytecode, the enclosing functions info
	//is stored in parserCurrent->enclosing
	current = new CurrentChunkInfo(current, FuncType::TYPE_FUNC);
	//no need for a endScope, since returning from the function discards the entire callstack
	beginScope();
	//we define the args as locals, when the function is called, the args will be sitting on the stack in order
	//we just assign those positions to each arg
	for (Token& var : decl->args) {
		updateLine(var);
		uint8_t constant = parseVar(var);
		defineVar(constant);
	}
    for(auto stmt : decl->body->statements){
        stmt->accept(this);
    }
	current->func->arity = decl->args.size();
	current->func->name = decl->getName().getLexeme();
	//have to do this here since endFuncDecl() deletes the compilerInfo
	std::array<Upvalue, UPVAL_MAX> upvals = current->upvalues;

	ObjFunc* func = endFuncDecl();

    //Not possible but just in case
    if(func->upvalueCount != 0){
        error(decl->getName(), "Global function with upvalues detected, aborting...");
    }
    // Assigning at compile time to save on bytecode
    globals[index].val = encodeObj(new ObjClosure(func));
}

void Compiler::visitClassDecl(AST::ClassDecl* decl) {
	Token className = decl->getName();
	uInt16 index = parseVar(className);
    auto klass = new object::ObjClass(className.getLexeme());

	ClassChunkInfo temp(currentClass, nullptr);
	currentClass = &temp;

	if (decl->inherits) {
		//if the class inherits from some other class, load the parent class and declare 'super' as a local variable which holds the superclass
		//decl->inheritedClass is always either a LiteralExpr with an identifier token or a ModuleAccessExpr

		//if a class wants to inherit from a class in another file of the same name, the import has to use an alias, otherwise we get
		//undefined behavior (eg. class a : a)
        uint16_t superclassIndex = 0;
        Token token;
		if (decl->inheritedClass->type == AST::ASTType::LITERAL) {
            // TODO: dynamic cast :cry:
			AST::LiteralExpr* expr = dynamic_cast<AST::LiteralExpr*>(decl->inheritedClass.get());
            // Gets the index into globals in which the superclass is stored
            int arg = resolveGlobal(expr->token, false);
            if(arg == -1) error(expr->token, "Variable isn't defined.");
            else if(arg == -2) error(expr->token, fmt::format("Trying to access variable '{}' before it's initialized.", expr->token.getLexeme()));
            superclassIndex = arg;
            token = expr->token;
		}
        else if(decl->inheritedClass->type == AST::ASTType::MODULE_ACCESS){
            // TODO: And another one
            AST::ModuleAccessExpr* expr = dynamic_cast<AST::ModuleAccessExpr*>(decl->inheritedClass.get());
            superclassIndex = resolveModuleVariable(expr->moduleName, expr->ident);
            token = expr->ident;
        }

        if(!isClass(globals[superclassIndex].val)){
            error(token,"Variable isn't a class(perhaps you tried inheriting from class stored in a global variable, which is illegal, please use the class name).");
        }
        currentClass->superclass = asClass(globals[superclassIndex].val);
	}
    // Define the class here, so that it can be called inside its own methods
    // Defining after inheriting so that a class can't be used as its own parent
    defineVar(index);

	for (AST::ASTNodePtr _method : decl->methods) {
        // TODO: And another another one
        auto m = dynamic_cast<AST::FuncDecl*>(_method.get());
		klass->methods.insert_or_assign(m->getName().getLexeme(), encodeObj(method(m, className)));
	}
	currentClass = nullptr;
    // Assigning at compile time to save on bytecode
    globals[index].val = encodeObj(klass);
}

void Compiler::visitExprStmt(AST::ExprStmt* stmt) {
	stmt->expr->accept(this);
	emitByte(+OpCode::POP);
}

void Compiler::visitBlockStmt(AST::BlockStmt* stmt) {
	beginScope();
	for (AST::ASTNodePtr node : stmt->statements) {
		node->accept(this);
	}
	endScope();
}

void Compiler::visitIfStmt(AST::IfStmt* stmt) {
	//compile condition and emit a jump over then branch if the condition is false
	stmt->condition->accept(this);
	int thenJump = emitJump(+OpCode::JUMP_IF_FALSE_POP);
	stmt->thenBranch->accept(this);
	//only compile if there is a else branch
	if (stmt->elseBranch != nullptr) {
		//prevents fallthrough to else branch
		int elseJump = emitJump(+OpCode::JUMP);
		patchJump(thenJump);

		stmt->elseBranch->accept(this);
		patchJump(elseJump);
	}
	else patchJump(thenJump);

}

void Compiler::visitWhileStmt(AST::WhileStmt* stmt) {
	//loop inversion is applied to reduce the number of jumps if the condition is met
	stmt->condition->accept(this);
	int jump = emitJump(+OpCode::JUMP_IF_FALSE_POP);
	//if the condition is true, compile the body and then the condition again and if its true we loop back to the start of the body
	int loopStart = getChunk()->bytecode.size();
	//loop body gets it's own scope because we only patch break and continue jumps which are declared in higher scope depths
	//user might not use {} block when writing a loop, this ensures the body is always in it's own scope
	current->scopeWithLoop.push_back(current->scopeDepth);
	beginScope();
	stmt->body->accept(this);
	endScope();
	current->scopeWithLoop.pop_back();
	//continue skips the rest of the body and evals the condition again
	patchScopeJumps(ScopeJumpType::CONTINUE);
	stmt->condition->accept(this);
	emitLoop(loopStart);
	//break out of the loop
	patchJump(jump);
	patchScopeJumps(ScopeJumpType::BREAK);
}

void Compiler::visitForStmt(AST::ForStmt* stmt) {
	//we wrap this in a scope so if there is a var declaration in the initialization it's scoped to the loop
	beginScope();
	if (stmt->init != nullptr) stmt->init->accept(this);
	//if check to see if the condition is true the first time
	int exitJump = -1;
	if (stmt->condition != nullptr) {
		stmt->condition->accept(this);
		exitJump = emitJump(+OpCode::JUMP_IF_FALSE_POP);
	}

	int loopStart = getChunk()->bytecode.size();
	//loop body gets it's own scope because we only patch break and continue jumps which are declared in higher scope depths
	//user might not use {} block when writing a loop, this ensures the body is always in it's own scope
	current->scopeWithLoop.push_back(current->scopeDepth);
	beginScope();
	stmt->body->accept(this);
	endScope();
	current->scopeWithLoop.pop_back();
	//patching continue here to increment if a variable for incrementing has been defined
	patchScopeJumps(ScopeJumpType::CONTINUE);
	//if there is a increment expression, we compile it and emit a POP to get rid of the result
	if (stmt->increment != nullptr) {
		stmt->increment->accept(this);
		emitByte(+OpCode::POP);
	}
	//if there is a condition, compile it again and if true, loop to the start of the body
	if (stmt->condition != nullptr) {
		stmt->condition->accept(this);
		emitLoop(loopStart);
	}
	else {
		//if there isn't a condition, it's implictly defined as 'true'
		emitByte(+OpCode::LOOP);

		int offset = getChunk()->bytecode.size() - loopStart + 2;
		if (offset > UINT16_MAX) error("Loop body too large.");

		emit16Bit(offset);
	}
	if (exitJump != -1) patchJump(exitJump);
	patchScopeJumps(ScopeJumpType::BREAK);
	endScope();
}

void Compiler::visitBreakStmt(AST::BreakStmt* stmt) {
	//the amount of variables to pop and the amount of code to jump is determined in patchScopeJumps()
	//which is called at the end of loops or a switch
	updateLine(stmt->token);
	int toPop = 0;
	//since the body of the loop is always in its own scope, and the scope before it is declared as having a loop,
	//we pop locals until the first local that is in the same scope as the loop
	//meaning it was declared outside of the loop body and shouldn't be popped
	//break is a special case because it's used in both loops and switches, so we find either in the scope we're check, we bail
	for (int i = current->localCount - 1; i >= 0; i--) {
		Local& local = current->locals[i];
		if (local.depth != -1 && (CHECK_SCOPE_FOR_LOOP || CHECK_SCOPE_FOR_SWITCH)) break;
		toPop++;
	}
	if (toPop > UINT8_MAX) {
		error(stmt->token, "To many variables to pop.");
	}
	emitByte(+ScopeJumpType::BREAK);
	int breakJump = getChunk()->bytecode.size();
	emitBytes((current->scopeDepth >> 8) & 0xff, current->scopeDepth & 0xff);
	emitByte(toPop);
	current->scopeJumps.push_back(breakJump);
}

void Compiler::visitContinueStmt(AST::ContinueStmt* stmt) {
	//the amount of variables to pop and the amount of code to jump is determined in patchScopeJumps()
	//which is called at the end of loops
	updateLine(stmt->token);
	int toPop = 0;
	//since the body of the loop is always in its own scope, and the scope before it is declared as having a loop,
	//we pop locals until the first local that is in the same scope as the loop
	//meaning it was declared outside of the loop body and shouldn't be popped
	for (int i = current->localCount - 1; i >= 0; i--) {
		Local& local = current->locals[i];
		if (local.depth != -1 && CHECK_SCOPE_FOR_LOOP) break;
		toPop++;
	}
	if (toPop > UINT8_MAX) {
		error(stmt->token, "To many variables to pop.");
	}
	emitByte(+ScopeJumpType::CONTINUE);
	int continueJump = getChunk()->bytecode.size();
	emitBytes((current->scopeDepth >> 8) & 0xff, current->scopeDepth & 0xff);
	emitByte(toPop);
	current->scopeJumps.push_back(continueJump);
}

void Compiler::visitSwitchStmt(AST::SwitchStmt* stmt) {
	current->scopeWithSwitch.push_back(current->scopeDepth);
	//compile the expression in parentheses
	stmt->expr->accept(this);
	vector<uint16_t> constants;
	vector<uint16_t> jumps;
	bool isLong = false;
	for (const auto & _case : stmt->cases) {
		//a single case can contain multiple constants(eg. case 1 | 4 | 9:), each constant is compiled and its jump will point to the
		//same case code block
		for (const Token& constant : _case->constants) {
			Value val;
			updateLine(constant);
			//create constant and add it to the constants array
			try {
				switch (constant.type) {
				case TokenType::NUMBER: {
					double num = std::stod(constant.getLexeme());//doing this becuase stod doesn't accept string_view
					val = encodeNumber(num);
					break;
				}
				case TokenType::TRUE: val = encodeBool(true); break;
				case TokenType::FALSE: val = encodeBool(false); break;
				case TokenType::NIL: val = encodeNil(); break;
				case TokenType::STRING: {
					//this gets rid of quotes, "Hello world"->Hello world
					string temp = constant.getLexeme();
					temp.erase(0, 1);
					temp.erase(temp.size() - 1, 1);
					val = encodeObj(new ObjString(temp));
					break;
				}
				default: {
					error(constant, "Case expression can only be a constant.");
				}
				}
				constants.push_back(makeConstant(val));
				if (constants.back() > SHORT_CONSTANT_LIMIT) isLong = true;
			}
			catch (CompilerException e) {}
		}
	}
	//the arguments for a switch op code are:
	//16-bit number n of case constants
	//n 8 or 16 bit numbers for each constant
	//n + 1 16-bit numbers of jump offsets(default case is excluded from constants, so the number of jumps is the number of constants + 1)
	//the default jump offset is always the last
	if (isLong) {
		emitByteAnd16Bit(+OpCode::SWITCH_LONG, constants.size());
		for (uInt16 constant : constants) {
			emit16Bit(constant);
		}
	}
	else {
		emitByteAnd16Bit(+OpCode::SWITCH, constants.size());
		for (uInt16 constant : constants) {
			emitByte(constant);
		}
	}

	for (int i = 0; i < constants.size(); i++) {
		jumps.push_back(getChunk()->bytecode.size());
		emit16Bit(0xffff);
	}
	//default jump
	jumps.push_back(getChunk()->bytecode.size());
	emit16Bit(0xffff);

	//at the end of each case is a implicit break
	vector<uint32_t> implicitBreaks;

	//compile the code of all cases, before each case update the jump for that case to the parserCurrent ip
	int i = 0;
	for (const std::shared_ptr<AST::CaseStmt>& _case : stmt->cases) {
		if (_case->caseType.getLexeme() == "default") {
			patchJump(jumps[jumps.size() - 1]);
		}
		else {
			//a single case can contain multiple constants(eg. case 1 | 4 | 9:), need to update jumps for each constant
			int constantNum = _case->constants.size();
			for (int j = 0; j < constantNum; j++) {
				patchJump(jumps[i]);
				i++;
			}
		}
		//new scope because patchScopeJumps only looks at scope deeper than the one it's called at
		beginScope();
		_case->accept(this);
		endScope();
		//end scope takes care of upvalues
		implicitBreaks.push_back(emitJump(+OpCode::JUMP));
		//patch advance after the implicit break jump for fallthrough
		patchScopeJumps(ScopeJumpType::ADVANCE);
	}
	//if there is no default case the default jump goes to the end of the switch stmt
	if (!stmt->hasDefault) jumps[jumps.size() - 1] = getChunk()->bytecode.size();

	//all implicit breaks lead to the end of the switch statement
	for (uInt jmp : implicitBreaks) {
		patchJump(jmp);
	}
	current->scopeWithSwitch.pop_back();
	patchScopeJumps(ScopeJumpType::BREAK);
}

void Compiler::visitCaseStmt(AST::CaseStmt* stmt) {
	//compile every statement in the case
	for (AST::ASTNodePtr stmt : stmt->stmts) {
		stmt->accept(this);
	}
}

void Compiler::visitAdvanceStmt(AST::AdvanceStmt* stmt) {
	//the amount of variables to pop and the amount of code to jump is determined in patchScopeJumps()
	//which is called at the start of each case statement(excluding the first)
	updateLine(stmt->token);
	int toPop = 0;
	//advance can only be used inside a case of a switch statement, and when jumping, jumps to the next case
	//case body is compiled in its own scope, so advance is always in a scope higher than it's switch statement
	for (int i = current->localCount - 1; i >= 0; i--) {
		Local& local = current->locals[i];
		if (local.depth != -1 && CHECK_SCOPE_FOR_SWITCH) break;
		toPop++;
	}
	if (toPop > UINT8_MAX) {
		error(stmt->token, "To many variables to pop.");
	}
	emitByte(+ScopeJumpType::ADVANCE);
	int advanceJump = getChunk()->bytecode.size();
	emitBytes((current->scopeDepth >> 8) & 0xff, current->scopeDepth & 0xff);
	emitByte(toPop);
	current->scopeJumps.push_back(advanceJump);
}

void Compiler::visitReturnStmt(AST::ReturnStmt* stmt) {
	updateLine(stmt->keyword);
	if (current->type == FuncType::TYPE_SCRIPT) {
		error(stmt->keyword, "Can't return from top-level code.");
	}
	else if (current->type == FuncType::TYPE_CONSTRUCTOR) {
		error(stmt->keyword, "Can't return a value from a constructor.");
	}
	current->hasReturnStmt = true;
	//if no expression is given, null is returned
	if (stmt->expr == nullptr) {
		emitReturn();
		return;
	}
	stmt->expr->accept(this);
	emitByte(+OpCode::RETURN);
}

#pragma region helpers

#pragma region Emitting bytes

void Compiler::emitByte(byte byte) {
	//line is incremented whenever we find a statement/expression that contains tokens
	getChunk()->writeData(byte, current->line, sourceFiles.size() - 1);
}

void Compiler::emitBytes(byte byte1, byte byte2) {
	emitByte(byte1);
	emitByte(byte2);
}

void Compiler::emit16Bit(uInt16 number) {
	//Big endian
	emitBytes((number >> 8) & 0xff, number & 0xff);
}

void Compiler::emitByteAnd16Bit(byte byte, uInt16 num) {
	emitByte(byte);
	emit16Bit(num);
}

uint16_t Compiler::makeConstant(Value value) {
	uInt constant = getChunk()->addConstant(value);
	if (constant > UINT16_MAX) {
		error("Too many constants in one chunk.");
	}
	return constant;
}

void Compiler::emitConstant(Value value) {
	// Shorthand for adding a constant to the chunk and emitting it
	uint16_t constant = makeConstant(value);
	if (constant <= SHORT_CONSTANT_LIMIT) emitBytes(+OpCode::CONSTANT, constant);
	else emitByteAnd16Bit(+OpCode::CONSTANT_LONG, constant);
}

void Compiler::emitReturn() {
	//in a constructor, the first local variable refers to the new instance of a class('this')
	if (current->type == FuncType::TYPE_CONSTRUCTOR) emitBytes(+OpCode::GET_LOCAL, 0);
	else emitByte(+OpCode::NIL);
	emitByte(+OpCode::RETURN);
}

int Compiler::emitJump(byte jumpType) {
	emitByte(jumpType);
	emitBytes(0xff, 0xff);
	return getChunk()->bytecode.size() - 2;
}

void Compiler::patchJump(int offset) {
	// -2 to adjust for the bytecode for the jump offset itself.
	int jump = getChunk()->bytecode.size() - offset - 2;
	//fix for future: insert 2 more bytes into the array, but make sure to do the same in lines array
	if (jump > UINT16_MAX) {
		error("Too much code to jump over.");
	}
	getChunk()->bytecode[offset] = (jump >> 8) & 0xff;
	getChunk()->bytecode[offset + 1] = jump & 0xff;
}

void Compiler::emitLoop(int start) {
	emitByte(+OpCode::LOOP_IF_TRUE);

	int offset = getChunk()->bytecode.size() - start + 2;
	if (offset > UINT16_MAX) error("Loop body too large.");

	emit16Bit(offset);
}

void Compiler::patchScopeJumps(ScopeJumpType type) {
	int curCode = getChunk()->bytecode.size();
	//most recent jumps are going to be on top
	for (int i = current->scopeJumps.size() - 1; i >= 0; i--) {
		uint32_t jumpPatchPos = current->scopeJumps[i];
		byte jumpType = getChunk()->bytecode[jumpPatchPos - 1];
		uint32_t jumpDepth = (getChunk()->bytecode[jumpPatchPos] << 8) | getChunk()->bytecode[jumpPatchPos + 1];
		uint32_t toPop = getChunk()->bytecode[jumpPatchPos + 2];
		//break and advance statements which are in a strictly deeper scope get patched, on the other hand
		//continue statements which are in parserCurrent or a deeper scope get patched
		if (jumpDepth > current->scopeDepth && +type == jumpType) {
			int jumpLength = curCode - jumpPatchPos - 3;
			if (jumpLength > UINT16_MAX) error("Too much code to jump over.");
			if (toPop > UINT8_MAX) error("Too many variables to pop.");

			getChunk()->bytecode[jumpPatchPos - 1] = +OpCode::JUMP_POPN;
			//variables declared by the time we hit the break whose depth is lower or equal to this break stmt
			getChunk()->bytecode[jumpPatchPos] = toPop;
			//amount to jump
			getChunk()->bytecode[jumpPatchPos + 1] = (jumpLength >> 8) & 0xff;
			getChunk()->bytecode[jumpPatchPos + 2] = jumpLength & 0xff;

			current->scopeJumps.erase(current->scopeJumps.begin() + i);
		}
		//any jump after the one that has a depth lower than the parserCurrent one will also have a lower depth, thus we bail
		else if (jumpDepth < current->scopeDepth) break;
	}
}

#pragma endregion

#pragma region Variables

//creates a string constant from a token
uint16_t Compiler::identifierConstant(Token name) {
	updateLine(name);
	string temp = name.getLexeme();
	return makeConstant(encodeObj(new ObjString(temp)));
}

//if this is a local var, mark it as ready and then bail out, otherwise emit code to add the variable to the global table
void Compiler::defineVar(uInt16 index) {
	if (current->scopeDepth > 0) {
		markInit();
		return;
	}
    definedGlobals[index] = true;
}

//gets/sets a variable, respects the scoping rules(locals->upvalues->globals)
void Compiler::namedVar(Token token, bool canAssign) {
	updateLine(token);
	byte getOp;
	byte setOp;
	int arg = resolveLocal(token);
	if (arg != -1) {
		getOp = +OpCode::GET_LOCAL;
		setOp = +OpCode::SET_LOCAL;
	}
	else if ((arg = resolveUpvalue(current, token)) != -1) {
		getOp = +OpCode::GET_UPVALUE;
		setOp = +OpCode::SET_UPVALUE;
	}
	else if((arg = resolveGlobal(token, canAssign)) != -1 && arg != -2){
		// All global variables are stored in an array, resolveGlobal gets index into the array
		getOp = +OpCode::GET_GLOBAL;
		setOp = +OpCode::SET_GLOBAL;
		if (arg > SHORT_CONSTANT_LIMIT) {
			getOp = +OpCode::GET_GLOBAL_LONG;
			setOp = +OpCode::SET_GLOBAL_LONG;
			emitByteAnd16Bit((canAssign ? setOp : getOp), arg);
			return;
		}
	}
    else{
        string name = token.getLexeme();
        auto it = nativeFuncNames.find(name);
        if(it == nativeFuncNames.end()){
            if(arg == -1) error(token, fmt::format("'{}' doesn't match any declared variable name or native function name.", name));
            else if(arg == -2) error(token, fmt::format("Trying to access variable '{}' before it's initialized.", name));
        }

        emitByteAnd16Bit(+OpCode::GET_NATIVE, it->second);
        return;
    }
	emitBytes(canAssign ? setOp : getOp, arg);
}

// If 'name' is a global variable it's index in the globals array is returned
// Otherwise, if 'name' is a local variable it's passed to declareVar()
uint16_t Compiler::parseVar(Token name) {
	updateLine(name);
	declareVar(name);
	if (current->scopeDepth > 0) return 0;

	// Searches for the index of the global variable in the current file
    // (globals.size() - curGlobalIndex = globals declared in current file)
	int index = curGlobalIndex;
	for (int i = curGlobalIndex; i < globals.size(); i++) {
		if (name.getLexeme() == globals[i].name) return i;
	}
    // Should never be hit, but here just in case
	error(name, "Couldn't find variable.");
	return 0;
}

// Makes sure the compiler is aware that a stack slot is occupied by this local variable
void Compiler::declareVar(Token& name) {
	updateLine(name);
	// If we are currently in global scope, this has no use
	if (current->scopeDepth == 0) return;
	for (int i = current->localCount - 1; i >= 0; i--) {
		Local* local = &current->locals[i];
		if (local->depth != -1 && local->depth < current->scopeDepth) {
			break;
		}
		string str = name.getLexeme();
		if (str.compare(local->name) == 0) {
			error(name, "Already a variable with this name in this scope.");
		}
	}
	addLocal(name);
}

//locals are stored on the stack, at compile time this is tracked with the 'locals' array
void Compiler::addLocal(Token name) {
	updateLine(name);
	if (current->localCount == LOCAL_MAX) {
		error(name, "Too many local variables in function.");
		return;
	}
	Local* local = &current->locals[current->localCount++];
	local->name = name.getLexeme();
	local->depth = -1;
}

void Compiler::beginScope() {
	current->scopeDepth++;
}

void Compiler::endScope() {
	//Pop every variable that was declared in this scope
	current->scopeDepth--;//first lower the scope, the check for every var that is deeper than the parserCurrent scope
	int toPop = 0;
	while (current->localCount > 0 && current->locals[current->localCount - 1].depth > current->scopeDepth) {
		toPop++;
		current->localCount--;
	}
	if (toPop > 0) {
		if (toPop == 1) {
			emitByte(+OpCode::POP);
			return;
		}
		emitBytes(+OpCode::POPN, toPop);
	}
}

int Compiler::resolveLocal(CurrentChunkInfo* func, Token name) {
	//checks to see if there is a local variable with a provided name, if there is return the index of the stack slot of the var
	updateLine(name);
	for (int i = func->localCount - 1; i >= 0; i--) {
		Local* local = &func->locals[i];
		string str = name.getLexeme();
		if (str.compare(local->name) == 0) {
			if (local->depth == -1) {
				error(name, "Can't read local variable in its own initializer.");
			}
			return i;
		}
	}

	return -1;
}

int Compiler::resolveLocal(Token name) {
	return resolveLocal(current, name);
}

int Compiler::resolveUpvalue(CurrentChunkInfo* func, Token name) {
	if (func->enclosing == nullptr) return -1;

	int local = resolveLocal(func->enclosing, name);
	if (local != -1) {
		func->enclosing->locals[local].isCaptured = true;
		func->enclosing->hasCapturedLocals = true;
		return addUpvalue((uint8_t)local, true);
	}
	int upvalue = resolveUpvalue(func->enclosing, name);
	if (upvalue != -1) {
		return addUpvalue((uint8_t)upvalue, false);
	}

	return -1;
}

int Compiler::addUpvalue(byte index, bool isLocal) {
	int upvalueCount = current->func->upvalueCount;
	//first check if this upvalue has already been captured
	for (int i = 0; i < upvalueCount; i++) {
		Upvalue* upval = &current->upvalues[i];
		if (upval->index == index && upval->isLocal == isLocal) {
			return i;
		}
	}
	if (upvalueCount == UPVAL_MAX) {
		error("Too many closure variables in function.");
		return 0;
	}
	current->upvalues[upvalueCount].isLocal = isLocal;
	current->upvalues[upvalueCount].index = index;
	return current->func->upvalueCount++;
}

//marks the local at the top of the stack as ready to use
void Compiler::markInit() {
	if (current->scopeDepth == 0) return;
	current->locals[current->localCount - 1].depth = current->scopeDepth;
}

Token Compiler::syntheticToken(string str) {
	return Token(TokenType::IDENTIFIER, str);
}

#pragma endregion

#pragma region Classes and methods
object::ObjClosure* Compiler::method(AST::FuncDecl* _method, Token className) {
	updateLine(_method->getName());
	uint16_t name = identifierConstant(_method->getName());
	FuncType type = FuncType::TYPE_METHOD;
	// Constructors are treated separately, but are still methods
	if (_method->getName().equals(className)) type = FuncType::TYPE_CONSTRUCTOR;
    // "this" gets implicitly defined as the first local in methods and the constructor
	current = new CurrentChunkInfo(current, type);
	beginScope();
	// Args defined as local in order they were passed to the function
	for (Token& var : _method->args) {
		uInt16 constant = parseVar(var);
		defineVar(constant);
	}
    for(auto stmt : _method->body->statements){
        stmt->accept(this);
    }
	current->func->arity = _method->arity;

	current->func->name = _method->getName().getLexeme();
	ObjFunc* func = endFuncDecl();
	if (func->upvalueCount != 0) error(_method->getName(), "Upvalues captured in method, aborting...");
    return new ObjClosure(func);
}

bool Compiler::invoke(AST::CallExpr* expr) {
	if (expr->callee->type == AST::ASTType::FIELD_ACCESS) {
		//currently we only optimizes field invoking(struct.field() or array[field]())
		auto* call = dynamic_cast<AST::FieldAccessExpr*>(expr->callee.get());
        if(call->accessor.type == TokenType::LEFT_BRACKET) return false;

		call->callee->accept(this);

		int argCount = 0;
		for (AST::ASTNodePtr arg : expr->args) {
			arg->accept(this);
			argCount++;
		}
        uint16_t constant = identifierConstant(dynamic_cast<AST::LiteralExpr*>(call->field.get())->token);
        if(constant > SHORT_CONSTANT_LIMIT){
            emitBytes(+OpCode::INVOKE_LONG, argCount);
            emit16Bit(constant);
        }
        else {
            emitBytes(+OpCode::INVOKE, argCount);
            emitByte(constant);
        }
		return true;
	}
	else if (expr->callee->type == AST::ASTType::SUPER) {
		AST::SuperExpr* superCall = dynamic_cast<AST::SuperExpr*>(expr->callee.get());
		uint16_t name = identifierConstant(superCall->methodName);

		if (currentClass == nullptr) {
			error(superCall->methodName, "Can't use 'super' outside of a class.");
		}
		else if (!currentClass->superclass) {
			error(superCall->methodName, "Can't use 'super' in a class with no superclass.");
		}
		//in methods and constructors, "this" is implicitly defined as the first local
		namedVar(syntheticToken("this"), false);
		int argCount = 0;
		for (AST::ASTNodePtr arg : expr->args) {
			arg->accept(this);
			argCount++;
		}
		// superclass gets popped, leaving only the receiver and args on the stack
        emitConstant(encodeObj(currentClass->superclass));
        if(name > SHORT_CONSTANT_LIMIT){
            emitBytes(+OpCode::SUPER_INVOKE_LONG, argCount);
            emit16Bit(name);
        }
        else {
            emitBytes(+OpCode::SUPER_INVOKE, argCount);
            emitByte(name);
        }
		return true;
	}
	return false;
}
#pragma endregion

Chunk* Compiler::getChunk() {
	return &current->chunk;
}

void Compiler::error(const string& message) noexcept(false) {
	errorHandler::addSystemError("System compile error [line " + std::to_string(current->line) + "] in '" + curUnit->file->name + "': \n" + message + "\n");
	throw CompilerException();
}

void Compiler::error(Token token, const string& msg) noexcept(false) {
	errorHandler::addCompileError(msg, token);
	throw CompilerException();
}

ObjFunc* Compiler::endFuncDecl() {
	if (!current->hasReturnStmt) emitReturn();
	// Get the parserCurrent function we've just compiled, delete it's compiler info, and replace it with the enclosing functions compiler info
	ObjFunc* func = current->func;
	Chunk& chunk = current->chunk;

	//Add the bytecode, lines and constants to the main code block
	uInt64 bytecodeOffset = mainCodeBlock.bytecode.size();
	mainCodeBlock.bytecode.insert(mainCodeBlock.bytecode.end(), chunk.bytecode.begin(), chunk.bytecode.end());
	uInt64 constantsOffset = mainCodeBlock.constants.size();
	mainCodeBlock.constants.insert(mainCodeBlock.constants.end(), chunk.constants.begin(), chunk.constants.end());
    // For the last line of code
    chunk.lines[chunk.lines.size() - 1].end = chunk.bytecode.size();
	// Update lines to reflect the offset in the main code block
	for (codeLine& line : chunk.lines) {
		line.end += bytecodeOffset;
		mainCodeBlock.lines.push_back(line);
	}
    #ifdef COMPILER_DEBUG
    mainCodeBlock.disassemble(current->func->name.length() == 0 ? "script" : current->func->name, bytecodeOffset, constantsOffset);
    #endif
	// Set the offsets in the function object
	func->bytecodeOffset = bytecodeOffset;
	func->constantsOffset = constantsOffset;

	CurrentChunkInfo* temp = current->enclosing;
	delete current;
	current = temp;
	return func;
}

//a little helper for updating the lines emitted by the compiler(used for displaying runtime errors)
void Compiler::updateLine(Token token) {
	current->line = token.str.line;
}

// For every dependency that's imported without an alias, check if any of its exports match 'symbol', return -1 if not
int Compiler::checkSymbol(Token symbol) {
	string lexeme = symbol.getLexeme();
	for (Dependency dep : curUnit->deps) {
        auto& decls = dep.module->topDeclarations;
		if (dep.alias.type == TokenType::NONE) {
            int globalIndex = 0;
            for (int i = 0; i < units.size(); i++) {
                if (units[i] == dep.module) break;
                globalIndex += units[i]->topDeclarations.size();
            }
            int upperLimit = globalIndex + decls.size();
			for (auto it = decls.begin(); it != decls.end(); it++) {
                if(!(*it)->getName().equals(symbol)) continue;
                // If the correct symbol is found, find the index of the global variable inside globals array
                for(int i = globalIndex; i < upperLimit; i++){
                    if (lexeme == globals[i].name) return i;
                }
                // Should never be hit
                error(symbol, "Error, variable wasn't loaded into globals array.");
			}
		}
	}
    return -1;
}

// Checks if 'symbol' is declared in the current file, if not passes it to checkSymbol
// -1 is returned only if no global variable 'symbol' exists, which is checked by checkSymbol
// -2 if variable exists but isn't defined
int Compiler::resolveGlobal(Token symbol, bool canAssign) {
	bool inThisFile = false;
	int index = curGlobalIndex;
    std::shared_ptr<AST::ASTDecl> ptr = nullptr;
	for (auto decl : curUnit->topDeclarations) {
		if (symbol.equals(decl->getName())) {
            // It's an error to read from a variable during its initialization
            if(!definedGlobals[index]) return -2;
			inThisFile = true;
            ptr = decl;
			break;
		}
		index++;
	}
	if (canAssign) {
		if (inThisFile){
            if(ptr->type == AST::ASTType::FUNC) error(symbol, "Cannot assign to a function.");
            else if(ptr->type == AST::ASTType::CLASS) error(symbol, "Cannot assign to a class.");

            return index;
        }
        error(symbol, "Cannot assign to a variable not declared in this module.");
	}
	else {
		if (inThisFile) return index;
		else {
            // Global variables defined in an imported file are guaranteed to be already defined
			return checkSymbol(symbol);
		}
	}
    // Never hit, checkSymbol returns -1 upon failure
	return -1;
}

// Checks if 'variable' exists in a module which was imported with the alias 'moduleAlias',
// If it exists return the index of the 'variable' in globals array
uint32_t Compiler::resolveModuleVariable(Token moduleAlias, Token variable) {
	//first find the module with the correct alias
	Dependency* depPtr = nullptr;
	for (Dependency dep : curUnit->deps) {
		if (dep.alias.equals(moduleAlias)) {
			depPtr = &dep;
			break;
		}
	}
	if (depPtr == nullptr) {
		error(moduleAlias, "Module alias doesn't exist.");
	}

	CSLModule* unit = depPtr->module;
	int index = 0;
	for (auto& i : units) {
		if (i != unit) {
			index += i->topDeclarations.size();
			continue;
		}
		// Checks every export of said module against 'variable'
		for (auto decl : unit->exports) {
			if (decl->getName().equals(variable)) return index;
			index++;
		}
	}

	error(variable, fmt::format("Module {} doesn't export this symbol.", depPtr->alias.getLexeme()));
    // Never hit
    return -1;
}
#pragma endregion

// Only used when debugging _LONG versions of op codes
#undef SHORT_CONSTANT_LIMIT

#undef CHECK_SCOPE_FOR_LOOP
#undef CHECK_SCOPE_FOR_SWITCH
