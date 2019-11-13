/*
 * Copyright (c) 2019, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cuml/manifold/umapparams.h>
#include <cuml/neighbors/knn.hpp>
#include "optimize.h"

#include "fuzzy_simpl_set/runner.h"
#include "init_embed/runner.h"
#include "knn_graph/runner.h"
#include "simpl_set_embed/runner.h"

#include <thrust/count.h>
#include <thrust/device_ptr.h>
#include <thrust/extrema.h>
#include <thrust/reduce.h>
#include <thrust/scan.h>
#include <thrust/system/cuda/execution_policy.h>

#include "sparse/coo.h"
#include "sparse/csr.h"

#include "cuda_utils.h"

#include <cuda_runtime.h>
#include <iostream>

namespace UMAPAlgo {

namespace Supervised {

using namespace ML;

using namespace MLCommon::Sparse;

template <int TPB_X, typename T>
__global__ void fast_intersection_kernel(int *rows, int *cols, T *vals, int nnz,
                                         T *target, float unknown_dist = 1.0,
                                         float far_dist = 5.0) {
  int row = (blockIdx.x * TPB_X) + threadIdx.x;
  if (row < nnz) {
    int i = rows[row];
    int j = cols[row];
    if (target[i] == T(-1.0) || target[j] == T(-1.0))
      vals[row] *= exp(-unknown_dist);
    else if (target[i] != target[j])
      vals[row] *= exp(-far_dist);
  }
}

template <typename T, int TPB_X>
void reset_local_connectivity(COO<T> *in_coo, COO<T> *out_coo,
                              std::shared_ptr<deviceAllocator> alloc,
                              cudaStream_t stream  // size = nnz*2
) {
  MLCommon::device_buffer<int> row_ind(alloc, stream, in_coo->n_rows);

  MLCommon::Sparse::sorted_coo_to_csr(in_coo, row_ind.data(), alloc, stream);

  // Perform l_inf normalization
  MLCommon::Sparse::csr_row_normalize_max<TPB_X, T>(
    row_ind.data(), in_coo->get_vals(), in_coo->nnz, in_coo->n_rows,
    in_coo->get_vals(), stream);
  CUDA_CHECK(cudaPeekAtLastError());

  MLCommon::Sparse::coo_symmetrize<TPB_X, T>(
    in_coo, out_coo,
    [] __device__(int row, int col, T result, T transpose) {
      T prod_matrix = result * transpose;
      return result + transpose - prod_matrix;
    },
    alloc, stream);

  CUDA_CHECK(cudaPeekAtLastError());
}

/**
 * Combine a fuzzy simplicial set with another fuzzy simplicial set
 * generated from categorical data using categorical distances. The target
 * data is assumed to be categorical label data (a vector of labels),
 * and this will update the fuzzy simplicial set to respect that label
 * data.
 */
template <typename T, int TPB_X>
void categorical_simplicial_set_intersection(COO<T> *graph_coo, T *target,
                                             cudaStream_t stream,
                                             float far_dist = 5.0,
                                             float unknown_dist = 1.0) {
  dim3 grid(MLCommon::ceildiv(graph_coo->nnz, TPB_X), 1, 1);
  dim3 blk(TPB_X, 1, 1);
  fast_intersection_kernel<TPB_X, T><<<grid, blk, 0, stream>>>(
    graph_coo->get_rows(), graph_coo->get_cols(), graph_coo->get_vals(),
    graph_coo->nnz, target, unknown_dist, far_dist);
}

