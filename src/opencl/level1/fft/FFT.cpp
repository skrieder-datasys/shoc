#include <cfloat>
#include <iostream>
#include <sstream>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include "support.h"
#include "ResultDatabase.h"
#include "Event.h"
#include "OptionParser.h"
#include "Timer.h"

#include "fftlib.h"

using namespace std;

// ****************************************************************************
// Function: addBenchmarkSpecOptions
//
// Purpose:
//   Add benchmark specific options parsing.  The user is allowed to specify
//   the size of the input data in megabytes.
//
// Arguments:
//   op: the options parser / parameter database
//
// Programmer: Collin McCurdy
// Creation: September 08, 2009
// Returns:  nothing
//
// ****************************************************************************
void 
addBenchmarkSpecOptions(OptionParser &op) 
{
    op.addOption("MB", OPT_INT, "0", "data size (in megabytes)");
    op.addOption("use-native", OPT_BOOL, "false", "call native (HW) versions of sin/cos");
    op.addOption("dump-sp", OPT_BOOL, "false", "dump result after SP fft/ifft");
    op.addOption("dump-dp", OPT_BOOL, "false", "dump result after DP fft/ifft");
}


// ****************************************************************************
// Function: RunBenchmark
//
// Purpose:
//   Calls single precision and, if viable, double precision FFT
//   benchmark.  Optionally dumps data arrays for correctness check.
//
// Arguments:
//  resultDB: the benchmark stores its results in this ResultDatabase
//  op: the options parser / parameter database
//
// Returns:  nothing
//
// Programmer: Collin McCurdy
// Creation: September 08, 2009
//
// Modifications:
//
// ****************************************************************************

template <class T2> void runTest(const string& name, cl_device_id id, 
                                 cl_context ctx, cl_command_queue queue,
                                 ResultDatabase &resultDB, OptionParser& op);
template <class T2> void dump(OptionParser& op);

static void
fillResultDB(const string& name, const string& reason, OptionParser &op, 
             ResultDatabase& resultDB)
{
    // resultDB requires neg entry for every possible result
    int passes = op.getOptionInt("passes");
    for (int k=0; k<passes; k++) {
        resultDB.AddResult(name , reason, "GB/s", FLT_MAX);
        resultDB.AddResult(name+"_PCIe" , reason, "GB/s", FLT_MAX);
        resultDB.AddResult(name+"_Parity" , reason, "GB/s", FLT_MAX);
        resultDB.AddResult(name+"-INV" , reason, "GB/s", FLT_MAX);
        resultDB.AddResult(name+"-INV_PCIe" , reason, "GB/s", FLT_MAX);
        resultDB.AddResult(name+"-INV_Parity" , reason, "GB/s", FLT_MAX);
    }
}


void
RunBenchmark(cl::Device& devcpp,
                  cl::Context& ctxcpp,
                  cl::CommandQueue& queuecpp,
                  ResultDatabase &resultDB,
                  OptionParser &op)
{
    // convert from C++ bindings to C bindings
    // TODO propagate use of C++ bindings
    cl_device_id dev = devcpp();
    cl_context ctx = ctxcpp();
    cl_command_queue queue = queuecpp();

    if (getMaxWorkGroupSize(dev) < 64) {
        cout << "FFT requires MaxWorkGroupSize of at least 64" << endl;
        fillResultDB("SP-FFT", "MaxWorkGroupSize<64", op, resultDB);
        fillResultDB("DP-FFT", "MaxWorkGroupSize<64", op, resultDB);
        return;
    }
    
    bool has_dp = checkExtension(dev, "cl_khr_fp64") ||
        checkExtension(dev, "cl_amd_fp64");

    if (op.getOptionBool("dump-sp")) {
        dump<cplxflt>(op);
    }
    else if (op.getOptionBool("dump-dp")) {
        if (!has_dp) {
            cout << "dump-dp: no double precision support!\n";
            return;
        }
        dump<cplxdbl>(op);
    }
    else {
        // Always run single precision test
        runTest<cplxflt>("SP-FFT", dev, ctx, queue, resultDB, op);

        // If double precision is supported, run the DP test
        if (has_dp) {
            cout << "DP Supported\n";
            runTest<cplxdbl>("DP-FFT", dev, ctx, queue, resultDB, op);
        }
        else {
            cout << "DP Not Supported\n";
            fillResultDB("DP-FFT", "DP_Not_Supported", op, resultDB);
        }
    }
}


