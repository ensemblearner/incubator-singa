/************************************************************
*
* Licensed to the Apache Software Foundation (ASF) under one
* or more contributor license agreements.  See the NOTICE file
* distributed with this work for additional information
* regarding copyright ownership.  The ASF licenses this file
* to you under the Apache License, Version 2.0 (the
* "License"); you may not use this file except in compliance
* with the License.  You may obtain a copy of the License at
*
*   http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing,
* software distributed under the License is distributed on an
* "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
* KIND, either express or implied.  See the License for the
* specific language governing permissions and limitations
* under the License.
*
*************************************************************/

#include "neuralnet/neuron_layer.h"

#include <glog/logging.h>
#include <algorithm>
#include "utils/singleton.h"
#include "mshadow/tensor.h"
#include "mshadow/cxxnet_op.h"

namespace singa {

using namespace mshadow;
using mshadow::cpu;

using mshadow::Shape;
using mshadow::Shape1;
using mshadow::Shape2;
using mshadow::Shape3;
using mshadow::Shape4;
using mshadow::Tensor;

using std::string;
using std::vector;

inline Tensor<cpu, 4> Tensor4(Blob<float>* blob) {
  const vector<int>& shape = blob->shape();
  Tensor<cpu, 4> tensor(blob->mutable_cpu_data(),
      Shape4(shape[0], shape[1], shape[2], shape[3]));
  return tensor;
}

inline Tensor<cpu, 3> Tensor3(Blob<float>* blob) {
  const vector<int>& shape = blob->shape();
  Tensor<cpu, 3> tensor(blob->mutable_cpu_data(),
      Shape3(shape[0], shape[1], blob->count() / shape[0] / shape[1]));
  return tensor;
}

inline Tensor<cpu, 2> Tensor2(Blob<float>* blob) {
  const vector<int>& shape = blob->shape();
  Tensor<cpu, 2> tensor(blob->mutable_cpu_data(),
      Shape2(shape[0], blob->count() / shape[0]));
  return tensor;
}

inline Tensor<cpu, 1> Tensor1(Blob<float>* blob) {
  Tensor<cpu, 1> tensor(blob->mutable_cpu_data(), Shape1(blob->count()));
  return tensor;
}

/************ Implementation for ConvolutionLayer*************************/
ConvolutionLayer::~ConvolutionLayer() {
  delete weight_;
  delete bias_;
}
void ConvolutionLayer::Setup(const LayerProto& conf,
    const vector<Layer*>& srclayers) {
  CHECK_EQ(srclayers.size(), 1);
  Layer::Setup(conf, srclayers);
  ConvolutionProto conv_conf = conf.convolution_conf();
  kernel_ = conv_conf.kernel();
  CHECK_GT(kernel_, 0) << "Filter size cannot be zero.";
  pad_ = conv_conf.pad();
  stride_ = conv_conf.stride();
  num_filters_ = conv_conf.num_filters();
  if (partition_dim() > 0)
    num_filters_ /= srclayers.at(0)->num_partitions();
  const vector<int>& srcshape = srclayers[0]->data(this).shape();
  int dim = srcshape.size();
  CHECK_GT(dim, 2);
  width_ = srcshape[dim - 1];
  height_ = srcshape[dim - 2];
  if (dim > 3)
    channels_ = srcshape[dim - 3];
  else if (dim > 2)
    channels_ = 1;
  batchsize_ = srcshape[0];
  conv_height_ = (height_ + 2 * pad_ - kernel_) / stride_ + 1;
  conv_width_ = (width_ + 2 * pad_ - kernel_) / stride_ + 1;
  col_height_ = channels_ * kernel_ * kernel_;
  col_width_ = conv_height_ * conv_width_;
  vector<int> shape{batchsize_, num_filters_, conv_height_, conv_width_};
  data_.Reshape(shape);
  grad_.Reshape(shape);
  col_data_.Reshape(vector<int>{col_height_, col_width_});
  col_grad_.Reshape(vector<int>{col_height_, col_width_});
  weight_ = Param::Create(conf.param(0));
  bias_ = Param::Create(conf.param(1));
  weight_->Setup(vector<int>{num_filters_, col_height_});
  bias_->Setup(vector<int>{num_filters_});
}

void ConvolutionLayer::ComputeFeature(int flag,
    const vector<Layer*>& srclayers) {
  auto src = Tensor4(srclayers[0]->mutable_data(this));
  auto data = Tensor3(&data_);
  auto col = Tensor2(&col_data_);
  auto weight = Tensor2(weight_->mutable_data());
  auto bias = Tensor1(bias_->mutable_data());
  for (int n = 0; n < batchsize_; n++) {
    if (pad_ > 0)
      col = expr::unpack_patch2col(pad(src[n], pad_), kernel_, stride_);
    else
      col = expr::unpack_patch2col(src[n], kernel_, stride_);
    data[n] = dot(weight, col);
  }
  data += expr::broadcast<1>(bias, data.shape);
}

void ConvolutionLayer::ComputeGradient(int flag,
    const vector<Layer*>& srclayers) {
  auto src = Tensor4(srclayers[0]->mutable_data(this));
  auto col = Tensor2(&col_data_);
  auto weight = Tensor2(weight_->mutable_data());
  auto grad = Tensor3(&grad_);
  auto gcol = Tensor2(&col_grad_);
  auto gweight = Tensor2(weight_->mutable_grad());
  auto gbias = Tensor1(bias_->mutable_grad());
  Blob<float>* gsrcblob = srclayers[0]->mutable_grad(this);
  Tensor<cpu, 4> gsrc(nullptr, Shape4(batchsize_, channels_, height_, width_));
  if (gsrcblob != nullptr)
    gsrc.dptr = gsrcblob->mutable_cpu_data();
  gbias = expr::sumall_except_dim<1>(grad);
  gweight = 0.0f;
  Shape<3> padshp(gsrc.shape.SubShape());
  padshp[0] += 2 * pad_;
  padshp[1] += 2 * pad_;
  Shape<2> imgshp = Shape2(height_, width_);
  for (int n = 0; n < batchsize_; n++) {
    if (pad_ > 0)
      col = expr::unpack_patch2col(pad(src[n], pad_), kernel_, stride_);
    else
      col = expr::unpack_patch2col(src[n], kernel_, stride_);
    gweight += dot(grad[n], col.T());
    if (gsrcblob != nullptr) {
      gcol = dot(weight.T(), grad[n]);
      gsrc[n] = crop(expr::pack_col2patch(gcol, padshp, kernel_, stride_),
          imgshp);
    }
  }
}

/******************* Implementation for CConvolutionLayer *********/
void CConvolutionLayer::ComputeFeature(int flag,
    const vector<Layer*>& srclayers) {
  auto src = Tensor4(srclayers[0]->mutable_data(this));
  auto data = Tensor3(&data_);
  auto col = Tensor2(&col_data_);
  auto weight = Tensor2(weight_->mutable_data());
  auto bias = Tensor1(bias_->mutable_data());

  for (int n = 0; n < batchsize_; n++) {
    Im2col(src[n].dptr, channels_, height_, width_,
        kernel_, kernel_, pad_, pad_, stride_, stride_, col.dptr);
    data[n] = dot(weight, col);
  }
  data += expr::broadcast<1>(bias, data.shape);
}

void CConvolutionLayer::ComputeGradient(int flag,
    const vector<Layer*>& srclayers) {
  auto src = Tensor4(srclayers[0]->mutable_data(this));
  auto col = Tensor2(&col_data_);
  auto weight = Tensor2(weight_->mutable_data());

  auto grad = Tensor3(&grad_);
  auto gcol = Tensor2(&col_grad_);
  auto gweight = Tensor2(weight_->mutable_grad());
  auto gbias = Tensor1(bias_->mutable_grad());
  gweight = 0.f;
  Blob<float>* gsrcblob = srclayers[0]->mutable_grad(this);
  Tensor<cpu, 4> gsrc(nullptr, Shape4(batchsize_, channels_, height_, width_));
  if (gsrcblob != nullptr)
    gsrc.dptr = gsrcblob->mutable_cpu_data();
  gbias = expr::sumall_except_dim<1>(grad);
  for (int n = 0; n < batchsize_; n++) {
    Im2col(src[n].dptr, channels_, height_, width_,
        kernel_, kernel_, pad_, pad_, stride_, stride_, col.dptr);
    gweight += dot(grad[n], col.T());
    if (gsrcblob != nullptr) {
      gcol = dot(weight.T(), grad[n]);
      Col2im(gcol.dptr, channels_, height_, width_,
          kernel_, kernel_, pad_, pad_, stride_, stride_, gsrc[n].dptr);
    }
  }
}

/****************** Implementation for DropoutLayer ***********************/
void DropoutLayer::Setup(const LayerProto& conf,
    const vector<Layer*>& srclayers) {
  Layer::Setup(conf, srclayers);
  data_.ReshapeLike(srclayers[0]->data(this));
  grad_.ReshapeLike(*srclayers[0]->mutable_grad(this));
  mask_.Reshape(srclayers[0]->data(this).shape());
  pdrop_ = conf.dropout_conf().dropout_ratio();
}

void DropoutLayer::ComputeFeature(int flag, const vector<Layer*>& srclayers) {
  // check training
  if ((flag & kTrain) != kTrain) {
    data_.CopyFrom(srclayers[0]->data(this));
    return;
  }
  float pkeep = 1 - pdrop_;
  auto mask = Tensor1(&mask_);
  mask = expr::F<op::threshold>(TSingleton<Random<cpu>>::Instance() \
                      ->uniform(mask.shape), pkeep) * (1.0f/pkeep);
  auto data = Tensor1(&data_);
  auto src = Tensor1(srclayers[0]->mutable_data(this));
  data = src * mask;
}

void DropoutLayer::ComputeGradient(int flag, const vector<Layer*>& srclayers)  {
  auto mask = Tensor1(&mask_);
  auto grad = Tensor1(&grad_);
  auto gsrc = Tensor1(srclayers[0]->mutable_grad(this));
  gsrc = grad * mask;
}


/**************** Implementation for RBMLayer********************/
Blob<float>* RBMLayer::Sample(int flag) {
  Tensor<cpu, 2> sample, data;
  if ((flag & kPositive) == kPositive || first_gibbs_) {
    data = Tensor2(&data_);
    sample = Tensor2(&sample_);
  } else {
    data = Tensor2(&neg_data_);
    sample = Tensor2(&neg_sample_);
  }
  auto random = TSingleton<Random<cpu>>::Instance();
  if (gaussian_) {
    random->SampleGaussian(sample, 0.0f, 1.0f);
    sample += data;
  } else {
    random->SampleBinary(sample, data);
  }
  return (flag & kPositive) == kPositive || first_gibbs_ ?
    &sample_ : &neg_sample_;
}
void RBMLayer::Setup(const LayerProto& conf, const vector<Layer*>& srclayers) {
  Layer::Setup(conf, srclayers);
  hdim_ = conf.rbm_conf().hdim();
  gaussian_ = conf.rbm_conf().gaussian();
  first_gibbs_ = true;
}
/**************** Implementation for RBMVisLayer********************/
RBMVisLayer::~RBMVisLayer() {
  delete weight_;
  delete bias_;
}

void RBMVisLayer::Setup(const LayerProto& conf,
    const vector<Layer*>& srclayers) {
  CHECK_EQ(srclayers.size(), 2);
  RBMLayer::Setup(conf, srclayers);
  CHECK_EQ(srclayers.size(), 2);
  hid_layer_ = nullptr;
  for (auto src : srclayers) {
    if (typeid(*src) == typeid(RBMHidLayer)) {
      // note the hid layer has may not been set up.
      CHECK(hid_layer_ == nullptr);
      hid_layer_ = dynamic_cast<RBMHidLayer*>(src);
    }
  }
  input_layer_ = srclayers[0] != hid_layer_ ? srclayers[0]: srclayers[1];
  const auto& src = input_layer_->data(this);
  batchsize_ = src.shape()[0];
  data_.ReshapeLike(src);
  neg_data_.ReshapeLike(data_);
  neg_sample_.ReshapeLike(data_);
  vdim_ = src.count() / batchsize_;
  weight_ = Param::Create(conf.param(0));
  weight_ ->Setup(vector<int>{hdim_, vdim_});
  bias_ = Param::Create(conf.param(1));
  bias_->Setup(vector<int>{vdim_});
}

void RBMVisLayer::ComputeFeature(int flag, const vector<Layer*>& srclayers) {
  if ((flag & kPositive) == kPositive) {
    data_.CopyFrom(input_layer_->data(this), true);
    first_gibbs_ = true;
  } else if ((flag & kNegative) == kNegative) {
    // fetch sampling results from hidden layer
    auto hid_sample = Tensor2(hid_layer_->Sample(flag));
    auto data = Tensor2(&neg_data_);
    auto weight = Tensor2(weight_->mutable_data());
    auto bias = Tensor1(bias_->mutable_data());
    data = dot(hid_sample, weight);
    data += expr::repmat(bias, batchsize_);
    data = expr::F<op::sigmoid>(data);
    if ((flag & kTest) == kTest) {
      const float *dptr = data_.cpu_data(), *rcns = neg_data_.cpu_data();
      float err = 0.f;
      for (int i = 0; i < data_.count(); i++) {
        err += (dptr[i] - rcns[i]) * (dptr[i] - rcns[i]);
      }
      metric_.Add("Squared Error", err / batchsize_);
    }
    first_gibbs_ = false;
  }
}

void RBMVisLayer::ComputeGradient(int flag, const vector<Layer*>& srclayers) {
  auto vis_pos = Tensor2(&data_);
  auto vis_neg = Tensor2(&neg_data_);
  auto hid_pos = Tensor2(hid_layer_->mutable_data(this));
  auto hid_neg = Tensor2(hid_layer_->mutable_neg_data(this));

  auto gbias = Tensor1(bias_->mutable_grad());
  gbias = expr::sum_rows(vis_neg);
  gbias -= expr::sum_rows(vis_pos);
  gbias /= batchsize_;

  auto gweight = Tensor2(weight_->mutable_grad());
  gweight = dot(hid_neg.T(), vis_neg);
  gweight -= dot(hid_pos.T(), vis_pos);
  gweight /= batchsize_;
}
/**************** Implementation for RBMHidLayer********************/
RBMHidLayer::~RBMHidLayer() {
  delete weight_;
  delete bias_;
}

void RBMHidLayer::Setup(const LayerProto& conf,
      const vector<Layer*>& srclayers) {
  RBMLayer::Setup(conf, srclayers);
  CHECK_EQ(srclayers.size(), 1);
  const auto& src_data = srclayers[0]->data(this);
  batchsize_ = src_data.shape()[0];
  vdim_ = src_data.count() / batchsize_;
  data_.Reshape(vector<int>{batchsize_, hdim_});
  neg_data_.ReshapeLike(data_);
  sample_.ReshapeLike(data_);
  neg_sample_.ReshapeLike(data_);
  weight_ = Param::Create(conf.param(0));
  weight_->Setup(vector<int>{hdim_, vdim_});
  bias_ = Param::Create(conf.param(1));
  bias_->Setup(vector<int>{hdim_});
  vis_layer_ = dynamic_cast<RBMVisLayer*> (srclayers[0]);
}

void RBMHidLayer::ComputeFeature(int flag, const vector<Layer*>& srclayers) {
  auto weight = Tensor2(weight_->mutable_data());
  auto bias = Tensor1(bias_->mutable_data());

  Tensor<cpu, 2> data, src;
  if ((flag & kPositive) == kPositive) {
    data = Tensor2(&data_);
    src = Tensor2(vis_layer_->mutable_data(this));
    first_gibbs_ = true;
  } else {
    data = Tensor2(&neg_data_);
    // hinton's science paper does not sample the vis layer
    src = Tensor2(vis_layer_->mutable_neg_data(this));
    first_gibbs_ = false;
  }
  data = dot(src, weight.T());
  data += expr::repmat(bias, batchsize_);

  if (!gaussian_)
    data = expr::F<op::sigmoid>(data);
}

void RBMHidLayer::ComputeGradient(int flag, const vector<Layer*>& srclayers) {
  auto hid_pos = Tensor2(&data_);
  auto hid_neg = Tensor2(&neg_data_);
  auto gbias = Tensor1(bias_->mutable_grad());
  gbias = expr::sum_rows(hid_neg);
  gbias -= expr::sum_rows(hid_pos);
  gbias /= batchsize_;
}
/*********** Implementation for InnerProductLayer**********/
InnerProductLayer::~InnerProductLayer() {
  delete weight_;
  delete bias_;
}

void InnerProductLayer::Setup(const LayerProto& conf,
    const vector<Layer*>& srclayers) {
  Layer::Setup(conf, srclayers);
  CHECK_EQ(srclayers.size(), 1);
  const auto& src = srclayers[0]->data(this);
  batchsize_ = src.shape()[0];
  vdim_ = src.count() / batchsize_;
  hdim_ = layer_conf_.innerproduct_conf().num_output();
  transpose_ = conf.innerproduct_conf().transpose();
  if (partition_dim() > 0)
    hdim_ /= srclayers.at(0)->num_partitions();
  data_.Reshape(vector<int>{batchsize_, hdim_});
  grad_.ReshapeLike(data_);
  weight_ = Param::Create(conf.param(0));
  bias_ = Param::Create(conf.param(1));
  if (transpose_)
    weight_->Setup(vector<int>{vdim_, hdim_});
  else
    weight_->Setup(vector<int>{hdim_, vdim_});
  bias_->Setup(vector<int>{hdim_});
}

void InnerProductLayer::ComputeFeature(int flag,
    const vector<Layer*>& srclayers) {
  auto data = Tensor2(&data_);
  auto src = Tensor2(srclayers[0]->mutable_data(this));
  auto weight = Tensor2(weight_->mutable_data());
  auto bias = Tensor1(bias_->mutable_data());
  if (transpose_)
    data = dot(src, weight);
  else
    data = dot(src, weight.T());
  // repmat: repeat bias vector into batchsize rows
  data += expr::repmat(bias, batchsize_);
}

void InnerProductLayer::ComputeGradient(int flag,
    const vector<Layer*>& srclayers) {
  auto src = Tensor2(srclayers[0]->mutable_data(this));
  auto grad = Tensor2(&grad_);
  auto weight = Tensor2(weight_->mutable_data());
  auto gweight = Tensor2(weight_->mutable_grad());
  auto gbias = Tensor1(bias_->mutable_grad());

  gbias = expr::sum_rows(grad);
  if (transpose_)
    gweight = dot(src.T(), grad);
  else
    gweight = dot(grad.T(), src);
  if (srclayers[0]->mutable_grad(this) != nullptr) {
    auto gsrc = Tensor2(srclayers[0]->mutable_grad(this));
    if (transpose_)
      gsrc = dot(grad, weight.T());
    else
      gsrc = dot(grad, weight);
  }
}
/***************** Implementation for LRNLayer *************************/
void LRNLayer::Setup(const LayerProto& conf, const vector<Layer*>& srclayers) {
  Layer::Setup(conf, srclayers);
  CHECK_EQ(srclayers.size(), 1);
  lsize_ = conf.lrn_conf().local_size();
  CHECK_EQ(lsize_ % 2, 1) << "LRN only supports odd values for Localvol";
  knorm_ = conf.lrn_conf().knorm();
  alpha_ = conf.lrn_conf().alpha();
  beta_ = conf.lrn_conf().beta();
  const vector<int>& s = srclayers[0]->data(this).shape();
  data_.Reshape(s);
  grad_.Reshape(s);
  norm_.Reshape(s);
  batchsize_ = s[0];
  channels_ = s[1];
  height_ = s[2];
  width_ = s[3];
}

void LRNLayer::ComputeFeature(int flag, const vector<Layer*>& srclayers) {
  const float salpha = alpha_ / lsize_;
  auto src = Tensor4(srclayers[0]->mutable_data(this));
  auto data = Tensor4(&data_);
  auto norm = Tensor4(&norm_);
  // stores normalizer without power
  norm = expr::chpool<red::sum>(expr::F<op::square>(src), lsize_) * salpha
    + knorm_;
  data = src * expr::F<op::power>(norm, -beta_);
}

void LRNLayer::ComputeGradient(int flag, const vector<Layer*>& srclayers) {
  const float salpha = alpha_ / lsize_;
  auto src = Tensor4(srclayers[0]->mutable_data(this));
  auto norm = Tensor4(&norm_);
  auto grad = Tensor4(&grad_);
  auto gsrc = Tensor4(srclayers[0]->mutable_grad(this));

  gsrc = grad * expr::F<op::power>(norm, -beta_);
  gsrc += (- 2.0f * beta_ * salpha) * expr::chpool<red::sum>(
      grad * src * expr::F<op::power>(norm, -beta_ - 1.0f), lsize_)  * src;
}

/******************** Implementation for PoolingLayer******************/
void PoolingLayer::Setup(const LayerProto& conf,
    const vector<Layer*>& srclayers) {
  Layer::Setup(conf, srclayers);
  CHECK_EQ(srclayers.size(), 1);
  PoolingProto pool_conf = conf.pooling_conf();
  kernel_ = pool_conf.kernel();
  stride_ = pool_conf.stride();
  CHECK_LT(pad_, kernel_);
  pool_ = conf.pooling_conf().pool();
  CHECK(pool_ == PoolingProto_PoolMethod_AVG
        || pool_ == PoolingProto_PoolMethod_MAX)
        << "Padding implemented only for average and max pooling.";
  const auto& srcshape = srclayers[0]->data(this).shape();
  int dim = srcshape.size();
  CHECK_GT(dim, 2);
  width_ = srcshape[dim - 1];
  height_ = srcshape[dim - 2];
  if (dim > 3)
    channels_ = srcshape[dim-3];
  else
    channels_ = 1;
  batchsize_ = srcshape[0];
  pooled_height_ = static_cast<int>((height_ - kernel_) / stride_) + 1;
  pooled_width_ = static_cast<int>((width_ - kernel_) / stride_) + 1;
  data_.Reshape(vector<int>{batchsize_, channels_, pooled_height_,
                            pooled_width_});
  grad_.ReshapeLike(data_);
}

void PoolingLayer::ComputeFeature(int flag, const vector<Layer*>& srclayers) {
  auto src = Tensor4(srclayers[0]->mutable_data(this));
  auto data = Tensor4(&data_);
  if (pool_ == PoolingProto_PoolMethod_MAX)
    data = expr::pool<red::maximum>(src, kernel_, stride_);
  else if (pool_ == PoolingProto_PoolMethod_AVG)
    data = expr::pool<red::sum>(src, kernel_, stride_)
      * (1.0f / (kernel_ * kernel_));
}

/*
 * partition only on num/channel dim
 * assume grad and data have the same paritition
 */
void PoolingLayer::ComputeGradient(int flag, const vector<Layer*>& srclayers) {
  auto src = Tensor4(srclayers[0]->mutable_data(this));
  auto gsrc = Tensor4(srclayers[0]->mutable_grad(this));
  auto data = Tensor4(&data_);
  auto grad = Tensor4(&grad_);
  if (pool_ == PoolingProto_PoolMethod_MAX)
    gsrc = expr::unpool<red::maximum>(src, data, grad, kernel_, stride_);
  else if (pool_ == PoolingProto_PoolMethod_AVG)
    gsrc = expr::unpool<red::sum>(src, data, grad, kernel_, stride_)
           * (1.0f / (kernel_ * kernel_));
}

/***************** Implementation of CPoolingLayer ***************/

void CPoolingLayer::Setup(const LayerProto& conf,
    const vector<Layer*>& srclayers) {
  PoolingLayer::Setup(conf, srclayers);
  if (pool_ == PoolingProto_PoolMethod_MAX)
      mask_.ReshapeLike(data_);
}
void CPoolingLayer::ComputeFeature(int flag, const vector<Layer*>& srclayers) {
  if (pool_ == PoolingProto_PoolMethod_MAX)
    ForwardMaxPooling(srclayers[0]->mutable_data(this)->mutable_cpu_data(),
        batchsize_, channels_, height_, width_, kernel_, kernel_, pad_, pad_,
        stride_, stride_, data_.mutable_cpu_data(), mask_.mutable_cpu_data());
  else if (pool_ == PoolingProto_PoolMethod_AVG)
    ForwardAvgPooling(srclayers[0]->mutable_data(this)->mutable_cpu_data(),
        batchsize_, channels_, height_, width_, kernel_, kernel_, pad_, pad_,
        stride_, stride_, data_.mutable_cpu_data());
  else
    LOG(FATAL) << "unknow pooling method";
}

void CPoolingLayer::ComputeGradient(int flag, const vector<Layer*>& srclayers) {
  if (pool_ == PoolingProto_PoolMethod_MAX)
    BackwardMaxPooling(grad_.cpu_data(), mask_.cpu_data(), batchsize_,
        channels_, height_, width_, kernel_, kernel_, pad_, pad_,
        stride_, stride_, srclayers[0]->mutable_grad(this)->mutable_cpu_data());
  else if (pool_ == PoolingProto_PoolMethod_AVG)
    BackwardAvgPooling(grad_.cpu_data(), batchsize_,
        channels_, height_, width_, kernel_, kernel_, pad_, pad_,
        stride_, stride_, srclayers[0]->mutable_grad(this)->mutable_cpu_data());
  else
    LOG(FATAL) << "unknow pooling method";
}

/***************** Implementation for ReLULayer *****************************/
void ReLULayer::Setup(const LayerProto& conf,
    const vector<Layer*>& srclayers) {
  Layer::Setup(conf, srclayers);
  data_.ReshapeLike(srclayers[0]->data(this));
  grad_.ReshapeLike(*(srclayers[0]->mutable_grad(this)));
}

void ReLULayer::ComputeFeature(int flag, const vector<Layer*>& srclayers) {
  auto data = Tensor1(&data_);
  auto src = Tensor1(srclayers[0]->mutable_data(this));
  data = expr::F<op::relu>(src);
}

void ReLULayer::ComputeGradient(int flag, const vector<Layer*>& srclayers) {
  auto data = Tensor1(&data_);
  auto grad = Tensor1(&grad_);
  auto gsrc = Tensor1(srclayers[0]->mutable_grad(this));
  gsrc = expr::F<op::relu_grad>(data)*grad;
}

/*******************Implementation of SigmoidLayer***************************/
void SigmoidLayer::Setup(const LayerProto& conf,
    const vector<Layer*>& srclayers) {
  Layer::Setup(conf, srclayers);
  data_.ReshapeLike(srclayers[0]->data(this));
  grad_.ReshapeLike(srclayers[0]->grad(this));
}

void SigmoidLayer::ComputeFeature(int flag, const vector<Layer*>& srclayers) {
  auto data = Tensor1(&data_);
  auto src = Tensor1(srclayers[0]->mutable_data(this));
  data = expr::F<op::sigmoid>(src);
}

void SigmoidLayer::ComputeGradient(int flag, const vector<Layer*>& srclayers) {
  auto data = Tensor1(&data_);
  auto grad = Tensor1(&grad_);
  auto gsrc = Tensor1(srclayers[0]->mutable_grad(this));
  gsrc = expr::F<op::sigmoid_grad>(data) * grad;
}
/*******************Implementation of TanLayer***************************/
void STanhLayer::Setup(const LayerProto& conf,
    const vector<Layer*>& srclayers) {
  Layer::Setup(conf, srclayers);
  data_.ReshapeLike(srclayers[0]->data(this));
  grad_.ReshapeLike(srclayers[0]->grad(this));
}

void STanhLayer::ComputeFeature(int flag, const vector<Layer*>& srclayers) {
  auto data = Tensor1(&data_);
  auto src = Tensor1(srclayers[0]->mutable_data(this));
  data = expr::F<op::stanh>(src);
}

void STanhLayer::ComputeGradient(int flag, const vector<Layer*>& srclayers) {
  auto data = Tensor1(&data_);
  auto grad = Tensor1(&grad_);
  auto gsrc = Tensor1(srclayers[0]->mutable_grad(this));
  gsrc = expr::F<op::stanh_grad>(data) * grad;
}


}  // namespace singa
