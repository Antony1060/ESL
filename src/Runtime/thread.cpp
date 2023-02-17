#include "thread.h"
#include "vm.h"
#include <iostream>
#include <utility>
#include "../Includes/fmt/format.h"
#include "../Includes/fmt/color.h"
#include "../codegen/valueHelpersInline.cpp"
#include "../DebugPrinting/BytecodePrinter.h"

using std::get;
using namespace valueHelpers;

runtime::Thread::Thread(VM* _vm){
	stackTop = stack;
	frameCount = 0;
    cancelToken.store(false);
	vm = _vm;
}

// Copies the callee and all arguments, otherStack points to the callee, arguments are on top of it on the stack
void runtime::Thread::startThread(Value* otherStack, int num) {
	memcpy(stackTop, otherStack, sizeof(Value) * num);
	stackTop += num;
	callValue(*otherStack, num - 1);
}

// Copies value to the stack
void runtime::Thread::copyVal(Value val) {
	push(val);
}

runtime::BuiltinMethod& runtime::Thread::findNativeMethod(Value& receiver, string& name){
    runtime::Builtin type = runtime::Builtin::COMMON;
    if(isObj(receiver)){
        switch(decodeObj(receiver)->type){
            case object::ObjType::STRING: type = runtime::Builtin::STRING; break;
            case object::ObjType::ARRAY: type = runtime::Builtin::ARRAY; break;
            case object::ObjType::FILE: type = runtime::Builtin::FILE; break;
            case object::ObjType::MUTEX: type = runtime::Builtin::MUTEX; break;
            case object::ObjType::FUTURE: type = runtime::Builtin::FUTURE; break;
            default: break;
        }
    }
    auto& methods = vm->nativeClasses[+type].methods;
    auto it = methods.find(name);
    if(it != methods.end()) return it->second;
    runtimeError(fmt::format("{} doesn't contain property '{}'.", typeToStr(receiver), name), 4);
}

void runtime::Thread::mark(memory::GarbageCollector* gc) {
	for (Value* i = stack; i < stackTop; i++) {
		valueHelpers::mark(*i);
	}
	for (int i = 0; i < frameCount; i++) gc->markObj(frames[i].closure);
}

void runtime::Thread::push(Value val) {
	if (stackTop >= stack + STACK_MAX) {
		runtimeError("Stack overflow", 1);
	}
	*stackTop = val;
	stackTop++;
}

Value runtime::Thread::pop() {
	stackTop--;
	return *stackTop;
}

void runtime::Thread::popn(int n) {
    stackTop-= n;
}

Value& runtime::Thread::peek(int depth) {
    return stackTop[-1 - depth];
}

void runtime::Thread::runtimeError(string err, int errorCode) {
    errorString = std::move(err);
    throw errorCode;
}

void runtime::Thread::callValue(Value& callee, int argCount) {
	if (isObj(callee)) {
		switch (decodeObj(callee)->type) {
		case object::ObjType::CLOSURE:
			return call(asClosure(callee), argCount);
		case object::ObjType::NATIVE: {
			int arity = asNativeFn(callee)->arity;
			//if arity is -1 it means that the function takes in a variable number of args
			if (arity != -1 && argCount != arity) {
				runtimeError(fmt::format("Function {} expects {} arguments but got {}.", asNativeFn(callee)->name, arity, argCount), 2);
			}
			object::NativeFn native = asNativeFn(callee)->func;
            // If native returns true then the ObjNativeFunc is still on the stack and should be popped
            if(native(this, argCount)) {
                stackTop[-2] = stackTop[-1];
                stackTop--;
            }
            return;
		}
        case object::ObjType::BOUND_NATIVE:{
            object::ObjBoundNativeFunc* bound = asBoundNativeFunc(callee);
            stackTop[-argCount - 1] = bound->receiver;
            int arity = bound->arity;
            //if arity is -1 it means that the function takes in a variable number of args
            if (arity != -1 && argCount != arity) {
                runtimeError(fmt::format("Function {} expects {} arguments but got {}.", asNativeFn(callee)->name, arity, argCount), 2);
            }
            object::NativeFn native = bound->func;
            // If native returns true then the ObjNativeFunc is still on the stack and should be popped
            if(native(this, argCount)) {
                stackTop[-2] = stackTop[-1];
                stackTop--;
            }
            return;
        }
		case object::ObjType::CLASS: {
			// We do this so if a GC runs we safely update all the pointers(since the stack is considered a root)
            object::ObjClass* klass = asClass(callee);
			stackTop[-argCount - 1] = encodeObj(new object::ObjInstance(klass));
			if (klass->methods.contains(klass->name)) {
				return call(asClosure(klass->methods[klass->name]), argCount);
			}
			else if (argCount != 0) {
				runtimeError(fmt::format("Class constructor expects 0 arguments but got {}.", argCount), 2);
			}
			return;
		}
		case object::ObjType::BOUND_METHOD: {
			//puts the receiver instance in the 0th slot of the current callframe('this' points to the 0th slot)
			object::ObjBoundMethod* bound = asBoundMethod(callee);
			stackTop[-argCount - 1] = bound->receiver;
			return call(bound->method, argCount);
		}
		default:
			break; // Non-callable object type.
		}
	}
	runtimeError("Can only call functions and classes.", 3);
}

void runtime::Thread::call(object::ObjClosure* closure, int argCount) {
	if (argCount != closure->func->arity) {
		runtimeError(fmt::format("Expected {} arguments for function call but got {}.", closure->func->arity, argCount), 2);
	}

	if (frameCount == FRAMES_MAX) {
		runtimeError("Stack overflow.", 1);
	}

	CallFrame* frame = &frames[frameCount++];
	frame->closure = closure;
	frame->ip = &vm->code.bytecode[closure->func->bytecodeOffset];
	frame->slots = stackTop - argCount - 1;
}

