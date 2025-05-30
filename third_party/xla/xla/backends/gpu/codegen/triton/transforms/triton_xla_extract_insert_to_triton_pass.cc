/* Copyright 2025 The OpenXLA Authors.

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

#include <stdbool.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/log/check.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Analysis/SliceAnalysis.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/Value.h"
#include "mlir/IR/ValueRange.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "xla/backends/gpu/codegen/triton/emitter_helpers.h"
#include "xla/backends/gpu/codegen/triton/ir/triton_xla_ops.h"
#include "xla/backends/gpu/codegen/triton/transforms/passes.h"
#include "xla/codegen/emitter_loc_op_builder.h"
#include "xla/codegen/emitters/ir/xla_ops.h"
#include "xla/hlo/analysis/indexing_analysis.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/stream_executor/cuda/cuda_compute_capability.h"
#include "xla/stream_executor/device_description.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "triton/Dialect/Triton/IR/Types.h"

namespace mlir::triton::xla {

namespace xg = ::xla::gpu;
namespace xgt = xg::triton;

namespace {

#define GEN_PASS_DEF_TRITONXLAEXTRACTINSERTTOTRITONPASS
#include "xla/backends/gpu/codegen/triton/transforms/passes.h.inc"

PointerType GetTensorPtrType(Type type) {
  return PointerType::get(xgt::StorageType(type),
                          mlir::NVVM::kGlobalMemorySpace);
}

PointerType GetTensorPtrTypeForTma(::xla::EmitterLocOpBuilder& builder) {
  // Triton frontend is passing zero in the address space. This doesn't map to
  // anything meaningful in NVVM dialect. Setting it to be consistent with
  // Triton.
  return PointerType::get(xgt::StorageType(builder.getI8Type()),
                          /*addrspace=*/0);
}

bool AreRankedTensors(ArrayRef<Type> types) {
  return llvm::all_of(types, [](mlir::Type type) {
    return mlir::isa<mlir::RankedTensorType>(type);
  });
}

SmallVector<Value> IndexCastUI(::xla::EmitterLocOpBuilder& builder, Type type,
                               ValueRange values) {
  SmallVector<Value> result;
  result.reserve(values.size());
  for (auto value : values) {
    result.push_back(builder.create<arith::IndexCastUIOp>(type, value));
  }
  return result;
}

bool TmaIsEnabledForDevice(
    const stream_executor::DeviceDescription& device_info) {
  bool is_cuda = std::holds_alternative<stream_executor::CudaComputeCapability>(
      device_info.gpu_compute_capability());
  return is_cuda && device_info.cuda_compute_capability().IsAtLeastHopper();
}

bool CanUseTMA(::xla::EmitterLocOpBuilder& builder, bool tma_enabled,
               const stream_executor::DeviceDescription& device_description,
               const ArrayRef<int64_t>& tile_shape,
               const TypedValue<RankedTensorType>& tensor) {
  if (!tma_enabled) {
    return false;
  }
  if (!TmaIsEnabledForDevice(device_description)) {
    return false;
  }
  // Currently only 2D tensors are supported.
  if (tile_shape.size() != 2) {
    return false;
  }

  // We only enable TMA for inputs that have one use only.
  auto block_arg = mlir::dyn_cast<BlockArgument>(tensor);
  if (!block_arg || !block_arg.hasOneUse()) {
    return false;
  }
  auto func_op =
      mlir::dyn_cast<func::FuncOp>(block_arg.getOwner()->getParentOp());
  if (!func_op) {
    return false;
  }

  // Limitations of TMA:
  // - The minor dimension of the global input must be divisible by 16.
  // - The block size must be less than 256 in every dimension.
  // See source:
  // https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__TENSOR__MEMORY.html
  // TODO(b/413351367): Figure out if we need (and how) to handle non-normalized
  // layouts.
  if (tensor.getType().getShape()[1] % 16 != 0) {
    return false;
  }
  return llvm::none_of(tile_shape, [](int64_t dim) { return dim > 256; });
}

SmallVector<int32_t> ComputeBoundaryChecks(
    const ArrayRef<int64_t>& original_shape,
    const ArrayRef<int64_t>& tile_shape) {
  SmallVector<int32_t> boundary_checks;
  for (auto [dim_idx, sizes] :
       llvm::enumerate(llvm::zip(original_shape, tile_shape))) {
    auto [dim_size, tile_size] = sizes;
    if (dim_size % tile_size) {
      boundary_checks.push_back(dim_idx);
    }
  }
  return boundary_checks;
}

