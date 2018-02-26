#include "cuda_runtime_api.h"

#include "BadCommandLine.h"
#include "CUDAPMSMemMgr.h"
#include "CUDAStencil.cpp"
#include "CUDAStencil.h"
#include "CUDAStencilFactory.h"
#include "CommonCUDAStencilFactory.cpp"
#include "HostStencil.cpp"
#include "HostStencil.h"
#include "HostStencilFactory.h"
#include "InitializeMatrix2D.cpp"
#include "InitializeMatrix2D.h"
#include "InvalidArgValue.h"
#include "Matrix2D.cpp"
#include "Matrix2D.h"
#include "Matrix2DFileSupport.cpp"
#include "OptionParser.h"
#include "ResultDatabase.h"
#include "SerialStencilUtil.cpp"
#include "SerialStencilUtil.h"
#include "StencilFactory.cpp"
#include "StencilUtil.cpp"
#include "StencilUtil.h"
#include "Timer.h"
#include "ValidateMatrix2D.cpp"
#include "ValidateMatrix2D.h"
#include <assert.h>
#include <fstream>
#include <iostream>
#include <sstream>

#include "CUDAStencilFactory.cpp"
#include "HostStencilFactory.cpp"

// prototypes of auxiliary functions defined in this file or elsewhere
void CheckOptions(const OptionParser &opts);

void EnsureStencilInstantiation(void);

