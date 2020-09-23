/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/service/qr_expander.h"

#include <memory>
#include <vector>

#include "tensorflow/compiler/xla/client/lib/arithmetic.h"
#include "tensorflow/compiler/xla/client/lib/constants.h"
#include "tensorflow/compiler/xla/client/lib/loops.h"
#include "tensorflow/compiler/xla/client/lib/math.h"
#include "tensorflow/compiler/xla/client/lib/matrix.h"
#include "tensorflow/compiler/xla/client/lib/slicing.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/primitive_util.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/core/errors.h"

namespace xla {

namespace {

std::vector<int64> ConcatVectors(absl::Span<const int64> xs,
                                 absl::Span<const int64> ys) {
  std::vector<int64> output;
  output.reserve(xs.size() + ys.size());
  std::copy(xs.begin(), xs.end(), std::back_inserter(output));
  std::copy(ys.begin(), ys.end(), std::back_inserter(output));
  return output;
}

// Computes a Householder reflection of the form:
// H = I - tau v v.T.
// such that
// H . ( x1  ) = ( x1   )
//     ( x2  ) = ( x2   )
//     ( ... ) = ( ...  )
//     ( xk  ) = ( beta )
//     ( ... )   ( 0    )
//     ( ... )   ( 0    )
// Unlike the usual formulation, we allow the caller to supply 'k' rather than
// only providing the relevant part of 'x' to maintain XLA's static shape
// invariant. In addition, the implementation supports batching.
// Pseudo-code, without batching:
//   alpha = x[k]
//   x_copy = np.copy(x)
//   x_copy[:k+1] = 0
//   xnorm = norm2(x_copy)
//   if xnorm == 0:
//     beta = alpha
//     tau = 0
//     v = np.zeros_like(x)
//   else:
//     beta = - np.sign(alpha) * dlapy2(alpha, xnorm)
//     tau = (beta - alpha) / beta
//     v = x / (alpha - beta)
//   v[k] = 1
//   return (v, tau, beta)
// TODO(phawkins): LAPACK's xLARFG implementation has code for handling
// overflows in the norm/beta calculations. Perhaps do the same here.
Status House(XlaOp x, XlaOp k, absl::Span<const int64> batch_dims,
             const int64 m, XlaOp* v, XlaOp* tau, XlaOp* beta) {
  XlaBuilder* const builder = x.builder();
  TF_ASSIGN_OR_RETURN(Shape x_shape, builder->GetShape(x));
  const PrimitiveType type = x_shape.element_type();

  std::vector<int64> batch_dim_ids(batch_dims.size());
  std::iota(batch_dim_ids.begin(), batch_dim_ids.end(), 0);
  const int64 minor_dim = batch_dims.size();

  XlaOp zero = ScalarLike(x, 0.0);
  XlaOp one = ScalarLike(x, 1.0);

  // alpha = x[k]
  XlaOp alpha = Reshape(DynamicSliceInMinorDims(x, {k}, {1}), batch_dims);

  // Compute x[k+1:] (padded with zeros in elements 0..k)
  XlaOp iota = Iota(builder, S32, m);
  XlaOp x_after_k = Mul(x, ConvertElementType(Gt(iota, k), type),
                        /*broadcast_dimensions=*/{minor_dim});

  // sigma = np.dot(x[k+1:], x[k+1:])
  // TODO(phawkins): this calculation may be numerically unstable.
  auto sigma = Reduce(x_after_k * x_after_k, zero,
                      CreateScalarAddComputation(type, builder), {minor_dim});
  // mu = np.sqrt(x[k]*x[k] + sigma)
  auto mu = Sqrt(Square(alpha) + sigma);

  auto sigma_is_zero = Eq(sigma, zero);

  *beta = Select(sigma_is_zero, alpha, Select(Lt(alpha, zero), one, -one) * mu);
  *tau = Select(sigma_is_zero, Broadcast(zero, batch_dims),
                (*beta - alpha) / *beta);
  auto divisor =
      Select(sigma_is_zero, Broadcast(one, batch_dims), alpha - *beta);

  auto e_k = Broadcast(ConvertElementType(Eq(iota, k), type),
                       std::vector<int64>(batch_dims.size(), 1));

  // Form v as [0, 0, ..., 1] ++ x[k+1:] / divisor
  // If sigma is zero, x[k+1:] is zero, so use any non-zero divisor.
  *v = e_k + Div(x_after_k, divisor, /*broadcast_dimensions=*/batch_dim_ids);
  return Status::OK();
}

}  // namespace

// Householder QR decomposition. Algorithm 5.2.1 from Golub and Van
// Loan "Matrix Computations", 4th Edition. This is an unblocked implementation
// used as an inner routine of the blocked implementation.
// Algorithm is adapted slightly so the shapes inside the loop are static, at
// the cost of some redundant computation. Since this is used as an inner block
// kernel, accumulates the Householder transformations (vs, taus) rather than
// the matrix q.
// Equivalent Python code, without batching:
// def qr(a):
//   m = a.shape[0]
//   n = a.shape[1]
//   taus = np.zeros([n])
//   for j in xrange(min(m, n)):
//     v, tau, beta = house(a[:, j], j)
//     a[:, j+1:] -= tau * np.dot(v[:, np.newaxis],
//                                np.dot(v[np.newaxis, :], a[:, j+1:]))
//     # Form column j explicitly rather than relying on the precision of the
//     # Householder update.
//     a[j, j] = beta
//     a[j+1:, j] = v[j+1:]
//     taus[j] = tau
//   return (a, taus)
StatusOr<QrExpander::QrResult> QrExpander::QrBlock(
    XlaOp a, PrecisionConfig::Precision precision) {
  XlaBuilder* builder = a.builder();
  TF_ASSIGN_OR_RETURN(Shape a_shape, builder->GetShape(a));
  const int num_dims = a_shape.rank();
  if (num_dims < 2) {
    return InvalidArgument("Argument to QR must have rank >= 2; got shape %s",
                           a_shape.ToString());
  }
  PrimitiveType type = a_shape.element_type();

  const int64 m = ShapeUtil::GetDimension(a_shape, -2);
  const int64 n = ShapeUtil::GetDimension(a_shape, -1);

  const int64 num_batch_dims = num_dims - 2;
  std::vector<int64> batch_dims(num_batch_dims);
  for (int i = 0; i < num_batch_dims; ++i) {
    batch_dims[i] = ShapeUtil::GetDimension(a_shape, i);
  }

  std::vector<int64> batch_dim_indices(num_batch_dims);
  std::iota(batch_dim_indices.begin(), batch_dim_indices.end(), 0);

  auto qr_body_fn = [&](XlaOp j, absl::Span<const XlaOp> values,
                        XlaBuilder* builder) -> StatusOr<std::vector<XlaOp>> {
    auto a = values[0];
    auto taus = values[1];

    // v, tau, beta = house(a[:, j], j)
    auto x = DynamicSliceInMinorDims(a, {j}, {1});
    XlaOp v, tau, beta;
    TF_RETURN_IF_ERROR(House(Collapse(x, {num_dims - 2, num_dims - 1}), j,
                             batch_dims, m, &v, &tau, &beta));

    const int64 minor_dim = batch_dims.size();
    auto iota_mn = Iota(
        builder, ShapeUtil::MakeShape(S32, ConcatVectors(batch_dims, {m, n})),
        minor_dim + 1);

    std::vector<int64> shape = batch_dims;
    shape.push_back(1);
    shape.push_back(m);
    auto v_broadcast = Reshape(v, shape);
    // a[:, j+1:] -= tau * (v[:, np.newaxis] @ (v[np.newaxis, :] @ a[:, j+1:]))
    // We use masking rather than a loop-variant shape to handle the j+1:
    // indexing.
    auto vva = BatchDot(v_broadcast, Select(Lt(j, iota_mn), a, ZerosLike(a)),
                        precision);
    vva = BatchDot(v_broadcast, true, vva, false, precision);
    a = a - Mul(tau, vva,
                /*broadcast_dimensions=*/batch_dim_indices);

    // a[j, j] = beta
    // a[j+1:,j] = v[j+1:]
    auto iota = Reshape(Iota(a.builder(), S32, m), {m, 1});
    auto predecessor_mask = ConvertElementType(Lt(iota, j), type);
    auto mask = Broadcast(ConvertElementType(Eq(iota, j), type),
                          std::vector<int64>(batch_dims.size(), 1));
    auto successor_mask = Gt(Iota(a.builder(), S32, m), j);
    auto new_x = Mul(x, predecessor_mask,
                     /*broadcast_dimensions=*/{num_dims - 2, num_dims - 1}) +
                 Mul(beta, mask, /*broadcast_dimensions=*/batch_dim_indices);
    new_x = Add(
        new_x, Select(Broadcast(successor_mask, batch_dims), v, ZerosLike(v)),
        /*broadcast_dimensions=*/ConcatVectors(batch_dim_indices, {minor_dim}));
    // Update a[:,j]
    std::vector<int64> dim_ids(num_dims);
    std::iota(dim_ids.begin(), dim_ids.end(), 0);
    new_x = BroadcastInDim(new_x, ConcatVectors(batch_dims, {m, n}),
                           /*broadcast_dimensions=*/dim_ids);
    a = Select(Eq(iota_mn, j), new_x, a);

    // taus[j] = tau
    std::vector<int64> tau_broadcast_dims(batch_dims.size());
    std::iota(tau_broadcast_dims.begin(), tau_broadcast_dims.end(), 0);

    auto iota_n =
        Iota(builder, ShapeUtil::MakeShape(S32, ConcatVectors(batch_dims, {n})),
             minor_dim);
    auto taus_zeros = ZerosLike(taus);
    auto taus_update = Select(
        Eq(iota_n, j),
        Add(taus_zeros, tau, /*broadcast_dimensions=*/tau_broadcast_dims),
        taus_zeros);
    taus = taus + taus_update;
    return std::vector<XlaOp>{a, taus};
  };

  auto taus = Zeros(
      builder,
      ShapeUtil::MakeShape(type, ConcatVectors(batch_dims, {std::min(m, n)})));

  TF_ASSIGN_OR_RETURN(auto values, ForEachIndex(std::min(m, n), S32, qr_body_fn,
                                                {a, taus}, "qr", builder));

  QrResult result;
  result.a = values[0];
  result.taus = values[1];
  return result;
}

// Computes an upper triangular matrix T such that (I - Y @ T @ Y^t) is a
// product of the elementary Householder reflectors given by `vs` and `taus`.
//
// Schreiber, Robert, and Charles Van Loan. "A storage-efficient WY
// representation for products of Householder transformations." SIAM Journal on
// Scientific and Statistical Computing 10.1 (1989): 53-57.
//
// def compact_wy(vs, taus):
//   m, n = vs.shape[-2:]
//   t = np.eye(n) * -taus
//   # We premultiply Y.T @ vs, since we would prefer to compute a single matrix
//   # multiplication to many matrix-vector products.
//   vtv = -taus[None, :] * np.triu(vs.T @ vs, 1) + np.eye(n)
//   for i in range(1, n):
//     t[:, i] = scipy.linalg.blas.strmm(t, vtv[:, i])
//   return t
StatusOr<XlaOp> QrExpander::CompactWYRepresentation(
    PrimitiveType type, absl::Span<const int64> batch_dims, XlaOp vs,
    XlaOp taus, int64 m, int64 n, PrecisionConfig::Precision precision) {
  XlaBuilder* builder = vs.builder();

  std::vector<int64> batch_dim_indices(batch_dims.size());
  std::iota(batch_dim_indices.begin(), batch_dim_indices.end(), 0);
  int64 n_index = batch_dims.size() + 1;

  auto body_fn = [&](XlaOp j, absl::Span<const XlaOp> values,
                     XlaBuilder* builder) -> StatusOr<std::vector<XlaOp>> {
    // w has shape [..., m, n]
    auto t = values[0];
    const auto vtv = values[1];

    // yv has shape [..., n, 1]
    auto yv = DynamicSliceInMinorDims(vtv, {j}, {1});

    // z has shape [..., n, 1]
    auto z = BatchDot(t, yv, precision);

    t = DynamicUpdateSliceInMinorDims(t, z, {j});

    return std::vector<XlaOp>{t, vtv};
  };

  auto tau_scale = BroadcastInDim(-taus, ConcatVectors(batch_dims, {1, n}),
                                  ConcatVectors(batch_dim_indices, {n_index}));

  auto eye = Broadcast(IdentityMatrix(builder, type, n, n), batch_dims);
  auto t = eye;

  auto vtv =
      BatchDot(vs, /*transpose_x=*/true, vs, /*transpose_y=*/false, precision);
  vtv = Select(TriangleMask(vtv, 0), ZerosLike(vtv), vtv);
  vtv = (vtv + eye) * tau_scale;

  TF_ASSIGN_OR_RETURN(auto values,
                      ForEachIndex(n, S32, body_fn, {t, vtv}, "wy", builder));
  return values[0];
}

// Block Householder QR Factorization. Algorithm 5.2.2 of Golub and van Loan.
// def qr_blocked(a, block_size):
//   m = a.shape[0]
//   n = a.shape[1]
//   q = np.eye(m)
//   for i in xrange(0, min(m, n), block_size):
//     k = min(block_size, min(m, n) - s)
//     (a, taus) = qr(a[i:, i:i+k])
//     y = np.eye(m, n) + np.tril(a, -1)
//     t = CompactWYRepresentation(vs, taus, m-i, k)
//     a[i:, i+k:] += (y @ t.T) @ (y.T @ a[i:, i+k:])
//     q[:, i:] += (q[:, i:] @ y) @ (y @ t.T).T
//   return (q, a)
StatusOr<XlaOp> QrExpander::BuildQrDecomposition(
    XlaOp a, int64 block_size, PrecisionConfig::Precision precision) {
  XlaBuilder* builder = a.builder();
  TF_ASSIGN_OR_RETURN(Shape a_shape, builder->GetShape(a));
  const int num_dims = a_shape.rank();
  if (num_dims < 2) {
    return InvalidArgument("Arguments to QR must have rank >= 2: got shape %s",
                           a_shape.ToString());
  }
  PrimitiveType type = a_shape.element_type();

  const int64 m = ShapeUtil::GetDimension(a_shape, -2);
  const int64 n = ShapeUtil::GetDimension(a_shape, -1);
  const int64 p = std::min(m, n);

  if (block_size < 1) {
    return InvalidArgument("block_size argument to QR must be >= 1; got %d",
                           block_size);
  }

  const int64 num_batch_dims = num_dims - 2;
  std::vector<int64> batch_dims(num_batch_dims);
  for (int i = 0; i < num_batch_dims; ++i) {
    batch_dims[i] = ShapeUtil::GetDimension(a_shape, i);
  }

  auto q = Broadcast(IdentityMatrix(builder, type, m, m), batch_dims);
  for (int64 i = 0; i < p; i += block_size) {
    int64 k = std::min(block_size, p - i);

    auto a_block = SliceInMinorDims(a, {i, i}, {m, i + k});
    TF_ASSIGN_OR_RETURN(auto qr_block, QrBlock(a_block, precision));
    auto y = Add(
        IdentityMatrix(builder, type, m - i, k),
        Select(TriangleMask(qr_block.a, -1), qr_block.a, ZerosLike(qr_block.a)),
        /*broadcast_dimensions=*/{num_dims - 2, num_dims - 1});

    a = UpdateSliceInMinorDims(a, qr_block.a, {i, i});

    // Compute the I + Y @ T @ Y^t block representation of a product of
    // Householder matrices.
    TF_ASSIGN_OR_RETURN(
        auto t, CompactWYRepresentation(type, batch_dims, y, qr_block.taus,
                                        m - i, k, precision));

    // a[i:, i+k:] += (y @ t.T) @ (y.T @ a[i:, i+k:])
    auto yt =
        BatchDot(y, /*transpose_x=*/false, t, /*transpose_y=*/true, precision);
    auto a_panel = SliceInMinorDims(a, {i, i + k}, {m, n});
    auto a_update = BatchDot(y, /*transpose_x=*/true, a_panel,
                             /*transpose_y=*/false, precision);
    a_update = BatchDot(yt, a_update, precision);
    a_panel = a_panel + a_update;
    a = UpdateSliceInMinorDims(a, a_panel, {i, i + k});

    // q[:, i:] += (q[:, i:] @ y) @ (y @ t.T).T
    auto q_panel = SliceInMinorDims(q, {0, i}, {m, m});
    auto q_update = BatchDot(q_panel, y, precision);
    q_update = BatchDot(q_update, /*transpose_x=*/false, yt,
                        /*transpose_y=*/true, precision);
    q_panel = q_panel + q_update;
    q = UpdateSliceInMinorDims(q, q_panel, {0, i});
  }

  return Tuple(builder, {q, UpperTriangle(a)});
}

bool QrExpander::InstructionMatchesPattern(HloInstruction* instruction) {
  return instruction->opcode() == HloOpcode::kCustomCall &&
         instruction->custom_call_target() == "QrDecomposition";
}

StatusOr<HloInstruction*> QrExpander::ExpandInstruction(
    HloInstruction* instruction) {
  const string name =
      absl::StrFormat("xla.qr_%s", instruction->operand(0)->shape().ToString());

  HloModule* module = instruction->parent()->parent();

  HloComputation*& computation =
      computation_cache_.emplace(name, nullptr).first->second;
  if (!computation) {
    // Builds a new expansion.
    //
    // TODO(b/62327888): We do something unusual here: we build the computation
    // using the XlaBuilder API, which is nominally an XLA client API. We do
    // this because the external APIs for building complicated computations
    // (XlaBuilder) are much more ergonomic than the internal ones. As it turns
    // out, XlaBuilder isn't really a client API—what it does is build a
    // HloModuleProto protocol buffer, that we can then deserialize and clone
    // into our HloModule. Ideally we would avoid the protocol buffer step;
    // that is left as an exercise for future work.
    XlaBuilder builder(name);
    XlaOp a = Parameter(&builder, 0, instruction->operand(0)->shape(), "a");
    TF_ASSIGN_OR_RETURN(
        XlaOp l, BuildQrDecomposition(a,
                                      /*block_size=*/128,
                                      /*precision=*/PrecisionConfig::HIGHEST));

    TF_ASSIGN_OR_RETURN(XlaComputation xla_computation, builder.Build(l));

    TF_ASSIGN_OR_RETURN(ProgramShape program_shape,
                        xla_computation.GetProgramShape());
    HloModuleConfig config(program_shape);
    TF_ASSIGN_OR_RETURN(auto new_module, HloModule::CreateFromProto(
                                             xla_computation.proto(), config));
    HloCloneContext context(module);
    computation =
        module->DeepCloneComputation(new_module->entry_computation(), &context);
  }

  return instruction->parent()->AddInstruction(HloInstruction::CreateCall(
      instruction->shape(), instruction->operands(), computation));
}

}  // namespace xla