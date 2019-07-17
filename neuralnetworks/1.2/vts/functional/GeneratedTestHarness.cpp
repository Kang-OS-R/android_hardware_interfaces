/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "GeneratedTestHarness.h"

#include <android-base/logging.h>
#include <android/hardware/neuralnetworks/1.0/IDevice.h>
#include <android/hardware/neuralnetworks/1.0/IExecutionCallback.h>
#include <android/hardware/neuralnetworks/1.0/IPreparedModel.h>
#include <android/hardware/neuralnetworks/1.0/IPreparedModelCallback.h>
#include <android/hardware/neuralnetworks/1.0/types.h>
#include <android/hardware/neuralnetworks/1.1/IDevice.h>
#include <android/hardware/neuralnetworks/1.2/IDevice.h>
#include <android/hardware/neuralnetworks/1.2/IExecutionCallback.h>
#include <android/hardware/neuralnetworks/1.2/IPreparedModel.h>
#include <android/hardware/neuralnetworks/1.2/IPreparedModelCallback.h>
#include <android/hidl/allocator/1.0/IAllocator.h>
#include <android/hidl/memory/1.0/IMemory.h>
#include <hidlmemory/mapping.h>

#include <iostream>

#include "1.0/Utils.h"
#include "1.2/Callbacks.h"
#include "ExecutionBurstController.h"
#include "MemoryUtils.h"
#include "TestHarness.h"
#include "Utils.h"