// ****************************************************************************
// Function: runTest
//
// Purpose:
//   This benchmark measures the performance of a single or double
//   precision fast fourier transform (FFT).  Data transfer time over
//   the PCIe bus is not included in this measurement.
//
// Arguments:
//  resultDB: the benchmark stores its results in this ResultDatabase
//  op: the options parser / parameter database
//
// Returns:  nothing
//
// Programmer: Collin McCurdy
// Creation: September 08, 2009
//
// Modifications:
//   Jeremy Meredith, Thu Aug 19 15:43:55 EDT 2010
//   Added PCIe timings.  Added size index bounds check.
//
// ****************************************************************************

template <class T2> inline bool dp(void);
template <> inline bool dp<cplxflt>(void) { return false; }
template <> inline bool dp<cplxdbl>(void) { return true; }

template <class T2>
void runTest(const string& name, 
             cl_device_id id, 
             cl_context ctx,
             cl_command_queue queue,
             ResultDatabase &resultDB,
             OptionParser &op)
{
    int i;
    void *work, *chk;

    unsigned long bytes = 0;

    if (op.getOptionInt("MB") == 0) {
        int probSizes[4] = { 1, 8, 96, 256 };
	int sizeIndex = op.getOptionInt("size")-1;
	if (sizeIndex < 0 || sizeIndex >= 4) {
	    cerr << "Invalid size index specified\n";
	    exit(-1);
	}
        bytes = probSizes[sizeIndex];
    } else {
        bytes = op.getOptionInt("MB");
    }
    // Convert to MB
    bytes *= 1024 * 1024;

    int passes = op.getOptionInt("passes");

    fftDev = id;
    fftCtx = ctx;
    fftQueue = queue;

    bool do_dp = dp<T2>();
    init(op, do_dp);
    
#if 0
    bytes = findAvailBytes(fftDev);
    fprintf(stderr, "findAvailBytes()=%lu\n", bytes);
#endif

    // now determine how much available memory will be used
    int half_n_ffts = bytes / (512*sizeof(T2)*2);
    int n_ffts = half_n_ffts * 2;
    int half_n_cmplx = half_n_ffts * 512;
    unsigned long used_bytes = half_n_cmplx * 2 * sizeof(T2);
    double N = (double)half_n_cmplx*2.0;

    // allocate host memory
    T2 *source, *result;
    allocHostBuffer((void**)&source, used_bytes);
    allocHostBuffer((void**)&result, used_bytes);

    // init host memory...
    for (i = 0; i < half_n_cmplx; i++) {
        source[i].x = (rand()/(float)RAND_MAX)*2-1;
        source[i].y = (rand()/(float)RAND_MAX)*2-1;
        source[i+half_n_cmplx].x = source[i].x;
        source[i+half_n_cmplx].y = source[i].y;
    }

    // alloc device memory
    allocDeviceBuffer(&work, used_bytes);
    allocDeviceBuffer(&chk, sizeof(cl_int));

    // copy to device, and record transfer time
    cl_int chk_init = 0;
    copyToDevice(chk, &chk_init, sizeof(cl_int));
    clFinish(queue);

    // (warm up)
    copyToDevice(work, source, used_bytes);
    clFinish(queue);

    // (measure)
    int pcie_TH = Timer::Start();
    copyToDevice(work, source, used_bytes);
    clFinish(queue);
    double transfer_time = Timer::Stop(pcie_TH, "PCIe Transfer Time");

    const char *sizeStr;
    stringstream ss;
    ss << "N=" << (long)N;
    sizeStr = strdup(ss.str().c_str());
    
    for (int k=0; k<passes; k++) {
        // time fft kernel
        forward(work, n_ffts);
        fftEvent.FillTimingInfo();
        double nsec = (double)fftEvent.SubmitEndRuntime();
        double fftsz = 512;
        double Gflops = n_ffts*(5*fftsz*log2(fftsz))/nsec;
        double gflopsPCIe = n_ffts*(5*fftsz*log2(fftsz)) /
                (transfer_time*1e9f + nsec);
        resultDB.AddResult(name, sizeStr, "GFLOPS", Gflops);
        resultDB.AddResult(name+"_PCIe", sizeStr, "GFLOPS", gflopsPCIe);
        resultDB.AddResult(name+"_Parity", sizeStr, "N", transfer_time*1e9f / nsec);

        // time ifft kernel
        inverse(work, n_ffts);
        ifftEvent.FillTimingInfo();
        nsec = (double)ifftEvent.SubmitEndRuntime();
        Gflops = n_ffts*(5*fftsz*log2(fftsz))/nsec;
        gflopsPCIe = n_ffts*(5*fftsz*log2(fftsz)) /
                (transfer_time*1e9f + nsec);
        resultDB.AddResult(name+"-INV", sizeStr, "GFLOPS", Gflops);
        resultDB.AddResult(name+"-INV_PCIe", sizeStr, "GFLOPS", gflopsPCIe);
        resultDB.AddResult(name+"-INV_Parity", sizeStr, "N", transfer_time*1e9f / nsec);
        // check kernel
        int failed = check(work, chk, half_n_ffts, half_n_cmplx);
        cout << "pass " << k << ((failed) ? ": failed\n" : ": passed\n");
    }
    
    freeDeviceBuffer(work);
    freeDeviceBuffer(chk);
    freeHostBuffer(source);
    freeHostBuffer(result);
    deinit();
}


