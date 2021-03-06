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

#include "tensorflow/lite/delegates/gpu/cl/kernels/conv_3d.h"

#include <algorithm>
#include <string>
#include <utility>

#include "absl/strings/substitute.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/util.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/work_group_picking.h"
#include "tensorflow/lite/delegates/gpu/cl/precision.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor_type.h"
#include "tensorflow/lite/delegates/gpu/common/data_type.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"

namespace tflite {
namespace gpu {
namespace cl {

Conv3D::Conv3D(const OperationDef& definition,
               const Convolution3DAttributes& attr, const CLDevice& device)
    : GPUOperation(definition),
      stride_(attr.strides.w, attr.strides.h, attr.strides.d),
      padding_(-attr.padding.prepended.w, -attr.padding.prepended.h,
               -attr.padding.prepended.d),
      kernel_size_(attr.weights.shape.w, attr.weights.shape.h,
                   attr.weights.shape.d),
      dilation_(attr.dilations.w, attr.dilations.h, attr.dilations.d),
      conv_params_(GuessBestParams(device, definition, attr)) {}

Conv3D::Conv3D(Conv3D&& operation)
    : GPUOperation(std::move(operation)),
      stride_(operation.stride_),
      padding_(operation.padding_),
      kernel_size_(operation.kernel_size_),
      dilation_(operation.dilation_),
      conv_params_(operation.conv_params_),
      kernel_(std::move(operation.kernel_)) {}

Conv3D& Conv3D::operator=(Conv3D&& operation) {
  if (this != &operation) {
    std::swap(stride_, operation.stride_);
    std::swap(padding_, operation.padding_);
    std::swap(kernel_size_, operation.kernel_size_);
    std::swap(dilation_, operation.dilation_);
    std::swap(conv_params_, operation.conv_params_);
    kernel_ = std::move(operation.kernel_);
    GPUOperation::operator=(std::move(operation));
  }
  return *this;
}

absl::Status Conv3D::Compile(const CreationContext& creation_context) {
  const bool stride_correction =
      definition_.IsBatchSupported() && stride_.x != 1;
  std::string code =
      GenerateConv3D(definition_, stride_correction, conv_params_, &args_);
  std::string element_wise_code;
  RETURN_IF_ERROR(
      MergeOperations(linked_operations_, &args_, &element_wise_code));
  RETURN_IF_ERROR(args_.TransformToCLCode(creation_context.device->GetInfo(),
                                          {{"dst_tensor", element_wise_code}},
                                          &code));

  std::vector<CompilerOptions> options;
  if (definition_.precision == CalculationsPrecision::F16 &&
      creation_context.device->IsPowerVR()) {
    options.push_back(CompilerOptions::POWERVR_FP16);
  }
  return creation_context.cache->GetOrCreateCLKernel(
      code, "main_function", options, *creation_context.context,
      *creation_context.device, &kernel_);
}

absl::Status Conv3D::BindArguments() {
  RETURN_IF_ERROR(args_.SetObjectRef("src_tensor", src_[0]));
  RETURN_IF_ERROR(args_.SetObjectRef("dst_tensor", dst_[0]));
  if (!conv_params_.x_kernel_is_1) {
    RETURN_IF_ERROR(args_.SetInt("stride_x", stride_.x));
    RETURN_IF_ERROR(args_.SetInt("padding_x", padding_.x * src_[0]->Batch()));
    RETURN_IF_ERROR(args_.SetInt("kernel_size_x", kernel_size_.x));
    RETURN_IF_ERROR(args_.SetInt("dilation_x", dilation_.x * src_[0]->Batch()));
  }
  if (!conv_params_.y_kernel_is_1) {
    RETURN_IF_ERROR(args_.SetInt("stride_y", stride_.y));
    RETURN_IF_ERROR(args_.SetInt("padding_y", padding_.y));
    RETURN_IF_ERROR(args_.SetInt("kernel_size_y", kernel_size_.y));
    RETURN_IF_ERROR(args_.SetInt("dilation_y", dilation_.y));
  }
  if (!conv_params_.z_kernel_is_1) {
    RETURN_IF_ERROR(args_.SetInt("stride_z", stride_.z));
    RETURN_IF_ERROR(args_.SetInt("padding_z", padding_.z));
    RETURN_IF_ERROR(args_.SetInt("kernel_size_z", kernel_size_.z));
    RETURN_IF_ERROR(args_.SetInt("dilation_z", dilation_.z));
  }
  RETURN_IF_ERROR(args_.SetInt(
      "grid_size_s",
      DivideRoundUp(dst_[0]->Slices(), conv_params_.block_size.w)));
  RETURN_IF_ERROR(SetArguments(linked_operations_, &args_));
  return args_.Bind(kernel_.kernel());
}

int3 Conv3D::GetGridSize() const {
  const int grid_x = DivideRoundUp(dst_[0]->Width() * dst_[0]->Batch(),
                                   conv_params_.block_size.x);
  const int grid_y =
      DivideRoundUp(dst_[0]->Height(), conv_params_.block_size.y);
  const int grid_z =
      DivideRoundUp(dst_[0]->Slices(), conv_params_.block_size.w) *
      DivideRoundUp(dst_[0]->Depth(), conv_params_.block_size.z);
  int3 wg;
  wg.x = DivideRoundUp(grid_x, conv_params_.work_group_size.x);
  wg.y = DivideRoundUp(grid_y, conv_params_.work_group_size.y);
  wg.z = DivideRoundUp(grid_z, conv_params_.work_group_size.z);
  return int3(wg[conv_params_.work_group_launch_order[0]] *
                  conv_params_.work_group_size.x,
              wg[conv_params_.work_group_launch_order[1]] *
                  conv_params_.work_group_size.y,
              wg[conv_params_.work_group_launch_order[2]] *
                  conv_params_.work_group_size.z);
}

absl::Status Conv3D::Tune(const TuningParameters& params) {
  if (conv_params_.weights_upload_type ==
          WeightsUploadType::LOCAL_MEM_ASYNC_SUBGROUP ||
      conv_params_.weights_upload_type ==
          WeightsUploadType::LOCAL_MEM_BY_THREADS) {
    return absl::OkStatus();
  }
  if (conv_params_.work_group_launch_order[0] == 0 &&
      conv_params_.work_group_launch_order[1] == 1 &&
      conv_params_.work_group_launch_order[2] == 2) {
    RETURN_IF_ERROR(BindArguments());
    return GetBestWorkGroupConv(params, kernel_, GetGridSize(),
                                &conv_params_.work_group_size);
  }
  return absl::OkStatus();
}

absl::Status Conv3D::AddToQueue(CLCommandQueue* queue) {
  RETURN_IF_ERROR(BindArguments());
  return queue->DispatchImplicit(kernel_, GetGridSize(),
                                 conv_params_.work_group_size);
}

namespace {
std::string GenerateUploadByThreads(const std::string& local_ptr_name,
                                    const std::string& global_ptr_name,
                                    const std::string& global_offset_name,
                                    const std::string& lid_name,
                                    int total_work_items,
                                    int elements_to_upload) {
  std::string c;
  std::string offset =
      global_offset_name.empty() ? "" : global_offset_name + " + ";
  const int groups = elements_to_upload / total_work_items;
  const int reminder = elements_to_upload % total_work_items;
  for (int i = 0; i < groups; ++i) {
    c += "    " + local_ptr_name + "[" + lid_name + " + " +
         std::to_string(total_work_items * i) + "] = " + global_ptr_name + "[" +
         offset + lid_name + " + " + std::to_string(total_work_items * i) +
         "];\n";
  }
  if (reminder != 0) {
    c += "    if (" + lid_name + " < " + std::to_string(reminder) + ") {\n";
    c += "      " + local_ptr_name + "[" + lid_name + " + " +
         std::to_string(total_work_items * groups) + "] = " + global_ptr_name +
         "[" + offset + lid_name + " + " +
         std::to_string(total_work_items * groups) + "];\n";
    c += "    }\n";
  }
  return c;
}

std::string GenerateAsyncUpload(const std::string& local_ptr_name,
                                const std::string& global_ptr_name,
                                const std::string& global_offset_name,
                                int elements_to_upload) {
  std::string c;
  std::string offset =
      global_offset_name.empty() ? "" : " + " + global_offset_name;
  c += "    async_work_group_copy(" + local_ptr_name + ", " + global_ptr_name +
       offset + ", " + std::to_string(elements_to_upload) + ", 0);\n";
  return c;
}

std::string GenerateGlobalCoordinates(const int4& block_size,
                                      const int3& work_group_launch_order) {
  std::string c;
  int3 launch_remap;
  launch_remap[work_group_launch_order.x] = 0;
  launch_remap[work_group_launch_order.y] = 1;
  launch_remap[work_group_launch_order.z] = 2;
  if (work_group_launch_order[0] == 0) {
    c += "  int DST_X = get_global_id(0) * " + std::to_string(block_size.x) +
         ";\n";
  } else {
    c += "  int DST_X = (get_group_id(" + std::to_string(launch_remap[0]) +
         ") * get_local_size(0) + get_local_id(0)) * " +
         std::to_string(block_size.x) + ";\n";
  }
  if (work_group_launch_order[1] == 1) {
    c += "  int DST_Y = get_global_id(1) * " + std::to_string(block_size.y) +
         ";\n";
  } else {
    c += "  int DST_Y = (get_group_id(" + std::to_string(launch_remap[1]) +
         ") * get_local_size(1) + get_local_id(1)) * " +
         std::to_string(block_size.y) + ";\n";
  }
  if (work_group_launch_order[2] == 2) {
    c += "  int linear_id_z = get_global_id(2);\n";
  } else {
    c += "  int linear_id_z = get_group_id(" + std::to_string(launch_remap[2]) +
         ") * get_local_size(2) + get_local_id(2);\n";
  }
  c += "  int DST_S = (linear_id_z % args.grid_size_s) * " +
       std::to_string(block_size.w) + ";\n";
  c += "  int DST_Z = (linear_id_z / args.grid_size_s) * " +
       std::to_string(block_size.z) + ";\n";
  return c;
}

std::string GenerateConv(CalculationsPrecision precision,
                         const int4& block_size, int offset,
                         bool weights_are_buffer) {
  std::string c;
  const std::string channels[] = {"x", "y", "z", "w"};
  for (int s = 0; s < block_size.w; ++s) {
    switch (precision) {
      case CalculationsPrecision::F32:
      case CalculationsPrecision::F16:
        for (int ch = 0; ch < 4; ++ch) {
          const std::string weight_id = std::to_string(s * 4 + ch + offset);
          std::string weight_name;
          if (weights_are_buffer) {
            weight_name = "weights_cache[" + weight_id + "]";
          } else {
            weight_name = "f" + weight_id;
          }
          for (int z = 0; z < block_size.z; ++z) {
            for (int y = 0; y < block_size.y; ++y) {
              for (int x = 0; x < block_size.x; ++x) {
                std::string id =
                    std::to_string(z) + std::to_string(y) + std::to_string(x);
                c += "    r" + std::to_string(s) + id + " += " + weight_name +
                     " * src" + id + "." + channels[ch] + ";\n";
              }
            }
          }
        }
        break;
      case CalculationsPrecision::F32_F16:
        for (int z = 0; z < block_size.z; ++z) {
          for (int y = 0; y < block_size.y; ++y) {
            for (int x = 0; x < block_size.x; ++x) {
              std::string id =
                  std::to_string(z) + std::to_string(y) + std::to_string(x);
              std::vector<std::string> weight_names(4);
              for (int i = 0; i < 4; ++i) {
                std::string weight_id = std::to_string(s * 4 + i + offset);
                if (weights_are_buffer) {
                  weight_names[i] = "weights_cache[" + weight_id + "]";
                } else {
                  weight_names[i] = "f" + weight_id;
                }
              }
              c += absl::Substitute(
                  "    $0 += convert_float4($1.x * $2 + $1.y * $3 + $1.z * "
                  "$4 + $1.w * $5);\n",
                  "r" + std::to_string(s) + id, "src" + id, weight_names[0],
                  weight_names[1], weight_names[2], weight_names[3]);
            }
          }
        }
        break;
    }
  }
  return c;
}
}  // namespace

std::string GenerateConv3D(const OperationDef& op_def, bool stride_correction,
                           const Conv3D::ConvParams& conv_params,
                           Arguments* args) {
  auto src_desc = absl::make_unique<TensorDescriptor>(op_def.src_tensors[0]);
  src_desc->SetTextureAddressMode(TextureAddressMode::ZERO);
  if (op_def.IsBatchSupported()) {
    src_desc->SetStateVar("BatchedWidth", "true");
  }
  args->AddObjectRef("src_tensor", AccessType::READ, std::move(src_desc));
  auto dst_desc = absl::make_unique<TensorDescriptor>(op_def.dst_tensors[0]);
  if (op_def.IsBatchSupported()) {
    dst_desc->SetStateVar("BatchedWidth", "true");
  }
  args->AddObjectRef("dst_tensor", AccessType::WRITE, std::move(dst_desc));
  if (!conv_params.x_kernel_is_1) {
    args->AddInt("stride_x");
    args->AddInt("padding_x");
    args->AddInt("kernel_size_x");
    args->AddInt("dilation_x");
  }
  if (!conv_params.y_kernel_is_1) {
    args->AddInt("stride_y");
    args->AddInt("padding_y");
    args->AddInt("kernel_size_y");
    args->AddInt("dilation_y");
  }
  if (!conv_params.z_kernel_is_1) {
    args->AddInt("stride_z");
    args->AddInt("padding_z");
    args->AddInt("kernel_size_z");
    args->AddInt("dilation_z");
  }
  args->AddInt("grid_size_s");

  const auto src_tensor_type = op_def.src_tensors[0].storage_type;
  const bool buffer_type = src_tensor_type == TensorStorageType::BUFFER ||
                           src_tensor_type == TensorStorageType::IMAGE_BUFFER;

  const bool manual_clamp_x = buffer_type && !conv_params.x_kernel_is_1;
  const bool manual_clamp_y = buffer_type && !conv_params.y_kernel_is_1;
  const bool manual_clamp_z =
      src_tensor_type != TensorStorageType::TEXTURE_3D &&
      !conv_params.z_kernel_is_1;

  const bool can_read_out_of_x = !buffer_type;
  const bool can_read_out_of_y = !buffer_type;
  const bool can_read_out_of_z =
      src_tensor_type == TensorStorageType::TEXTURE_3D ||
      src_tensor_type == TensorStorageType::TEXTURE_2D ||
      src_tensor_type == TensorStorageType::SINGLE_TEXTURE_2D;

  const bool is1x1x1 = conv_params.x_kernel_is_1 && conv_params.y_kernel_is_1 &&
                       conv_params.z_kernel_is_1;

  const bool need_local_mem =
      conv_params.weights_upload_type ==
          Conv3D::WeightsUploadType::LOCAL_MEM_BY_THREADS ||
      conv_params.weights_upload_type ==
          Conv3D::WeightsUploadType::LOCAL_MEM_ASYNC_SUBGROUP;

  const int3 work_group_size = conv_params.work_group_size;
  const int4 block_size = conv_params.block_size;
  std::string c = GetCommonDefines(op_def.precision);
  if (need_local_mem) {  // we use fixed workgroup size when use local mem
    c += "__attribute__((reqd_work_group_size(" +
         std::to_string(work_group_size.x) + ", " +
         std::to_string(work_group_size.y) + ", " +
         std::to_string(work_group_size.z) + ")))\n";
  }
  c += "__kernel void main_function(\n";
  c += "$0) {\n";
  c += GenerateGlobalCoordinates(block_size,
                                 conv_params.work_group_launch_order);
  if (!need_local_mem) {
    c += "  if (DST_X >= args.dst_tensor.Width() || DST_Y >= "
         "args.dst_tensor.Height() || DST_Z >= args.dst_tensor.Depth()) "
         "return;\n";
  }
  if (conv_params.weights_upload_type ==
      Conv3D::WeightsUploadType::LOCAL_MEM_BY_THREADS) {
    c += "  int lid = get_local_id(1) * " + std::to_string(work_group_size.x) +
         " + get_local_id(0);\n";
  }
  for (int s = 0; s < block_size.w; ++s) {
    for (int z = 0; z < block_size.z; ++z) {
      for (int y = 0; y < block_size.y; ++y) {
        for (int x = 0; x < block_size.x; ++x) {
          c += "  ACCUM_FLT4 r" + std::to_string(s) + std::to_string(z) +
               std::to_string(y) + std::to_string(x) +
               " = (ACCUM_FLT4)(0.0f, 0.0f, 0.0f, 0.0f);\n";
        }
      }
    }
  }
  if (!conv_params.x_kernel_is_1) {
    for (int x = 0; x < block_size.x; ++x) {
      const std::string xc = "(DST_X + " + std::to_string(x) + ")";
      if (stride_correction) {
        c += "  int xc" + std::to_string(x) + " = " +
             GetXStrideCorrected(xc, "args.src_tensor.Batch()", "args.stride_x",
                                 "args.padding_x") +
             ";\n";
      } else {
        c += "  int xc" + std::to_string(x) + " = " + xc +
             " * args.stride_x + args.padding_x;\n";
      }
    }
  } else if (!can_read_out_of_x) {
    for (int x = 0; x < block_size.x; ++x) {
      const std::string xc = "(DST_X + " + std::to_string(x) + ")";
      c += "  int xc" + std::to_string(x) + " = clamp(" + xc +
           ", 0, args.src_tensor.Width() - 1);\n";
    }
  }
  if (!conv_params.y_kernel_is_1) {
    for (int y = 0; y < block_size.y; ++y) {
      const std::string yc = "(DST_Y + " + std::to_string(y) + ")";
      c += "  int yc" + std::to_string(y) + " = " + yc +
           " * args.stride_y + args.padding_y;\n";
    }
  } else if (!can_read_out_of_y) {
    for (int y = 0; y < block_size.y; ++y) {
      const std::string yc = "(DST_Y + " + std::to_string(y) + ")";
      c += "  int yc" + std::to_string(y) + " = clamp(" + yc +
           ", 0, args.src_tensor.Height() - 1);\n";
    }
  }
  if (!conv_params.z_kernel_is_1) {
    for (int z = 0; z < block_size.z; ++z) {
      const std::string zc = "(DST_Z + " + std::to_string(z) + ")";
      c += "  int zc" + std::to_string(z) + " = " + zc +
           " * args.stride_z + args.padding_z;\n";
    }
  } else if (!can_read_out_of_z) {
    for (int z = 0; z < block_size.z; ++z) {
      const std::string zc = "(DST_Z + " + std::to_string(z) + ")";
      c += "  int zc" + std::to_string(z) + " = clamp(" + zc +
           ", 0, args.src_tensor.Depth() - 1);\n";
    }
  }
  if (need_local_mem) {
    c += "  __local FLT4 weights_cache[" +
         std::to_string(block_size.w * 4 * conv_params.src_depth_loop_size) +
         "];\n";
  }
  if (conv_params.weights_upload_type ==
      Conv3D::WeightsUploadType::GLOBAL_MEM) {
    c += "  __global FLT4* weights_cache;\n";
  }
  std::string kernel_size;
  kernel_size += conv_params.x_kernel_is_1 ? "" : " * args.kernel_size_x";
  kernel_size += conv_params.y_kernel_is_1 ? "" : " * args.kernel_size_y";
  kernel_size += conv_params.z_kernel_is_1 ? "" : " * args.kernel_size_z";
  if (conv_params.AreWeightsBuffer()) {
    c += "  __global FLT4* filters_loc = args.weights.GetPtr() + DST_S * 4 * "
         "args.src_tensor.Slices()" +
         kernel_size + ";\n";
  }
  if (buffer_type) {
    c += "  const int src_layer_offset = args.src_tensor.SliceStride();\n";
  }
  if (!is1x1x1) {
    c += "  int filter_offset = 0;\n";
  }
  if (!conv_params.z_kernel_is_1) {
    c += "  for (int kz = 0; kz < args.kernel_size_z; ++kz) {\n";
    for (int z = 0; z < block_size.z; ++z) {
      const std::string zck = "zck" + std::to_string(z);
      c += "  int zck" + std::to_string(z) + " = kz * args.dilation_z + zc" +
           std::to_string(z) + ";\n";
      if (manual_clamp_z) {
        c += "  bool mz" + std::to_string(z) + " = " + zck + " >= 0 && " + zck +
             " < args.src_tensor.Depth();\n";
        c += "  " + zck + " = clamp(" + zck +
             ", 0, args.src_tensor.Depth() - 1);\n";
      }
    }
  }
  if (!conv_params.y_kernel_is_1) {
    c += "  for (int ky = 0; ky < args.kernel_size_y; ++ky) {\n";
    for (int y = 0; y < block_size.y; ++y) {
      const std::string yck = "yck" + std::to_string(y);
      c += "  int " + yck + " = ky * args.dilation_y + yc" + std::to_string(y) +
           ";\n";
      if (manual_clamp_y) {
        c += "  bool my" + std::to_string(y) + " = " + yck + " >= 0 && " + yck +
             " < args.src_tensor.Height();\n";
        c += "  " + yck + " = clamp(" + yck +
             ", 0, args.src_tensor.Height() - 1);\n";
      }
    }
  }
  if (!conv_params.x_kernel_is_1) {
    c += "  for (int kx = 0; kx < args.kernel_size_x; ++kx) {\n";
    for (int x = 0; x < block_size.x; ++x) {
      const std::string xck = "xck" + std::to_string(x);
      c += "  int xck" + std::to_string(x) + " = kx * args.dilation_x + xc" +
           std::to_string(x) + ";\n";
      if (manual_clamp_x) {
        c += "  bool mx" + std::to_string(x) + " = " + xck + " >= 0 && " + xck +
             " < args.src_tensor.Width();\n";
        c += "  " + xck + " = clamp(" + xck +
             ", 0, args.src_tensor.Width() - 1);\n";
      }
    }
  }

  auto get_src_x_coord = [&](int id) {
    std::string xs = std::to_string(id);
    std::string xc = "xck" + xs;
    if (conv_params.x_kernel_is_1) {
      if (can_read_out_of_x) {
        xc = "DST_X + " + xs;
      } else {
        xc = "xc" + xs;
      }
    }
    return xc;
  };
  auto get_src_y_coord = [&](int id) {
    std::string ys = std::to_string(id);
    std::string yc = "yck" + ys;
    if (conv_params.y_kernel_is_1) {
      if (can_read_out_of_y) {
        yc = "DST_Y + " + ys;
      } else {
        yc = "yc" + ys;
      }
    }
    return yc;
  };
  auto get_src_z_coord = [&](int id) {
    std::string zs = std::to_string(id);
    std::string zc = "zck" + zs;
    if (conv_params.z_kernel_is_1) {
      if (can_read_out_of_z) {
        zc = "DST_Z + " + zs;
      } else {
        zc = "zc" + zs;
      }
    }
    return zc;
  };

  if (buffer_type) {
    for (int z = 0; z < block_size.z; ++z) {
      const std::string zs = std::to_string(z);
      const std::string zc = get_src_z_coord(z);
      for (int y = 0; y < block_size.y; ++y) {
        const std::string ys = std::to_string(y);
        const std::string yc = get_src_y_coord(y);
        for (int x = 0; x < block_size.x; ++x) {
          const std::string xs = std::to_string(x);
          const std::string xc = get_src_x_coord(x);
          const std::string id = zs + ys + xs;
          c += "  args.src_tensor.GetAddress(src_a_" + id + ", " + xc + ", " +
               yc + ", " + zc + ", 0);\n";
          if (!is1x1x1 && src_tensor_type == TensorStorageType::IMAGE_BUFFER) {
            std::string condition;
            if (manual_clamp_x) {
              if (!condition.empty()) {
                condition += " && ";
              }
              condition += "mx" + xs;
            }
            if (manual_clamp_y) {
              if (!condition.empty()) {
                condition += " && ";
              }
              condition += "my" + ys;
            }
            if (manual_clamp_z) {
              if (!condition.empty()) {
                condition += " && ";
              }
              condition += "mz" + zs;
            }
            c += "  src_a_" + id + " = select(-1, src_a_" + id + ", " +
                 condition + ");\n";
            c += "  int dz_" + id + " = select(0, src_layer_offset, " +
                 condition + ");\n";
          }
        }
      }
    }
  }

  auto declare_src = [&]() {
    for (int z = 0; z < block_size.z; ++z) {
      const std::string zs = std::to_string(z);
      for (int y = 0; y < block_size.y; ++y) {
        const std::string ys = std::to_string(y);
        for (int x = 0; x < block_size.x; ++x) {
          const std::string xs = std::to_string(x);
          const std::string id = zs + ys + xs;
          c += "  FLT4 src" + id + ";\n";
        }
      }
    }
  };

  auto read_src = [&]() {
    for (int z = 0; z < block_size.z; ++z) {
      const std::string zs = std::to_string(z);
      const std::string zc = get_src_z_coord(z);
      for (int y = 0; y < block_size.y; ++y) {
        const std::string ys = std::to_string(y);
        const std::string yc = get_src_y_coord(y);
        for (int x = 0; x < block_size.x; ++x) {
          const std::string xs = std::to_string(x);
          const std::string xc = get_src_x_coord(x);
          std::string multiplier;
          multiplier += manual_clamp_x ? " * (FLT)(mx" + xs + ")" : "";
          multiplier += manual_clamp_y ? " * (FLT)(my" + ys + ")" : "";
          multiplier += manual_clamp_z ? " * (FLT)(mz" + zs + ")" : "";
          const std::string id = zs + ys + xs;
          if (buffer_type) {
            if (src_tensor_type == TensorStorageType::IMAGE_BUFFER) {
              multiplier = "";
            }
            c += "    src" + id + " = args.src_tensor.Read(src_a_" + id + ")" +
                 multiplier + ";\n";
            if (!is1x1x1 &&
                src_tensor_type == TensorStorageType::IMAGE_BUFFER) {
              c += "    src_a_" + id + " += dz_" + id + ";\n";
            } else {
              c += "    src_a_" + id + " += src_layer_offset;\n";
            }
          } else {
            c += "    src" + id + " = args.src_tensor.Read(" + xc + ", " + yc +
                 ", " + zc + ", s)" + multiplier + ";\n";
          }
        }
      }
    }
  };
  c += "  int s = 0;\n";
  declare_src();
  c += "  do {\n";
  const int total_work_items =
      work_group_size.x * work_group_size.y * work_group_size.z;
  if (conv_params.weights_upload_type ==
      Conv3D::WeightsUploadType::LOCAL_MEM_ASYNC_SUBGROUP) {
    c +=
        GenerateAsyncUpload("weights_cache", "filters_loc",
                            /*global_offset_name*/ "",
                            block_size.w * 4 * conv_params.src_depth_loop_size);
  } else if (conv_params.weights_upload_type ==
             Conv3D::WeightsUploadType::LOCAL_MEM_BY_THREADS) {
    c += "    barrier(CLK_LOCAL_MEM_FENCE);\n";
    c += GenerateUploadByThreads(
        "weights_cache", "filters_loc",
        /*global_offset_name*/ "", "lid", total_work_items,
        block_size.w * 4 * conv_params.src_depth_loop_size);
  } else if (conv_params.weights_upload_type ==
             Conv3D::WeightsUploadType::GLOBAL_MEM) {
    c += "    weights_cache = filters_loc;\n";
  } else {  // TEXTURES_MEM
    for (int dst_s = 0; dst_s < block_size.w; ++dst_s) {
      const std::string f_y = is1x1x1 ? "s" : "filter_offset";
      c += absl::Substitute(
          R"(    FLT4 f$2 = args.weights0.Read(DST_S + $0, $1);
    FLT4 f$3 = args.weights1.Read(DST_S + $0, $1);
    FLT4 f$4 = args.weights2.Read(DST_S + $0, $1);
    FLT4 f$5 = args.weights3.Read(DST_S + $0, $1);
)",
          dst_s, f_y, dst_s * 4 + 0, dst_s * 4 + 1, dst_s * 4 + 2,
          dst_s * 4 + 3);
    }
    if (!is1x1x1) {
      c += "    filter_offset++;\n";
    }
  }
  read_src();
  c += "    s += 1;\n";
  if (conv_params.weights_upload_type ==
      Conv3D::WeightsUploadType::LOCAL_MEM_BY_THREADS) {
    c += "    barrier(CLK_LOCAL_MEM_FENCE);\n";
  }
  c += GenerateConv(op_def.precision, block_size, 0,
                    conv_params.AreWeightsBuffer());
  for (int i = 1; i < conv_params.src_depth_loop_size; ++i) {
    read_src();
    c += GenerateConv(op_def.precision, block_size, i * block_size.w * 4,
                      conv_params.AreWeightsBuffer());
    c += "    s += 1;\n";
  }
  if (conv_params.AreWeightsBuffer()) {
    c += "    filters_loc += " +
         std::to_string(block_size.w * 4 * conv_params.src_depth_loop_size) +
         ";\n";
  }
  c += "  } while (s < args.src_tensor.Slices());\n";
  if (!conv_params.z_kernel_is_1) {
    c += "  }\n";
  }
  if (!conv_params.y_kernel_is_1) {
    c += "  }\n";
  }
  if (!conv_params.x_kernel_is_1) {
    c += "  }\n";
  }
  if (conv_params.weights_upload_type ==
      Conv3D::WeightsUploadType::LOCAL_MEM_ASYNC_SUBGROUP) {
    c += GenerateAsyncUpload("weights_cache", "args.biases.GetPtr()", "DST_S",
                             block_size.w);
  } else if (conv_params.weights_upload_type ==
             Conv3D::WeightsUploadType::LOCAL_MEM_BY_THREADS) {
    c += "  barrier(CLK_LOCAL_MEM_FENCE);\n";
    c +=
        GenerateUploadByThreads("weights_cache", "args.biases.GetPtr()",
                                "DST_S", "lid", total_work_items, block_size.w);
    c += "  barrier(CLK_LOCAL_MEM_FENCE);\n";
  } else if (conv_params.weights_upload_type ==
             Conv3D::WeightsUploadType::GLOBAL_MEM) {
    c += "  weights_cache = args.biases.GetPtr() + DST_S;\n";
  }
  if (need_local_mem) {
    c += "  if (DST_X >= args.dst_tensor.Width() || DST_Y >= "
         "args.dst_tensor.Height() || DST_Z >= args.dst_tensor.Depth()) "
         "return;\n";
  }
  for (int s = 0; s < block_size.w; ++s) {
    const std::string dsts =
        "DST_S" + (s == 0 ? "" : " + " + std::to_string(s));
    c += "  if (" + dsts + " >= args.dst_tensor.Slices()) return;\n";
    for (int z = 0; z < block_size.z; ++z) {
      const std::string dstz =
          "DST_Z" + (z == 0 ? "" : " + " + std::to_string(z));
      for (int y = 0; y < block_size.y; ++y) {
        const std::string dsty =
            "DST_Y" + (y == 0 ? "" : " + " + std::to_string(y));
        for (int x = 0; x < block_size.x; ++x) {
          const std::string dstx =
              "DST_X" + (x == 0 ? "" : " + " + std::to_string(x));
          const std::string r_id = std::to_string(s) + std::to_string(z) +
                                   std::to_string(y) + std::to_string(x);
          c += "  if (" + dstx + " < args.dst_tensor.Width() && " + dsty +
               " < args.dst_tensor.Height() && " + dstz +
               " < args.dst_tensor.Depth()) {\n";
          if (conv_params.AreWeightsBuffer()) {
            c += "    FLT4 res = TO_FLT4(r" + r_id + ") + weights_cache[" +
                 std::to_string(s) + "];\n";
          } else {
            c += "    FLT4 res = TO_FLT4(r" + r_id + ") + args.biases.Read(" +
                 dsts + ");\n";
          }
          c += "    args.dst_tensor.Write(res, " + dstx + ", " + dsty + ", " +
               dstz + ", " + dsts + ");\n";
          c += "  }\n";
        }
      }
    }
  }
  c += "}\n";
  return c;
}

Conv3D::ConvParams Conv3D::GuessBestParams(const CLDevice& device,
                                           const OperationDef& definition,
                                           int src_slices, int dst_slices,
                                           bool x_kernel_is_1,
                                           bool y_kernel_is_1,
                                           bool z_kernel_is_1) const {
  ConvParams conv_params;
  conv_params.x_kernel_is_1 = x_kernel_is_1;
  conv_params.y_kernel_is_1 = y_kernel_is_1;
  conv_params.z_kernel_is_1 = z_kernel_is_1;
  if (device.IsNvidia()) {
    conv_params.block_size = int4(1, 1, 1, 4);
    conv_params.work_group_size = int3(8, 4, 1);
    conv_params.work_group_launch_order = int3(2, 0, 1);
    conv_params.src_depth_loop_size = 1;
    conv_params.weights_upload_type = WeightsUploadType::LOCAL_MEM_BY_THREADS;
    if (dst_slices % 4 == 0 || dst_slices >= 8) {
      conv_params.block_size.w = 4;
    } else if (dst_slices % 2 == 0 || dst_slices >= 4) {
      conv_params.block_size.w = 2;
    } else {
      conv_params.block_size.w = dst_slices;
    }
    if (src_slices % 2 == 0) {
      conv_params.src_depth_loop_size = 2;
    }
    if (src_slices % 4 == 0 && conv_params.block_size.w <= 2) {
      conv_params.src_depth_loop_size = 4;
    }
  } else if (device.IsPowerVR()) {
    conv_params.block_size = int4(1, 1, 1, 4);
    conv_params.work_group_size = int3(8, 4, 1);
    conv_params.work_group_launch_order = int3(2, 0, 1);
    conv_params.src_depth_loop_size = 1;
    conv_params.weights_upload_type =
        WeightsUploadType::LOCAL_MEM_ASYNC_SUBGROUP;
    if (dst_slices % 8 == 0 || dst_slices >= 32) {
      conv_params.block_size.w = 8;
    } else if (dst_slices % 4 == 0 || dst_slices >= 8) {
      conv_params.block_size.w = 4;
    } else if (dst_slices % 2 == 0 || dst_slices >= 4) {
      conv_params.block_size.w = 2;
    } else {
      conv_params.block_size.w = dst_slices;
    }
    if (definition.precision == CalculationsPrecision::F16) {
      conv_params.block_size.w = std::min(4, conv_params.block_size.w);
      if (src_slices % 2 == 0) {
        conv_params.src_depth_loop_size = 2;
      }
      if (src_slices % 4 == 0 && conv_params.block_size.w <= 2) {
        conv_params.src_depth_loop_size = 4;
      }
      if (conv_params.block_size.w == 1) {
        if (src_slices % 2 == 0) {
          conv_params.src_depth_loop_size = 2;
        }
        if (src_slices % 4 == 0) {
          conv_params.src_depth_loop_size = 4;
        }
        if (src_slices <= 8) {
          conv_params.src_depth_loop_size = src_slices;
        }
      }
      conv_params.block_size.x = 2;
      conv_params.work_group_size = int3(4, 8, 1);
    }
  } else if (device.IsAdreno()) {
    conv_params.block_size = int4(2, 2, 1, 2);
    conv_params.work_group_size = int3(8, 4, 1);
    conv_params.work_group_launch_order = int3(0, 1, 2);
    conv_params.src_depth_loop_size = 1;
    conv_params.weights_upload_type = WeightsUploadType::TEXTURES_MEM;
  } else if (device.IsMali()) {
    conv_params.block_size = int4(1, 1, 1, 4);
    conv_params.work_group_size = int3(8, 4, 1);
    conv_params.work_group_launch_order = int3(0, 1, 2);
    conv_params.src_depth_loop_size = 1;
    conv_params.weights_upload_type = WeightsUploadType::GLOBAL_MEM;
    if (dst_slices % 4 == 0 || dst_slices >= 8) {
      conv_params.block_size.w = 4;
    } else if (dst_slices % 2 == 0 || dst_slices >= 4) {
      conv_params.block_size.w = 2;
    } else {
      conv_params.block_size.w = dst_slices;
    }
    if (src_slices % 2 == 0) {
      conv_params.src_depth_loop_size = 2;
    }
    if (src_slices % 4 == 0 && conv_params.block_size.w <= 2) {
      conv_params.src_depth_loop_size = 4;
    }
  } else {
    conv_params.block_size = int4(2, 2, 1, 2);
    conv_params.work_group_size = int3(8, 4, 1);
    conv_params.work_group_launch_order = int3(0, 1, 2);
    conv_params.src_depth_loop_size = 1;
    conv_params.weights_upload_type = WeightsUploadType::TEXTURES_MEM;
  }

  return conv_params;
}

Conv3D::ConvParams Conv3D::GuessBestParams(
    const CLDevice& device, const OperationDef& definition,
    const Convolution3DAttributes& attr) const {
  const int dst_slices = DivideRoundUp(attr.weights.shape.o, 4);
  const int src_slices = DivideRoundUp(attr.weights.shape.i, 4);
  const bool x_kernel_is_1 = attr.weights.shape.w == 1 && attr.strides.w == 1 &&
                             attr.dilations.w == 1 &&
                             attr.padding.prepended.w == 0 &&
                             attr.padding.appended.w == 0;
  const bool y_kernel_is_1 = attr.weights.shape.h == 1 && attr.strides.h == 1 &&
                             attr.dilations.h == 1 &&
                             attr.padding.prepended.h == 0 &&
                             attr.padding.appended.h == 0;
  const bool z_kernel_is_1 = attr.weights.shape.d == 1 && attr.strides.d == 1 &&
                             attr.dilations.d == 1 &&
                             attr.padding.prepended.d == 0 &&
                             attr.padding.appended.d == 0;
  return GuessBestParams(device, definition, src_slices, dst_slices,
                         x_kernel_is_1, y_kernel_is_1, z_kernel_is_1);
}

absl::Status CreateConv3D(const CreationContext& creation_context,
                          const OperationDef& definition,
                          const Convolution3DAttributes& attr, Conv3D* result) {
  *result = Conv3D(definition, attr, *creation_context.device);
  return result->UploadData(attr.weights, attr.bias, creation_context.context);
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