namespace android {
namespace hardware {
namespace neuralnetworks {
namespace V1_2 {
namespace generated_tests {

using ::android::hardware::neuralnetworks::V1_0::ErrorStatus;
using ::android::hardware::neuralnetworks::V1_0::Request;
using ::android::hardware::neuralnetworks::V1_0::RequestArgument;
using ::android::hardware::neuralnetworks::V1_1::ExecutionPreference;
using ::android::hardware::neuralnetworks::V1_2::Constant;
using ::android::hardware::neuralnetworks::V1_2::IDevice;
using ::android::hardware::neuralnetworks::V1_2::IPreparedModel;
using ::android::hardware::neuralnetworks::V1_2::MeasureTiming;
using ::android::hardware::neuralnetworks::V1_2::Model;
using ::android::hardware::neuralnetworks::V1_2::OutputShape;
using ::android::hardware::neuralnetworks::V1_2::Timing;
using ::android::hardware::neuralnetworks::V1_2::implementation::ExecutionCallback;
using ::android::hardware::neuralnetworks::V1_2::implementation::PreparedModelCallback;
using ::android::hidl::memory::V1_0::IMemory;
using ::test_helper::compare;
using ::test_helper::expectMultinomialDistributionWithinTolerance;
using ::test_helper::filter;
using ::test_helper::for_all;
using ::test_helper::for_each;
using ::test_helper::MixedTyped;
using ::test_helper::MixedTypedExample;
using ::test_helper::resize_accordingly;
using HidlToken = hidl_array<uint8_t, static_cast<uint32_t>(Constant::BYTE_SIZE_OF_CACHE_TOKEN)>;

static bool isZeroSized(const MixedTyped& example, uint32_t index) {
    for (auto i : example.operandDimensions.at(index)) {
        if (i == 0) return true;
    }
    return false;
}

static Return<ErrorStatus> ExecutePreparedModel(sp<IPreparedModel>& preparedModel,
                                                const Request& request, MeasureTiming measure,
                                                sp<ExecutionCallback>& callback) {
    return preparedModel->execute_1_2(request, measure, callback);
}
static Return<ErrorStatus> ExecutePreparedModel(sp<IPreparedModel>& preparedModel,
                                                const Request& request, MeasureTiming measure,
                                                hidl_vec<OutputShape>* outputShapes,
                                                Timing* timing) {
    ErrorStatus result;
    Return<void> ret = preparedModel->executeSynchronously(
            request, measure,
            [&result, outputShapes, timing](ErrorStatus error, const hidl_vec<OutputShape>& shapes,
                                            const Timing& time) {
                result = error;
                *outputShapes = shapes;
                *timing = time;
            });
    if (!ret.isOk()) {
        return ErrorStatus::GENERAL_FAILURE;
    }
    return result;
}
static std::shared_ptr<::android::nn::ExecutionBurstController> CreateBurst(
        const sp<IPreparedModel>& preparedModel) {
    return ::android::nn::ExecutionBurstController::create(preparedModel, /*blocking=*/true);
}
enum class Executor { ASYNC, SYNC, BURST };
enum class OutputType { FULLY_SPECIFIED, UNSPECIFIED, INSUFFICIENT };
const float kDefaultAtol = 1e-5f;
const float kDefaultRtol = 1e-5f;
void EvaluatePreparedModel(sp<IPreparedModel>& preparedModel, std::function<bool(int)> is_ignored,
                           const std::vector<MixedTypedExample>& examples,
                           bool hasRelaxedFloat32Model, float fpAtol, float fpRtol,
                           Executor executor, MeasureTiming measure, OutputType outputType) {
    const uint32_t INPUT = 0;
    const uint32_t OUTPUT = 1;

    int example_no = 1;
    for (auto& example : examples) {
        SCOPED_TRACE(example_no++);
        const MixedTyped& inputs = example.operands.first;
        const MixedTyped& golden = example.operands.second;

        const bool hasFloat16Inputs = !inputs.float16Operands.empty();
        if (hasRelaxedFloat32Model || hasFloat16Inputs) {
            // TODO: Adjust the error limit based on testing.
            // If in relaxed mode, set the absolute tolerance to be 5ULP of FP16.
            fpAtol = 5.0f * 0.0009765625f;
            // Set the relative tolerance to be 5ULP of the corresponding FP precision.
            fpRtol = 5.0f * 0.0009765625f;
        }

        std::vector<RequestArgument> inputs_info, outputs_info;
        uint32_t inputSize = 0, outputSize = 0;
        // This function only partially specifies the metadata (vector of RequestArguments).
        // The contents are copied over below.
        for_all(inputs, [&inputs_info, &inputSize](int index, auto, auto s) {
            if (inputs_info.size() <= static_cast<size_t>(index)) inputs_info.resize(index + 1);
            RequestArgument arg = {
                    .location = {.poolIndex = INPUT,
                                 .offset = 0,
                                 .length = static_cast<uint32_t>(s)},
                    .dimensions = {},
            };
            RequestArgument arg_empty = {
                    .hasNoValue = true,
            };
            inputs_info[index] = s ? arg : arg_empty;
            inputSize += s;
        });
        // Compute offset for inputs 1 and so on
        {
            size_t offset = 0;
            for (auto& i : inputs_info) {
                if (!i.hasNoValue) i.location.offset = offset;
                offset += i.location.length;
            }
        }

        MixedTyped test;  // holding test results

        // Go through all outputs, initialize RequestArgument descriptors
        resize_accordingly(golden, test);
        bool sizeLargerThanOne = true;
        for_all(golden, [&golden, &outputs_info, &outputSize, &outputType, &sizeLargerThanOne](
                                int index, auto, auto s) {
            if (outputs_info.size() <= static_cast<size_t>(index)) outputs_info.resize(index + 1);
            if (index == 0) {
                // On OutputType::INSUFFICIENT, set the output operand with index 0 with
                // buffer size one byte less than needed.
                if (outputType == OutputType::INSUFFICIENT) {
                    if (s > 1 && !isZeroSized(golden, index)) {
                        s -= 1;
                    } else {
                        sizeLargerThanOne = false;
                    }
                }
            }
            RequestArgument arg = {
                    .location = {.poolIndex = OUTPUT,
                                 .offset = 0,
                                 .length = static_cast<uint32_t>(s)},
                    .dimensions = {},
            };
            outputs_info[index] = arg;
            outputSize += s;
        });
        // If output0 does not have size larger than one byte,
        // we can not provide an insufficient buffer
        if (!sizeLargerThanOne && outputType == OutputType::INSUFFICIENT) return;
        // Compute offset for outputs 1 and so on
        {
            size_t offset = 0;
            for (auto& i : outputs_info) {
                i.location.offset = offset;
                offset += i.location.length;
            }
        }
        std::vector<hidl_memory> pools = {nn::allocateSharedMemory(inputSize),
                                          nn::allocateSharedMemory(outputSize)};
        ASSERT_NE(0ull, pools[INPUT].size());
        ASSERT_NE(0ull, pools[OUTPUT].size());

        // load data
        sp<IMemory> inputMemory = mapMemory(pools[INPUT]);
        sp<IMemory> outputMemory = mapMemory(pools[OUTPUT]);
        ASSERT_NE(nullptr, inputMemory.get());
        ASSERT_NE(nullptr, outputMemory.get());
        char* inputPtr = reinterpret_cast<char*>(static_cast<void*>(inputMemory->getPointer()));
        char* outputPtr = reinterpret_cast<char*>(static_cast<void*>(outputMemory->getPointer()));
        ASSERT_NE(nullptr, inputPtr);
        ASSERT_NE(nullptr, outputPtr);
        inputMemory->update();
        outputMemory->update();

        // Go through all inputs, copy the values
        for_all(inputs, [&inputs_info, inputPtr](int index, auto p, auto s) {
            char* begin = (char*)p;
            char* end = begin + s;
            // TODO: handle more than one input
            std::copy(begin, end, inputPtr + inputs_info[index].location.offset);
        });

        inputMemory->commit();
        outputMemory->commit();

        const Request request = {.inputs = inputs_info, .outputs = outputs_info, .pools = pools};

        ErrorStatus executionStatus;
        hidl_vec<OutputShape> outputShapes;
        Timing timing;
        switch (executor) {
            case Executor::ASYNC: {
                SCOPED_TRACE("asynchronous");

                // launch execution
                sp<ExecutionCallback> executionCallback = new ExecutionCallback();
                ASSERT_NE(nullptr, executionCallback.get());
                Return<ErrorStatus> executionLaunchStatus =
                        ExecutePreparedModel(preparedModel, request, measure, executionCallback);
                ASSERT_TRUE(executionLaunchStatus.isOk());
                EXPECT_EQ(ErrorStatus::NONE, static_cast<ErrorStatus>(executionLaunchStatus));

                // retrieve execution status
                executionCallback->wait();
                executionStatus = executionCallback->getStatus();
                outputShapes = executionCallback->getOutputShapes();
                timing = executionCallback->getTiming();

                break;
            }
            case Executor::SYNC: {
                SCOPED_TRACE("synchronous");

                // execute
                Return<ErrorStatus> executionReturnStatus = ExecutePreparedModel(
                        preparedModel, request, measure, &outputShapes, &timing);
                ASSERT_TRUE(executionReturnStatus.isOk());
                executionStatus = static_cast<ErrorStatus>(executionReturnStatus);

                break;
            }
            case Executor::BURST: {
                SCOPED_TRACE("burst");

                // create burst
                const std::shared_ptr<::android::nn::ExecutionBurstController> controller =
                        CreateBurst(preparedModel);
                ASSERT_NE(nullptr, controller.get());

                // create memory keys
                std::vector<intptr_t> keys(request.pools.size());
                for (size_t i = 0; i < keys.size(); ++i) {
                    keys[i] = reinterpret_cast<intptr_t>(&request.pools[i]);
                }

                // execute burst
                std::tie(executionStatus, outputShapes, timing) =
                        controller->compute(request, measure, keys);

                break;
            }
        }

        if (outputType != OutputType::FULLY_SPECIFIED &&
            executionStatus == ErrorStatus::GENERAL_FAILURE) {
            LOG(INFO) << "NN VTS: Early termination of test because vendor service cannot "
                         "execute model that it does not support.";
            std::cout << "[          ]   Early termination of test because vendor service cannot "
                         "execute model that it does not support."
                      << std::endl;
            GTEST_SKIP();
        }
        if (measure == MeasureTiming::NO) {
            EXPECT_EQ(UINT64_MAX, timing.timeOnDevice);
            EXPECT_EQ(UINT64_MAX, timing.timeInDriver);
        } else {
            if (timing.timeOnDevice != UINT64_MAX && timing.timeInDriver != UINT64_MAX) {
                EXPECT_LE(timing.timeOnDevice, timing.timeInDriver);
            }
        }

        switch (outputType) {
            case OutputType::FULLY_SPECIFIED:
                // If the model output operands are fully specified, outputShapes must be either
                // either empty, or have the same number of elements as the number of outputs.
                ASSERT_EQ(ErrorStatus::NONE, executionStatus);
                ASSERT_TRUE(outputShapes.size() == 0 ||
                            outputShapes.size() == test.operandDimensions.size());
                break;
            case OutputType::UNSPECIFIED:
                // If the model output operands are not fully specified, outputShapes must have
                // the same number of elements as the number of outputs.
                ASSERT_EQ(ErrorStatus::NONE, executionStatus);
                ASSERT_EQ(outputShapes.size(), test.operandDimensions.size());
                break;
            case OutputType::INSUFFICIENT:
                ASSERT_EQ(ErrorStatus::OUTPUT_INSUFFICIENT_SIZE, executionStatus);
                ASSERT_EQ(outputShapes.size(), test.operandDimensions.size());
                ASSERT_FALSE(outputShapes[0].isSufficient);
                return;
        }
        // Go through all outputs, overwrite output dimensions with returned output shapes
        if (outputShapes.size() > 0) {
            for_each<uint32_t>(test.operandDimensions,
                               [&outputShapes](int idx, std::vector<uint32_t>& dim) {
                                   dim = outputShapes[idx].dimensions;
                               });
        }

        // validate results
        outputMemory->read();
        copy_back(&test, outputs_info, outputPtr);
        outputMemory->commit();
        // Filter out don't cares
        MixedTyped filtered_golden = filter(golden, is_ignored);
        MixedTyped filtered_test = filter(test, is_ignored);

        // We want "close-enough" results for float
        compare(filtered_golden, filtered_test, fpAtol, fpRtol);

        if (example.expectedMultinomialDistributionTolerance > 0) {
            expectMultinomialDistributionWithinTolerance(test, example);
        }
    }
}
void EvaluatePreparedModel(sp<IPreparedModel>& preparedModel, std::function<bool(int)> is_ignored,
                           const std::vector<MixedTypedExample>& examples,
                           bool hasRelaxedFloat32Model, Executor executor, MeasureTiming measure,
                           OutputType outputType) {
    EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model, kDefaultAtol,
                          kDefaultRtol, executor, measure, outputType);
}

void EvaluatePreparedModel(sp<IPreparedModel>& preparedModel, std::function<bool(int)> is_ignored,
                           const std::vector<MixedTypedExample>& examples,
                           bool hasRelaxedFloat32Model, bool testDynamicOutputShape) {
    if (testDynamicOutputShape) {
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::ASYNC, MeasureTiming::NO, OutputType::UNSPECIFIED);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::SYNC, MeasureTiming::NO, OutputType::UNSPECIFIED);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::BURST, MeasureTiming::NO, OutputType::UNSPECIFIED);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::ASYNC, MeasureTiming::YES, OutputType::UNSPECIFIED);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::SYNC, MeasureTiming::YES, OutputType::UNSPECIFIED);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::BURST, MeasureTiming::YES, OutputType::UNSPECIFIED);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::ASYNC, MeasureTiming::NO, OutputType::INSUFFICIENT);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::SYNC, MeasureTiming::NO, OutputType::INSUFFICIENT);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::BURST, MeasureTiming::NO, OutputType::INSUFFICIENT);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::ASYNC, MeasureTiming::YES, OutputType::INSUFFICIENT);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::SYNC, MeasureTiming::YES, OutputType::INSUFFICIENT);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::BURST, MeasureTiming::YES, OutputType::INSUFFICIENT);
    } else {
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::ASYNC, MeasureTiming::NO, OutputType::FULLY_SPECIFIED);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::SYNC, MeasureTiming::NO, OutputType::FULLY_SPECIFIED);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::BURST, MeasureTiming::NO, OutputType::FULLY_SPECIFIED);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::ASYNC, MeasureTiming::YES, OutputType::FULLY_SPECIFIED);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::SYNC, MeasureTiming::YES, OutputType::FULLY_SPECIFIED);
        EvaluatePreparedModel(preparedModel, is_ignored, examples, hasRelaxedFloat32Model,
                              Executor::BURST, MeasureTiming::YES, OutputType::FULLY_SPECIFIED);
    }
}

