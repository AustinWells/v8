// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WASM_RUN_UTILS_H
#define WASM_RUN_UTILS_H

#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <array>
#include <memory>

#include "src/base/utils/random-number-generator.h"
#include "src/code-stubs.h"
#include "src/compiler/compiler-source-position-table.h"
#include "src/compiler/graph-visualizer.h"
#include "src/compiler/int64-lowering.h"
#include "src/compiler/js-graph.h"
#include "src/compiler/node.h"
#include "src/compiler/pipeline.h"
#include "src/compiler/wasm-compiler.h"
#include "src/compiler/zone-stats.h"
#include "src/trap-handler/trap-handler.h"
#include "src/wasm/function-body-decoder.h"
#include "src/wasm/local-decl-encoder.h"
#include "src/wasm/wasm-external-refs.h"
#include "src/wasm/wasm-interpreter.h"
#include "src/wasm/wasm-js.h"
#include "src/wasm/wasm-module.h"
#include "src/wasm/wasm-objects.h"
#include "src/wasm/wasm-opcodes.h"
#include "src/zone/accounting-allocator.h"
#include "src/zone/zone.h"

#include "test/cctest/cctest.h"
#include "test/cctest/compiler/call-tester.h"
#include "test/cctest/compiler/graph-builder-tester.h"
#include "test/common/wasm/flag-utils.h"

namespace v8 {
namespace internal {
namespace wasm {

constexpr uint32_t kMaxFunctions = 10;
constexpr uint32_t kMaxGlobalsSize = 128;

enum WasmExecutionMode { kExecuteInterpreted, kExecuteCompiled };

using compiler::CallDescriptor;
using compiler::MachineTypeForC;
using compiler::Node;

// TODO(titzer): check traps more robustly in tests.
// Currently, in tests, we just return 0xdeadbeef from the function in which
// the trap occurs if the runtime context is not available to throw a JavaScript
// exception.
#define CHECK_TRAP32(x) \
  CHECK_EQ(0xdeadbeef, (bit_cast<uint32_t>(x)) & 0xFFFFFFFF)
#define CHECK_TRAP64(x) \
  CHECK_EQ(0xdeadbeefdeadbeef, (bit_cast<uint64_t>(x)) & 0xFFFFFFFFFFFFFFFF)
#define CHECK_TRAP(x) CHECK_TRAP32(x)

#define WASM_WRAPPER_RETURN_VALUE 8754

#define BUILD(r, ...)                      \
  do {                                     \
    byte code[] = {__VA_ARGS__};           \
    r.Build(code, code + arraysize(code)); \
  } while (false)

// A buildable ModuleEnv. Globals are pre-set, however, memory and code may be
// progressively added by a test. In turn, we piecemeal update the runtime
// objects, i.e. {WasmInstanceObject}, {WasmCompiledModule} and, if necessary,
// the interpreter.
class TestingModuleBuilder {
 public:
  TestingModuleBuilder(Zone*, WasmExecutionMode,
                       compiler::RuntimeExceptionSupport);

  void ChangeOriginToAsmjs() { test_module_.set_origin(kAsmJsOrigin); }

  byte* AddMemory(uint32_t size);

  size_t CodeTableLength() const { return function_code_.size(); }

  template <typename T>
  T* AddMemoryElems(uint32_t count) {
    AddMemory(count * sizeof(T));
    return raw_mem_start<T>();
  }

  template <typename T>
  T* AddGlobal(
      ValueType type = WasmOpcodes::ValueTypeFor(MachineTypeForC<T>())) {
    const WasmGlobal* global = AddGlobal(type);
    return reinterpret_cast<T*>(globals_data_ + global->offset);
  }

  byte AddSignature(FunctionSig* sig) {
    test_module_.signatures.push_back(sig);
    size_t size = test_module_.signatures.size();
    CHECK(size < 127);
    return static_cast<byte>(size - 1);
  }

  template <typename T>
  T* raw_mem_start() {
    DCHECK(mem_start_);
    return reinterpret_cast<T*>(mem_start_);
  }

  template <typename T>
  T* raw_mem_end() {
    DCHECK(mem_start_);
    return reinterpret_cast<T*>(mem_start_ + mem_size_);
  }

  template <typename T>
  T raw_mem_at(int i) {
    DCHECK(mem_start_);
    return ReadMemory(&(reinterpret_cast<T*>(mem_start_)[i]));
  }

