/* Copyright 2023 Stanford University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef COMMBENCH_H
#define COMMBENCH_H

// GPU PORTS
// For NVIDIA: #define PORT_CUDA
// For AMD: #define PORT_HIP
// For INTEL: #define PORT_ONEAPI
//
// CAPABILITIES
// CAP_NCCL
// CAP_ONECCL
// CAP_ZE
// CAP_GASNET
// 
// MODES
// USE_MPI
// USE_GASNET

#ifdef USE_GASNET
#define CAP_GASNET
#else
#define USE_MPI
#endif

#if defined PORT_CUDA || defined PORT_HIP
#define CAP_NCCL
#define CAP_NCCL_LOCAL
#endif
#ifdef PORT_ONEAPI
// #define CAP_ZE
// #define CAP_ONECCL
#endif

// DEPENDENCIES
#ifdef USE_MPI
#include <mpi.h>
#endif
#ifdef PORT_CUDA
#ifdef CAP_NCCL
#include <nccl.h>
#else
#include <cuda_runtime.h>
#endif
#elif defined PORT_HIP
#ifdef CAP_NCCL
#include <rccl.h>
#else
#include <hip_runtime.h>
#endif
#elif defined PORT_ONEAPI
#ifdef CAP_ONECCL
#include <oneapi/ccl.hpp>
#else
#include <sycl.hpp>
#endif
#ifdef CAP_ZE
#include <ze_api.h>
#endif
#endif
#ifdef CAP_GASNET
#define GASNET_PAR
#include <gasnetex.h>
#include <gasnet_mk.h>
// #include <gasnet.h>
#endif

// CPP LIBRARIES
#include <stdio.h> // for printf
#include <string.h> // for memcpy
#include <algorithm> // for std::sort
#include <vector> // for std::vector
#include <omp.h> // for omp_get_wtime()
#include <unistd.h> // for fd
#include <sys/syscall.h> // for syscall

namespace CommBench
{
  static int printid = 0;
  static int numbench = 0;
  static std::vector<void*> benchlist;
  static int mydevice = -1;

  enum library {dummy, MPI, NCCL, IPC, IPC_get, GEX, GEX_get, numlib};

#ifdef USE_MPI
  static MPI_Comm comm_mpi;
#endif
  static int myid;
  static int numproc;
#ifdef PORT_ONEAPI
  static sycl::queue q(sycl::gpu_selector_v);
#endif
#ifdef CAP_NCCL
  static ncclComm_t comm_nccl;
#endif
#ifdef CAP_ONECCL
  static ccl::communicator *comm_ccl;
#endif
#ifdef CAP_GASNET
  static gex_Client_t myclient;
  static gex_EP_t ep_primordial;
  static gex_TM_t myteam;
  static gex_MK_t memkind;
  static std::vector<void*> myep_ptr;
  static std::vector<gex_EP_t> myep;
#endif

  static void print_data(size_t data) {
    if (data < 1e3)
      printf("%d bytes", (int)data);
    else if (data < 1e6)
      printf("%.4f KB", data / 1e3);
    else if (data < 1e9)
      printf("%.4f MB", data / 1e6);
    else if (data < 1e12)
      printf("%.4f GB", data / 1e9);
    else
      printf("%.4f TB", data / 1e12);
  }
  static void print_lib(library lib) {
    switch(lib) {
      case dummy      : printf("dummy");        break;
      case IPC        : printf("IPC (PUT)");    break;
      case IPC_get    : printf("IPC (GET)");    break;
      case MPI        : printf("MPI");          break;
      case NCCL       : printf("NCCL");         break;
      case GEX        : printf("GASNET (PUT)"); break;
      case GEX_get    : printf("GASNET (GET)"); break;
      case numlib     : printf("numlib");       break;
    }
  }

  // MEMORY MANAGEMENT
  void barrier();
  template <typename T>
  void allocate(T *&buffer,size_t n);
  template <typename T>
  void allocateHost(T *&buffer, size_t n);
  template <typename T>
  void memcpyD2H(T *host, T *device, size_t n);
  template <typename T>
  void memcpyH2D(T *device, T *host, size_t n);
  template <typename T>
  void free(T *buffer);
  template <typename T>
  void freeHost(T *buffer);

  // PAIR COMMUNICATION
#ifdef USE_GASNET
  std::vector<bool> am_ready;
  void* am_ptr;
  bool am_busy = false;
  gex_AM_Index_t am_recv_index = GEX_AM_INDEX_BASE + 0;
  gex_AM_Index_t am_send_index = GEX_AM_INDEX_BASE + 1;
  void am_recv(gex_Token_t token, gex_AM_Arg_t dst) {
    am_ready[dst] = true;
  };
  void am_send(gex_Token_t token, void *buf, size_t nbytes) {
    memcpy(am_ptr, buf, nbytes);
    am_busy = false;
  };
#endif
  template <typename T>
  void send(T *sendbuf, int recvid) {
#ifdef USE_GASNET
    GASNET_BLOCKUNTIL(am_ready[recvid]);
    am_ready[recvid] = false;
    gex_AM_RequestMedium0(myteam, recvid, am_send_index, sendbuf, sizeof(T), GEX_EVENT_NOW, 0);
#else
    MPI_Ssend(sendbuf, sizeof(T), MPI_BYTE, recvid, 0, comm_mpi);
#endif
  };
  template <typename T>
  void recv(T *recvbuf, int sendid) {
#ifdef USE_GASNET
    am_ptr = recvbuf;
    am_busy = true;
    gex_AM_RequestShort1(myteam, sendid, am_recv_index, 0, myid);
    GASNET_BLOCKUNTIL(!am_busy);
#else
    MPI_Recv(recvbuf, sizeof(T), MPI_BYTE, sendid, 0, comm_mpi, MPI_STATUS_IGNORE);
#endif
  };
  template <typename T>
  void pair(T *sendbuf, T *recvbuf, int sendid, int recvid) {
    if(sendid == recvid) {
      if(myid == sendid)
        memcpy(recvbuf, sendbuf, sizeof(T));
      return;
    }
    if(myid == sendid)
      send(sendbuf, recvid);
    if(myid == recvid)
      recv(recvbuf, sendid);
  }
  template <typename T>
  void broadcast(T *sendbuf, T *recvbuf, int root) {
    T temp;
    for(int i = 0; i < numproc; i++)
      pair(sendbuf, &temp, root, i);
    *recvbuf = temp;
  }
  template <typename T> void broadcast(T *sendbuf) { broadcast(sendbuf, sendbuf, 0); };
  template <typename T>
  void allgather(T *sendval, T *recvbuf) {
    for(int root = 0; root < numproc; root++)
      broadcast(sendval, recvbuf + root, root);
  }
  template <typename T>
  void allreduce_sum(T *sendbuf, T *recvbuf) {
    std::vector<T> temp(numproc);
    allgather(sendbuf, temp.data());
    T sum = 0;
    for(int i = 0; i < numproc; i++)
      sum += temp[i];
    *recvbuf = sum;
  }
  template <typename T> void allreduce_sum(T *sendbuf) { allreduce_sum(sendbuf, sendbuf); }
  template <typename T>
  void allreduce_max(T *sendbuf, T *recvbuf) {
    std::vector<T> temp(numproc);
    allgather(sendbuf, temp.data());
    T max = *sendbuf;
    for(int i = 0; i < numproc; i++)
      if(temp[i] > max)
        max = temp[i];
    *recvbuf = max;
  }
  template <typename T> void allreduce_max(T *sendbuf) { allreduce_max(sendbuf, sendbuf); }

  char allreduce_land(char logic) {
    std::vector<char> temp(numproc);
    allgather(&logic, temp.data());
    for(int i = 0; i < numproc; i++)
      if(temp[i] == 0)
        return 0;
    return 1;
  }

  // MEASUREMENT
  template <typename C>
  static void measure(int warmup, int numiter, double &minTime, double &medTime, double &maxTime, double &avgTime, C &comm);

  template <typename T>
  struct pyalloc {
    T* ptr;
    pyalloc(size_t n) {
      allocate(ptr, n);
    }
    void pyfree() {
      free(ptr);
    }
  };

#include "util.h"

  // one-time initialization of CommBench
  static void init() {
    static bool init = false;
    if(init) return;
    init = true;
#ifdef USE_MPI
    {
      int init_mpi;
      MPI_Initialized(&init_mpi);
      if(!init_mpi) {
        MPI_Init(NULL, NULL);
        // MPI_Init_thread(NULL, NULL, MPI_THREAD_MULTIPLE, NULL);
      }
      MPI_Comm_dup(MPI_COMM_WORLD, &comm_mpi); // CREATE SEPARATE COMMUNICATOR EXPLICITLY
      MPI_Comm_rank(comm_mpi, &myid);
      MPI_Comm_size(comm_mpi, &numproc);
      if(myid == printid) {
        if(!init_mpi) {
          printf("#################### MPI IS INITIALIZED\n");
          int provided;
          MPI_Query_thread(&provided);
          printf("provided thread support: %d\n", provided);
        }
        printf("******************** MPI COMMUNICATOR IS CREATED\n");
      }
    }
#endif
#ifdef CAP_GASNET
    {
      // initialize
      gex_Client_Init(&myclient, &ep_primordial, &myteam, "CommBench+GASNet-EX", NULL, NULL, 0);
      myid = gex_TM_QueryRank(myteam);
      numproc = gex_TM_QuerySize(myteam);
      myep.push_back(ep_primordial); // primordial is index 0
      myep_ptr.push_back(nullptr); // primordial segment is 0
      if(myid == printid)
        printf("******************** GASNET CLIENT IS CREATED\n");
#ifdef USE_GASNET
      gex_AM_Entry_t handlers[] = {
        {am_recv_index, (gex_AM_Fn_t)am_recv, GEX_FLAG_AM_REQUEST | GEX_FLAG_AM_SHORT, 0},
        {am_send_index, (gex_AM_Fn_t)am_send, GEX_FLAG_AM_REQUEST | GEX_FLAG_AM_MEDIUM, 0}
        // Add more handlers if needed
      };
      gex_EP_RegisterHandlers(ep_primordial, handlers, sizeof(handlers) / sizeof(gex_AM_Entry_t));
      for (int i = 0; i < numproc; i++)
        am_ready.push_back(false);
      barrier();
#endif
    }
#endif

    // grab a single GPU
    setup_gpu();

#ifdef CAP_GASNET
    {
#if defined(PORT_CUDA) || defined(PORT_HIP) || defined(PORT_ONEAPI)
      // create device memory kind
      gex_MK_Create_args_t args;
      args.gex_flags = 0;
#ifdef PORT_CUDA
      args.gex_class = GEX_MK_CLASS_CUDA_UVA;
      args.gex_args.gex_class_cuda_uva.gex_CUdevice = mydevice;
#elif defined PORT_HIP
      args.gex_class = GEX_MK_CLASS_HIP;
      args.gex_args.gex_class_hip.gex_hipDevice = mydevice;
#elif defined PORT_ONEAPI
      args.gex_class = GEX_MK_CLASS_ZE; // TODO: implement MK args for ZE
      // args.gex_args.gex_class_ze.gex_zeDevice =
      // args.gex_args.gex_class_ze.gex_zeContext =
      // args.gex_args.gex_class_ze.gex_zeMemoryOrdinal =
#endif
      gex_MK_Create(&memkind, myclient, &args, 0);
#else
      memkind = GEX_MK_HOST;
#endif
    }
#endif
  }

  static void finalize() {
    static bool finalize = false;
    if(finalize) return;
    finalize = true;
#ifdef USE_MPI
    int finalize_mpi;
    MPI_Finalized(&finalize_mpi);
    if (!finalize_mpi) {
      MPI_Finalize();
      if (myid == printid)
        printf("#################### MPI IS FINALIZED\n");
    }
#endif
#ifdef CAP_GASNET
   gex_EP_PublishBoundSegment(myteam, myep.data(), myep.size(), 0); // register EP's all-at-once
#endif
  }

#include "comm.h"
  // THIS IS TO INITIALIZE COMMBENCH
  // static Comm<char> init(dummy);

  static void print_stats(std::vector<double> times, size_t data) {

    std::sort(times.begin(), times.end(),  [](const double & a, const double & b) -> bool {return a < b;});

    int numiter = times.size();

    if(myid == printid) {
      printf("%d measurement iterations (sorted):\n", numiter);
      for(int iter = 0; iter < numiter; iter++) {
        printf("time: %.4e", times[iter] * 1e6);
        if(iter == 0)
          printf(" -> min\n");
        else if(iter == numiter / 2)
          printf(" -> median\n");
        else if(iter == numiter - 1)
          printf(" -> max\n");
        else
          printf("\n");
      }
      printf("\n");
    }
    double minTime = times[0];
    double medTime = times[numiter / 2];
    double maxTime = times[numiter - 1];
    double avgTime = 0;
    for(int iter = 0; iter < numiter; iter++)
      avgTime += times[iter];
    avgTime /= numiter;
    if(myid == printid) {
      printf("data: "); print_data(data); printf("\n");
      printf("minTime: %.4e us, %.4e ms/GB, %.4e GB/s\n", minTime * 1e6, minTime / data * 1e12, data / minTime / 1e9);
      printf("medTime: %.4e us, %.4e ms/GB, %.4e GB/s\n", medTime * 1e6, medTime / data * 1e12, data / medTime / 1e9);
      printf("maxTime: %.4e us, %.4e ms/GB, %.4e GB/s\n", maxTime * 1e6, maxTime / data * 1e12, data / maxTime / 1e9);
      printf("avgTime: %.4e us, %.4e ms/GB, %.4e GB/s\n", avgTime * 1e6, avgTime / data * 1e12, data / avgTime / 1e9);
      printf("\n");
    }
  }

  template <typename T>
  static void measure_async(std::vector<Comm<T>> commlist, int warmup, int numiter, size_t count) {
    std::vector<double> t;
    for(int iter = -warmup; iter < numiter; iter++) {
      barrier();
      double time = omp_get_wtime();
      for (auto &i : commlist) {
        i.start();
        i.wait();
      }
      time = omp_get_wtime() - time;
      allreduce_max(&time);
      if(iter >= 0)
        t.push_back(time);
    }
    print_stats(t, count * sizeof(T));
  }

  template <typename T>
  static void measure_concur(std::vector<Comm<T>> commlist, int warmup, int numiter, size_t count) {
    std::vector<double> t;
    for(int iter = -warmup; iter < numiter; iter++) {
      barrier();
      double time = omp_get_wtime();
      for (auto &i : commlist) {
        i.start();
      }
      for (auto &i : commlist) {      
        i.wait();
      }
      time = omp_get_wtime() - time;
      allreduce_max(&time);
      if(iter >= 0)
        t.push_back(time);
    }
    print_stats(t, count * sizeof(T));
  }

#ifdef USE_MPI
  template <typename T>
  static void measure_MPI_Alltoallv(std::vector<std::vector<int>> pattern, int warmup, int numiter) {
    std::vector<int> sendcount;
    std::vector<int> recvcount;
    for(int i = 0; i < numproc; i++) {
      sendcount.push_back(pattern[myid][i]);
      recvcount.push_back(pattern[i][myid]);
    }
    std::vector<int> senddispl(numproc + 1, 0);
    std::vector<int> recvdispl(numproc + 1, 0);
    for(int i = 1; i < numproc + 1; i++) {
      senddispl[i] = senddispl[i-1] + sendcount[i-1];
      recvdispl[i] = recvdispl[i-1] + recvcount[i-1];

    }
    //for(int i = 0; i < numproc; i++)
    //  printf("myid %d i: %d sendcount %d senddispl %d recvcount %d recvdispl %d\n", myid, i, sendcount[i], senddispl[i], recvcount[i], recvdispl[i]);
    T *sendbuf;
    T *recvbuf;
    allocate(sendbuf, senddispl[numproc]);
    allocate(recvbuf, recvdispl[numproc]);
    for(int p = 0; p < numproc; p++) {
      sendcount[p] *= sizeof(T);
      recvcount[p] *= sizeof(T);
      senddispl[p] *= sizeof(T);
      recvdispl[p] *= sizeof(T);
    }

    std::vector<double> t;
    for(int iter = -warmup; iter < numiter; iter++) {
      barrier();
      double time = omp_get_wtime();
      MPI_Alltoallv(sendbuf, &sendcount[0], &senddispl[0], MPI_BYTE, recvbuf, &recvcount[0], &recvdispl[0], MPI_BYTE, comm_mpi);
      time = omp_get_wtime() - time;
      allreduce_max(&time);
      if(iter >= 0)
        t.push_back(time);
    }

    free(sendbuf);
    free(recvbuf);
    int data;
    allreduce_sum(&senddispl[numproc], &data);
    print_stats(t, data * sizeof(T));
  }
#endif

  template <typename C>
  static void measure(int warmup, int numiter, double &minTime, double &medTime, double &maxTime, double &avgTime, C &comm) {

    double times[numiter];
    double starts[numiter];

    if(myid == printid)
      printf("%d warmup iterations (in order):\n", warmup);
    for (int iter = -warmup; iter < numiter; iter++) {
      for(int send = 0; send < comm.numsend; send++) {
#if defined PORT_CUDA
        // cudaMemset(sendbuf[send], -1, sendcount[send] * sizeof(T));
#elif defined PORT_HIP
        // hipMemset(sendbuf[send], -1, sendcount[send] * sizeof(T));
#elif defined PORT_ONEAPI
        // q->memset(sendbuf[send], -1, sendcount[send] * sizeof(T)).wait();
#else
        // memset(comm.sendbuf[send], -1, comm.sendcount[send] * sizeof(T)); // NECESSARY FOR CPU TO PREVENT CACHING
#endif
      }
      barrier();
      double time = omp_get_wtime();
      comm.start();
      double start = omp_get_wtime() - time;
      comm.wait();
      time = omp_get_wtime() - time;
      barrier();
      allreduce_max(&start);
      allreduce_max(&time);
      if(iter < 0) {
        if(myid == printid)
          printf("startup %.2e warmup: %.2e\n", start * 1e6, time * 1e6);
      }
      else {
        starts[iter] = start;
        times[iter] = time;
      }
    }
    std::sort(times, times + numiter,  [](const double & a, const double & b) -> bool {return a < b;});
    std::sort(starts, starts + numiter,  [](const double & a, const double & b) -> bool {return a < b;});

    if(myid == printid) {
      printf("%d measurement iterations (sorted):\n", numiter);
      for(int iter = 0; iter < numiter; iter++) {
        printf("start: %.4e time: %.4e", starts[iter] * 1e6, times[iter] * 1e6);
        if(iter == 0)
          printf(" -> min\n");
        else if(iter == numiter / 2)
          printf(" -> median\n");
        else if(iter == numiter - 1)
          printf(" -> max\n");
        else
          printf("\n");
      }
      printf("\n");
    }
    minTime = times[0];
    medTime = times[numiter / 2];
    maxTime = times[numiter - 1];
    avgTime = 0;
    for(int iter = 0; iter < numiter; iter++)
      avgTime += times[iter];
    avgTime /= numiter;
  }

  // MEMORY MANAGEMENT
  size_t memory = 0;
  void report_memory() {
    std::vector<size_t> memory_all(numproc);
    allgather(&memory, memory_all.data());
    if(myid == printid) {
      size_t memory_total = 0;
      printf("\n");
      printf("CommBench memory report:\n");
      for(int i = 0; i < numproc; i++) {
        printf("proc: %d memory ", i);
        print_data(memory_all[i]);
        printf("\n");
        memory_total += memory_all[i];
      }
      printf("total memory: ");
      print_data(memory_total);
      printf("\n");
      printf("\n");
    }
  }

  void barrier() {
#ifdef CAP_GASNET
    gex_Event_Wait(gex_Coll_BarrierNB(myteam, 0));
#else
    MPI_Barrier(comm_mpi);
#endif
  }

  template <typename T>
  void allocate(T *&buffer, size_t n) {
#ifdef PORT_CUDA
    cudaMalloc((void**)&buffer, n * sizeof(T));
#elif defined PORT_HIP
    hipMalloc((void**)&buffer, n * sizeof(T));
#elif defined PORT_ONEAPI
    buffer = sycl::malloc_device<T>(n, CommBench::q);
#else
    allocateHost(buffer, n);
#endif
#ifdef CAP_GASNET
    // save ep/segment pair for later publication
    gex_Segment_t segment; // lose it
    gex_EP_t ep; // save it
    gex_Segment_Create(&segment, myclient, buffer, n * sizeof(T), memkind, 0);
    gex_EP_Create(&ep, myclient, GEX_EP_CAPABILITY_RMA, 0);
    gex_EP_BindSegment(ep, segment, 0);
    myep.push_back(ep);
    myep_ptr.push_back(buffer);
    // gex_EP_PublishBoundSegment(myteam, &myep, 1, 0); // currently works only for symmetric allocation
#endif
    memory += n * sizeof(T);
  };

  template <typename T>
  void allocateHost(T *&buffer, size_t n) {
#ifdef PORT_CUDA
    cudaMallocHost((void**)&buffer, n * sizeof(T));
#elif defined PORT_HIP
    hipHostMalloc((void**)&buffer, n * sizeof(T));
#elif defined PORT_ONEAPI
    buffer = sycl::malloc_host<T>(n, CommBench::q);
#else
    buffer = new T[n];
#endif
  }

  template <typename T>
  void memcpyD2D(T *recvbuf, T *sendbuf, size_t n) {
#ifdef PORT_CUDA
    cudaMemcpy(recvbuf, sendbuf, n * sizeof(T), cudaMemcpyDeviceToDevice);
#elif defined PORT_HIP
    hipMemcpy(recvbuf, sendbuf, n * sizeof(T), hipMemcpyDeviceToDevice);
#elif defined PORT_ONEAPI
    CommBench::q.memcpy(recvbuf, sendbuf, n * sizeof(T)).wait();
#else
    memcpy(recvbuf, sendbuf, n * sizeof(T));
#endif
  }

  template <typename T>
  void memcpyH2D(T *device, T *host, size_t n) {
#ifdef PORT_CUDA
    cudaMemcpy(device, host, n * sizeof(T), cudaMemcpyHostToDevice);
#elif defined PORT_HIP  
    hipMemcpy(device, host, n * sizeof(T), hipMemcpyHostToDevice);
#elif defined PORT_ONEAPI
    CommBench::q.memcpy(device, host, n * sizeof(T)).wait();
#else
    memcpy(device, host, n * sizeof(T));
#endif
  }

  template <typename T>
  void memcpyD2H(T *host, T *device, size_t n) {
#ifdef PORT_CUDA
    cudaMemcpy(host, device, n * sizeof(T), cudaMemcpyDeviceToHost);
#elif defined PORT_HIP
    hipMemcpy(host, device, n * sizeof(T), hipMemcpyDeviceToHost);
#elif defined PORT_ONEAPI
    CommBench::q.memcpy(host, device, n * sizeof(T)).wait();
#else
    memcpy(host, device, n * sizeof(T));
#endif
  }

  template <typename T>
  void free(T *buffer) {
#ifdef PORT_CUDA
    cudaFree(buffer);
#elif defined PORT_HIP
    hipFree(buffer);
#elif defined PORT_ONEAPI
    sycl::free(buffer, CommBench::q);
#else
    freeHost(buffer);
#endif
  }

  template <typename T>
  void freeHost(T *buffer) {
#ifdef PORT_CUDA
    cudaFreeHost(buffer);
#elif defined PORT_HIP
    hipHostFree(buffer);
#elif defined PORT_ONEAPI
    sycl::free(buffer, CommBench::q);
#else
    delete[] buffer;
#endif
  }

} // namespace CommBench
#endif
