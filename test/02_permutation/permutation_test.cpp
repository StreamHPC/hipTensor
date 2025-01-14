/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (C) 2021-2024 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/
#include <hiptensor/hiptensor.hpp>

#include "data_types.hpp"
#include "logger.hpp"
#include "permutation/permutation_cpu_reference.hpp"
#include "permutation_test.hpp"
#include "utils.hpp"
#include "llvm/hiptensor_options.hpp"

namespace hiptensor
{
    /*static*/ std::stringstream PermutationTest::sAPILogBuff = std::stringstream();

    static void logMessage(int32_t logLevel, const char* funcName /*=""*/, const char* msg /*=""*/)
    {
        PermutationTest::sAPILogBuff << msg;
    }

    PermutationTest::PermutationTest()
        : Base()
    {
        reset();

        // Handle our own outputs
        hiptensorLoggerOpenFile("/dev/null");
        hiptensorLoggerSetCallback(logMessage);
    }

    // Kernel run checks. Virtual as different Permutation kernels have different requirements
    // True = run test
    // False = skip test
    bool PermutationTest::checkDevice(hipDataType datatype) const
    {
        return (isF32Supported() && datatype == HIP_R_32F)
               || (isF64Supported() && datatype == HIP_R_64F);
    }

    bool PermutationTest::checkSizes() const
    {
        return true;
    }

    void PermutationTest::reset()
    {
        handle = nullptr;

        mRepeats          = 1u;
        mRunFlag          = true;
        mValidationResult = false;
        mMaxRelativeError = 0.0;
    }

    PermutationResource* PermutationTest::getResource() const
    {
        return DataStorage::instance().get();
    }

    void PermutationTest::SetUp()
    {
        // reset API log buffer
        sAPILogBuff.str(std::string());

        auto param        = Base::GetParam();
        auto testType     = std::get<0>(param);
        auto logLevel     = std::get<1>(param);
        auto lengths      = std::get<2>(param);
        auto permutedDims = std::get<3>(param);
        auto alpha        = std::get<4>(param);

        // 4D tensors only at the moment.
        EXPECT_EQ(lengths.size(), 4); // Format {'n', 'c', 'w', 'h'}
        EXPECT_EQ(permutedDims.size(), 4); // permutation of {0, 1, 2, 3}

        EXPECT_EQ(testType.size(), 2); // HIP_R_16F or HIP_R_32F
        auto abDataType = testType[0];
        EXPECT_TRUE((abDataType == HIP_R_16F) || (abDataType == HIP_R_32F));

        getResource()->setupStorage(lengths, abDataType);

        // set mPrintElements to true to print element
        mPrintElements = false;
    }

    void PermutationTest::reportResults(std::ostream& stream,
                                        hipDataType   dataType,
                                        bool          omitSkipped,
                                        bool          omitFailed,
                                        bool          omitPassed) const
    {
        // Conditionally print outputs
        if((mRunFlag || !omitSkipped) && (mValidationResult || !omitFailed)
           && (!mValidationResult || !omitPassed))
        {
            stream << PermutationTest::sAPILogBuff.str();

            if(mPrintElements)
            {
                auto resource = getResource();

                size_t elementsA = resource->getCurrentMatrixElement();
                size_t elementsB = elementsA;

                if(dataType == HIP_R_32F)
                {
                    stream << "Tensor A elements (" << elementsA << "):\n";
                    hiptensorPrintArrayElements<float>(
                        stream, (float*)resource->hostA().get(), elementsA);
                    stream << std::endl;

                    stream << "Tensor B elements (" << elementsB << "):\n";
                    hiptensorPrintArrayElements<float>(
                        stream, (float*)resource->hostB().get(), elementsB);
                    stream << std::endl;
                }
                else
                {
                    stream << "Tensor A elements (" << elementsA << "):\n";
                    hiptensorPrintArrayElements<_Float16>(
                        stream, (_Float16*)resource->hostA().get(), elementsA);
                    stream << std::endl;

                    stream << "Tensor B elements (" << elementsB << "):\n";
                    hiptensorPrintArrayElements<_Float16>(
                        stream, (_Float16*)resource->hostB().get(), elementsB);
                    stream << std::endl;
                }
            }
        }
    }

