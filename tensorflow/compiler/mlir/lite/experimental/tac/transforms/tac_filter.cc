/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <algorithm>
#include <functional>
#include <memory>
#include <string>

#include "google/protobuf/text_format.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Regex.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/Diagnostics.h"  // from @llvm-project
#include "mlir/IR/Location.h"  // from @llvm-project
#include "mlir/Interfaces/CallInterfaces.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Pass/PassRegistry.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/lite/experimental/tac/common/targets.h"
#include "tensorflow/compiler/mlir/lite/experimental/tac/common/utils.h"
#include "tensorflow/compiler/mlir/lite/experimental/tac/tac_filter.pb.h"
#include "tensorflow/compiler/mlir/lite/experimental/tac/transforms/passes.h"
#include "tensorflow/compiler/mlir/lite/utils/utils.h"

namespace mlir {
namespace TFL {
namespace tac {
namespace {

using ::third_party::tensorflow::compiler::mlir::lite::experimental::tac::
    FunctionFilter;
using ::third_party::tensorflow::compiler::mlir::lite::experimental::tac::
    OpFilter;
using ::third_party::tensorflow::compiler::mlir::lite::experimental::tac::
    TacFilter;
using ::third_party::tensorflow::compiler::mlir::lite::experimental::tac::
    TacFilters;

class TacFilterPass
    : public PassWrapper<TacFilterPass, OperationPass<ModuleOp>> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(TacFilterPass)

  TacFilterPass() = default;
  TacFilterPass(const TacFilterPass& other) {
    this->tac_filters_ = other.tac_filters_;
  }
  explicit TacFilterPass(
      TacFilters* tac_filters,
      std::function<void(Operation*, const google::protobuf::Any&)>
          custom_options_callback) {
    tac_filters_ = tac_filters;
    custom_options_callback_ = custom_options_callback;
  }

 private:
  TacFilters* tac_filters_ = nullptr;

  llvm::StringRef getArgument() const final { return "tfl-tac-filter"; }
  llvm::StringRef getDescription() const final {
    return "This pass marks the ops to skip target annotation by inserting "
           "`tac.skip_target_annotation` attribute to them based on user "
           "provided config.";
  }

  Option<bool> use_test_setting_{
      *this, "use-test-setting",
      llvm::cl::desc(
          "Whether to use the test config for the tac filter protobuf."),
      llvm::cl::init(false)};

  std::function<void(Operation*, const google::protobuf::Any&)>
      custom_options_callback_;