void PrepareModel(const sp<IDevice>& device, const Model& model,
                  sp<IPreparedModel>* preparedModel) {
    // see if service can handle model
    bool fullySupportsModel = false;
    Return<void> supportedCall = device->getSupportedOperations_1_2(
            model, [&fullySupportsModel](ErrorStatus status, const hidl_vec<bool>& supported) {
                ASSERT_EQ(ErrorStatus::NONE, status);
                ASSERT_NE(0ul, supported.size());
                fullySupportsModel = std::all_of(supported.begin(), supported.end(),
                                                 [](bool valid) { return valid; });
            });
    ASSERT_TRUE(supportedCall.isOk());

    // launch prepare model
    sp<PreparedModelCallback> preparedModelCallback = new PreparedModelCallback();
    ASSERT_NE(nullptr, preparedModelCallback.get());
    Return<ErrorStatus> prepareLaunchStatus = device->prepareModel_1_2(
            model, ExecutionPreference::FAST_SINGLE_ANSWER, hidl_vec<hidl_handle>(),
            hidl_vec<hidl_handle>(), HidlToken(), preparedModelCallback);
    ASSERT_TRUE(prepareLaunchStatus.isOk());
    ASSERT_EQ(ErrorStatus::NONE, static_cast<ErrorStatus>(prepareLaunchStatus));

    // retrieve prepared model
    preparedModelCallback->wait();
    ErrorStatus prepareReturnStatus = preparedModelCallback->getStatus();
    sp<V1_0::IPreparedModel> preparedModelV1_0 = preparedModelCallback->getPreparedModel();
    *preparedModel = IPreparedModel::castFrom(preparedModelV1_0).withDefault(nullptr);

    // early termination if vendor service cannot fully prepare model
    if (!fullySupportsModel && prepareReturnStatus != ErrorStatus::NONE) {
        ASSERT_EQ(nullptr, preparedModel->get());
        LOG(INFO) << "NN VTS: Early termination of test because vendor service cannot "
                     "prepare model that it does not support.";
        std::cout << "[          ]   Early termination of test because vendor service cannot "
                     "prepare model that it does not support."
                  << std::endl;
        return;
    }
    EXPECT_EQ(ErrorStatus::NONE, prepareReturnStatus);
    ASSERT_NE(nullptr, preparedModel->get());
}

void Execute(const sp<IDevice>& device, std::function<Model(void)> create_model,
             std::function<bool(int)> is_ignored, const std::vector<MixedTypedExample>& examples,
             bool testDynamicOutputShape) {
    Model model = create_model();
    sp<IPreparedModel> preparedModel = nullptr;
    PrepareModel(device, model, &preparedModel);
    if (preparedModel == nullptr) {
        GTEST_SKIP();
    }
    EvaluatePreparedModel(preparedModel, is_ignored, examples,
                          model.relaxComputationFloat32toFloat16, testDynamicOutputShape);
}

}  // namespace generated_tests
}  // namespace V1_2
}  // namespace neuralnetworks
}  // namespace hardware
}  // namespace android