template <class T>
void DoTest(const char *timerDesc, ResultDatabase &resultDB,
            OptionParser &opts) {
  StencilFactory<T> *stdStencilFactory = NULL;
  Stencil<T> *stdStencil = NULL;
  StencilFactory<T> *testStencilFactory = NULL;
  Stencil<T> *testStencil = NULL;

  try {
    stdStencilFactory = new HostStencilFactory<T>;
    testStencilFactory = new CUDAStencilFactory<T>;

    assert((stdStencilFactory != NULL) && (testStencilFactory != NULL));

    // do a sanity check on option values
    CheckOptions(opts);
    stdStencilFactory->CheckOptions(opts);
    testStencilFactory->CheckOptions(opts);

    // extract options for running the benchmark
    bool beVerbose = opts.getOptionBool("verbose");
    unsigned int nPasses = (unsigned int)opts.getOptionInt("passes");
    unsigned int nIters = (unsigned int)opts.getOptionInt("num-iters");
    double valErrThreshold = (double)opts.getOptionFloat("val-threshold");
    unsigned int nValErrsToPrint =
        (unsigned int)opts.getOptionInt("val-print-limit");

    // extract and validate properties of matrix
    long long matrixRows = opts.getOptionInt("matrixRows");
    long long matrixCols = opts.getOptionInt("matrixCols");
    static const long long matrixArr[] = {matrixRows, matrixCols};
    std::vector<long long> arrayDims(
        matrixArr, matrixArr + sizeof(matrixArr) / sizeof(long long));
    if (arrayDims[0] == 0 ||
        arrayDims[1] == 0) // User has not specified a custom size
    {
      std::cout
          << "Matrix dimensions not specified, using a preset problem size."
          << std::endl;
      int sizeClass = opts.getOptionInt("size");
      arrayDims = StencilFactory<T>::GetStandardProblemSize(sizeClass);
    }

    long int seed = (long)opts.getOptionInt("seed");
    float haloVal = (float)opts.getOptionFloat("haloVal");
    unsigned int haloWidth = 1;

    // build a description of this experiment
    long long blockRows = opts.getOptionInt("blockRows");
    long long blockCols = opts.getOptionInt("blockCols");
    static const long long blockArr[] = {blockRows, blockCols};
    std::vector<long long> lDims(
        blockArr, blockArr + sizeof(blockArr) / sizeof(long long));
    std::ostringstream experimentDescriptionStr;
    experimentDescriptionStr << nIters << ':' << arrayDims[0] << 'x'
                             << arrayDims[1] << ':' << lDims[0] << 'x'
                             << lDims[1];

    // compute the expected result on the host
    // or read it from a pre-existing file
    std::string matrixFilenameBase =
        (std::string)opts.getOptionString("expMatrixFile");
    if (!matrixFilenameBase.empty()) {
      std::cout << "\nReading expected stencil operation result from file for "
                   "later comparison with CUDA output.\n"
                << std::endl;
    } else {
      std::cout << "\nPerforming stencil operation on host for later "
                   "comparison with CUDA output.\n"
                << std::endl;
    }
    Matrix2D<T> expected(arrayDims[0] + 2 * haloWidth,
                         arrayDims[1] + 2 * haloWidth);
    Initialize<T> init(seed, haloWidth, haloVal);

    bool haveExpectedData = false;
    if (!matrixFilenameBase.empty()) {
      bool readOK = ReadMatrixFromFile(
          expected, GetMatrixFileName<T>(matrixFilenameBase));
      if (readOK) {

        if ((expected.GetNumRows() != arrayDims[0] + 2 * haloWidth) ||
            (expected.GetNumColumns() != arrayDims[1] + 2 * haloWidth)) {
          std::cerr << "The matrix read from file \'"
                    << GetMatrixFileName<T>(matrixFilenameBase)
                    << "\' does not match the matrix size specified on the "
                       "command line.\n";
          expected.Reset(arrayDims[0] + 2 * haloWidth,
                         arrayDims[1] + 2 * haloWidth);
        } else {
          haveExpectedData = true;
        }
      }

      if (!haveExpectedData) {
        std::cout << "\nPerforming stencil operation on host for later "
                     "comparison with CUDA output.\n"
                  << std::endl;
      }
    }
    if (!haveExpectedData) {
      init(expected);
      haveExpectedData = true;
      if (beVerbose) {
        std::cout << "Initial state:\n" << expected << std::endl;
      }
      stdStencil = stdStencilFactory->BuildStencil(opts);
      (*stdStencil)(expected, nIters);
    }
    if (beVerbose) {
      std::cout << "Expected result:\n" << expected << std::endl;
    }

    // determine whether we are to save the expected matrix values to a file
    // to speed up future runs
    matrixFilenameBase = (std::string)opts.getOptionString("saveExpMatrixFile");
    if (!matrixFilenameBase.empty()) {
      SaveMatrixToFile(expected, GetMatrixFileName<T>(matrixFilenameBase));
    }
    assert(haveExpectedData);

    // compute the result on the CUDA device
    Matrix2D<T> data(arrayDims[0] + 2 * haloWidth,
                     arrayDims[1] + 2 * haloWidth);
    Stencil<T> *testStencil = testStencilFactory->BuildStencil(opts);

    // Compute the number of floating point operations we will perform.
    //
    // Note: in the truly-parallel case, we count flops for redundant
    // work due to the need for a halo.
    // But we do not add to the count for the local 1-wide halo since
    // we aren't computing new values for those items.
    unsigned long npts =
        (arrayDims[0] + 2 * haloWidth - 2) * (arrayDims[1] + 2 * haloWidth - 2);

    // In our 9-point stencil, there are 11 floating point operations
    // per point (3 multiplies and 11 adds):
    //
    // newval = weight_center * centerval +
    //      weight_cardinal * (northval + southval + eastval + westval) +
    //      weight_diagnoal * (neval + nwval + seval + swval)
    //
    // we do this stencil operation 'nIters' times
    unsigned long nflops = npts * 11 * nIters;

    for (unsigned int pass = 0; pass < nPasses; pass++) {
      std::cout << "Pass " << pass << ": ";
      init(data);

      int timerHandle = Timer::Start();
      (*testStencil)(data, nIters);
      double elapsedTime = Timer::Stop(timerHandle, "CUDA stencil");

      // find and report the computation rate
      double gflops = (nflops / elapsedTime) / 1e9;

      resultDB.AddResult(timerDesc, experimentDescriptionStr.str(), "GFLOPS",
                         gflops);
      if (beVerbose) {
        std::cout << "observed result, pass " << pass << ":\n"
                  << data << std::endl;
      }

      // validate the result
      StencilValidater<T> *validater = new SerialStencilValidater<T>;
      validater->ValidateResult(expected, data, valErrThreshold,
                                nValErrsToPrint);
    }
  } catch (...) {
    // clean up - abnormal termination
    // wish we didn't have to do this, but C++ exceptions do not
    // support a try-catch-finally approach
    delete stdStencil;
    delete stdStencilFactory;
    delete testStencil;
    delete testStencilFactory;
    throw;
  }

  // clean up - normal termination
  delete stdStencil;
  delete stdStencilFactory;
  delete testStencil;
  delete testStencilFactory;
}

