#ifdef USE_OPENCV
#include <opencv2/core/core.hpp>

#include <fstream>  // NOLINT(readability/streams)
#include <iostream>  // NOLINT(readability/streams)
#include <string>
#include <utility>
#include <vector>

#include "caffe/data_transformer.hpp"
#include "caffe/layers/base_data_layer.hpp"
#include "caffe/layers/image_data_layer.hpp"
#include "caffe/util/benchmark.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"

namespace caffe {

template <typename Dtype>
ImageDataLayer<Dtype>::~ImageDataLayer<Dtype>() {
  this->StopInternalThread();
}

template <typename Dtype>
void ImageDataLayer<Dtype>::DataLayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const int new_height = this->layer_param_.image_data_param().new_height();
  const int new_width  = this->layer_param_.image_data_param().new_width();
  const bool is_color  = this->layer_param_.image_data_param().is_color();
  string root_folder = this->layer_param_.image_data_param().root_folder();

  CHECK((new_height == 0 && new_width == 0) ||
      (new_height > 0 && new_width > 0)) << "Current implementation requires "
      "new_height and new_width to be set at the same time.";
  // Read the file with filenames and labels
  const string& source = this->layer_param_.image_data_param().source();
  LOG(INFO) << "Opening file " << source;
  std::ifstream infile(source.c_str());
  string line;
  size_t pos;
  string label;
  while (std::getline(infile, line)) {
    pos = line.find_last_of(' ');
    label = line.substr(pos + 1).c_str();
    lines_.push_back(std::make_pair(line.substr(0, pos), label));
  }

  CHECK(!lines_.empty()) << "File is empty";

  if (this->layer_param_.image_data_param().shuffle()) {
    // randomly shuffle data
    LOG(INFO) << "Shuffling data";
    const unsigned int prefetch_rng_seed = caffe_rng_rand();
    prefetch_rng_.reset(new Caffe::RNG(prefetch_rng_seed));
    ShuffleImages();
  }
  LOG(INFO) << "A total of " << lines_.size() << " images.";

  lines_id_ = 0;
  // Check if we would need to randomly skip a few data points
  if (this->layer_param_.image_data_param().rand_skip()) {
    unsigned int skip = caffe_rng_rand() %
        this->layer_param_.image_data_param().rand_skip();
    LOG(INFO) << "Skipping first " << skip << " data points.";
    CHECK_GT(lines_.size(), skip) << "Not enough points to skip";
    lines_id_ = skip;
  }
  vector<cv::Mat> cv_img_list;
  //int cv_img_list[11][64][64];
  for (int i = 0; i < 11; i++) {
    // Read an image, and use it to initialize the top blob.
    cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].first,
                                      new_height, new_width, is_color);
    CHECK(cv_img.data) << "Could not load " << lines_[lines_id_].first;
    cv_img_list.push_back(cv_img);
    /*
    for (int j = 0; j < 64; j++) {
      for (int k = 0; k < 64; k++) {
        cv_img_list[i][j][k] = cv_img.at<int>(j, k);
      }
    }
    */
    //cv_img_list[i] = cv_img[0];
  }
  /*
  // Read an image, and use it to initialize the top blob.
  cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].first,
                                    new_height, new_width, is_color);
  CHECK(cv_img.data) << "Could not load " << lines_[lines_id_].first;
  // Use data_transformer to infer the expected blob shape from a cv_image.
  */
  vector<int> top_shape = this->data_transformer_->InferBlobShape(cv_img_list);
  this->transformed_data_.Reshape(top_shape);
  // Reshape prefetch_data and top[0] according to the batch_size.
  const int batch_size = this->layer_param_.image_data_param().batch_size();
  CHECK_GT(batch_size, 0) << "Positive batch size required";
  top_shape[0] = batch_size;
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].data_.Reshape(top_shape);
  }
  top[0]->Reshape(top_shape);

  LOG(INFO) << "output data size: " << top[0]->num() << ","
      << top[0]->channels() << "," << top[0]->height() << ","
      << top[0]->width();

  // label
  /* add by lin */
  cv::Mat cv_label_img = ReadImageToCVMat(root_folder + lines_[lines_id_].second,
                                    64, 64, is_color);
  CHECK(cv_label_img.data) << "Could not load " << lines_[lines_id_].second;
  //vector<int> label_shape = this->data_transformer_->InferBlobShape(cv_label_img);
  //this->transformed_data_.Reshape(label_shape);
  vector<int> label_shape(4);
  label_shape[0] = batch_size;
  label_shape[1] = 3;
  label_shape[2] = 64;
  label_shape[3] = 64;
  top[1]->Reshape(label_shape);
  /* add by lin */
  //vector<int> label_shape(1, batch_size);
  //top[1]->Reshape(label_shape);
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].label_.Reshape(label_shape);
  }
}

template <typename Dtype>
void ImageDataLayer<Dtype>::ShuffleImages() {
  caffe::rng_t* prefetch_rng =
      static_cast<caffe::rng_t*>(prefetch_rng_->generator());
  shuffle(lines_.begin(), lines_.end(), prefetch_rng);
}