template <typename T, int TPB_X>
__global__ void sset_intersection_kernel(int *row_ind1, int *cols1, T *vals1,
                                         int nnz1, int *row_ind2, int *cols2,
                                         T *vals2, int nnz2, int *result_ind,
                                         int *result_cols, T *result_vals,
                                         int nnz, T left_min, T right_min,
                                         int m, float mix_weight = 0.5) {
  int row = (blockIdx.x * TPB_X) + threadIdx.x;

  if (row < m) {
    int start_idx_res = result_ind[row];
    int stop_idx_res = MLCommon::Sparse::get_stop_idx(row, m, nnz, result_ind);

    int start_idx1 = row_ind1[row];
    int stop_idx1 = MLCommon::Sparse::get_stop_idx(row, m, nnz1, row_ind1);

    int start_idx2 = row_ind2[row];
    int stop_idx2 = MLCommon::Sparse::get_stop_idx(row, m, nnz2, row_ind2);

    for (int j = start_idx_res; j < stop_idx_res; j++) {
      int col = result_cols[j];

      T left_val = left_min;
      for (int k = start_idx1; k < stop_idx1; k++) {
        if (cols1[k] == col) {
          left_val = vals1[k];
        }
      }

      T right_val = right_min;
      for (int k = start_idx2; k < stop_idx2; k++) {
        if (cols2[k] == col) {
          right_val = vals2[k];
        }
      }

      if (left_val > left_min || right_val > right_min) {
        if (mix_weight < 0.5) {
          result_vals[j] =
            left_val * powf(right_val, mix_weight / (1.0 - mix_weight));
        } else {
          result_vals[j] =
            powf(left_val, (1.0 - mix_weight) / mix_weight) * right_val;
        }
      }
    }
  }
}

/**
 * Computes the CSR column index pointer and values
 * for the general simplicial set intersecftion.
 */
template <typename T, int TPB_X>
void general_simplicial_set_intersection(int *row1_ind, COO<T> *in1,
                                         int *row2_ind, COO<T> *in2,
                                         COO<T> *result, float weight,
                                         std::shared_ptr<deviceAllocator> alloc,
                                         cudaStream_t stream) {
  MLCommon::device_buffer<int> result_ind(alloc, stream, in1->n_rows);
  CUDA_CHECK(
    cudaMemsetAsync(result_ind.data(), 0, in1->n_rows * sizeof(int), stream));

  int result_nnz = MLCommon::Sparse::csr_add_calc_inds<float, 32>(
    row1_ind, in1->get_cols(), in1->get_vals(), in1->nnz, row2_ind,
    in2->get_cols(), in2->get_vals(), in2->nnz, in1->n_rows, result_ind.data(),
    alloc, stream);

  result->allocate(result_nnz, in1->n_rows, stream);

  /**
   * Element-wise sum of two simplicial sets
   */
  MLCommon::Sparse::csr_add_finalize<float, 32>(
    row1_ind, in1->get_cols(), in1->get_vals(), in1->nnz, row2_ind,
    in2->get_cols(), in2->get_vals(), in2->nnz, in1->n_rows, result_ind.data(),
    result->get_cols(), result->get_vals(), stream);

  //@todo: Write a wrapper function for this
  MLCommon::Sparse::csr_to_coo<TPB_X>(result_ind.data(), result->n_rows,
                                      result->get_rows(), result->nnz, stream);

  thrust::device_ptr<const T> d_ptr1 =
    thrust::device_pointer_cast(in1->get_vals());
  T min1 = *(thrust::min_element(thrust::cuda::par.on(stream), d_ptr1,
                                 d_ptr1 + in1->nnz));

  thrust::device_ptr<const T> d_ptr2 =
    thrust::device_pointer_cast(in2->get_vals());
  T min2 = *(thrust::min_element(thrust::cuda::par.on(stream), d_ptr2,
                                 d_ptr2 + in2->nnz));

  T left_min = max(min1 / 2.0, 1e-8);
  T right_min = max(min2 / 2.0, 1e-8);

  dim3 grid(MLCommon::ceildiv(in1->nnz, TPB_X), 1, 1);
  dim3 blk(TPB_X, 1, 1);

  sset_intersection_kernel<T, TPB_X><<<grid, blk, 0, stream>>>(
    row1_ind, in1->get_cols(), in1->get_vals(), in1->nnz, row2_ind,
    in2->get_cols(), in2->get_vals(), in2->nnz, result_ind.data(),
    result->get_cols(), result->get_vals(), result->nnz, left_min, right_min,
    in1->n_rows, weight);

  dim3 grid_n(MLCommon::ceildiv(result->nnz, TPB_X), 1, 1);
}