// ****************************************************************************
// Function: dump
//
// Purpose:
//   Dump result array to stdout after FFT and IFFT.  For correctness 
//   checking.
//
// Arguments:
//  op: the options parser / parameter database
//
// Returns:  nothing
//
// Programmer: Collin McCurdy
// Creation: September 30, 2010
//
// Modifications:
//
// ****************************************************************************
template <class T2> 
void dump(OptionParser& op)
{	
    int i;
    void* work;
    T2* source, * result;
    unsigned long bytes = 0;

    if (op.getOptionInt("MB") == 0) {
        int probSizes[4] = { 1, 8, 96, 256 };
	int sizeIndex = op.getOptionInt("size")-1;
	if (sizeIndex < 0 || sizeIndex >= 4) {
	    cerr << "Invalid size index specified\n";
	    exit(-1);
	}
        bytes = probSizes[sizeIndex];
    } else {
        bytes = op.getOptionInt("MB");
    }

    // Convert to MB
    bytes *= 1024 * 1024;

    bool do_dp = dp<T2>();
    init(op, do_dp);

    // now determine how much available memory will be used
    int half_n_ffts = bytes / (512*sizeof(T2)*2);
    int n_ffts = half_n_ffts * 2;
    int half_n_cmplx = half_n_ffts * 512;
    unsigned long used_bytes = half_n_cmplx * 2 * sizeof(T2);
    double N = half_n_cmplx*2;
    
    fprintf(stderr, "used_bytes=%lu, N=%g\n", used_bytes, N);

    // allocate host and device memory
    allocHostBuffer((void**)&source, used_bytes);
    allocHostBuffer((void**)&result, used_bytes);

    // init host memory...
    for (i = 0; i < half_n_cmplx; i++) {
        source[i].x = (rand()/(float)RAND_MAX)*2-1;
        source[i].y = (rand()/(float)RAND_MAX)*2-1;
        source[i+half_n_cmplx].x = source[i].x;
        source[i+half_n_cmplx].y = source[i].y;
    }

    // alloc device memory
    allocDeviceBuffer(&work, used_bytes);

    copyToDevice(work, source, used_bytes);

    fprintf(stdout, "INITIAL:\n");
    for (i = 0; i < N; i++) {
        fprintf(stdout, "(%g, %g)\n", source[i].x, source[i].y);
    }
    
    forward(work, n_ffts);
    copyFromDevice(result, work, used_bytes);
        
    fprintf(stdout, "FORWARD:\n");
    for (i = 0; i < N; i++) {
        fprintf(stdout, "(%g, %g)\n", result[i].x, result[i].y);
    }
    
    inverse(work, n_ffts);
    copyFromDevice(result, work, used_bytes);
        
    fprintf(stdout, "\nINVERSE:\n");
    for (i = 0; i < N; i++) {
        fprintf(stdout, "(%g, %g)\n", result[i].x, result[i].y);
    }

    freeDeviceBuffer(work);
    freeHostBuffer(source);
    freeHostBuffer(result);
}