  template <typename T>
  T raw_val_at(int i) {
    return ReadMemory(reinterpret_cast<T*>(mem_start_ + i));
  }

  template <typename T>
  void WriteMemory(T* p, T val) {
    WriteLittleEndianValue<T>(p, val);
  }

  template <typename T>
  T ReadMemory(T* p) {
    return ReadLittleEndianValue<T>(p);
  }

  // Zero-initialize the memory.
  void BlankMemory() {
    byte* raw = raw_mem_start<byte>();
    memset(raw, 0, mem_size_);
  }

  // Pseudo-randomly intialize the memory.
  void RandomizeMemory(unsigned int seed = 88) {
    byte* raw = raw_mem_start<byte>();
    byte* end = raw_mem_end<byte>();
    v8::base::RandomNumberGenerator rng;
    rng.SetSeed(seed);
    rng.NextBytes(raw, end - raw);
  }

  void SetMaxMemPages(uint32_t maximum_pages) {
    test_module_.maximum_pages = maximum_pages;
  }

  void SetHasSharedMemory() { test_module_.has_shared_memory = true; }

  uint32_t AddFunction(FunctionSig* sig, Handle<Code> code, const char* name);

  uint32_t AddJsFunction(FunctionSig* sig, const char* source,
                         Handle<FixedArray> js_imports_table);

  Handle<JSFunction> WrapCode(uint32_t index);

  void SetFunctionCode(uint32_t index, Handle<Code> code) {
    function_code_[index] = code;
  }

  void AddIndirectFunctionTable(uint16_t* function_indexes,
                                uint32_t table_size);

  void PopulateIndirectFunctionTable();

  uint32_t AddBytes(Vector<const byte> bytes);

  WasmFunction* GetFunctionAt(int index) {
    return &test_module_.functions[index];
  }

  WasmInterpreter* interpreter() { return interpreter_; }
  bool interpret() { return interpreter_ != nullptr; }
  Isolate* isolate() { return isolate_; }
  Handle<WasmInstanceObject> instance_object() { return instance_object_; }
  Handle<Code> GetFunctionCode(int index) { return function_code_[index]; }
  void SetFunctionCode(int index, Handle<Code> code) {
    function_code_[index] = code;
  }
  Address globals_start() { return reinterpret_cast<Address>(globals_data_); }

  compiler::ModuleEnv CreateModuleEnv();

  compiler::RuntimeExceptionSupport runtime_exception_support() const {
    return runtime_exception_support_;
  }

 private:
  WasmModule test_module_;
  WasmModule* test_module_ptr_;
  Isolate* isolate_;
  uint32_t global_offset;
  byte* mem_start_;
  uint32_t mem_size_;
  std::vector<Handle<Code>> function_code_;
  std::vector<GlobalHandleAddress> function_tables_;
  std::vector<GlobalHandleAddress> signature_tables_;
  V8_ALIGNED(8) byte globals_data_[kMaxGlobalsSize];
  WasmInterpreter* interpreter_;
  Handle<WasmInstanceObject> instance_object_;
  compiler::RuntimeExceptionSupport runtime_exception_support_;

  const WasmGlobal* AddGlobal(ValueType type);

  Handle<WasmInstanceObject> InitInstanceObject();
};

void TestBuildingGraph(
    Zone* zone, compiler::JSGraph* jsgraph, compiler::ModuleEnv* module,
    FunctionSig* sig, compiler::SourcePositionTable* source_position_table,
    const byte* start, const byte* end,
    compiler::RuntimeExceptionSupport runtime_exception_support);

class WasmFunctionWrapper : private compiler::GraphAndBuilders {
 public:
  WasmFunctionWrapper(Zone* zone, int num_params);

  void Init(CallDescriptor* descriptor, MachineType return_type,
            Vector<MachineType> param_types);

  template <typename ReturnType, typename... ParamTypes>
  void Init(CallDescriptor* descriptor) {
    std::array<MachineType, sizeof...(ParamTypes)> param_machine_types{
        {MachineTypeForC<ParamTypes>()...}};
    Vector<MachineType> param_vec(param_machine_types.data(),
                                  param_machine_types.size());
    Init(descriptor, MachineTypeForC<ReturnType>(), param_vec);
  }

