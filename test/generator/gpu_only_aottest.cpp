#include <math.h>
#include <stdio.h>
#include "HalideRuntime.h"
#include <assert.h>
#if defined(TEST_OPENCL)
#include "HalideRuntimeOpenCL.h"
#elif defined(TEST_CUDA)
#include "HalideRuntimeCuda.h"
#endif

#include "gpu_only.h"
#include "halide_image.h"
using namespace Halide::Tools;

int main(int argc, char **argv) {
#if defined(TEST_OPENCL) || defined(TEST_CUDA)
    const int W = 32, H = 32;
    Image<int> input(W, H);
    for (int y = 0; y < input.height(); y++) {
        for (int x = 0; x < input.width(); x++) {
            input(x, y) = x + y;
        }
    }

    // Explicitly copy data to the GPU.
    input.set_host_dirty();
#if defined(TEST_OPENCL)
    input.copy_to_device(halide_opencl_device_interface());
#elif defined(TEST_CUDA)
    input.copy_to_device(halide_cuda_device_interface());
#endif

    Image<int> output(W, H);

    // Create buffer_ts without host pointers.
    buffer_t input_no_host = *((buffer_t *)input);
    input_no_host.host = NULL;

    buffer_t output_no_host = *((buffer_t *)output);
    // We need a fake pointer here to trick Halide into creating the
    // device buffer (and not do bounds inference instead of running
    // the pipeline). Halide will not dereference this pointer.
    output_no_host.host = (uint8_t *)1;

    gpu_only(&input_no_host, &output_no_host);

    // Restore the host pointer and copy to host.
    output_no_host.host = (uint8_t *)output.data();
    halide_copy_to_host(NULL, &output_no_host);

    // Verify output.
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (input(x, y) * 2 != output(x, y)) {
                printf("Error at %d, %d: %d != %d\n", x, y, input(x, y), output(x, y));
                return -1;
            }
        }
    }

    printf("Success!\n");
#else
    printf("No GPU target enabled, skipping...\n");
#endif
    return 0;
}
