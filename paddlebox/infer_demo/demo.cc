/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include <iostream>
#include <glog/logging.h>

#include "helper.h"
#include "paddle/include/paddle_inference_api.h"

DEFINE_string(model_path, "model", "Directory of the inference model.");
DEFINE_string(data_path, "data/sample.data", "Path of the dataset.");
DEFINE_string(fetch_var, "", "Variable name to fetch.");

DEFINE_int32(start_line, 0, "The starting line of the text file read (this line will be read).");
DEFINE_int32(end_line, 1000000, "The ending line of the text file read (this line will be read).");
DEFINE_int32(iter, 1, "The running iteration.");

// Donot change "optimize" flag for the moment
DEFINE_bool(optimize, false, "Whether optimize or not.");
DEFINE_bool(profile, true, "Whether profile or not.");

namespace paddle {

void set_config(AnalysisConfig* config, bool optimize) {
  config->SwitchUseFeedFetchOps(false);
  // If fetch var is set, do not optimize IR to skip variable fusion
  if (FLAGS_fetch_var.empty()) {
    config->SwitchIrOptim(true);
  }
  config->DisableGpu();
  config->EnableMKLDNN();
}

double run(paddle::PaddlePredictor* predictor, bool need_profile) {
  if (need_profile) {
    paddle::inference::Timer timer;
    timer.tic();
    for (size_t i = 0; i < FLAGS_iter; i++) {
      CHECK(predictor->ZeroCopyRun());
    }
    return timer.toc();
  } else {
    for (size_t i = 0; i < FLAGS_iter; i++) {
      CHECK(predictor->ZeroCopyRun());
    }
    return 0;
  }
};

void fetch_internal_data(paddle::PaddlePredictor* predictor, const std::string& var_list) {
  std::vector<std::string> var_names;
  inference::split(var_list, ',', &var_names);
  for (auto& var_name : var_names) {
    try {
      auto out = predictor->GetOutputTensor(var_name);
      const std::vector<int>& output_shape = out->shape();
      int size = std::accumulate(output_shape.begin(), output_shape.end(), 1, std::multiplies<int>());
      float out_data[size];
      // if data is on GPU
      out->copy_to_cpu(out_data);
      std::stringstream ss;
      for (int i = 0; i < size; ++i) {
        ss << out_data[i] << " ";
      }
      LOG(INFO) << "Fetched variable \"" << var_name << "\" of size " << size << ":\n" << ss.str();
    } catch (...) {
      LOG(ERROR) << "Failed to fetch variable \"" << var_name << "\"";
      continue;
    }
  }
}

std::unordered_map<std::string, std::vector<float>>
predict(bool optimize, bool profile) {
  // 1. Read datas from file.
  std::string feature_file = FLAGS_data_path;
  helper::Data data(feature_file,
                    FLAGS_start_line,
                    FLAGS_end_line);

  // 2. Configure and init the predictor.
  AnalysisConfig config;
  set_config(&config, optimize);
  config.SetModel(FLAGS_model_path + "/program.bin", FLAGS_model_path + "/model.bin");
  auto predictor = CreatePaddlePredictor(config);
  LOG(INFO) << "finish loading model";

  // 3. Get the map between slot-ids and the unique_ptr of zero-copy-tensors.
  // The unique_ptr must be placed in the outer scope to ensure that will not
  // be released during the entire execution.
  std::unordered_map<std::string, std::unique_ptr<paddle::ZeroCopyTensor>>
      slot_id_tensor_map;
  helper::link_slots_and_tensors(predictor.get(), &slot_id_tensor_map);
  LOG(INFO) << "finish linking slots";

  // 4. Get slots and feed to tensors.
  // The optimization of seqpool_cvm_concat will fuse the inputs.
  std::vector<helper::Slot<float>> slots;
  data.get_slots(&slots);
  // NOTICE: There are two versions of seqpool_cvm_concat_pass for this model,
  // with or without fused inputs. For versatility, a scheme that does not
  // incorporate input is used.
  helper::slots_to_tensors(slots, &slot_id_tensor_map, PaddlePlace::kCPU, 1);
  LOG(INFO) << "finish feeding data";

  // 5. Run the predictor.
  double time = run(predictor.get(), profile);
  LOG(INFO) << "[predict] iter = " << FLAGS_iter << ", time = " << time << " ms.";

  // 6. Insert the tensor pointer to the map.
  std::unordered_map<std::string, std::vector<float>> output_tensors;
  const std::vector<std::string>& out_names = predictor->GetOutputNames();
  helper::fill_output_tensors_by_names(predictor.get(), &output_tensors, out_names);

  // 7. Print the value of output tensors.
  std::stringstream ss;
  for (auto& it: output_tensors) {
    ss.str("");
    const auto& vec = it.second;
    ss << it.first << ": " << vec[0];
    for (size_t i = 1; i < vec.size(); i++) {
      ss << "," << vec[i];
    }
    LOG(INFO) << ss.str();
  }

  // 8. Fetch internal data if needed
  if (!FLAGS_fetch_var.empty()) {
    fetch_internal_data(predictor.get(), FLAGS_fetch_var);
  }

  return output_tensors;
}

}  // namespace paddle

int main(int argc, char** argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  auto tensors = paddle::predict(FLAGS_optimize, FLAGS_profile);
  return 0;
}