  void SetInnerCode(Handle<Code> code_handle) {
    compiler::NodeProperties::ChangeOp(inner_code_node_,
                                       common()->HeapConstant(code_handle));
  }

  Handle<Code> GetWrapperCode();

  Signature<MachineType>* signature() const { return signature_; }

 private:
  Node* inner_code_node_;
  Handle<Code> code_;
  Signature<MachineType>* signature_;
};

// A helper for compiling wasm functions for testing.
// It contains the internal state for compilation (i.e. TurboFan graph) and
// interpretation (by adding to the interpreter manually).
class WasmFunctionCompiler : public compiler::GraphAndBuilders {
 public:
  Isolate* isolate() { return builder_->isolate(); }
  CallDescriptor* descriptor() {
    if (descriptor_ == nullptr) {
      descriptor_ = compiler::GetWasmCallDescriptor(zone(), sig);
    }
    return descriptor_;
  }
  uint32_t function_index() { return function_->func_index; }

  void Build(const byte* start, const byte* end);

  byte AllocateLocal(ValueType type) {
    uint32_t index = local_decls.AddLocals(1, type);
    byte result = static_cast<byte>(index);
    DCHECK_EQ(index, result);
    return result;
  }

  void SetSigIndex(int sig_index) { function_->sig_index = sig_index; }

 private:
  friend class WasmRunnerBase;

  WasmFunctionCompiler(Zone* zone, FunctionSig* sig,
                       TestingModuleBuilder* builder, const char* name);

  compiler::JSGraph jsgraph;
  FunctionSig* sig;
  // The call descriptor is initialized when the function is compiled.
  CallDescriptor* descriptor_;
  TestingModuleBuilder* builder_;
  WasmFunction* function_;
  LocalDeclEncoder local_decls;
  compiler::SourcePositionTable source_position_table_;
  WasmInterpreter* interpreter_;
};

// A helper class to build a module around Wasm bytecode, generate machine
// code, and run that code.
class WasmRunnerBase : public HandleAndZoneScope {
 public:
  WasmRunnerBase(WasmExecutionMode execution_mode, int num_params,
                 compiler::RuntimeExceptionSupport runtime_exception_support)
      : zone_(&allocator_, ZONE_NAME),
        builder_(&zone_, execution_mode, runtime_exception_support),
        wrapper_(&zone_, num_params) {}

  // Builds a graph from the given Wasm code and generates the machine
  // code and call wrapper for that graph. This method must not be called
  // more than once.
  void Build(const byte* start, const byte* end) {
    CHECK(!compiled_);
    compiled_ = true;
    functions_[0]->Build(start, end);
  }

  // Resets the state for building the next function.
  // The main function called will always be the first function.
  template <typename ReturnType, typename... ParamTypes>
  WasmFunctionCompiler& NewFunction(const char* name = nullptr) {
    return NewFunction(CreateSig<ReturnType, ParamTypes...>(), name);
  }

  // Resets the state for building the next function.
  // The main function called will be the last generated function.
  // Returns the index of the previously built function.
  WasmFunctionCompiler& NewFunction(FunctionSig* sig,
                                    const char* name = nullptr) {
    functions_.emplace_back(
        new WasmFunctionCompiler(&zone_, sig, &builder_, name));
    return *functions_.back();
  }

  byte AllocateLocal(ValueType type) {
    return functions_[0]->AllocateLocal(type);
  }

  uint32_t function_index() { return functions_[0]->function_index(); }
  WasmFunction* function() { return functions_[0]->function_; }
  WasmInterpreter* interpreter() {
    DCHECK(interpret());
    return functions_[0]->interpreter_;
  }
  bool possible_nondeterminism() { return possible_nondeterminism_; }
  TestingModuleBuilder& builder() { return builder_; }
  Zone* zone() { return &zone_; }

  bool interpret() { return builder_.interpret(); }

  template <typename ReturnType, typename... ParamTypes>
  FunctionSig* CreateSig() {
    std::array<MachineType, sizeof...(ParamTypes)> param_machine_types{
        {MachineTypeForC<ParamTypes>()...}};
    Vector<MachineType> param_vec(param_machine_types.data(),
                                  param_machine_types.size());
    return CreateSig(MachineTypeForC<ReturnType>(), param_vec);
  }

 private:
  FunctionSig* CreateSig(MachineType return_type,
                         Vector<MachineType> param_types);