// TensorPtr is intended to wrap the base pointer of the TiledHloInstruction and
// the necessary offsets so that Triton can compute the pointer to the
// block specific to the given pid. This option would yield simpler code,
// but cannot handle all combinations of strides and offsets, because Triton
// always multiplies the offset by the stride. E.g., it's not possible to
// slice [10] with [1:5:2] because the offset is misaligned with regards to the
// stride.
//
// Instead, we output a TensorPtr that points directly to the tile specific
// to the pid. All offset computation is done in advance. MakeTensorPtrOp
// sees 0 offsets. This allows Triton to read any block regardless of
// strides size or offsets. To make sure that masking is correct, we compute
// a "residual shape" which is the original parent shape minus the offsets.
SmallVector<Value> ComputeResidualShape(::xla::EmitterLocOpBuilder& builder,
                                        ArrayRef<int64_t> original_shape,
                                        ValueRange tile_offsets) {
  SmallVector<Value> residual_shape;
  for (auto [dim_idx, shape_and_tile_offset] :
       llvm::enumerate(llvm::zip(original_shape, tile_offsets))) {
    auto [shape, tile_offset] = shape_and_tile_offset;
    Value size =
        ::xla::gpu::triton::CreateConst(builder, builder.getI64Type(), shape)
            .UnwrapScalar();
    // Offsets are necessarily positive since they represent a distance
    // between 0 and the size of the tensor on the given axis. Therefore, it
    // is safe to use 'IndexCastUI' here. This allows index canonicalizations
    // later on.
    Value offset =
        builder.create<arith::IndexCastUIOp>(builder.getI64Type(), tile_offset);
    residual_shape.push_back(builder.create<arith::SubIOp>(size, offset));
  }

  return residual_shape;
}

// Compute physical strides of the tile. `tile_strides` contains strides for
// individual dimensions. We need to convert them to strides in the buffer
// taking into account physical layout. Note that we should pass in the
// minor-to-major layout for this to work correctly.
SmallVector<Value> ComputeStrides(::xla::EmitterLocOpBuilder& builder,
                                  ArrayRef<int64_t> original_shape,
                                  ValueRange tile_strides,
                                  ArrayRef<int64_t> minor_to_major_layout) {
  SmallVector<Value> strides(tile_strides.size());
  int64_t current_stride = 1;
  for (int64_t cur_dim : minor_to_major_layout) {
    strides[cur_dim] = builder.create<arith::MulIOp>(
        builder.create<arith::IndexCastUIOp>(builder.getI64Type(),
                                             tile_strides[cur_dim]),
        ::xla::gpu::triton::CreateConst(builder, builder.getI64Type(),
                                        current_stride)
            .UnwrapScalar());
    current_stride *= original_shape[cur_dim];
  }
  return strides;
}

// Based on the multi-dimensional offsets and layout of the shape, we compute
// a linear offset. We do this because we move the pointer to the correct
// position via tt.addptr prior to calling tt.make_tensor_ptr.
Value ComputeLinearOffset(::xla::EmitterLocOpBuilder& builder,
                          const RankedTensorType& tensor_type,
                          ValueRange offsets, llvm::ArrayRef<int64_t> layout) {
  ::xla::Shape shape = ::xla::ShapeUtil::MakeShapeWithDenseLayout(
      xgt::GetPrimitiveType(tensor_type.getElementType()).value(),
      tensor_type.getShape(), layout);

  ::xla::Shape linear_shape = ::xla::ShapeUtil::MakeShape(
      shape.element_type(), {::xla::ShapeUtil::ElementsIn(shape)});
  auto bitcast_map =
      ::xla::GetBitcastMap(shape, linear_shape, builder.getContext());

  return builder.create<arith::IndexCastUIOp>(
      builder.getI64Type(),
      builder.create<::xla::ApplyIndexingOp>(offsets, bitcast_map)
          .getResult(0));
}