// This function is called on prefetch thread
template <typename Dtype>
void ImageDataLayer<Dtype>::load_batch(Batch<Dtype>* batch) {
  CPUTimer batch_timer;
  batch_timer.Start();
  double read_time = 0;
  double trans_time = 0;
  CPUTimer timer;
  CHECK(batch->data_.count());
  CHECK(this->transformed_data_.count());
  ImageDataParameter image_data_param = this->layer_param_.image_data_param();
  const int batch_size = image_data_param.batch_size();
  const int new_height = image_data_param.new_height();
  const int new_width = image_data_param.new_width();
  const bool is_color = image_data_param.is_color();
  string root_folder = image_data_param.root_folder();
  
  vector<cv::Mat> cv_img_list;
  for (int i = 0; i < 11; i++) {
    // Reshape according to the first image of each batch
    // on single input batches allows for inputs of varying dimension.
    cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].first,
        new_height, new_width, is_color);
    CHECK(cv_img.data) << "Could not load " << lines_[lines_id_].first;
    // Use data_transformer to infer the expected blob shape from a cv_img.
    cv_img_list.push_back(cv_img);
  }
  vector<int> top_shape = this->data_transformer_->InferBlobShape(cv_img_list);
  this->transformed_data_.Reshape(top_shape);
  // Reshape batch according to the batch_size.
  top_shape[0] = batch_size;
  batch->data_.Reshape(top_shape);

  /* add by lin */
  cv::Mat cv_label_img = ReadImageToCVMat(root_folder + lines_[lines_id_].second,
      new_height, new_width, is_color);
  CHECK(cv_label_img.data) << "Could not load " << lines_[lines_id_].second;
  //vector<int> top_label_shape = this->data_transformer_->InferBlobShape(cv_label_img);
  //this->transformed_data_.Reshape(top_label_shape);
  vector<int> top_label_shape(4);
  top_label_shape[0] = batch_size;
  top_label_shape[1] = 3;
  top_label_shape[2] = 64;
  top_label_shape[3] = 64;
  batch->label_.Reshape(top_label_shape);
  /* add by lin */

  Dtype* prefetch_data = batch->data_.mutable_cpu_data();
  Dtype* prefetch_label = batch->label_.mutable_cpu_data();

  // datum scales
  const int lines_size = lines_.size();
  for (int item_id = 0; item_id < batch_size; ++item_id) {
    // get a blob
    timer.Start();
    CHECK_GT(lines_size, lines_id_);
    vector<cv::Mat> cv_img_list;
    //int cv_img_list[11][64][64];
    for (int i = 0; i < 11; i++) {
      cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].first,
          new_height, new_width, is_color);
      CHECK(cv_img.data) << "Could not load " << lines_[lines_id_].first;
      cv_img_list.push_back(cv_img);
      /*
      for (int j = 0; j < 64; j++) {
        for (int k = 0; k < 64; k++) {
          cv_img_list[i][j][k] = cv_img.at<int>(j, k);
        }
      }
      */
      //cv_img_list[i] = cv_img[0];
    }
    read_time += timer.MicroSeconds();
    timer.Start();
    // Apply transformations (mirror, crop...) to the image
    //int offset = batch->data_.offset(item_id);
    int offset = item_id * 11 * 3 * 64 * 64;
    this->transformed_data_.set_cpu_data(prefetch_data + offset);
    this->data_transformer_->Transform(cv_img_list, &(this->transformed_data_));
    trans_time += timer.MicroSeconds();
    /* add by lin */
    cv::Mat cv_label_img = ReadImageToCVMat(root_folder + lines_[lines_id_].second,
                                    64, 64, is_color);
    
    //this->transformed_data_.set_cpu_data(prefetch_label + offset);
    //this->data_transformer_->Transform(cv_label_img, &(this->transformed_data_));
    //trans_time += timer.MicroSeconds();
    /* add by lin */
    /*
    offset = batch->label_.offset(item_id);
    Dtype* prefetch_label_item = prefetch_label + offset;
    for (int i = 0; i < 64; i++) {
      for (int j = 0; j < 64; j++) {
        if (int(cv_label_img.at<uchar>(i, j) == 0)) {
          *prefetch_label_item = Dtype(0);
        } else {
          *prefetch_label_item = Dtype(1);
        }
        prefetch_label_item++;
      }
   }
  */

    offset = item_id * 3 * 64 * 64;
    for (int i = 0; i < 3; i++) {
      for(int r = 0; r < 64; ++r) {
        for(int c = 0; c < 64; ++c) {
          prefetch_label[offset] = cv_label_img.at<int>(r, c)[i];
          /*
          if(int(cv_label_img.at<unsigned char>(r, c)) == 0) 
            prefetch_label[offset] = Dtype(0);
          else
            prefetch_label[offset] = Dtype(1);
          */
          ++offset;
        }
      }
    }



    //prefetch_label[item_id] = lines_[lines_id_].second;
    // go to the next iter
    lines_id_++;
    if (lines_id_ >= lines_size) {
      // We have reached the end. Restart from the first.
      DLOG(INFO) << "Restarting data prefetching from start.";
      lines_id_ = 0;
      if (this->layer_param_.image_data_param().shuffle()) {
        ShuffleImages();
      }
    }
  }
  batch_timer.Stop();
  DLOG(INFO) << "Prefetch batch: " << batch_timer.MilliSeconds() << " ms.";
  DLOG(INFO) << "     Read time: " << read_time / 1000 << " ms.";
  DLOG(INFO) << "Transform time: " << trans_time / 1000 << " ms.";
}

INSTANTIATE_CLASS(ImageDataLayer);
REGISTER_LAYER_CLASS(ImageData);

}  // namespace caffe
#endif  // USE_OPENCV