 protected:
  v8::internal::AccountingAllocator allocator_;
  Zone zone_;
  TestingModuleBuilder builder_;
  std::vector<std::unique_ptr<WasmFunctionCompiler>> functions_;
  WasmFunctionWrapper wrapper_;
  bool compiled_ = false;
  bool possible_nondeterminism_ = false;

 public:
  // This field has to be static. Otherwise, gcc complains about the use in
  // the lambda context below.
  static bool trap_happened;
};

template <typename ReturnType, typename... ParamTypes>
class WasmRunner : public WasmRunnerBase {
 public:
  WasmRunner(WasmExecutionMode execution_mode,
             const char* main_fn_name = "main",
             compiler::RuntimeExceptionSupport runtime_exception_support =
                 compiler::kNoRuntimeExceptionSupport)
      : WasmRunnerBase(execution_mode, sizeof...(ParamTypes),
                       runtime_exception_support) {
    NewFunction<ReturnType, ParamTypes...>(main_fn_name);
    if (!interpret()) {
      wrapper_.Init<ReturnType, ParamTypes...>(functions_[0]->descriptor());
    }
  }

  ReturnType Call(ParamTypes... p) {
    DCHECK(compiled_);
    if (interpret()) return CallInterpreter(p...);

    ReturnType return_value = static_cast<ReturnType>(0xdeadbeefdeadbeef);
    WasmRunnerBase::trap_happened = false;
    auto trap_callback = []() -> void {
      WasmRunnerBase::trap_happened = true;
      set_trap_callback_for_testing(nullptr);
    };
    set_trap_callback_for_testing(trap_callback);

    wrapper_.SetInnerCode(builder_.GetFunctionCode(0));
    compiler::CodeRunner<int32_t> runner(CcTest::InitIsolateOnce(),
                                         wrapper_.GetWrapperCode(),
                                         wrapper_.signature());
    int32_t result = runner.Call(static_cast<void*>(&p)...,
                                 static_cast<void*>(&return_value));
    CHECK_EQ(WASM_WRAPPER_RETURN_VALUE, result);
    return WasmRunnerBase::trap_happened
               ? static_cast<ReturnType>(0xdeadbeefdeadbeef)
               : return_value;
  }

  ReturnType CallInterpreter(ParamTypes... p) {
    WasmInterpreter::Thread* thread = interpreter()->GetThread(0);
    thread->Reset();
    std::array<WasmValue, sizeof...(p)> args{{WasmValue(p)...}};
    thread->InitFrame(function(), args.data());
    WasmInterpreter::HeapObjectsScope heap_objects_scope(
        interpreter(), builder().instance_object());
    if (thread->Run() == WasmInterpreter::FINISHED) {
      WasmValue val = thread->GetReturnValue();
      possible_nondeterminism_ |= thread->PossibleNondeterminism();
      return val.to<ReturnType>();
    } else if (thread->state() == WasmInterpreter::TRAPPED) {
      // TODO(titzer): return the correct trap code
      int64_t result = 0xdeadbeefdeadbeef;
      return static_cast<ReturnType>(result);
    } else {
      // TODO(titzer): falling off end
      return ReturnType{0};
    }
  }
};

// A macro to define tests that run in different engine configurations.
#define WASM_EXEC_TEST(name)                                               \
  void RunWasm_##name(WasmExecutionMode execution_mode);                   \
  TEST(RunWasmCompiled_##name) { RunWasm_##name(kExecuteCompiled); }       \
  TEST(RunWasmInterpreted_##name) { RunWasm_##name(kExecuteInterpreted); } \
  void RunWasm_##name(WasmExecutionMode execution_mode)

#define WASM_EXEC_TEST_WITH_TRAP(name)                   \
  void RunWasm_##name(WasmExecutionMode execution_mode); \
  TEST(RunWasmCompiled_##name) {                         \
    if (trap_handler::UseTrapHandler()) {                \
      return;                                            \
    }                                                    \
    RunWasm_##name(kExecuteCompiled);                    \
  }                                                      \
  TEST(RunWasmInterpreted_##name) {                      \
    if (trap_handler::UseTrapHandler()) {                \
      return;                                            \
    }                                                    \
    RunWasm_##name(kExecuteInterpreted);                 \
  }                                                      \
  void RunWasm_##name(WasmExecutionMode execution_mode)

}  // namespace wasm
}  // namespace internal
}  // namespace v8

#endif