// Add TMA attributes to the corresponding argument in the function.
void AddTmaAttributes(::xla::EmitterLocOpBuilder& builder,
                      const TypedValue<RankedTensorType>& tensor,
                      const ArrayRef<int64_t>& tile_shape) {
  auto block_arg = mlir::dyn_cast<BlockArgument>(tensor);
  auto func_op =
      mlir::dyn_cast<func::FuncOp>(block_arg.getOwner()->getParentOp());
  func_op.setArgAttr(block_arg.getArgNumber(), "tt.nv_tma_desc",
                     builder.getI32IntegerAttr(1));
  // Prefixing the attribute name with "tt", otherwise tt.func will
  // complain that it is not part of the dialect. Not the best way to
  // do this, but it works for now.
  func_op.setArgAttr(
      block_arg.getArgNumber(), "tt.tma_descriptor",
      builder.getAttr<TmaDescriptorAttr>(
          tensor.getType().getShape(), tile_shape,
          tensor.getType().getElementType().getIntOrFloatBitWidth() / 8));
}

Value CreateAddPtrOp(::xla::EmitterLocOpBuilder& builder,
                     const TypedValue<RankedTensorType>& tensor,
                     ValueRange offsets, llvm::ArrayRef<int64_t> layout) {
  // tensor -> !tt.ptr<>
  auto cast_to_tensor_ptr_type =
      builder
          .create<mlir::UnrealizedConversionCastOp>(
              GetTensorPtrType(tensor.getType().getElementType()), tensor)
          .getResult(0);

  auto linear_offset =
      ComputeLinearOffset(builder, tensor.getType(), offsets, layout);
  return builder.create<AddPtrOp>(cast_to_tensor_ptr_type.getType(),
                                  cast_to_tensor_ptr_type, linear_offset);
}

Value CreateMakeTensorPtrOp(::xla::EmitterLocOpBuilder& builder, Value ptr,
                            ArrayRef<int64_t> original_shape,
                            ArrayRef<int64_t> tile_shape,
                            SmallVector<Value> offsets,
                            SmallVector<Value> tile_strides,
                            ArrayRef<int64_t> layout) {
  // TODO(b/342989850): Clarify and comment what `order` exactly is. It's
  // not entirely clear from the Triton docs. Currently we are propagating
  // the layout from the original tensor.
  auto dim_order = llvm::to_vector_of<int32_t>(layout);

  SmallVector<Value> residual_shape =
      ComputeResidualShape(builder, original_shape, offsets);

  // Offsets are always passed as 0 since we are using "residual shape".
  SmallVector<Value> zero_offsets(
      tile_shape.size(),
      ::xla::gpu::triton::CreateConst(builder, builder.getI32Type(), 0)
          .UnwrapScalar());

  SmallVector<Value> strides =
      ComputeStrides(builder, original_shape, tile_strides, layout);

  return builder
      .create<MakeTensorPtrOp>(ptr, residual_shape, strides, zero_offsets,
                               llvm::to_vector_of<int32_t>(tile_shape),
                               dim_order)
      .getResult();
}

Value CreateReinterpretTensorDescOp(
    ::xla::EmitterLocOpBuilder& builder,
    const TypedValue<RankedTensorType>& tensor_arg,
    RankedTensorType result_type) {
  // tensor -> !tt.ptr<i8, 0>
  auto cast_to_tensor_ptr_type =
      builder
          .create<mlir::UnrealizedConversionCastOp>(
              GetTensorPtrTypeForTma(builder), tensor_arg)
          .getResult(0);

  return builder.create<mlir::triton::ReinterpretTensorDescOp>(
      mlir::triton::TensorDescType::get(builder.getContext(), result_type),
      cast_to_tensor_ptr_type);
}

class RewriteFuncOp : public mlir::OpRewritePattern<func::FuncOp> {
 public:
  using OpRewritePattern::OpRewritePattern;