object::ObjUpval* captureUpvalue(Value* local) {
	auto* upval = new object::ObjUpval(*local);
	*local = encodeObj(upval);
	return upval;
}

void runtime::Thread::defineMethod(string& name) {
	//no need to type check since the compiler made sure to emit code in this order
	Value& method = peek(0);
	object::ObjClass* klass = asClass(peek(1));
	klass->methods.insert_or_assign(name, method);
	//we only pop the method, since other methods we're compiling will also need to know their class
	pop();
}

bool runtime::Thread::bindMethod(object::ObjClass* klass, string& name) {
	auto it = klass->methods.find(name);
	if (it == klass->methods.end()) return false;
	//peek(0) to get the ObjInstance
	auto* bound = new object::ObjBoundMethod(peek(0), asClosure(it->second));
    // Replace top of the stack
    *(stackTop - 1) = encodeObj(bound);
    return true;
}

void runtime::Thread::invoke(string& fieldName, int argCount) {
	Value& receiver = peek(argCount);

	if (isInstance(receiver)) {
        object::ObjInstance* instance = asInstance(receiver);
        auto it = instance->fields.find(fieldName);
        // Invoke can be used on functions that are part of a struct or in a instances field
        // when not used for methods they need to replace the instance
        if (it != instance->fields.end()) {
            stackTop[-argCount - 1] = it->second;
            return callValue(it->second, argCount);
        }
        // This check is used because we also use objInstance to represent struct literals
        // and if this instance is a struct it can only contain functions inside its field table
        if (instance->klass != nullptr && invokeFromClass(instance->klass, fieldName, argCount)) return;
	}
    auto native = findNativeMethod(receiver, fieldName);
    int arity = native.arity;
    // If arity is -1 it means that the function takes in a variable number of args
    if (arity != -1 && argCount != arity) {
        runtimeError(fmt::format("Method {} expects {} arguments but got {}.", fieldName, arity, argCount), 2);
    }
    // If native returns true then the ObjNativeFunc is still on the stack and should be popped
    if(native.func(this, argCount)) {
        stackTop[-2] = stackTop[-1];
        stackTop--;
    }
}

bool runtime::Thread::invokeFromClass(object::ObjClass* klass, string& methodName, int argCount) {
	auto it = klass->methods.find(methodName);
	if (it == klass->methods.end()) return false;
	// The bottom of the call stack will contain the receiver instance
	call(asClosure(it->second), argCount);
    return true;
}

void runtime::Thread::bindMethodToPrimitive(Value& receiver, string& methodName){
    auto func = findNativeMethod(receiver, methodName);
    push(encodeObj(new object::ObjBoundNativeFunc(func.func, func.arity, methodName, receiver)));
}
#pragma endregion

