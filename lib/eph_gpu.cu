
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <stdio.h>

__global__
void dummy_test(int myID, int nrPS)
{
  int thread_index = threadIdx.x;
  int block_dimension = blockDim.x;
  int grid_dimension = gridDim.x;

  printf("GPU %d %d %d %d %d\n", myID, nrPS, thread_index, block_dimension, grid_dimension);
}

__global__
void calculate_environment_gpu_cu()
{
  
}


void run_dummy_test_cu(int myID, int nrPS) 
{
  dummy_test<<<1, 1>>>(myID, nrPS);
}