  // Rewrite tensors<> to !tt.ptr<tensor>
  // Remove any returns. i.e. tt.return with no operands.
  mlir::LogicalResult matchAndRewrite(
      func::FuncOp op, mlir::PatternRewriter& rewriter) const override {
    ::xla::EmitterLocOpBuilder builder(op.getLoc(), rewriter);

    auto input_types = op.getFunctionType().getInputs();
    auto output_types = op.getFunctionType().getResults();

    if (!AreRankedTensors(input_types) || !AreRankedTensors(output_types)) {
      return rewriter.notifyMatchFailure(
          op, "Expected all inputs and results to have tensor type.");
    }

    SmallVector<Type> new_operand_types(input_types);
    for (auto&& [index, operand_type] : llvm::enumerate(new_operand_types)) {
      mlir::BlockArgument func_arg = op.getArgument(index);

      mlir::UnrealizedConversionCastOp cast_to_orig_type;
      if (op.getArgAttr(index, "tt.nv_tma_desc")) {
        operand_type = GetTensorPtrTypeForTma(builder);
        // !tt.ptr<i8, 0> -> tensor
        cast_to_orig_type = builder.create<mlir::UnrealizedConversionCastOp>(
            operand_type, func_arg);
      } else {
        // !tt.ptr<> -> tensor
        cast_to_orig_type = builder.create<mlir::UnrealizedConversionCastOp>(
            operand_type, func_arg);
        operand_type = GetTensorPtrType(
            mlir::cast<TensorType>(operand_type).getElementType());
      }
      func_arg.replaceAllUsesExcept(cast_to_orig_type.getResult(0),
                                    cast_to_orig_type);
    }

    // Replace the function arguments with the new types.
    mlir::Block* entry_block = &op.getBody().front();
    for (auto [arg, arg_type] :
         llvm::zip(entry_block->getArguments(), new_operand_types)) {
      arg.setType(arg_type);
    }

    auto new_function_type = FunctionType::get(
        op.getContext(), new_operand_types, /*result_types=*/{});

    // Transfer the argument attributes from the old function to the new one.
    SmallVector<DictionaryAttr> arg_attrs;
    if (op.getArgAttrs().has_value()) {
      auto oldArgAttrsArray = op.getArgAttrs().value();
      for (int i = 0; i < oldArgAttrsArray.size(); ++i) {
        arg_attrs.push_back(
            mlir::cast<mlir::DictionaryAttr>(oldArgAttrsArray[i]));
      }
    }

    // Currently not propagating any function attributes to the new function.
    ArrayRef<NamedAttribute> attrs;
    auto new_func = rewriter.create<triton::FuncOp>(
        op.getLoc(), op.getName(), new_function_type, attrs, arg_attrs);

    for (int i = 0; i < new_func.getNumArguments(); ++i) {
      // TMA arguments don't require tt.divisibility.
      if (op.getArgAttr(i, "tt.nv_tma_desc")) {
        continue;
      }
      new_func.setArgAttr(i, "tt.divisibility",
                          builder.getIntegerAttr(builder.getI32Type(), 16));
    }

    rewriter.inlineRegionBefore(op.getRegion(), new_func.getFunctionBody(),
                                new_func.end());
    rewriter.replaceOp(op, new_func);

    auto terminator = new_func.getBody().front().getTerminator();
    rewriter.setInsertionPoint(terminator);
    rewriter.create<triton::ReturnOp>(new_func.getLoc());
    rewriter.eraseOp(terminator);

    return mlir::success();
  }
};

class RewriteExtract : public mlir::OpRewritePattern<ExtractOp> {
 public:
  RewriteExtract(mlir::MLIRContext* context,
                 const stream_executor::DeviceDescription* device_description,
                 bool tma_enabled)
      : OpRewritePattern(context),
        device_description_(device_description),
        tma_enabled_(tma_enabled) {}
  using OpRewritePattern::OpRewritePattern;