void RunBenchmark(ResultDatabase &resultDB, OptionParser &opts) {
  int device;

  cudaGetDevice(&device);
  cudaDeviceProp deviceProps;
  cudaGetDeviceProperties(&deviceProps, device);

  // Configure to allocate performance-critical memory in
  // a programming model-specific way.
  Matrix2D<float>::SetAllocator(new CUDAPMSMemMgr<float>);

  std::cout << "Running single precision test" << std::endl;
  DoTest<float>("SP_Sten2D", resultDB, opts);

  // check if we can run double precision tests
  if (((deviceProps.major == 1) && (deviceProps.minor >= 3)) ||
      (deviceProps.major >= 2)) {
    // Configure to allocate performance-critical memory in
    // a programming model-specific way.
    Matrix2D<double>::SetAllocator(new CUDAPMSMemMgr<double>);

    std::cout << "\n\nDP supported" << std::endl;
    DoTest<double>("DP_Sten2D", resultDB, opts);
  } else {
    std::cout << "Double precision not supported - skipping" << std::endl;
    // resultDB requires neg entry for every possible result
    int nPasses = (int)opts.getOptionInt("passes");
    for (int p = 0; p < nPasses; p++) {
      resultDB.AddResult((const char *)"DP_Sten2D", "N/A", "GFLOPS", FLT_MAX);
    }
  }
  std::cout << "\n" << std::endl;
}

// Adds command line options to given OptionParser
void addBenchmarkSpecOptions(OptionParser &opts) {
  opts.addOption("matrixRows", OPT_INT, "0",
                 "specify number of rows in the matrix");
  opts.addOption("matrixCols", OPT_INT, "0",
                 "specify number of columns in the matrix");
  opts.addOption("blockRows", OPT_INT, "8",
                 "specify number of rows in the block");
  opts.addOption("blockCols", OPT_INT, "256",
                 "specify number of columns in the block");
  opts.addOption("num-iters", OPT_INT, "1000", "number of stencil iterations");
  opts.addOption("weight-center", OPT_FLOAT, "0.25", "center value weight");
  opts.addOption("weight-cardinal", OPT_FLOAT, "0.15",
                 "cardinal values weight");
  opts.addOption("weight-diagonal", OPT_FLOAT, "0.05",
                 "diagonal values weight");
  opts.addOption("seed", OPT_INT, "71594", "random number generator seed");
  opts.addOption("haloVal", OPT_FLOAT, "0.0", "value to use for halo data");
  opts.addOption("val-threshold", OPT_FLOAT, "0.01",
                 "validation error threshold");
  opts.addOption("val-print-limit", OPT_INT, "15",
                 "number of validation errors to print");

  opts.addOption("expMatrixFile", OPT_STRING, "",
                 "Basename for file(s) holding expected matrices");
  opts.addOption(
      "saveExpMatrixFile", OPT_STRING, "",
      "Basename for output file(s) that will hold expected matrices");
}

// validate stencil-independent values
void CheckOptions(const OptionParser &opts) {
  // check matrix dimensions - must be 2d, must be positive
  long long matrixRows = opts.getOptionInt("matrixRows");
  long long matrixCols = opts.getOptionInt("matrixCols");
  if (matrixRows < 0 || matrixCols < 0) {
    throw InvalidArgValue("Each size dimension must be positive");
  }

  // validation error threshold must be positive
  float valThreshold = opts.getOptionFloat("val-threshold");
  if (valThreshold <= 0.0f) {
    throw InvalidArgValue("Validation threshold must be positive");
  }

  // number of validation errors to print must be non-negative
  int nErrsToPrint = opts.getOptionInt("val-print-limit");
  if (nErrsToPrint < 0) {
    throw InvalidArgValue(
        "Number of validation errors to print must be non-negative");
  }
}