  void runOnOperation() override;
};

void ApplyFunctionTacFilter(func::FuncOp func,
                            FunctionFilter::FunctionFilterType type,
                            OpBuilder& builder) {
  for (Operation& op : func.front()) {
    if (type == FunctionFilter::SKIP_TARGET_ANNOTATION) {
      op.setAttr(kSkipTargetAnnotation, builder.getUnitAttr());
    } else if (type == FunctionFilter::INCLUDE_TARGET_ANNOTATION) {
      op.removeAttr(kSkipTargetAnnotation);
    }
  }
}

void ApplyTacFilter(
    ModuleOp module, const TacFilter& tac_filter,
    SmallVector<Operation*>& filtered_ops, OpBuilder& builder,
    std::function<void(Operation*, const google::protobuf::Any&)>
        custom_options_callback) {
  if (tac_filter.has_function_filter()) {
    llvm::Regex func_regex(
        tac_filter.function_filter().function_name_pattern());
    for (auto func : module.getOps<func::FuncOp>()) {
      if (!func_regex.match(func.getName())) {
        continue;
      }

      ApplyFunctionTacFilter(func, tac_filter.function_filter().filter_type(),
                             builder);
      filtered_ops.push_back(func);
    }
    return;
  }

  auto should_filter_op = [](mlir::Operation* op) {
    return IsNonConstOp(op) && !IsTerminatorOp(op) &&
           !llvm::isa<func::ReturnOp, func::FuncOp, CallOpInterface>(op);
  };

  auto map_op_to_cpu = [&](mlir::Operation* op) {
    if (!should_filter_op(op)) {
      return;
    }
    op->setAttr(kSkipTargetAnnotation, builder.getUnitAttr());
    filtered_ops.push_back(op);
  };

  auto map_op_to_custom_device = [&](mlir::Operation* op) {
    if (!should_filter_op(op)) {
      return;
    }
    if (!tac_filter.op_filter().has_custom_options()) {
      return;
    }

    const google::protobuf::Any& custom_options =
        tac_filter.op_filter().custom_options();
    custom_options_callback(op, custom_options);
  };

  llvm::Regex op_regex(tac_filter.op_filter().op_name_pattern());
  OpFilter::MatchType match_type = tac_filter.op_filter().match_type();
  OpFilter::DeviceType device_type = tac_filter.op_filter().device_type();
  module.walk([&](Operation* op) {
    NameLoc loc;
    if (auto name_loc = mlir::dyn_cast<NameLoc>(op->getLoc())) {
      loc = name_loc;
    } else if (auto fused_loc = mlir::dyn_cast<FusedLoc>(op->getLoc())) {
      loc = dyn_cast<NameLoc>(fused_loc.getLocations().front());
    }

    if (!loc) {
      return;
    }
    // There can be two kinds of `match_type`:
    // 1. MATCH: the op name matches the pattern.
    // 2. INVERT_MATCH: the op name does not match the pattern.
    //
    // On each of the above, the op filter can specify if it should be run on
    // the CPU or on a custom device, with the `device_type` field.
    // Running on CPU and the match type MATCH is the default if not specified.
    //
    // The code below maps an op to the appropriate device based on the above
    // fields.
    if (op_regex.match(loc.getName())) {
      switch (match_type) {
        case OpFilter::MATCH:
          if (device_type == OpFilter::CPU) {
            map_op_to_cpu(op);
            return;
          }
          map_op_to_custom_device(op);
          break;
        default:
          break;
      }
    } else {
      switch (match_type) {
        case OpFilter::INVERT_MATCH:
          if (device_type == OpFilter::CPU) {
            map_op_to_cpu(op);
            return;
          }
          map_op_to_custom_device(op);
          break;
        default:
          break;
      }
    }
  });
}

// A custom string for tac filter.
std::string TacFilterToString(const TacFilter& tac_filter) {
  std::string tac_filter_type_str;
  std::string tac_filter_name_pattern_str;
  if (tac_filter.has_function_filter()) {
    tac_filter_type_str = (llvm::Twine("function filter ") +
                           FunctionFilter::FunctionFilterType_Name(
                               tac_filter.function_filter().filter_type()))
                              .str();
    tac_filter_name_pattern_str =
        tac_filter.function_filter().function_name_pattern();
  } else {
    tac_filter_type_str = "op filter";
    tac_filter_name_pattern_str = tac_filter.op_filter().op_name_pattern();
  }
  return (llvm::Twine("filter type: ") + tac_filter_type_str +
          ", filter_pattern: \"" + tac_filter_name_pattern_str + "\"")
      .str();
}

void PrintTacFilterResult(Location module_loc, const TacFilter& tac_filter,
                          int count,
                          const SmallVector<Operation*>& filtered_ops) {
  emitRemark(module_loc) << llvm::formatv("Tac filter ({0}): {1}", count,
                                          TacFilterToString(tac_filter));
  if (filtered_ops.empty()) {
    emitRemark(module_loc) << llvm::formatv(
        "Tac filter ({0}) specified but not applied to any op", count);
    return;
  }

  if (tac_filter.has_function_filter()) {
    for (Operation* op : filtered_ops) {
      auto func = cast<func::FuncOp>(op);
      func.emitRemark() << llvm::formatv("filtered by tac filter ({0})", count);
    }
    return;
  }

  DenseMap<func::FuncOp, SmallVector<Operation*>> func_to_filtered_ops_map;
  for (Operation* op : filtered_ops) {
    auto func = op->getParentOfType<func::FuncOp>();
    func_to_filtered_ops_map[func].push_back(op);
  }
  for (auto& [func, ops] : func_to_filtered_ops_map) {
    std::string interleaved_op_name;
    llvm::raw_string_ostream os(interleaved_op_name);
    llvm::interleaveComma(
        ops, os, [&](Operation* op) { os << "\"" << op->getName() << "\""; });
    os.flush();
    func.emitRemark() << llvm::formatv(
        "all ops filtered by tac filter ({0}): {1}", count,
        interleaved_op_name);
  }
}

void TacFilterPass::runOnOperation() {
  TacFilters test_tac_filters;
  if (use_test_setting_) {
    // Sets up the test config used in the mlir LIT test.
    google::protobuf::TextFormat::ParseFromString(R"(
      tac_filters {
        function_filter {
          function_name_pattern: "^testFunction"
        }
      }
      tac_filters {
        function_filter {
          function_name_pattern: "testFunctionInclude"
          filter_type: INCLUDE_TARGET_ANNOTATION
        }
      }
      tac_filters {
        op_filter {
          op_name_pattern: "^test_op"
        }
      }
    )",
                                        &test_tac_filters);
    tac_filters_ = &test_tac_filters;
  }

  if (!tac_filters_) {
    return;
  }

  ModuleOp module = getOperation();
  OpBuilder builder(module);
  std::sort(tac_filters_->mutable_tac_filters()->pointer_begin(),
            tac_filters_->mutable_tac_filters()->pointer_end(),
            [](const TacFilter* a, const TacFilter* b) {
              const bool a_is_function_filter = a->has_function_filter();
              const bool b_is_function_filter = b->has_function_filter();
              if (a_is_function_filter != b_is_function_filter) {
                // Function filter is applied before op filter.
                return a_is_function_filter > b_is_function_filter;
              }

              if (!a_is_function_filter && !b_is_function_filter) {
                // The order of 2 op filters don't matter.
                return false;
              }

              const bool a_is_function_exclude =
                  (a->function_filter().filter_type() ==
                   FunctionFilter::SKIP_TARGET_ANNOTATION);
              const bool b_is_function_exclude =
                  (b->function_filter().filter_type() ==
                   FunctionFilter::SKIP_TARGET_ANNOTATION);
              // Function exclude filter is applied before function include
              // filter.
              return a_is_function_exclude > b_is_function_exclude;
            });

  for (const auto& tac_filter : llvm::enumerate(tac_filters_->tac_filters())) {
    SmallVector<Operation*> filtered_ops;
    ApplyTacFilter(module, tac_filter.value(), filtered_ops, builder,
                   custom_options_callback_);
    PrintTacFilterResult(module.getLoc(), tac_filter.value(),
                         tac_filter.index(), filtered_ops);
  }
}

}  // namespace

std::unique_ptr<OperationPass<ModuleOp>> CreateTacFilterPass(
    TacFilters* tac_filters,
    std::function<void(Operation*, const google::protobuf::Any&)>
        custom_options_callback) {
  return std::make_unique<TacFilterPass>(tac_filters, custom_options_callback);
}

static PassRegistration<TacFilterPass> pass;

}  // namespace tac
}  // namespace TFL
}  // namespace mlir