  // Rewriting ExtractOp as:
  // Without TMA:
  // tt.addptr + tt.make_tensor_ptr + tt.load.
  // Offsets are resolved in tt.addptr.
  //
  // With TMA:
  // tt.reinterpret_tensor_desc + tt.descriptor_load.
  // Offsets are resolved in tt.descriptor_load.
  mlir::LogicalResult matchAndRewrite(
      ExtractOp op, mlir::PatternRewriter& rewriter) const override {
    ::xla::EmitterLocOpBuilder builder(op.getLoc(), rewriter);
    auto original_shape = op.getSrc().getType().getShape();
    auto tile_shape = op.getResult().getType().getShape();

    if (CanUseTMA(builder, tma_enabled_, *device_description_, tile_shape,
                  op.getSrc())) {
      AddTmaAttributes(builder, op.getSrc(), tile_shape);

      auto reinterpret_tensor_desc = CreateReinterpretTensorDescOp(
          builder, op.getSrc(), op.getResult().getType());

      auto descriptor_load =
          builder
              .create<DescriptorLoadOp>(
                  op.getResult().getType(), reinterpret_tensor_desc,
                  IndexCastUI(builder, builder.getI32Type(), op.getOffsets()))
              .getResult();

      rewriter.replaceOp(op, descriptor_load);
      return mlir::success();
    }

    auto ptr =
        CreateAddPtrOp(builder, op.getSrc(), op.getOffsets(), op.getLayout());
    ptr =
        CreateMakeTensorPtrOp(builder, ptr, original_shape, tile_shape,
                              op.getOffsets(), op.getStrides(), op.getLayout());

    auto boundary_checks = ComputeBoundaryChecks(original_shape, tile_shape);
    std::optional<PaddingOption> padding;
    if (!boundary_checks.empty()) {
      padding = PaddingOption::PAD_ZERO;
    }
    auto load = builder
                    .create<LoadOp>(ptr, boundary_checks, padding,
                                    CacheModifier::NONE, EvictionPolicy::NORMAL,
                                    /*isVolatile=*/false)
                    .getResult();

    rewriter.replaceOp(op, load);
    return mlir::success();
  }

  const stream_executor::DeviceDescription* device_description_;
  const bool tma_enabled_;
};

class RewriteInsert : public mlir::OpRewritePattern<InsertOp> {
 public:
  RewriteInsert(mlir::MLIRContext* context,
                const stream_executor::DeviceDescription* device_description,
                bool tma_enabled)
      : OpRewritePattern(context),
        device_description_(device_description),
        tma_enabled_(tma_enabled) {}
  using OpRewritePattern::OpRewritePattern;

  // Rewriting InsertOp as:
  // Without TMA:
  // tt.addptr + tt.make_tensor_ptr + tt.store.
  // Offsets are resolved in tt.addptr.
  //
  // With TMA:
  // tt.reinterpret_tensor_desc + tt.descriptor_store.
  // Offsets are resolved in tt.descriptor_store.
  mlir::LogicalResult matchAndRewrite(
      InsertOp op, mlir::PatternRewriter& rewriter) const override {
    ::xla::EmitterLocOpBuilder builder(op.getLoc(), rewriter);
    auto original_shape = op.getDst().getType().getShape();
    auto tile_shape = op.getSrc().getType().getShape();

    if (CanUseTMA(builder, tma_enabled_, *device_description_, tile_shape,
                  op.getDst())) {
      AddTmaAttributes(builder, op.getDst(), tile_shape);

      auto reinterpret_tensor_desc = CreateReinterpretTensorDescOp(
          builder, op.getDst(), op.getSrc().getType());

      builder.create<DescriptorStoreOp>(
          reinterpret_tensor_desc, op.getSrc(),
          IndexCastUI(builder, builder.getI32Type(), op.getOffsets()));
    } else {
      auto ptr =
          CreateAddPtrOp(builder, op.getDst(), op.getOffsets(), op.getLayout());
      ptr = CreateMakeTensorPtrOp(builder, ptr, original_shape, tile_shape,
                                  op.getOffsets(), op.getStrides(),
                                  op.getLayout());

      rewriter.create<StoreOp>(
          op->getLoc(), ptr, op.getSrc(),
          ComputeBoundaryChecks(original_shape, tile_shape),
          CacheModifier::NONE, EvictionPolicy::NORMAL);
    }

    // InsertOp has a result, so we propagate it to the users.
    op->replaceAllUsesWith(ValueRange(op.getDst()));

    return mlir::success();
  }

  const stream_executor::DeviceDescription* device_description_;
  const bool tma_enabled_;
};

// Rewriting tensor::InsertOp as tt.store.
class RewriteScalarInsert : public mlir::OpRewritePattern<tensor::InsertOp> {
 public:
  using OpRewritePattern::OpRewritePattern;