    void PermutationTest::RunKernel()
    {
        auto param        = Base::GetParam();
        auto testType     = std::get<0>(param);
        auto logLevel     = std::get<1>(param);
        auto lengths      = std::get<2>(param);
        auto permutedDims = std::get<3>(param);
        auto alpha        = std::get<4>(param);

        auto abDataType      = testType[0];
        auto computeDataType = testType[1];

        if(!mRunFlag)
        {
            GTEST_SKIP();
        }
        auto resource = getResource();

        if(mRunFlag)
        {
            /**********************
              B_{w, h, c, n} = 1.0 *  \textsl{IDENTITY}(A_{c, n, h, w})
             **********************/

            std::vector<int> modeA{'n', 'c', 'w', 'h'};
            std::vector<int> modeB;
            for(auto dim : permutedDims)
            {
                modeB.push_back(modeA[dim]);
            }

            int                              nmodeA = modeA.size();
            int                              nmodeB = modeB.size();
            std::unordered_map<int, int64_t> extent;
            for(auto [modeIt, i] = std::tuple{modeA.begin(), 0}; modeIt != modeA.end();
                ++modeIt, ++i)
            {
                extent[*modeIt] = lengths[i];
            }

            std::vector<int64_t> extentA;
            for(auto mode : modeA)
                extentA.push_back(extent[mode]);
            std::vector<int64_t> extentB;
            for(auto mode : modeB)
                extentB.push_back(extent[mode]);

            hiptensorStatus_t  err;
            hiptensorHandle_t* handle;
            CHECK_HIPTENSOR_ERROR(hiptensorCreate(&handle));

            hiptensorTensorDescriptor_t descA;
            CHECK_HIPTENSOR_ERROR(hiptensorInitTensorDescriptor(handle,
                                                                &descA,
                                                                nmodeA,
                                                                extentA.data(),
                                                                NULL /* stride */,
                                                                abDataType,
                                                                HIPTENSOR_OP_IDENTITY));

            hiptensorTensorDescriptor_t descB;
            CHECK_HIPTENSOR_ERROR(hiptensorInitTensorDescriptor(handle,
                                                                &descB,
                                                                nmodeB,
                                                                extentB.data(),
                                                                NULL /* stride */,
                                                                abDataType,
                                                                HIPTENSOR_OP_IDENTITY));

            float alphaValue{};
            if(computeDataType == HIP_R_16F)
            {
                *(reinterpret_cast<_Float16*>(&alphaValue)) = static_cast<_Float16>(alpha);
            }
            else
            {
                *(reinterpret_cast<float*>(&alphaValue)) = static_cast<float>(alpha);
            }
            CHECK_HIPTENSOR_ERROR(hiptensorPermutation(handle,
                                                       &alphaValue,
                                                       resource->deviceA().get(),
                                                       &descA,
                                                       modeA.data(),
                                                       resource->deviceB().get(),
                                                       &descB,
                                                       modeB.data(),
                                                       computeDataType,
                                                       0 /* stream */));
            resource->copyBToHost();

            if(abDataType == HIP_R_32F)
            {
                hiptensor::detail::permuteByCpu(&alphaValue,
                                                (const float*)resource->hostA().get(),
                                                &descA,
                                                modeA.data(),
                                                (float*)resource->hostReference().get(),
                                                &descB,
                                                modeB.data(),
                                                computeDataType);
                resource->copyReferenceToDevice();
                std::tie(mValidationResult, mMaxRelativeError)
                    = compareEqualLaunchKernel<float>((float*)resource->deviceB().get(),
                                                      (float*)resource->deviceReference().get(),
                                                      resource->getCurrentMatrixElement());
            }
            else if(abDataType == HIP_R_16F)
            {
                hiptensor::detail::permuteByCpu(&alphaValue,
                                                (const _Float16*)resource->hostA().get(),
                                                &descA,
                                                modeA.data(),
                                                (_Float16*)resource->hostReference().get(),
                                                &descB,
                                                modeB.data(),
                                                computeDataType);
                resource->copyReferenceToDevice();
                std::tie(mValidationResult, mMaxRelativeError) = compareEqualLaunchKernel<_Float16>(
                    (_Float16*)resource->deviceB().get(),
                    (_Float16*)resource->deviceReference().get(),
                    resource->getCurrentMatrixElement());
            }
        }

        EXPECT_TRUE(mValidationResult) << "Max relative error: " << mMaxRelativeError;

        using Options        = hiptensor::HiptensorOptions;
        auto& loggingOptions = Options::instance();

        if(!loggingOptions->omitCout())
        {
            reportResults(std::cout,
                          abDataType,
                          loggingOptions->omitSkipped(),
                          loggingOptions->omitFailed(),
                          loggingOptions->omitPassed());
        }

        if(loggingOptions->ostream().isOpen())
        {
            reportResults(loggingOptions->ostream().fstream(),
                          abDataType,
                          loggingOptions->omitSkipped(),
                          loggingOptions->omitFailed(),
                          loggingOptions->omitPassed());
        }
    }

    void PermutationTest::TearDown() {}

} // namespace hiptensor
