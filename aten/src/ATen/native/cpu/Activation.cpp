#define _USE_MATH_DEFINES

#include <ATen/native/Activation.h>

#include <math.h>

#include <ATen/ATen.h>
#include <ATen/Config.h>
#include <ATen/cpu/vec256/vec256.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/cpu/Loops.h>
#include <ATen/Parallel.h>

#if AT_MKL_ENABLED()
#include <mkl.h>
#endif // AT_MKL_ENABLED()

namespace at {
namespace native {

namespace {

template <typename scalar_t>
inline void _vec_log_sigmoid(Tensor& output, Tensor& buffer, const Tensor& input) {
  using Vec = Vec256<scalar_t>;
  scalar_t* output_data = output.data_ptr<scalar_t>();
  scalar_t* buffer_data = buffer.data_ptr<scalar_t>();
  scalar_t* input_data = input.data_ptr<scalar_t>();
  parallel_for(0, input.numel(), 1, [&] (int64_t begin, int64_t end) {
    int64_t size = end - begin;
    int64_t d = 0;
    for (; d < size - (size % Vec::size()); d += Vec::size()) {
      Vec data_vec = Vec::loadu(input_data + begin+ d);
      Vec max_vec = vec256::maximum(data_vec.neg(), Vec(scalar_t(0)));
      Vec buffer_vec =  max_vec.neg().exp() + (data_vec.neg() - max_vec).exp();
      Vec output_vec = (max_vec + buffer_vec.log()).neg();
      buffer_vec.store(buffer_data + begin + d);
      output_vec.store(output_data + begin + d);
    }
    if (size - d > 0) {
      Vec data_vec = Vec::loadu(input_data + begin + d, size - d);
      Vec max_vec = vec256::maximum(data_vec.neg(), Vec(scalar_t(0)));
      Vec buffer_vec =  max_vec.neg().exp() + (data_vec.neg() - max_vec).exp();
      Vec output_vec = (max_vec + buffer_vec.log()).neg();
      buffer_vec.store(buffer_data + begin + d, size - d);
      output_vec.store(output_data + begin + d, size - d);
    }
  });
}

static void log_sigmoid_cpu_kernel(Tensor& output, Tensor& buffer, const Tensor& input) {
  AT_DISPATCH_FLOATING_TYPES(input.scalar_type(), "log_sigmoid_cpu", [&] {
    _vec_log_sigmoid<scalar_t>(output, buffer, input);
  });
}

static void log_sigmoid_backward_cpu_kernel(TensorIterator& iter) {
  AT_DISPATCH_FLOATING_TYPES(iter.dtype(), "log_sigmoid_backward_cpu", [&]() {
    using Vec = Vec256<scalar_t>;
    auto zero_val = scalar_t(0);
    auto zero_vec = Vec(zero_val);
    auto one_val = scalar_t(1);
    auto one_vec = Vec(one_val);
    cpu_kernel_vec(iter,
      [=](scalar_t a, scalar_t b, scalar_t c) -> scalar_t {
        auto max_deriv_val = zero_val;
        auto sign_val = -one_val;
        if (a < zero_val) {
          max_deriv_val = -one_val;
          sign_val = one_val;
        }
        return (-max_deriv_val - sign_val * ((b - one_val) / b)) * c;
      },
      [=](Vec a, Vec b, Vec c) -> Vec {
        auto mask = a < zero_vec;
        auto max_deriv_vec = Vec::blendv(zero_vec, one_vec.neg(), mask);
        auto sign_vec = Vec::blendv(one_vec.neg(), one_vec, mask);
        return (max_deriv_vec + sign_vec * ((b - one_vec) / b)).neg() * c;
      });
  });
}

static void threshold_kernel(
    TensorIterator& iter,
    Scalar threshold_scalar,
    Scalar value_scalar) {
  AT_DISPATCH_ALL_TYPES(iter.dtype(), "threshold_cpu", [&] {
    using Vec = Vec256<scalar_t>;
    scalar_t threshold = threshold_scalar.to<scalar_t>();
    Vec threshold_v = Vec(threshold);
    scalar_t value = value_scalar.to<scalar_t>();
    Vec value_v = Vec(value);
    cpu_kernel_vec(
        iter,
        [&](scalar_t x, scalar_t other) -> scalar_t {
          return x <= threshold ? value : other;
        },
        [&](Vec x, Vec other) -> Vec {
          return Vec::blendv(other, value_v, x <= threshold_v);
        });
  });
}

#if AT_MKL_ENABLED()

template <typename T>
void MKLCdfNorm(int64_t N, const T* X, T* Y);

template <>
void MKLCdfNorm<float>(int64_t N, const float* X, float* Y) {
  vsCdfNorm(N, X, Y);
}

template <>
void MKLCdfNorm<double>(int64_t N, const double* X, double* Y) {
  vdCdfNorm(N, X, Y);
}

template <typename T>
void MKLMul(int64_t N, const T* A, const T* B, T* Y);

template <>
void MKLMul<float>(int64_t N, const float* A, const float* B, float* Y) {
  vsMul(N, A, B, Y);
}

template <>
void MKLMul<double>(int64_t N, const double* A, const double* B, double* Y) {
  vdMul(N, A, B, Y);
}

template <typename T>
void MKLExp(int64_t N, const T* X, T* Y);

template <>
void MKLExp<float>(int64_t N, const float* X, float* Y) {
  vsExp(N, X, Y);
}

template <>
void MKLExp<double>(int64_t N, const double* X, double* Y) {
  vdExp(N, X, Y);
}

template <typename T>
void GeluMKLKernelImpl(TensorIterator* it) {
  if (!it->can_use_32bit_indexing()) {
    for (auto& sub_it : it->with_32bit_indexing()) {
      GeluMKLKernelImpl<T>(&sub_it);
    }
    return;
  }
  const int64_t N = it->numel();
  const T* X_data = static_cast<T*>(it->data_ptr(1));
  T* Y_data = static_cast<T*>(it->data_ptr(0));
  MKLCdfNorm<T>(N, X_data, Y_data);
  MKLMul<T>(N, X_data, Y_data, Y_data);
}

template <typename T>
void GeluBackwardMKLKernelImpl(TensorIterator* it) {
  if (!it->can_use_32bit_indexing()) {
    for (auto& sub_it : it->with_32bit_indexing()) {
      GeluBackwardMKLKernelImpl<T>(&sub_it);
    }
    return;
  }
  constexpr T kBeta = M_2_SQRTPI * M_SQRT1_2 * T(0.5);
  const int64_t N = it->numel();
  const T* dY_data = static_cast<T*>(it->data_ptr(1));
  const T* X_data = static_cast<T*>(it->data_ptr(2));
  T* dX_data = static_cast<T*>(it->data_ptr(0));
  Tensor cdf = at::empty({N}, it->input(1).options());
  T* cdf_data = cdf.template data_ptr<T>();
  MKLCdfNorm<T>(N, X_data, cdf_data);
  for (int64_t i = 0; i < N; ++i) {
    dX_data[i] = T(-0.5) * X_data[i] * X_data[i];
  }
  MKLExp(N, dX_data, dX_data);
  for (int64_t i = 0; i < N; ++i) {
    dX_data[i] = dY_data[i] * (cdf_data[i] + kBeta * X_data[i] * dX_data[i]);
  }
}

#else // AT_MKL_ENABLED()

template <typename T>
void GeluMKLKernelImpl(TensorIterator* /* it */) {
  AT_ASSERTM(false, "ATen not compiled with MKL");
}

template <typename T>
void GeluBackwardMKLKernelImpl(TensorIterator* /* it */) {
  AT_ASSERTM(false, "ATen not compiled with MKL");
}

#endif // AT_MKL_ENABLED()

void elu_kernel(TensorIterator& it, Scalar alpha, Scalar scale, Scalar input_scale) {
  AT_DISPATCH_FLOATING_TYPES(it.dtype(), "elu_cpu", [&]() {
    auto negcoef = alpha.to<scalar_t>() * scale.to<scalar_t>();
    auto poscoef = scale.to<scalar_t>();
    auto negiptcoef = input_scale.to<scalar_t>();
    cpu_kernel(it, [=](scalar_t a) -> scalar_t {
      return a <= scalar_t(0) ? (std::exp(a * negiptcoef) - scalar_t(1)) * negcoef : a * poscoef;
    });
  });
}

void elu_backward_kernel(TensorIterator& it, Scalar alpha, Scalar scale, Scalar input_scale) {
  AT_DISPATCH_FLOATING_TYPES(it.dtype(), "elu_backward_cpu", [&]() {
    auto negcoef = alpha.to<scalar_t>() * scale.to<scalar_t>();
    auto poscoef = scale.to<scalar_t>();
    auto negiptcoef = input_scale.to<scalar_t>();
    cpu_kernel(it, [=](scalar_t a, scalar_t b) -> scalar_t {
      return b <= scalar_t(0) ? a * negiptcoef * (b + negcoef) : a * poscoef;
    });
  });
}

// TODO(yangxm): Add another fast kernel using formula
// y = 0.5x * (1 + tanh(sqrt(2/Pi) * (x + 0.044715x^3)))
// and the fast tanh impl from Eigen.
void GeluKernelImpl(TensorIterator& it) {
  if (at::hasMKL() && it.is_contiguous()) {
    AT_DISPATCH_FLOATING_TYPES(it.dtype(), "GeluKernelImpl", [&]() {
      GeluMKLKernelImpl<scalar_t>(&it);
    });
  } else {
    AT_DISPATCH_FLOATING_TYPES(it.dtype(), "GeluKernelImpl", [&]() {
      using Vec = vec256::Vec256<scalar_t>;
      const Vec kAlphaVec(M_SQRT1_2);
      const Vec kOneVec(1);
      const Vec kPointFiveVec(0.5);
      cpu_kernel_vec(
          it,
          [](scalar_t x) {
            constexpr scalar_t kAlpha = M_SQRT1_2;
            return x * scalar_t(0.5) * (scalar_t(1) + std::erf(x * kAlpha));
          },
          [&](Vec x_vec) {
            return x_vec * kPointFiveVec *
                (kOneVec + (x_vec * kAlphaVec).erf());
          });
    });
  }
}

void GeluBackwardKernelImpl(TensorIterator& it) {
  if (hasMKL() && it.is_contiguous()) {
    AT_DISPATCH_FLOATING_TYPES(it.dtype(), "GeluBackwardKernelImpl", [&]() {
      GeluBackwardMKLKernelImpl<scalar_t>(&it);
    });
  } else {
    AT_DISPATCH_FLOATING_TYPES(it.dtype(), "GeluBackwardKernelImpl", [&]() {
      using Vec = vec256::Vec256<scalar_t>;
      const Vec kAlphaVec(M_SQRT1_2);
      const Vec kBetaVec(M_2_SQRTPI * M_SQRT1_2 * 0.5);
      const Vec kOneVec(1);
      const Vec kPointFiveVec(0.5);
      const Vec kMinusPointFiveVec(-0.5);
      cpu_kernel_vec(
          it,
          [](scalar_t dy, scalar_t x) {
            constexpr scalar_t kAlpha = M_SQRT1_2;
            constexpr scalar_t kBeta = M_2_SQRTPI * M_SQRT1_2 * 0.5;
            const scalar_t cdf =
                scalar_t(0.5) * (scalar_t(1) + std::erf(x * kAlpha));
            const scalar_t pdf = kBeta * std::exp(x * x * scalar_t(-0.5));
            return dy * (cdf + x * pdf);
          },
          [&](Vec dy_vec, Vec x_vec) {
            const Vec cdf_vec =
                kPointFiveVec * (kOneVec + (x_vec * kAlphaVec).erf());
            const Vec pdf_vec =
                kBetaVec * (x_vec * x_vec * kMinusPointFiveVec).exp();
            return dy_vec * (cdf_vec + x_vec * pdf_vec);
          });
    });
  }
}

void hardshrink_kernel(TensorIterator& iter, Scalar lambd) {
  AT_DISPATCH_FLOATING_TYPES(iter.dtype(), "hardshrink_cpu", [&] {
    auto lambd_val = lambd.to<scalar_t>();
    cpu_kernel_vec(
        iter,
        [=](scalar_t self_val) {
          return (self_val >= -lambd_val && self_val <= lambd_val) ? scalar_t(0)
                                                                   : self_val;
        },
        [=](Vec256<scalar_t> self_val) {
          return ((self_val < -lambd_val) | (self_val > lambd_val)) & self_val;
        });
  });
}

void softshrink_kernel(TensorIterator& iter, Scalar lambd) {
  AT_DISPATCH_FLOATING_TYPES_AND_HALF(iter.dtype(), "softshrink_cpu", [&]() {
    auto lambd_val = lambd.to<scalar_t>();
    cpu_kernel(iter, [=](scalar_t a) -> scalar_t {
      return a > lambd_val ? a - lambd_val : (a < -lambd_val ? a + lambd_val : scalar_t(0));
    });
  });
}

void shrink_backward_kernel(TensorIterator& iter, Scalar lambd) {
  AT_DISPATCH_FLOATING_TYPES(iter.dtype(), "shrink_backward_cpu", [&] {
    auto lambd_val = lambd.to<scalar_t>();
    cpu_kernel_vec(
        iter,
        [=](scalar_t grad_val, scalar_t self_val) {
          return (self_val >= -lambd_val && self_val <= lambd_val) ? scalar_t(0)
                                                                   : grad_val;
        },
        [=](Vec256<scalar_t> grad_val, Vec256<scalar_t> self_val) {
          return ((self_val < -lambd_val) | (self_val > lambd_val)) & grad_val;
        });
  });
}

void hardtanh_backward_kernel(TensorIterator& iter, Scalar min, Scalar max) {
  AT_DISPATCH_FLOATING_TYPES_AND_HALF(iter.dtype(), "hardshrink_backward_cpu", [&] {
    auto min_val = min.to<scalar_t>();
    auto max_val = max.to<scalar_t>();
    cpu_kernel_vec(
        iter,
        [=](scalar_t grad_val, scalar_t self_val) {
          return (self_val <= min_val || self_val >= max_val) ? scalar_t(0) : grad_val;
        },
        [=](Vec256<scalar_t> grad_val, Vec256<scalar_t> self_val) {
          return ((self_val > min_val) & (self_val < max_val)) & grad_val;
        });
  });
}

static void leaky_relu_kernel(TensorIterator& iter, Scalar negval_) {
  AT_DISPATCH_FLOATING_TYPES(iter.dtype(), "leaky_relu_cpu", [&] {
    using Vec = Vec256<scalar_t>;
    auto zero_vec = Vec((scalar_t)(0));
    auto one_vec = Vec((scalar_t)(1));
    scalar_t negval = negval_.to<scalar_t>();
    Vec negval_v = Vec(negval);
    cpu_kernel_vec(
        iter,
        [&](scalar_t a) -> scalar_t {
          return a > scalar_t(0) ? a : a * negval;
        },
        [&](Vec a) -> Vec {
          auto r = Vec::blendv(negval_v, one_vec, a > zero_vec);
          return a * r;
        });
  });
}

static void leaky_relu_backward_kernel(TensorIterator& iter, Scalar negval_) {
  AT_DISPATCH_FLOATING_TYPES(iter.dtype(), "leaky_relu_backward_cpu", [&] {
    using Vec = Vec256<scalar_t>;
    auto zero_vec = Vec((scalar_t)(0));
    auto one_vec = Vec((scalar_t)(1));
    scalar_t negval = negval_.to<scalar_t>();
    Vec negval_v = Vec(negval);
    cpu_kernel_vec(
        iter,
        [&](scalar_t a, scalar_t b) -> scalar_t {
          return a > scalar_t(0) ? b : b * negval;
        },
        [&](Vec a, Vec b) -> Vec {
          auto r = Vec::blendv(negval_v, one_vec, a > zero_vec);
          return b * r;
        });
  });
}

void softplus_kernel(TensorIterator& iter, Scalar beta_, Scalar threshold_) {
  AT_DISPATCH_FLOATING_TYPES(iter.dtype(), "softplus_cpu", [&]() {
    using Vec = Vec256<scalar_t>;
    auto beta = beta_.to<scalar_t>();
    auto threshold = threshold_.to<scalar_t>();
    const Vec beta_vec(beta);
    const Vec threshold_vec(threshold);
    cpu_kernel_vec(
        iter,
        [beta, threshold](scalar_t a) -> scalar_t {
          return (a * beta) > threshold ? a
            : static_cast<scalar_t>(std::log1p(std::exp(a * beta))) / beta;
        },
        [beta_vec, threshold_vec](Vec a) -> Vec {
          return Vec::blendv((a * beta_vec).exp().log1p() / beta_vec, a, (a * beta_vec) > threshold_vec);
        }
    );
  });
}

void softplus_backward_kernel(TensorIterator& iter, Scalar beta_, Scalar threshold_) {
  AT_DISPATCH_FLOATING_TYPES(iter.dtype(), "softplus_backward_cpu", [&]() {
    using Vec = Vec256<scalar_t>;
    auto beta = beta_.to<scalar_t>();
    auto threshold = threshold_.to<scalar_t>();
    const Vec beta_vec(beta);
    const Vec threshold_vec(threshold);
    const Vec one_vec(static_cast<scalar_t>(1.0));
    cpu_kernel_vec(
        iter,
        [beta, threshold](scalar_t a, scalar_t b) -> scalar_t {
          scalar_t z = std::exp(b * beta);
          return (b * beta) > threshold ? a : a * (z - scalar_t(1.)) / z;
        },
        [beta_vec, one_vec, threshold_vec](Vec a, Vec b) -> Vec {
          const Vec z = (b * beta_vec).exp();
          return Vec::blendv(a * (z - one_vec) / z, a, (b * beta_vec) > threshold_vec);
        }
    );
  });
}

} // namespace

REGISTER_DISPATCH(log_sigmoid_cpu_stub, &log_sigmoid_cpu_kernel);
REGISTER_DISPATCH(log_sigmoid_backward_cpu_stub, &log_sigmoid_backward_cpu_kernel);
REGISTER_DISPATCH(threshold_stub, &threshold_kernel);
REGISTER_DISPATCH(elu_stub, &elu_kernel);
REGISTER_DISPATCH(elu_backward_stub, &elu_backward_kernel);
REGISTER_DISPATCH(GeluKernel, &GeluKernelImpl);
REGISTER_DISPATCH(GeluBackwardKernel, &GeluBackwardKernelImpl);
REGISTER_DISPATCH(hardtanh_backward_stub, &hardtanh_backward_kernel);
REGISTER_DISPATCH(hardshrink_stub, &hardshrink_kernel);
REGISTER_DISPATCH(softshrink_stub, &softshrink_kernel);
REGISTER_DISPATCH(shrink_backward_stub, &shrink_backward_kernel);
REGISTER_DISPATCH(leaky_relu_stub, &leaky_relu_kernel);
REGISTER_DISPATCH(leaky_relu_backward_stub, &leaky_relu_backward_kernel);
REGISTER_DISPATCH(softplus_stub, &softplus_kernel);
REGISTER_DISPATCH(softplus_backward_stub, &softplus_backward_kernel);

} // namespace native
} // namespace at