template <int TPB_X, typename T>
void perform_categorical_intersection(T *y, COO<T> *rgraph_coo,
                                      COO<T> *final_coo, UMAPParams *params,
                                      std::shared_ptr<deviceAllocator> alloc,
                                      cudaStream_t stream) {
  float far_dist = 1.0e12;  // target weight
  if (params->target_weights < 1.0)
    far_dist = 2.5 * (1.0 / (1.0 - params->target_weights));

  categorical_simplicial_set_intersection<T, TPB_X>(rgraph_coo, y, stream,
                                                    far_dist);

  COO<T> comp_coo(alloc, stream);
  coo_remove_zeros<TPB_X, T>(rgraph_coo, &comp_coo, alloc, stream);

  reset_local_connectivity<T, TPB_X>(&comp_coo, final_coo, alloc, stream);

  CUDA_CHECK(cudaPeekAtLastError());
}

template <int TPB_X, typename T>
void perform_general_intersection(const cumlHandle &handle, T *y,
                                  COO<T> *rgraph_coo, COO<T> *final_coo,
                                  UMAPParams *params, cudaStream_t stream) {
  auto alloc = handle.getDeviceAllocator();

  /**
   * Calculate kNN for Y
   */
  int knn_dims = rgraph_coo->n_rows * params->target_n_neighbors;
  MLCommon::device_buffer<long> y_knn_indices(alloc, stream, knn_dims);
  MLCommon::device_buffer<T> y_knn_dists(alloc, stream, knn_dims);

  kNNGraph::run(y, rgraph_coo->n_rows, y, rgraph_coo->n_rows, 1,
                y_knn_indices.data(), y_knn_dists.data(),
                params->target_n_neighbors, params, stream);
  CUDA_CHECK(cudaPeekAtLastError());

  if (params->verbose) {
    std::cout << "Target kNN Graph" << std::endl;
    std::cout << MLCommon::arr2Str(
                   y_knn_indices.data(),
                   rgraph_coo->n_rows * params->target_n_neighbors,
                   "knn_indices", stream)
              << std::endl;
    std::cout << MLCommon::arr2Str(
                   y_knn_dists.data(),
                   rgraph_coo->n_rows * params->target_n_neighbors, "knn_dists",
                   stream)
              << std::endl;
  }

  /**
   * Compute fuzzy simplicial set
   */
  COO<T> ygraph_coo(alloc, stream);

  FuzzySimplSet::run<TPB_X, T>(rgraph_coo->n_rows, y_knn_indices.data(),
                               y_knn_dists.data(), params->target_n_neighbors,
                               &ygraph_coo, params, alloc, stream);
  CUDA_CHECK(cudaPeekAtLastError());

  if (params->verbose) {
    std::cout << "Target Fuzzy Simplicial Set" << std::endl;
    std::cout << ygraph_coo << std::endl;
  }

  /**
   * Compute general simplicial set intersection.
   */
  MLCommon::device_buffer<int> xrow_ind(alloc, stream, rgraph_coo->n_rows);
  MLCommon::device_buffer<int> yrow_ind(alloc, stream, ygraph_coo.n_rows);

  CUDA_CHECK(cudaMemsetAsync(xrow_ind.data(), 0,
                             rgraph_coo->n_rows * sizeof(int), stream));
  CUDA_CHECK(cudaMemsetAsync(yrow_ind.data(), 0,
                             ygraph_coo.n_rows * sizeof(int), stream));

  COO<T> cygraph_coo(alloc, stream);
  coo_remove_zeros<TPB_X, T>(&ygraph_coo, &cygraph_coo, alloc, stream);

  MLCommon::Sparse::sorted_coo_to_csr(&cygraph_coo, yrow_ind.data(), alloc,
                                      stream);
  MLCommon::Sparse::sorted_coo_to_csr(rgraph_coo, xrow_ind.data(), alloc,
                                      stream);

  COO<T> result_coo(alloc, stream);
  general_simplicial_set_intersection<T, TPB_X>(
    xrow_ind.data(), rgraph_coo, yrow_ind.data(), &cygraph_coo, &result_coo,
    params->target_weights, alloc, stream);

  /**
   * Remove zeros
   */
  COO<T> out(alloc, stream);
  coo_remove_zeros<TPB_X, T>(&result_coo, &out, alloc, stream);

  reset_local_connectivity<T, TPB_X>(&out, final_coo, alloc, stream);

  CUDA_CHECK(cudaPeekAtLastError());
}
}  // namespace Supervised
}  // namespace UMAPAlgo