  mlir::LogicalResult matchAndRewrite(
      tensor::InsertOp op, mlir::PatternRewriter& rewriter) const override {
    if (op.getDest().getType().getRank() != 0) {
      return rewriter.notifyMatchFailure(op, "Expected dest to be scalar.");
    }
    ::xla::EmitterLocOpBuilder builder(op.getLoc(), rewriter);
    auto ptr_type = GetTensorPtrType(op.getScalar().getType());
    auto cast_dst_to_tensor_ptr_type =
        builder.create<mlir::UnrealizedConversionCastOp>(ptr_type, op.getDest())
            .getResult(0);
    builder.create<StoreOp>(cast_dst_to_tensor_ptr_type, op.getScalar(),
                            /*boundary_checks=*/std::vector<int32_t>{},
                            CacheModifier::NONE, EvictionPolicy::NORMAL);
    rewriter.replaceOp(op, op.getDest());
    return mlir::success();
  }
};

class RewriteScalarExtract : public mlir::OpRewritePattern<tensor::ExtractOp> {
 public:
  using OpRewritePattern::OpRewritePattern;

  // Rewriting ExtractOp as tt.advance + tt.store.
  mlir::LogicalResult matchAndRewrite(
      tensor::ExtractOp op, mlir::PatternRewriter& rewriter) const override {
    if (op.getTensor().getType().getRank() != 0) {
      return rewriter.notifyMatchFailure(op, "Expected src to be scalar.");
    }
    ::xla::EmitterLocOpBuilder builder(op.getLoc(), rewriter);
    auto ptr_type = GetTensorPtrType(op.getType());
    auto cast_src_to_tensor_ptr_type =
        builder
            .create<mlir::UnrealizedConversionCastOp>(ptr_type, op.getTensor())
            .getResult(0);
    auto scalar =
        builder.create<LoadOp>(cast_src_to_tensor_ptr_type, CacheModifier::NONE,
                               EvictionPolicy::NORMAL, /*isVolatile=*/false);
    rewriter.replaceOp(op, scalar.getResult());
    return mlir::success();
  }
};

class TritonXLAExtractInsertToTritonPass
    : public impl::TritonXLAExtractInsertToTritonPassBase<
          TritonXLAExtractInsertToTritonPass> {
 public:
  explicit TritonXLAExtractInsertToTritonPass(
      const TritonXLAExtractInsertToTritonPassOptions& options)
      : TritonXLAExtractInsertToTritonPassBase(options) {}

  explicit TritonXLAExtractInsertToTritonPass(
      const stream_executor::DeviceDescription& device_description,
      bool tma_enabled)
      : device_description_(device_description), is_tma_enabled_(tma_enabled) {}

  void runOnOperation() override {
    if (!gpu_device_info_.empty()) {
      stream_executor::GpuDeviceInfoProto device_info;
      CHECK(tsl::protobuf::TextFormat::ParseFromString(gpu_device_info_,
                                                       &device_info));
      device_description_ = stream_executor::DeviceDescription(device_info);
    }
    if (tma_enabled_.hasValue()) {
      is_tma_enabled_ = tma_enabled_.getValue();
    }

    mlir::MLIRContext* mlir_context = &getContext();
    mlir::RewritePatternSet patterns(mlir_context);
    // clang-format off
    patterns.add<RewriteExtract,
                 RewriteInsert
                >(mlir_context, &device_description_, is_tma_enabled_);
    patterns.add<RewriteFuncOp,
                 RewriteScalarExtract,
                 RewriteScalarInsert>(mlir_context);
    // clang-format on

    if (mlir::failed(
            mlir::applyPatternsGreedily(getOperation(), std::move(patterns)))) {
      signalPassFailure();
    }
  }

  stream_executor::DeviceDescription device_description_;
  bool is_tma_enabled_;
};

}  // namespace

std::unique_ptr<mlir::Pass> CreateTritonXLAExtractInsertToTritonPass(
    const std::string& gpu_device_info, bool tma_enabled) {
  TritonXLAExtractInsertToTritonPassOptions options;
  options.gpu_device_info_ = gpu_device_info;
  options.tma_enabled_ = tma_enabled;
  return std::make_unique<TritonXLAExtractInsertToTritonPass>(options);
}

std::unique_ptr<mlir::Pass> CreateTritonXLAExtractInsertToTritonPass(
    const stream_executor::DeviceDescription& device_description,
    bool tma_enabled) {
  return std::make_unique<TritonXLAExtractInsertToTritonPass>(
      device_description, tma_enabled);
}

}  // namespace mlir::triton::xla
