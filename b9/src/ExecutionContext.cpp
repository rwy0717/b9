#include <b9/ExecutionContext.hpp>
#include <b9/VirtualMachine.hpp>
#include <b9/compiler/Compiler.hpp>

#include <omrgc.h>
#include "Jit.hpp"

#include <OMR/Om/ArrayOperations.hpp>
#include <OMR/Om/ShapeOperations.hpp>
#include <OMR/Om/ObjectOperations.hpp>
#include <OMR/Om/RootRef.hpp>
#include <OMR/Om/Value.hpp>

#include <sys/time.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

namespace b9 {

ExecutionContext::ExecutionContext(VirtualMachine &virtualMachine,
                                   const Config &cfg)
    : omContext_(virtualMachine.memoryManager()),
      virtualMachine_(&virtualMachine),
      cfg_(&cfg) {
  omContext().userMarkingFns().push_back(
      [this](Om::MarkingVisitor &v) { this->visit(v); });
}

void ExecutionContext::reset() {
  stack_.reset();
}

Om::Value ExecutionContext::callJitFunction(JitFunction jitFunction,
                                            std::size_t nargs) {
  if (cfg_->verbose) {
    std::cout << "Int: transition to jit: " << jitFunction << std::endl;
  }

  Om::RawValue result = 0;

  if (cfg_->passParam) {
    switch (nargs) {
      case 0: {
        result = jitFunction(this);
      } break;
      case 1: {
        StackElement p1 = pop();
        result = jitFunction(this, p1.raw());
      } break;
      case 2: {
        StackElement p2 = pop();
        StackElement p1 = pop();
        result = jitFunction(this, p1.raw(), p2.raw());
      } break;
      case 3: {
        StackElement p3 = pop();
        StackElement p2 = pop();
        StackElement p1 = pop();
        result = (*jitFunction)(this, p1.raw(), p2.raw(), p3.raw());
      } break;
      default:
        throw std::runtime_error{"Need to add handlers for more parameters"};
        break;
    }
  } else {
    result = jitFunction(this);
  }

  return Om::Value(Om::AS_RAW, result);
}

StackElement ExecutionContext::run(std::size_t target, std::vector<StackElement> arguments) {
  auto& callee = getFunction(target);
  assert(callee.nargs == arguments.size());

  for (auto arg : arguments) {
    stack_.push(arg);
  }
  enterCall(target);
  interpret();
  return stack_.pop();
}

StackElement ExecutionContext::run(std::size_t target) {
  auto& callee = getFunction(target);
  assert(callee.nargs == 0);

  enterCall(target);
  interpret();
  return stack_.pop();
}

void ExecutionContext::interpret() {
  while (*ip_ != END_SECTION) {
    switch (ip_->opCode()) {
      case OpCode::FUNCTION_CALL:
        doFunctionCall();
        break;
      case OpCode::FUNCTION_RETURN:
        doFunctionReturn();
        break;
      case OpCode::PRIMITIVE_CALL:
        doPrimitiveCall();
        break;
      case OpCode::JMP:
        doJmp();
        break;
      case OpCode::DUPLICATE:
        doDuplicate();
        break;
      case OpCode::DROP:
        doDrop();
        break;
      case OpCode::PUSH_FROM_VAR:
        doPushFromVar();
        break;
      case OpCode::POP_INTO_VAR:
        doPopIntoVar();
        break;
      case OpCode::INT_ADD:
        doIntAdd();
        break;
      case OpCode::INT_SUB:
        doIntSub();
        break;
      case OpCode::INT_MUL:
        doIntMul();
        break;
      case OpCode::INT_DIV:
        doIntDiv();
        break;
      case OpCode::INT_PUSH_CONSTANT:
        doIntPushConstant();
        break;
      case OpCode::INT_NOT:
        doIntNot();
        break;
      case OpCode::INT_JMP_EQ:
        doIntJmpEq();
        break;
      case OpCode::INT_JMP_NEQ:
        doIntJmpNeq();
        break;
      case OpCode::INT_JMP_GT:
        doIntJmpGt();
        break;
      case OpCode::INT_JMP_GE:
        doIntJmpGe();
        break;
      case OpCode::INT_JMP_LT:
        doIntJmpLt();
        break;
      case OpCode::INT_JMP_LE:
        doIntJmpLe();
        break;
      case OpCode::STR_PUSH_CONSTANT:
        doStrPushConstant();
        break;
      case OpCode::NEW_OBJECT:
        doNewObject();
        break;
      case OpCode::PUSH_FROM_OBJECT:
        doPushFromObject();
        break;
      case OpCode::POP_INTO_OBJECT:
        doPopIntoObject();
        break;
      case OpCode::CALL_INDIRECT:
        doCallIndirect();
        break;
      case OpCode::SYSTEM_COLLECT:
        doSystemCollect();
        break;
      default:
        assert(false);
        break;
    }
  }
  throw std::runtime_error("Reached end of function");
}

void ExecutionContext::enterCall(std::size_t target) {
  const FunctionDef& callee = getFunction(target);

  // reserve space for locals (args are already pushed)
  stack_.pushn(callee.nargs);

  // save caller state
  stack_.push({Om::AS_UINT48, fn_});
  stack_.push({Om::AS_PTR,    ip_});
  stack_.push({Om::AS_PTR,    bp_});

  // set up state for callee
  fn_ = target;
  ip_ = callee.instructions.data();
  bp_ = stack_.top();
}

void ExecutionContext::exitCall() {
  const FunctionDef& callee = getFunction(fn_);

  // pop callee scratch space
  stack_.restore(bp_);

  // restore caller state. note IP is restored verbatim, not incremented.
  bp_ = stack_.pop().getPtr<Om::Value>();
  ip_ = stack_.pop().getPtr<Instruction>();
  fn_ = stack_.pop().getUint48();

  // pop parameters and locals
  stack_.popn(callee.nargs + callee.nregs);
}

void ExecutionContext::doFunctionCall() {
  enterCall(ip_->immediate());
}

void ExecutionContext::doFunctionReturn() {
  StackElement result = stack_.pop();
  exitCall();
  stack_.push(result);
  ++ip_;
}

void ExecutionContext::doPrimitiveCall() {
  Immediate index = ip_->immediate();
  PrimitiveFunction *primitive = virtualMachine_->getPrimitive(index);
  (*primitive)(this);
  ++ip_;
}

void ExecutionContext::doJmp() {
  ip_ += ip_->immediate() + 1;
}

void ExecutionContext::doDuplicate() {
  stack_.push(stack_.peek());
  ++ip_;
}

void ExecutionContext::doDrop() {
  stack_.pop();
  ++ip_;
}

void ExecutionContext::doPushFromVar() {
  const FunctionDef& callee = getFunction(fn_);
  Om::Value* args = bp_ - (3 + callee.nargs + callee.nregs); // TODO: Improve variable indexing
  Immediate index = ip_->immediate();
  stack_.push(args[index]);
  ++ip_;
}

void ExecutionContext::doPopIntoVar() {
  const FunctionDef& callee = getFunction(fn_);
  Om::Value* args = bp_ - (3 + callee.nargs + callee.nregs); // TODO: Improve variable indexing
  Immediate index = ip_->immediate();
  args[index] = stack_.pop();
  ++ip_;
}

void ExecutionContext::doIntAdd() {
  auto right = stack_.pop().getInt48();
  auto left = stack_.pop().getInt48();
  push({Om::AS_INT48, left + right});
}

void ExecutionContext::doIntSub() {
  auto right = stack_.pop().getInt48();
  auto left = stack_.pop().getInt48();
  push({Om::AS_INT48, left - right});
}

void ExecutionContext::doIntMul() {
  auto right = stack_.pop().getInt48();
  auto left = stack_.pop().getInt48();
  push({Om::AS_INT48, left * right});
}

void ExecutionContext::doIntDiv() {
  auto right = stack_.pop().getInt48();
  auto left = stack_.pop().getInt48();
  push({Om::AS_INT48, left / right});
}

void ExecutionContext::doIntPushConstant() {
  stack_.push({Om::AS_INT48, ip_->immediate()});
  ++ip_;
}

void ExecutionContext::doIntNot() {
  auto x = stack_.pop().getInt48();
  push({Om::AS_INT48, !x});
  ++ip_;
}

void ExecutionContext::doIntJmpEq() {
  auto right = stack_.pop().getInt48();
  auto left = stack_.pop().getInt48();
  if (left == right) {
    ip_ += ip_->immediate() + 1;
  }
}

void ExecutionContext::doIntJmpNeq() {
  auto right = stack_.pop().getInt48();
  auto left = stack_.pop().getInt48();
  if (left != right) {
    ip_ += ip_->immediate() + 1;
  }
  else {
    ++ip_;
  }
}

void ExecutionContext::doIntJmpGt() {
  auto right = stack_.pop().getInt48();
  auto left = stack_.pop().getInt48();
  if (left > right) {
    ip_ += ip_->immediate() + 1;
  }
  else {
    ++ip_;
  }
}

// ( left right -- )
void ExecutionContext::doIntJmpGe() {
  auto right = stack_.pop().getInt48();
  auto left = stack_.pop().getInt48();
  if (left >= right) {
    ip_ += ip_->immediate() + 1;
  }
  else {
    ++ip_;
  }
}

// ( left right -- )
void ExecutionContext::doIntJmpLt() {
  std::int32_t right = stack_.pop().getInt48();
  std::int32_t left = stack_.pop().getInt48();
  if (left < right) {
    ip_ += ip_->immediate() + 1;
  }
  else {
    ++ip_;
  }
}

// ( left right -- )
void ExecutionContext::doIntJmpLe() {
  auto right = stack_.pop().getInt48();
  auto left = stack_.pop().getInt48();
  if (left <= right) {
    ip_ += ip_->immediate() + 1;
  }
  else {
    ++ip_;
  }
}

// ( -- string )
void ExecutionContext::doStrPushConstant() {
  stack_.push({Om::AS_INT48, ip_->immediate()});
  ++ip_;
}

// ( -- object )
void ExecutionContext::doNewObject() {
  auto ref = Om::allocateEmptyObject(*this);
  stack_.push({Om::AS_REF, ref});
  ++ip_;
}

// ( object -- value )
void ExecutionContext::doPushFromObject() {
  Om::Id slotId(ip_->immediate());

  auto value = stack_.pop();
  if (!value.isRef()) {
    throw std::runtime_error("Accessing non-object value as an object.");
  }
  auto obj = value.getRef<Om::Object>();
  Om::SlotDescriptor descriptor;
  auto found = Om::lookupSlot(*this, obj, slotId, descriptor);
  if (found) {
    Om::Value result;
    result = Om::getValue(*this, obj, descriptor);
    stack_.push(result);
  } else {
    throw std::runtime_error("Accessing an object's field that doesn't exist.");
  }
  ++ip_;
}

// ( object value -- )
void ExecutionContext::doPopIntoObject() {
  Om::Id slotId(ip_->immediate());

  if (!stack_.peek().isRef()) {
    throw std::runtime_error("Accessing non-object as an object");
  }

  std::size_t offset = 0;
  auto object = stack_.pop().getRef<Om::Object>();

  Om::SlotDescriptor descriptor;
  bool found = Om::lookupSlot(*this, object, slotId, descriptor);

  if (!found) {
    static constexpr Om::SlotType type(Om::Id(0), Om::CoreType::VALUE);

    Om::RootRef<Om::Object> root(*this, object);
    auto map = Om::transitionLayout(*this, root, {{type, slotId}});
    assert(map != nullptr);

    // TODO: Get the descriptor fast after a single-slot transition.
    Om::lookupSlot(*this, object, slotId, descriptor);
    object = root.get();
  }

  auto val = pop();
  Om::setValue(*this, object, descriptor, val);
  // TODO: Write barrier the object on store.
  ++ip_;
}

void ExecutionContext::doCallIndirect() {
  assert(0);  // TODO: Implement call indirect
  ++ip_;
}

void ExecutionContext::doSystemCollect() {
  std::cout << "SYSTEM COLLECT!!!" << std::endl;
  OMR_GC_SystemCollect(omContext_.vmContext(), 0);
  ++ip_;
}

}  // namespace b9