void runtime::Thread::executeBytecode() {

	#ifdef DEBUG_TRACE_EXECUTION
	std::cout << "-------------Code execution starts-------------\n";
	#endif // DEBUG_TRACE_EXECUTION
	// If this is the main thread fut will be nullptr
	object::ObjFuture* fut = asFuture(stack[0]);
	// C++ is more likely to put these locals in registers which speeds things up
	CallFrame* frame = &frames[frameCount - 1];
	byte* ip = &vm->code.bytecode[frame->closure->func->bytecodeOffset];
	Value* slotStart = frame->slots;
    uint32_t constantOffset = frame->closure->func->constantsOffset;


	#pragma region Helpers & Macros
	#define READ_BYTE() (*ip++)
	#define READ_SHORT() (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))
	#define READ_CONSTANT() (vm->code.constants[constantOffset + READ_BYTE()])
	#define READ_CONSTANT_LONG() (vm->code.constants[constantOffset + READ_SHORT()])
	#define READ_STRING() (asString(READ_CONSTANT()))
	#define READ_STRING_LONG() (asString(READ_CONSTANT_LONG()))
	auto checkArrayBounds = [](runtime::Thread* t, Value& field, Value& callee, object::ObjArray* arr) {
		if (!isInt(field)) { t->runtimeError(fmt::format("Index must be an integer, got {}.", typeToStr(callee)), 3); }
        int32_t index = decodeInt(field);
		if (index < 0 || index > arr->values.size() - 1) { t->runtimeError(fmt::format("Index {} outside of range [0, {}].", index, arr->values.size() - 1), 9); }
		return index;
	};
	auto deleteThread = [](object::ObjFuture* _fut, VM* vm) {
        std::condition_variable &cv = vm->mainThreadCv;
        // If execution is finishing and the main thread is waiting to run the gc
        // notify the main thread after deleting this thread object
        {
            // vm->pauseMtx to notify the main thread that this thread doesn't exist anymore,
            std::scoped_lock lk(vm->pauseMtx, vm->mtx);
            // Immediately delete the thread object to conserve memory
            for (auto it = vm->childThreads.begin(); it != vm->childThreads.end(); it++) {
                if (*it == _fut->thread) {
                    delete* it;
                    _fut->thread = nullptr;
                    vm->childThreads.erase(it);
                    break;
                }
            }
        }
        cv.notify_one();
	};

	// Stores the ip to the current frame before a new one is pushed
	#define STORE_FRAME() frame->ip = ip
	// When a frame is pushed/popped load its variables to the locals
	#define LOAD_FRAME() (													\
	frame = &frames[frameCount - 1],										\
	slotStart = frame->slots,												\
	ip = frame->ip,                                                         \
    constantOffset = frame->closure->func->constantsOffset)

	#define BINARY_OP(op)                                                                                                                                   \
        do {                                                                                                                                                \
            Value& a = peek(1), b = peek(0);                                                                                                                \
            if (!isNumber(a) || !isNumber(b)) { runtimeError(fmt::format("Operands must be numbers, got '{}' and '{}'.", typeToStr(a), typeToStr(b)), 3); } \
            if (isInt(a) && isInt(b)) {                                                                                                                     \
                int64_t res = decodeInt(a) op decodeInt(b);                                                                                                 \
                a = (INT_MIN <= res && res <= INT_MAX) ? encodeInt(res) : encodeDouble(res);                                                                \
            }                                                                                                                                               \
            else {                                                                                                                                          \
                double valA = (isInt(a)) ? decodeInt(a) : decodeDouble(a);                                                                                  \
                double valB = (isInt(b)) ? decodeInt(b) : decodeDouble(b);                                                                                  \
                a = encodeDouble(valA op valB);                                                                                                             \
            }                                                                                                                                               \
            --stackTop;                                                                                                                                     \
        } while(0)                                                                                                                                          \

	#define INT_BINARY_OP(op)                                                                                                                          \
        do {                                                                                                                                           \
            Value& a = peek(1), b = peek(0);                                                                                                           \
            if (!isInt(a) || !isInt(b)) { runtimeError(fmt::format("Operands must be integers, got '{}' and '{}'.", typeToStr(a), typeToStr(b)), 3); } \
            a = encodeInt(decodeInt(a) op decodeInt(b));                                                                                               \
            --stackTop;                                                                                                                                \
        } while(0)                                                                                                                                     \

    #pragma endregion

    #define DISPATCH() goto loop
    try {
        loop:
        if(cancelToken.load()) {
            // If this is a child thread that has a future attached to it, assign the value to the future
            fut->val = encodeNil();
            // Since this thread gets deleted by deleteThread, cond var to notify the main thread must be cached in the function
            std::condition_variable &cv = vm->mainThreadCv;
            // If execution is finishing and the main thread is waiting to run the gc
            // notify the main thread after deleting this thread object
            {
                // vm->pauseMtx to notify the main thread that this thread doesn't exist anymore,
                std::scoped_lock<std::mutex> lk(vm->pauseMtx);
                // deleteThread locks vm->mtx to delete itself from the pool
                deleteThread(fut, vm);
            }
            cv.notify_one();
            return;
        }
        #pragma region Multithreading
        if (!fut && memory::gc.shouldCollect.load()) {
            // If fut is null, this is the main thread of execution which runs the GC
            if (vm->allThreadsPaused()) {
                memory::gc.collect(vm);
            } else {
                // If some threads aren't sleeping yet, use a cond var to wait, every child thread will notify the var when it goes to sleep
                std::unique_lock lk(vm->pauseMtx);
                vm->mainThreadCv.wait(lk, [&] { return vm->allThreadsPaused(); });
                // Release the mutex here so that GC can acquire it
                lk.unlock();
                // After all threads are asleep, run the GC and subsequently awaken all child threads
                memory::gc.collect(vm);
            }
        } else if (fut && memory::gc.shouldCollect.load()) {
            // If this is a child thread and the GC must run, notify the main thread that this one is paused
            // Main thread sends the notification when to awaken
            {
                std::scoped_lock lk(vm->pauseMtx);
                vm->threadsPaused.fetch_add(1);
            }
            // Only the main thread waits for mainThreadCv
            vm->mainThreadCv.notify_one();

            // No need to propagate this since the main thread won't be listening
            std::unique_lock lk(vm->pauseMtx);
            vm->childThreadsCv.wait(lk, [] { return !memory::gc.shouldCollect.load(); });
            vm->threadsPaused.fetch_sub(1);
            lk.unlock();
        }
        #pragma endregion
        #ifdef DEBUG_TRACE_EXECUTION
        std::cout << "          ";
            for (Value* slot = stack; slot < stackTop; slot++) {
                std::cout << "[";
                (*slot).print();
                std::cout << "] ";
            }
            std::cout << "\n";
            disassembleInstruction(&vm->code, ip - vm->code.bytecode.data(), frame->closure->func->constantsOffset);
        #endif
        switch (READ_BYTE()) {
            #pragma region Helper opcodes
            case +OpCode::POP: {
                stackTop--;
                DISPATCH();
            }
            case +OpCode::POPN: {
                uint8_t nToPop = READ_BYTE();
                stackTop -= nToPop;
                DISPATCH();
            }
            case +OpCode::LOAD_INT: {
                push(encodeInt(READ_BYTE()));
                DISPATCH();
            }
            #pragma endregion

            #pragma region Constant opcodes
            case +OpCode::CONSTANT: {
                Value& constant = READ_CONSTANT();
                push(constant);
                DISPATCH();
            }
            case +OpCode::CONSTANT_LONG: {
                Value& constant = READ_CONSTANT_LONG();
                push(constant);
                DISPATCH();
            }
            case +OpCode::NIL:
                push(encodeNil());
                DISPATCH();
            case +OpCode::TRUE:
                push(encodeBool(true));
                DISPATCH();
            case +OpCode::FALSE:
                push(encodeBool(false));
                DISPATCH();
            #pragma endregion

            #pragma region Unary opcodes
            case +OpCode::NEGATE: {
                Value val = pop();
                if (!isNumber(val)) {
                    runtimeError(fmt::format("Operand must be a number, got {}.", typeToStr(val)), 3);
                }
                if (isInt(val)) { push(encodeInt(-decodeInt(val))); }
                else { push(encodeDouble(-decodeDouble(val))); }
                DISPATCH();
            }
            case +OpCode::NOT: {
                push(encodeBool(isFalsey(pop())));
                DISPATCH();
            }
            case +OpCode::BIN_NOT: {
                if (!isNumber(peek(0))) {
                    runtimeError(fmt::format("Operand must be a number, got {}.", typeToStr(peek(0))), 3);
                }
                if (!isInt(peek(0))) {
                    runtimeError("Number must be a integer, got a float.", 3);
                }
                stackTop[-1] = encodeInt(~decodeInt(peek(0)));
                DISPATCH();
            }
            case +OpCode::INCREMENT: {
                byte arg = READ_BYTE();
                int8_t sign = (arg & 0b00000001) == 1 ? 1 : -1;
                // True: prefix, false: postfix
                bool isPrefix = (arg & 0b00000010) == 2;

                byte type = arg >> 2;

                auto add = [&](Value& x, int y) {
                    // TODO: Might overflow int
                    if (isInt(x)) x = encodeInt(decodeInt(x) + y);
                    else x = encodeDouble(decodeDouble(x) + y);
                };

                auto tryIncrement = [&](runtime::Thread* t, bool isPrefix, int sign, Value &val) {
                    if (!isNumber(val)) { t->runtimeError(fmt::format("Operand must be a number, got {}.", typeToStr(val)), 3); }
                    if (isPrefix) {
                        add(val, sign);
                        t->push(val);
                    } else {
                        t->push(val);
                        add(val, sign);
                    }
                };

                #define INCREMENT(val) tryIncrement(this, isPrefix, sign, val); DISPATCH();


                switch (type) {
                    case 0: {
                        byte slot = READ_BYTE();
                        Value &num = slotStart[slot];
                        // If this is a local upvalue
                        if (isUpvalue(num)) {
                            Value &temp = asUpvalue(num)->val;
                            INCREMENT(temp);
                        }
                        INCREMENT(num);
                    }
                    case 1: {
                        byte slot = READ_BYTE();
                        Value &num = frame->closure->upvals[slot]->val;
                        INCREMENT(num);
                    }
                    case 2: {
                        byte index = READ_BYTE();
                        Globalvar &var = vm->globals[index];
                        INCREMENT(var.val);
                    }
                    case 3: {
                        byte index = READ_SHORT();
                        Globalvar &var = vm->globals[index];
                        INCREMENT(var.val);
                    }
                    case 4: {
                        Value inst = pop();
                        if (!isInstance(inst)) {
                            runtimeError(
                                    fmt::format("Only instances/structs have properties, got {}.", typeToStr(inst)),
                                    3);
                        }

                        object::ObjInstance *instance = asInstance(inst);
                        object::ObjString *str = READ_STRING();
                        auto it = instance->fields.find(str->str);
                        if (it == instance->fields.end()) {
                            runtimeError(fmt::format("Field '{}' doesn't exist.", str->str), 4);
                        }
                        Value &num = it->second;
                        INCREMENT(num);
                    }
                    case 5: {
                        Value inst = pop();
                        if (!isInstance(inst)) {
                            runtimeError(
                                    fmt::format("Only instances/structs have properties, got {}.", typeToStr(inst)),
                                    3);
                        }

                        object::ObjInstance *instance = asInstance(inst);
                        object::ObjString *str = READ_STRING_LONG();

                        auto it = instance->fields.find(str->str);
                        if (it == instance->fields.end()) {
                            runtimeError(fmt::format("Field '{}' doesn't exist.", str->str), 4);
                        }
                        Value &num = it->second;
                        INCREMENT(num);
                    }
                    case 6: {
                        Value field = pop();
                        Value callee = pop();

                        if (isArray(callee)) {
                            object::ObjArray *arr = asArray(callee);
                            uInt64 index = checkArrayBounds(this, field, callee, arr);
                            Value &num = arr->values[index];
                            INCREMENT(num);
                        }
                        // If it's not an array nor a instance, throw type error
                        if (!isInstance(callee)) runtimeError(fmt::format("Expected a array or struct, got {}.", typeToStr(callee)), 3);
                        if (!isString(field))
                            runtimeError(fmt::format("Expected a string for field name, got {}.", typeToStr(field)), 3);

                        object::ObjInstance *instance = asInstance(callee);
                        object::ObjString *str = asString(field);

                        auto it = instance->fields.find(str->str);
                        if (it == instance->fields.end()) {
                            runtimeError(fmt::format("Field '{}' doesn't exist.", str->str), 4);
                        }
                        Value &num = it->second;
                        INCREMENT(num);
                    }
                    default:
                        runtimeError(fmt::format("Unrecognized argument in OpCode::INCREMENT"), 6);
                }
            }
            #pragma endregion

            #pragma region Binary opcodes
            case +OpCode::BITWISE_XOR:
                INT_BINARY_OP(^);
                DISPATCH();
            case +OpCode::BITWISE_OR:
                INT_BINARY_OP(|);
                DISPATCH();
            case +OpCode::BITWISE_AND:
                INT_BINARY_OP(&);
                DISPATCH();
            case +OpCode::ADD: {
                if (isNumber(peek(0)) && isNumber(peek(1))) {
                    BINARY_OP(+);
                } else if (isString(peek(0)) && isString(peek(1))) {
                    object::ObjString *b = asString(pop());
                    object::ObjString *a = asString(pop());

                    push(Value(a->concat(b)));
                } else {
                    runtimeError(fmt::format("Operands must be two numbers or two strings, got {} and {}.",
                                             typeToStr(peek(1)), typeToStr(peek(0))), 3);
                }
                DISPATCH();
            }
            case +OpCode::SUBTRACT:
                BINARY_OP(-);
                DISPATCH();
            case +OpCode::MULTIPLY:
                BINARY_OP(*);
                DISPATCH();
            case +OpCode::DIVIDE:
                BINARY_OP(/);
                DISPATCH();
            case +OpCode::MOD:
                INT_BINARY_OP(%);
                DISPATCH();
            case +OpCode::BITSHIFT_LEFT:
                INT_BINARY_OP(<<);
                DISPATCH();
            case +OpCode::BITSHIFT_RIGHT:
                INT_BINARY_OP(>>);
                DISPATCH();
            #pragma endregion

            #pragma region Binary opcodes that return bool
            case +OpCode::EQUAL: {
                Value b = pop();
                Value a = pop();
                push(encodeBool(equals(a, b)));
                DISPATCH();
            }
            case +OpCode::NOT_EQUAL: {
                Value b = pop();
                Value a = pop();
                push(encodeBool(!equals(a, b)));
                DISPATCH();
            }
            case +OpCode::GREATER: {
                Value &a = peek(1), b = peek(0);
                if (!isNumber(a) || !isNumber(b)) {
                    runtimeError(fmt::format("Operands must be two numbers, got {} and {}.", typeToStr(peek(1)),
                                             typeToStr(peek(0))), 3);
                }
                double valA = (isInt(a)) ? decodeInt(a) : decodeDouble(a);
                double valB = (isInt(b)) ? decodeInt(b) : decodeDouble(b);

                a = encodeBool(valA > valB);
                stackTop--;
                DISPATCH();
            }
            case +OpCode::GREATER_EQUAL: {
                //Have to do this because of floating point comparisons
                Value& a = peek(1), b = peek(0);
                if (!isNumber(a) || !isNumber(b)) {
                    runtimeError(fmt::format("Operands must be two numbers, got {} and {}.", typeToStr(peek(1)), typeToStr(peek(0))), 3);
                }
                double valA = (isInt(a)) ? decodeInt(a) : decodeDouble(a);
                double valB = (isInt(b)) ? decodeInt(b) : decodeDouble(b);

                // TODO: Make this better? (differentiate between int and double comparisons)
                a = encodeBool(valA >= valB - DBL_EPSILON);
                stackTop--;
                DISPATCH();
            }
            case +OpCode::LESS: {
                Value &a = peek(1), b = peek(0);
                if (!isNumber(a) || !isNumber(b)) {
                    runtimeError(fmt::format("Operands must be two numbers, got {} and {}.", typeToStr(peek(1)),
                                             typeToStr(peek(0))), 3);
                }
                double valA = (isInt(a)) ? decodeInt(a) : decodeDouble(a);
                double valB = (isInt(b)) ? decodeInt(b) : decodeDouble(b);

                a = encodeBool(valA < valB);
                stackTop--;
                DISPATCH();
            }
            case +OpCode::LESS_EQUAL: {
                Value& a = peek(1), b = peek(0);
                if (!isNumber(a) || !isNumber(b)) {
                    runtimeError(fmt::format("Operands must be two numbers, got {} and {}.", typeToStr(peek(1)), typeToStr(peek(0))), 3);
                }
                double valA = (isInt(a)) ? decodeInt(a) : decodeDouble(a);
                double valB = (isInt(b)) ? decodeInt(b) : decodeDouble(b);

                a = encodeBool(valA < valB + DBL_EPSILON);
                stackTop--;
                DISPATCH();
            }
            #pragma endregion

            #pragma region Statements and var
            case +OpCode::GET_NATIVE: {
                push(encodeObj(vm->nativeFuncs[READ_SHORT()]));
                DISPATCH();
            }

            case +OpCode::DEFINE_GLOBAL: {
                byte index = READ_BYTE();
                vm->globals[index].val = pop();
                DISPATCH();
            }
            case +OpCode::DEFINE_GLOBAL_LONG: {
                uInt index = READ_SHORT();
                vm->globals[index].val = pop();
                DISPATCH();
            }

            case +OpCode::GET_GLOBAL: {
                byte index = READ_BYTE();
                Globalvar &var = vm->globals[index];
                push(var.val);
                DISPATCH();
            }
            case +OpCode::GET_GLOBAL_LONG: {
                uInt index = READ_SHORT();
                Globalvar &var = vm->globals[index];
                push(var.val);
                DISPATCH();
            }

            case +OpCode::SET_GLOBAL: {
                byte index = READ_BYTE();
                Globalvar &var = vm->globals[index];
                var.val = peek(0);
                DISPATCH();
            }
            case +OpCode::SET_GLOBAL_LONG: {
                uInt index = READ_SHORT();
                Globalvar &var = vm->globals[index];
                var.val = peek(0);
                DISPATCH();
            }

            case +OpCode::GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                Value &val = slotStart[slot];
                if (isUpvalue(val)) {
                    push(asUpvalue(val)->val);
                    DISPATCH();
                }
                push(slotStart[slot]);
                DISPATCH();
            }

            case +OpCode::SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                Value &val = slotStart[slot];
                if (isUpvalue(val)) {
                    asUpvalue(val)->val = peek(0);
                    DISPATCH();
                }
                slotStart[slot] = peek(0);
                DISPATCH();
            }

            case +OpCode::GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                push(frame->closure->upvals[slot]->val);
                DISPATCH();
            }
            case +OpCode::SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                frame->closure->upvals[slot]->val = peek(0);
                DISPATCH();
            }
            #pragma endregion

            #pragma region Control flow
            case +OpCode::JUMP: {
                uint16_t offset = READ_SHORT();
                ip += offset;
                DISPATCH();
            }

            case +OpCode::JUMP_IF_FALSE: {
                uint16_t offset = READ_SHORT();
                if (isFalsey(peek(0))) ip += offset;
                DISPATCH();
            }
            case +OpCode::JUMP_IF_TRUE: {
                uint16_t offset = READ_SHORT();
                if (!isFalsey(peek(0))) ip += offset;
                DISPATCH();
            }
            case +OpCode::JUMP_IF_FALSE_POP: {
                uint16_t offset = READ_SHORT();
                if (isFalsey(pop())) ip += offset;
                DISPATCH();
            }

            case +OpCode::LOOP_IF_TRUE: {
                uint16_t offset = READ_SHORT();
                if (!isFalsey(pop())) ip -= offset;
                DISPATCH();
            }
            case +OpCode::LOOP: {
                uint16_t offset = READ_SHORT();
                ip -= offset;
                DISPATCH();
            }

            case +OpCode::JUMP_POPN: {
                stackTop -= READ_BYTE();
                ip += READ_SHORT();
                DISPATCH();
            }

            case +OpCode::SWITCH: {
                Value val = pop();
                uInt caseNum = READ_SHORT();
                // Offset into constant indexes
                byte *offset = ip + caseNum;
                // Place in the bytecode where the jump is held
                byte *jumpOffset = nullptr;
                for (int i = 0; i < caseNum; i++) {
                    if (val == READ_CONSTANT()) {
                        jumpOffset = offset + (i * 2);
                        break;
                    }
                }
                // Default
                if (!jumpOffset) jumpOffset = offset + caseNum * 2;
                ip = jumpOffset;
                uInt debug = ip - vm->code.bytecode.data();
                uInt jmp = READ_SHORT();
                ip += jmp;
                DISPATCH();
            }
            case +OpCode::SWITCH_LONG: {
                Value val = pop();
                uInt caseNum = READ_SHORT();
                // Offset into constant indexes
                byte *offset = ip + caseNum * 2;
                // Place in the bytecode where the jump is held
                byte *jumpOffset = nullptr;
                for (int i = 0; i < caseNum; i++) {
                    if (val == READ_CONSTANT_LONG()) {
                        jumpOffset = offset + (i * 2);
                        break;
                    }
                }
                if (!jumpOffset) jumpOffset = offset + caseNum * 2;
                ip = jumpOffset;
                uInt jmp = READ_SHORT();
                ip += jmp;
                DISPATCH();
            }
            #pragma endregion

            #pragma region Functions
            case +OpCode::CALL: {
                // How many values are on the stack right now
                int argCount = READ_BYTE();
                STORE_FRAME();
                callValue(peek(argCount), argCount);
                // If the call is successful, there is a new call frame, so we need to update locals
                LOAD_FRAME();
                DISPATCH();
            }

            case +OpCode::RETURN: {
                Value result = pop();
                frameCount--;
                // If we're returning from the implicit function
                if (frameCount == 0) {
                    // Main thread doesn't have a future nor does it need to delete the thread
                    if (fut == nullptr) return;

                    // If this is a child thread that has a future attached to it, assign the value to the future
                    fut->val = result;
                    deleteThread(fut, vm);
                    return;
                }
                stackTop = slotStart;
                push(result);
                // Update locals with the values of the frame below
                LOAD_FRAME();
                DISPATCH();
            }

            case +OpCode::CLOSURE: {
                auto *closure = new object::ObjClosure(asFunction(READ_CONSTANT()));
                for (auto &upval: closure->upvals) {
                    uint8_t isLocal = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    if (isLocal) {
                        upval = captureUpvalue(slotStart + index);
                    } else {
                        upval = frame->closure->upvals[index];
                    }
                }
                push(encodeObj(closure));
                DISPATCH();
            }
            case +OpCode::CLOSURE_LONG: {
                auto *closure = new object::ObjClosure(asFunction(READ_CONSTANT_LONG()));
                for (auto &upval: closure->upvals) {
                    uint8_t isLocal = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    if (isLocal) {
                        upval = captureUpvalue(slotStart + index);
                    } else {
                        upval = frame->closure->upvals[index];
                    }
                }
                push(encodeObj(closure));
                DISPATCH();
            }
            #pragma endregion

            #pragma region Multithreading
            case +OpCode::LAUNCH_ASYNC: {
                byte argCount = READ_BYTE();
                auto *t = new Thread(vm);
                auto *newFut = new object::ObjFuture(t);
                // Ensures that ObjFuture tied to this thread lives long enough for the thread to finish execution
                t->copyVal(encodeObj(newFut));
                // Copies the function being called and the arguments
                t->startThread(&stackTop[-1 - argCount], argCount + 1);
                stackTop -= argCount + 1;
                {
                    // Only one thread can add/remove a new child thread at any time
                    std::lock_guard<std::mutex> lk(vm->mtx);
                    vm->childThreads.push_back(t);
                }
                newFut->startParallelExecution();
                push(encodeObj(newFut));
                DISPATCH();
            }

            case +OpCode::AWAIT: {
                Value val = pop();
                if (!isFuture(val))
                    runtimeError(fmt::format("Await can only be applied to a future, got {}", typeToStr(val)), 3);
                object::ObjFuture *futToAwait = asFuture(val);
                futToAwait->fut.wait();
                // Immediately delete the thread object to conserve memory
                deleteThread(futToAwait, vm);
                // Can safely access fut->val from this thread since the value is being read and won't be written to again
                push(futToAwait->val);
                DISPATCH();
            }
            #pragma endregion

            #pragma region Objects, arrays and maps
            case +OpCode::CREATE_ARRAY: {
                uInt64 size = READ_BYTE();
                uInt64 i = 0;
                auto *arr = new object::ObjArray(size);
                while (i < size) {
                    //size-i to because the values on the stack are in reverse order compared to how they're supposed to be in a array
                    Value val = pop();
                    //if numOfHeapPtr is 0 we don't trace or update the array when garbage collecting
                    if (isObj(val)) arr->numOfHeapPtr++;

                    arr->values[size - i - 1] = val;
                    i++;
                }
                push(encodeObj(arr));
                DISPATCH();
            }

            case +OpCode::GET: {
                // Structs and objects also get their own +OpCode::GET_PROPERTY operator for access using '.'
                // Use peek because in case this is a get call to a instance that has a defined "access" method
                // We want to use these 2 values as args and receiver
                Value field = pop();
                Value callee = pop();

                if (isArray(callee)) {
                    object::ObjArray *arr = asArray(callee);
                    uInt64 index = checkArrayBounds(this, field, callee, arr);
                    push(arr->values[index]);
                    DISPATCH();
                    // Only structs can be access with [](eg. struct["field"]
                }else if(isInstance(callee) && !asInstance(callee)->klass) {
                    if (!isString(field)) { runtimeError(fmt::format("Expected a string for field name, got {}.", typeToStr(field)), 3); }

                    object::ObjInstance *instance = asInstance(callee);
                    object::ObjString *name = asString(field);
                    auto it = instance->fields.find(name->str);
                    if (it != instance->fields.end()) {
                        push(it->second);
                        DISPATCH();
                    }
                    runtimeError(fmt::format("Field '{}' doesn't exist.", name->str), 4);
                }
                runtimeError(fmt::format("Expected an array or struct, got {}.", typeToStr(callee)), 3);
            }

            case +OpCode::SET: {
                //structs and objects also get their own +OpCode::SET_PROPERTY operator for setting using '.'
                Value field = pop();
                Value callee = pop();
                Value val = peek(0);

                if (isArray(callee)) {
                    object::ObjArray *arr = asArray(callee);
                    uInt64 index = checkArrayBounds(this, field, callee, arr);

                    //if numOfHeapPtr is 0 we don't trace or update the array when garbage collecting
                    if (isObj(val) && !isObj(arr->values[index])) arr->numOfHeapPtr++;
                    else if (!isObj(val) && isObj(arr->values[index])) arr->numOfHeapPtr--;
                    arr->values[index] = val;
                    DISPATCH();
                }else if(isInstance(callee) && !asInstance(callee)->klass) {
                    if (!isString(field)) { runtimeError(fmt::format("Expected a string for field name, got {}.", typeToStr(field)), 3); }

                    object::ObjInstance *instance = asInstance(callee);
                    object::ObjString *str = asString(field);
                    //setting will always succeed, and we don't care if we're overriding an existing field, or creating a new one
                    instance->fields.insert_or_assign(str->str, val);
                    DISPATCH();
                }
                runtimeError(fmt::format("Expected an array or struct, got {}.", typeToStr(callee)), 3);
            }

            case +OpCode::CLASS: {
                push(encodeObj(new object::ObjClass(READ_STRING_LONG()->str)));
                DISPATCH();
            }

            case +OpCode::GET_PROPERTY: {
                Value inst = pop();
                object::ObjString *name = READ_STRING();

                if (isInstance(inst)) {
                    object::ObjInstance *instance = asInstance(inst);
                    auto it = instance->fields.find(name->str);
                    if (it != instance->fields.end()) {
                        push(it->second);
                        DISPATCH();
                    }
                    if (instance->klass && bindMethod(instance->klass, name->str)) DISPATCH();
                }
                bindMethodToPrimitive(inst, name->str);
                DISPATCH();
            }
            case +OpCode::GET_PROPERTY_LONG: {
                Value inst = pop();
                object::ObjString *name = READ_STRING_LONG();

                if (isInstance(inst)) {
                    object::ObjInstance *instance = asInstance(inst);
                    auto it = instance->fields.find(name->str);
                    if (it != instance->fields.end()) {
                        push(it->second);
                        DISPATCH();
                    }
                    if (instance->klass && bindMethod(instance->klass, name->str)) DISPATCH();
                }
                bindMethodToPrimitive(inst, name->str);
                DISPATCH();
            }

            case +OpCode::SET_PROPERTY: {
                Value inst = pop();
                if (!isInstance(inst)) {
                    runtimeError(fmt::format("Only instances/structs have properties, got {}.", typeToStr(inst)), 3);
                }
                object::ObjInstance *instance = asInstance(inst);

                //we don't care if we're overriding or creating a new field
                instance->fields.insert_or_assign(READ_STRING()->str, peek(0));
                DISPATCH();
            }
            case +OpCode::SET_PROPERTY_LONG: {
                Value inst = pop();
                if (!isInstance(inst)) {
                    runtimeError(fmt::format("Only instances/structs have properties, got {}.", typeToStr(inst)), 3);
                }
                object::ObjInstance *instance = asInstance(inst);

                //we don't care if we're overriding or creating a new field
                instance->fields.insert_or_assign(READ_STRING_LONG()->str, peek(0));
                DISPATCH();
            }

            case +OpCode::CREATE_STRUCT: {
                int numOfFields = READ_BYTE();

                //passing null instead of class signals to the VM that this is a struct, and not a instance of a class
                auto *inst = new object::ObjInstance(nullptr);

                //the compiler emits the fields in reverse order, so we can loop through them normally and pop the values on the stack
                for (int i = 0; i < numOfFields; i++) {
                    object::ObjString *name = READ_STRING();
                    inst->fields.insert_or_assign(name->str, pop());
                }
                push(encodeObj(inst));
                DISPATCH();
            }
            case +OpCode::CREATE_STRUCT_LONG: {
                int numOfFields = READ_BYTE();

                //passing null instead of class signals to the VM that this is a struct, and not a instance of a class
                auto *inst = new object::ObjInstance(nullptr);

                //the compiler emits the fields in reverse order, so we can loop through them normally and pop the values on the stack
                for (int i = 0; i < numOfFields; i++) {
                    object::ObjString *name = READ_STRING_LONG();
                    inst->fields.insert_or_assign(name->str, pop());
                }
                push(encodeObj(inst));
                DISPATCH();
            }

            case +OpCode::METHOD: {
                //class that this method binds too
                defineMethod(READ_STRING_LONG()->str);
                DISPATCH();
            }

            case +OpCode::INVOKE: {
                //gets the method and calls it immediately, without converting it to a objBoundMethod
                int argCount = READ_BYTE();
                object::ObjString *method = READ_STRING();
                STORE_FRAME();
                invoke(method->str, argCount);
                LOAD_FRAME();
                DISPATCH();
            }
            case +OpCode::INVOKE_LONG: {
                //gets the method and calls it immediately, without converting it to a objBoundMethod
                int argCount = READ_BYTE();
                object::ObjString *method = READ_STRING_LONG();
                STORE_FRAME();
                invoke(method->str, argCount);
                LOAD_FRAME();
                DISPATCH();
            }

            case +OpCode::INHERIT: {
                Value superclass = peek(1);
                if (!isClass(superclass)) {
                    runtimeError(fmt::format("Superclass must be a class, got {}.", typeToStr(superclass)), 3);
                }
                object::ObjClass *subclass = asClass(peek(0));
                //copy down inheritance
                // TODO: Inefficient?
                for (auto it: asClass(superclass)->methods) {
                    subclass->methods.insert_or_assign(it.first, it.second);
                }
                DISPATCH();
            }

            case +OpCode::GET_SUPER: {
                //super is ALWAYS followed by a field
                object::ObjString *name = READ_STRING();
                object::ObjClass *superclass = asClass(pop());

                if(!bindMethod(superclass, name->str)) {
                    runtimeError(fmt::format("{} doesn't contain method '{}'", superclass->name, name->str), 4);
                }
                DISPATCH();
            }
            case +OpCode::GET_SUPER_LONG: {
                //super is ALWAYS followed by a field
                object::ObjString *name = READ_STRING_LONG();
                object::ObjClass *superclass = asClass(pop());

                if(!bindMethod(superclass, name->str)) {
                    runtimeError(fmt::format("{} doesn't contain method '{}'", superclass->name, name->str), 4);
                }
                DISPATCH();
            }

            case +OpCode::SUPER_INVOKE: {
                //works same as +OpCode::INVOKE, but uses invokeFromClass() to specify the superclass
                int argCount = READ_BYTE();
                object::ObjString *method = READ_STRING();
                object::ObjClass *superclass = asClass(pop());
                STORE_FRAME();
                if(!invokeFromClass(superclass, method->str, argCount)) {
                    runtimeError(fmt::format("{} doesn't contain method '{}'.", superclass->name, method->str), 4);
                }
                LOAD_FRAME();
                DISPATCH();
            }
            case +OpCode::SUPER_INVOKE_LONG: {
                //works same as +OpCode::INVOKE, but uses invokeFromClass() to specify the superclass
                int argCount = READ_BYTE();
                object::ObjString *method = READ_STRING_LONG();
                object::ObjClass *superclass = asClass(pop());
                STORE_FRAME();
                if(!invokeFromClass(superclass, method->str, argCount)) {
                    runtimeError(fmt::format("{} doesn't contain method '{}'.", superclass->name, method->str), 4);
                }
                LOAD_FRAME();
                DISPATCH();
            }
            #pragma endregion
        }
    } catch(int errCode) {
        frame->ip = ip;
        auto cyan = fmt::fg(fmt::color::cyan);
        auto white = fmt::fg(fmt::color::white);
        auto red = fmt::fg(fmt::color::red);
        auto yellow = fmt::fg(fmt::color::yellow);
        std::cout<<fmt::format("{} \n{}\n", fmt::styled("Runtime error: ", red), errorString);
        //prints callstack
        for (int i = frameCount - 1; i >= 0; i--) {
            CallFrame* frame = &frames[i];
            object::ObjFunc* function = frame->closure->func;
            // Converts ip from a pointer to a index in the array
            uInt64 instruction = (frame->ip - 1) - vm->code.bytecode.data();
            codeLine line = vm->code.getLine(instruction);
            //fileName:line | in <func name>
            std::cout<<fmt::format("{}:{} | in {}\n",
                                   fmt::styled(line.getFileName(vm->sourceFiles), yellow),
                                   fmt::styled(std::to_string(line.line + 1), cyan),
                                   (function->name.length() == 0 ? "script" : function->name));
        }
        fmt::print("\nExited with code: {}\n", errCode);
    }
	#undef READ_BYTE
	#undef READ_SHORT
	#undef READ_CONSTANT
	#undef READ_CONSTANT_LONG
	#undef READ_STRING
	#undef READ_STRING_LONG
	#undef BINARY_OP
	#undef INT_BINARY_OP
}
