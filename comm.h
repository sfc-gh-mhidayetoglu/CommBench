  template <typename T>
  Comm<T>::Comm(library lib) : lib(lib) {

    benchid = numbench;
    numbench++;

    int init_mpi;
    MPI_Initialized(&init_mpi);

    if(!init_mpi_comm) {
      if(!init_mpi) {
        MPI_Init(NULL, NULL);
      }
      MPI_Comm_dup(MPI_COMM_WORLD, &comm_mpi); // CREATE SEPARATE COMMUNICATOR EXPLICITLY
      MPI_Comm_rank(comm_mpi, &myid);
      MPI_Comm_size(comm_mpi, &numproc);
      setup_gpu();
      init_mpi_comm = true;
      if(myid == printid) {
        if(!init_mpi)
          printf("#################### MPI IS INITIALIZED, it is user's responsibility to finalize.\n");
        printf("******************** MPI COMMUNICATOR IS CREATED\n");
      }
    }

    numsend = 0;
    numrecv = 0;

    if(myid == printid) {
      printf("printid: %d Create Bench %d with %d processors\n", printid, benchid, numproc);
      printf("  Port: ");
#ifdef PORT_CUDA
      printf("CUDA ");
#elif defined PORT_HIP
      printf("HIP, ");
#elif defined PORT_SYCL
      printf("SYCL, ");
#else
      printf("CPU, ");
#endif
      printf("Library: ");
      print_lib(lib);
      printf("\n");
    }
    if(lib == XCCL) {
#ifdef CAP_NCCL
      if(!init_nccl_comm) {
        ncclUniqueId id;
        if(myid == 0)
          ncclGetUniqueId(&id);
        MPI_Bcast(&id, sizeof(id), MPI_BYTE, 0, comm_mpi);
        ncclCommInitRank(&comm_nccl, numproc, id, myid);
        if(myid == printid)
          printf("******************** NCCL COMMUNICATOR IS CREATED\n");
        init_nccl_comm = true;
      }
#ifdef PORT_CUDA
      cudaStreamCreate(&stream_nccl);
#elif defined PORT_HIP
      hipStreamCreate(&stream_nccl);
#endif
#elif defined CAP_ONECCL
      if(!init_ccl_comm) {
        /* initialize ccl */
        ccl::init();
        /* create kvs */
        ccl::shared_ptr_class<ccl::kvs> kvs;
        ccl::kvs::address_type main_addr;
        if (myid == 0) {
          kvs = ccl::create_main_kvs();
          main_addr = kvs->get_address();
          MPI_Bcast((void *)main_addr.data(), main_addr.size(), MPI_BYTE, 0, comm_mpi);
        }
        else {
          MPI_Bcast((void *)main_addr.data(), main_addr.size(), MPI_BYTE, 0, comm_mpi);
          kvs = ccl::create_kvs(main_addr);
        }
        /* create communicator */
        auto dev = ccl::create_device(CommBench::q.get_device());
        auto ctx = ccl::create_context(CommBench::q.get_context());
        comm_ccl = new ccl::communicator(ccl::create_communicator(numproc, myid, dev, ctx, kvs));
        init_ccl_comm = true;
        if(myid == printid)
          printf("******************** ONECCL COMMUNICATOR IS CREATED\n");
        stream_ccl = new ccl::stream(ccl::create_stream(CommBench::q));
      }
#endif
    }
  }

  template <typename T>
  void Comm<T>::free() {
    for(T *ptr : buffer_list)
      CommBench::free(ptr);
    buffer_list.clear();
    buffer_count.clear();
    if(myid == printid)
      printf("memory freed.\n");
  }

  template <typename T>
  void Comm<T>::allocate(T *&buffer, size_t count) {
    for (int i = 0; i < numproc; i++)
      allocate(buffer, count, i);
  }
  template <typename T>
  void Comm<T>::allocate(T *&buffer, size_t count, int i) {
    if(myid == i) {
      MPI_Send(&count, sizeof(size_t), MPI_BYTE, printid, 0, comm_mpi);
      if(count) {
        CommBench::allocate(buffer, count);
        buffer_list.push_back(buffer);
        buffer_count.push_back(count);
        MPI_Send(&buffer, sizeof(T*), MPI_BYTE, printid, 0, comm_mpi);
      }
    }
    if(myid == printid) {
      MPI_Recv(&count, sizeof(size_t), MPI_BYTE, i, 0, comm_mpi, MPI_STATUS_IGNORE);
      if(count) {
        T *ptr = nullptr;
        MPI_Recv(&ptr, sizeof(T*), MPI_BYTE, i, 0, comm_mpi, MPI_STATUS_IGNORE);
        printf("Bench %d proc %d allocate %p count %ld (", benchid, i, ptr, count);
        print_data(count * sizeof(T));
        printf(")\n");
      }
    }
  }

  template <typename T>
  void Comm<T>::add_lazy(size_t count, int sendid, int recvid) {
    T *sendbuf;
    T *recvbuf;
    allocate(sendbuf, count, sendid);
    allocate(recvbuf, count, recvid);
    add(sendbuf, 0, recvbuf, 0, count, sendid, recvid);
  }
  template <typename T>
  void Comm<T>::add(T *sendbuf, T *recvbuf, size_t count, int sendid, int recvid) {
    add(sendbuf, 0, recvbuf, 0, count, sendid, recvid);
  }
  template <typename T>
  void Comm<T>::add(T *sendbuf, size_t sendoffset, T *recvbuf, size_t recvoffset, size_t count, int sendid, int recvid) {
    if(count == 0) {
      if(myid == printid)
        printf("Bench %d communication (%d->%d) count = 0 (skipped)\n", benchid, sendid, recvid);
      return;
    }
    MPI_Barrier(comm_mpi); // THIS IS NECESSARY IN SOME MPI VERSIONS

    // REPORT
    if(printid > -1) {
      if(myid == sendid) {
        MPI_Send(&sendbuf, sizeof(T*), MPI_BYTE, printid, 0, comm_mpi);
        MPI_Send(&sendoffset, sizeof(size_t), MPI_BYTE, printid, 0, comm_mpi);
      }
      if(myid == recvid) {
        MPI_Send(&recvbuf, sizeof(T*), MPI_BYTE, printid, 0, comm_mpi);
        MPI_Send(&recvoffset, sizeof(size_t), MPI_BYTE, printid, 0, comm_mpi);
      }
      if(myid == printid) {
        T* sendbuf_sendid;
        T* recvbuf_recvid;
        size_t sendoffset_sendid;
        size_t recvoffset_recvid;
        MPI_Recv(&sendbuf_sendid, sizeof(T*), MPI_BYTE, sendid, 0, comm_mpi, MPI_STATUS_IGNORE);
        MPI_Recv(&sendoffset_sendid, sizeof(size_t), MPI_BYTE, sendid, 0, comm_mpi, MPI_STATUS_IGNORE);
        MPI_Recv(&recvbuf_recvid, sizeof(T*), MPI_BYTE, recvid, 0, comm_mpi, MPI_STATUS_IGNORE);
        MPI_Recv(&recvoffset_recvid, sizeof(size_t), MPI_BYTE, recvid, 0, comm_mpi, MPI_STATUS_IGNORE);
        printf("Bench %d comm %d (%d->%d) sendbuf %p sendoffset %zu recvbuf %p recvoffset %zu count %zu (", benchid, numcomm, sendid, recvid, sendbuf_sendid, sendoffset_sendid, recvbuf_recvid, recvoffset_recvid, count);
        print_data(count * sizeof(T));
        printf(") ");
        print_lib(lib);
        printf("\n");
      }
    }
    numcomm++;

    // SENDER DATA STRUCTURES
    if(myid == sendid) {

      // EXTEND REGISTRY
      this->sendbuf.push_back(sendbuf);
      this->sendproc.push_back(recvid);
      this->sendcount.push_back(count);
      this->sendoffset.push_back(sendoffset);

      // SETUP CAPABILITY
      switch(lib) {
        case dummy:
          break;
        case MPI:
          sendrequest.push_back(MPI_Request());
          break;
        case XCCL:
          break;
        case IPC:
          ack_sender.push_back(int());
          remotebuf.push_back(recvbuf);
          remoteoffset.push_back(recvoffset);
          // CREATE STREAMS
#ifdef PORT_CUDA
          stream_ipc.push_back(cudaStream_t());
          cudaStreamCreate(&stream_ipc[numsend]);
#elif defined PORT_HIP
          stream_ipc.push_back(hipStream_t());
          hipStreamCreate(&stream_ipc[numsend]);
#elif defined PORT_SYCL
          q_ipc.push_back(sycl::queue(sycl::gpu_selector_v));
#endif
          // RECIEVE REMOTE MEMORY HANDLE
          if(sendid != recvid) {
            int error = -1;
#ifdef PORT_CUDA
            cudaIpcMemHandle_t memhandle;
            MPI_Recv(&memhandle, sizeof(cudaIpcMemHandle_t), MPI_BYTE, recvid, 0, comm_mpi, MPI_STATUS_IGNORE);
            error = cudaIpcOpenMemHandle((void**)&remotebuf[numsend], memhandle, cudaIpcMemLazyEnablePeerAccess);
#elif defined PORT_HIP
            hipIpcMemHandle_t memhandle;
            MPI_Recv(&memhandle, sizeof(hipIpcMemHandle_t), MPI_BYTE, recvid, 0, comm_mpi, MPI_STATUS_IGNORE);
            error = hipIpcOpenMemHandle((void**)&remotebuf[numsend], memhandle, hipIpcMemLazyEnablePeerAccess);
#elif defined PORT_SYCL
            ze_ipc_mem_handle_t memhandle;
            {
              typedef struct { int fd; pid_t pid ; } clone_mem_t;
              clone_mem_t what_intel_should_have_done;
              MPI_Recv(&what_intel_should_have_done, sizeof(clone_mem_t), MPI_BYTE, recvid, 0, comm_mpi, MPI_STATUS_IGNORE);
              int pidfd = syscall(SYS_pidfd_open,what_intel_should_have_done.pid,0);
              //      int myfd  = syscall(SYS_pidfd_getfd,pidfd,what_intel_should_have_done.fd,0);
              int myfd  = syscall(438,pidfd,what_intel_should_have_done.fd,0);
	      memcpy((void *)&memhandle,(void *)&myfd,sizeof(int));
            }
            // MPI_Recv(&memhandle, sizeof(ze_ipc_mem_handle_t), MPI_BYTE, recvid, 0, comm_mpi, MPI_STATUS_IGNORE);
	    auto zeContext = sycl::get_native<sycl::backend::ext_oneapi_level_zero>(q_ipc[numsend].get_context());
	    auto zeDevice = sycl::get_native<sycl::backend::ext_oneapi_level_zero>(q_ipc[numsend].get_device());
	    error = zeMemOpenIpcHandle(zeContext, zeDevice, memhandle, 0, (void**)&remotebuf[numsend]);
#endif
            if(error)
              printf("IpcOpenMemHandle error %d\n", error);
            MPI_Recv(&remoteoffset[numsend], sizeof(size_t), MPI_BYTE, recvid, 0, comm_mpi, MPI_STATUS_IGNORE);
          }
          break;
        case IPC_get:
          ack_sender.push_back(int());
          // SEND REMOTE MEMORY HANDLE
          if(sendid != recvid)
          {
            int error = -1;
#ifdef PORT_CUDA
            cudaIpcMemHandle_t memhandle;
            error = cudaIpcGetMemHandle(&memhandle, sendbuf);
            MPI_Send(&memhandle, sizeof(cudaIpcMemHandle_t), MPI_BYTE, recvid, 0, comm_mpi);
#elif defined PORT_HIP
            hipIpcMemHandle_t memhandle;
            error = hipIpcGetMemHandle(&memhandle, sendbuf);
            MPI_Send(&memhandle, sizeof(hipIpcMemHandle_t), MPI_BYTE, recvid, 0, comm_mpi);
#elif defined PORT_SYCL
            ze_ipc_mem_handle_t memhandle;
            auto zeContext = sycl::get_native<sycl::backend::ext_oneapi_level_zero>(q.get_context());
            error = zeMemGetIpcHandle(zeContext, sendbuf, &memhandle);
            {
              typedef struct { int fd; pid_t pid ; } clone_mem_t;
              clone_mem_t what_intel_should_have_done;
              memcpy((void *)&what_intel_should_have_done.fd,(void *)&memhandle,sizeof(int));
              what_intel_should_have_done.pid = getpid();
              MPI_Send(&what_intel_should_have_done, sizeof(clone_mem_t), MPI_BYTE, recvid, 0, comm_mpi);
            }
            // MPI_Send(&memhandle, sizeof(ze_ipc_mem_handle_t), MPI_BYTE, sendid, 0, comm_mpi);
#endif
            if(error)
              printf("IpcGetMemHandle error %d\n", error);
            MPI_Send(&sendoffset, sizeof(size_t), MPI_BYTE, recvid, 0, comm_mpi);
          }
          break;
        case numlib:
          break;
      } // switch(lib)
      numsend++;
    }

    // RECEIVER DATA STRUCTURES
    if(myid == recvid) {

      // EXTEND REGISTRY
      this->recvbuf.push_back(recvbuf);
      this->recvproc.push_back(sendid);
      this->recvcount.push_back(count);
      this->recvoffset.push_back(recvoffset);

      // SETUP LIBRARY
      switch(lib) {
        case dummy:
          break;
        case MPI:
          recvrequest.push_back(MPI_Request());
          break;
        case XCCL:
          break;
        case IPC:
          ack_recver.push_back(int());
          // SEND REMOTE MEMORY HANDLE
          if(sendid != recvid)
          {
            int error = -1;
#ifdef PORT_CUDA
            cudaIpcMemHandle_t memhandle;
            error = cudaIpcGetMemHandle(&memhandle, recvbuf);
            MPI_Send(&memhandle, sizeof(cudaIpcMemHandle_t), MPI_BYTE, sendid, 0, comm_mpi);
#elif defined PORT_HIP
            hipIpcMemHandle_t memhandle;
            error = hipIpcGetMemHandle(&memhandle, recvbuf);
            MPI_Send(&memhandle, sizeof(hipIpcMemHandle_t), MPI_BYTE, sendid, 0, comm_mpi);
#elif defined PORT_SYCL
            ze_ipc_mem_handle_t memhandle;
	    auto zeContext = sycl::get_native<sycl::backend::ext_oneapi_level_zero>(q.get_context());
	    error = zeMemGetIpcHandle(zeContext, recvbuf, &memhandle);
            {
              typedef struct { int fd; pid_t pid ; } clone_mem_t;
              clone_mem_t what_intel_should_have_done;
	      memcpy((void *)&what_intel_should_have_done.fd,(void *)&memhandle,sizeof(int));
	      what_intel_should_have_done.pid = getpid();
              MPI_Send(&what_intel_should_have_done, sizeof(clone_mem_t), MPI_BYTE, sendid, 0, comm_mpi);
            }
            // MPI_Send(&memhandle, sizeof(ze_ipc_mem_handle_t), MPI_BYTE, sendid, 0, comm_mpi);
#endif
            if(error)
              printf("IpcGetMemHandle error %d\n", error);
            MPI_Send(&recvoffset, sizeof(size_t), MPI_BYTE, sendid, 0, comm_mpi);
          }
          break;
        case IPC_get:
          ack_recver.push_back(int());
          remotebuf.push_back(sendbuf);
          remoteoffset.push_back(sendoffset);
          // CREATE STREAMS
#ifdef PORT_CUDA
          stream_ipc.push_back(cudaStream_t());
          cudaStreamCreate(&stream_ipc[numrecv]);
#elif defined PORT_HIP
          stream_ipc.push_back(hipStream_t());
          hipStreamCreate(&stream_ipc[numrecv]);
#elif defined PORT_SYCL
          q_ipc.push_back(sycl::queue(sycl::gpu_selector_v));
#endif
          // RECV REMOTE MEMORY HANDLE
          if(sendid != recvid) {
            int error = -1;
#ifdef PORT_CUDA
            cudaIpcMemHandle_t memhandle;
            MPI_Recv(&memhandle, sizeof(cudaIpcMemHandle_t), MPI_BYTE, sendid, 0, comm_mpi, MPI_STATUS_IGNORE);
            error = cudaIpcOpenMemHandle((void**)&remotebuf[numrecv], memhandle, cudaIpcMemLazyEnablePeerAccess);
#elif defined PORT_HIP
            hipIpcMemHandle_t memhandle;
            MPI_Recv(&memhandle, sizeof(hipIpcMemHandle_t), MPI_BYTE, sendid, 0, comm_mpi, MPI_STATUS_IGNORE);
            error = hipIpcOpenMemHandle((void**)&remotebuf[numrecv], memhandle, hipIpcMemLazyEnablePeerAccess);
#elif defined PORT_SYCL
            ze_ipc_mem_handle_t memhandle;
            {
              typedef struct { int fd; pid_t pid ; } clone_mem_t;
              clone_mem_t what_intel_should_have_done;
              MPI_Recv(&what_intel_should_have_done, sizeof(clone_mem_t), MPI_BYTE, sendid, 0, comm_mpi, MPI_STATUS_IGNORE);
              int pidfd = syscall(SYS_pidfd_open,what_intel_should_have_done.pid,0);
              //      int myfd  = syscall(SYS_pidfd_getfd,pidfd,what_intel_should_have_done.fd,0);
              int myfd  = syscall(438,pidfd,what_intel_should_have_done.fd,0);
              memcpy((void *)&memhandle,(void *)&myfd,sizeof(int));
            }
            // MPI_Recv(&memhandle, sizeof(ze_ipc_mem_handle_t), MPI_BYTE, sendid, 0, comm_mpi, MPI_STATUS_IGNORE);
            auto zeContext = sycl::get_native<sycl::backend::ext_oneapi_level_zero>(q_ipc[numrecv].get_context());
            auto zeDevice = sycl::get_native<sycl::backend::ext_oneapi_level_zero>(q_ipc[numrecv].get_device());
            error = zeMemOpenIpcHandle(zeContext, zeDevice, memhandle, 0, (void**)&remotebuf[numrecv]);
#endif
            if(error)
              printf("IpcOpenMemHandle error %d\n", error);
            MPI_Recv(&remoteoffset[numrecv], sizeof(size_t), MPI_BYTE, sendid, 0, comm_mpi, MPI_STATUS_IGNORE);
          }
          break;
        case numlib:
          break;
      } // switch(lib)
      numrecv++;
    }
  }

  template <typename T>
  void Comm<T>::measure(int warmup, int numiter) {
    long count_total = 0;
    for(int send = 0; send < numsend; send++)
       count_total += sendcount[send];
    MPI_Allreduce(MPI_IN_PLACE, &count_total, 1, MPI_LONG, MPI_SUM, comm_mpi);
    measure(warmup, numiter, count_total);
  }
  template <typename T>
  void Comm<T>::measure(int warmup, int numiter, size_t count) {
    this->report();
    double minTime;
    double medTime;
    double maxTime;
    double avgTime;
    CommBench::measure(warmup, numiter, minTime, medTime, maxTime, avgTime, *this);
    if(myid == printid) {
      size_t data = count * sizeof(T);
      printf("data: "); print_data(data); printf("\n");
      printf("minTime: %.4e us, %.4e ms/GB, %.4e GB/s\n", minTime * 1e6, minTime / data * 1e12, data / minTime / 1e9);
      printf("medTime: %.4e us, %.4e ms/GB, %.4e GB/s\n", medTime * 1e6, medTime / data * 1e12, data / medTime / 1e9);
      printf("maxTime: %.4e us, %.4e ms/GB, %.4e GB/s\n", maxTime * 1e6, maxTime / data * 1e12, data / maxTime / 1e9);
      printf("avgTime: %.4e us, %.4e ms/GB, %.4e GB/s\n", avgTime * 1e6, avgTime / data * 1e12, data / avgTime / 1e9);
      printf("\n");
    }
  };

  template <typename T>
  void Comm<T>::report() {

    std::vector<size_t> matrix;
    getMatrix(matrix);

    if(myid == printid) {
      printf("\nCommBench %d: ", benchid);
      print_lib(lib);
      printf(" communication matrix (reciever x sender): %d\n", numcomm);
      for(int recver = 0; recver < numproc; recver++) {
        for(int sender = 0; sender < numproc; sender++) {
          size_t count = matrix[sender * numproc + recver];
          if(count)
            // printf("%ld ", count);
            printf("1 ");
          else
            printf(". ");
        }
        printf("\n");
      }
    }
    long sendTotal = 0;
    long recvTotal = 0;
    for(int send = 0; send < numsend; send++)
       sendTotal += sendcount[send];
    for(int recv = 0; recv < numrecv; recv++)
       recvTotal += recvcount[recv];

    MPI_Allreduce(MPI_IN_PLACE, &sendTotal, 1, MPI_LONG, MPI_SUM, comm_mpi);
    MPI_Allreduce(MPI_IN_PLACE, &recvTotal, 1, MPI_LONG, MPI_SUM, comm_mpi);

    int total_buff = buffer_list.size();
    std::vector<int> total_buffs(numproc);
    MPI_Allgather(&total_buff, 1, MPI_INT, total_buffs.data(), 1, MPI_INT, MPI_COMM_WORLD);
    size_t total_count = 0;
    for(size_t count : buffer_count)
      total_count += count;
    std::vector<size_t> total_counts(numproc);
    MPI_Allgather(&total_count, sizeof(size_t), MPI_BYTE, total_counts.data(), sizeof(size_t), MPI_BYTE, MPI_COMM_WORLD);
    if(myid == printid) {
      for(int p = 0; p < numproc; p++) {
        printf("proc %d: %d pieces count %ld ", p, total_buffs[p], total_counts[p]);
        print_data(total_counts[p] * sizeof(T));
        printf("\n");
      }
    }

    if(myid == printid) {
      printf("send footprint: %ld ", sendTotal);
      print_data(sendTotal * sizeof(T));
      printf("\n");
      printf("recv footprint: %ld ", recvTotal);
      print_data(recvTotal * sizeof(T));
      printf("\n");
      printf("\n");
    }
  }
  template <typename T>
  void Comm<T>::getMatrix(std::vector<size_t> &matrix) {

    std::vector<size_t> sendcount_temp(numproc, 0);
    std::vector<size_t> recvcount_temp(numproc, 0);
    for (int send = 0; send < numsend; send++)
      sendcount_temp[sendproc[send]] += sendcount[send];
    for (int recv = 0; recv < numrecv; recv++)
      recvcount_temp[recvproc[recv]] += recvcount[recv];
    std::vector<size_t> sendmatrix(numproc * numproc);
    std::vector<size_t> recvmatrix(numproc * numproc);
    MPI_Allgather(sendcount_temp.data(), numproc * sizeof(size_t), MPI_BYTE, sendmatrix.data(), numproc * sizeof(size_t), MPI_BYTE, comm_mpi);
    MPI_Allgather(recvcount_temp.data(), numproc * sizeof(size_t), MPI_BYTE, recvmatrix.data(), numproc * sizeof(size_t), MPI_BYTE, comm_mpi);

    for (int sender = 0; sender < numproc; sender++)
      for (int recver = 0; recver < numproc; recver++)
        matrix.push_back(sendmatrix[sender * numproc + recver]);

    /* if(myid == printid) {
      char filename[2048];
      sprintf(filename, "matrix_%d.txt", benchid);
      FILE *matfile = fopen(filename, "w");
      for(int recver = 0; recver < numproc; recver++) {
        for(int sender = 0; sender < numproc; sender++)
          fprintf(matfile, "%ld ", matrix[sender * numproc + recver]);
        fprintf(matfile, "\n");
      }
      fclose(matfile);
    }*/
  }

  template <typename T>
  void Comm<T>::start() {
    switch(lib) {
      case MPI:
        for (int send = 0; send < numsend; send++)
          MPI_Isend(sendbuf[send] + sendoffset[send], sendcount[send] * sizeof(T), MPI_BYTE, sendproc[send], 0, comm_mpi, &sendrequest[send]);
        for (int recv = 0; recv < numrecv; recv++)
          MPI_Irecv(recvbuf[recv] + recvoffset[recv], recvcount[recv] * sizeof(T), MPI_BYTE, recvproc[recv], 0, comm_mpi, &recvrequest[recv]);
        break;
      case XCCL:
#ifdef CAP_NCCL
        ncclGroupStart();
        for(int send = 0; send < numsend; send++)
          ncclSend(sendbuf[send] + sendoffset[send], sendcount[send] * sizeof(T), ncclInt8, sendproc[send], comm_nccl, stream_nccl);
        for(int recv = 0; recv < numrecv; recv++)
          ncclRecv(recvbuf[recv] + recvoffset[recv], recvcount[recv] * sizeof(T), ncclInt8, recvproc[recv], comm_nccl, stream_nccl);
        ncclGroupEnd();
#elif defined CAP_ONECCL
        for (int i = 0; i < numsend; i++)
          ccl::send<T>(sendbuf[i] + sendoffset[i], sendcount[i], sendproc[i], *comm_ccl, *stream_ccl);
        for (int i = 0; i < numrecv; i++)
          ccl::recv<T>(recvbuf[i] + recvoffset[i], recvcount[i], recvproc[i], *comm_ccl, *stream_ccl);
#endif
        break;
      case IPC:
        for(int send = 0; send < numsend; send++) {
#ifdef PORT_CUDA
          cudaMemcpyAsync(remotebuf[send] + remoteoffset[send], sendbuf[send] + sendoffset[send], sendcount[send] * sizeof(T), cudaMemcpyDeviceToDevice, stream_ipc[send]);
#elif defined PORT_HIP
          hipMemcpyAsync(remotebuf[send] + remoteoffset[send], sendbuf[send] + sendoffset[send], sendcount[send] * sizeof(T), hipMemcpyDeviceToDevice, stream_ipc[send]);
#elif defined PORT_SYCL
          q_ipc[send].memcpy(remotebuf[send] + remoteoffset[send], sendbuf[send] + sendoffset[send], sendcount[send] * sizeof(T));
#endif
        }
        break;
      case IPC_get:
        for(int send = 0; send < numsend; send++)
          MPI_Send(&ack_sender[send], 1, MPI_INT, sendproc[send], 0, comm_mpi);
        for(int recv = 0; recv < numrecv; recv++) {
          MPI_Recv(&ack_recver[recv], 1, MPI_INT, recvproc[recv], 0, comm_mpi, MPI_STATUS_IGNORE);
#ifdef PORT_CUDA
          cudaMemcpyAsync(recvbuf[recv] + recvoffset[recv], remotebuf[recv] + remoteoffset[recv], recvcount[recv] * sizeof(T), cudaMemcpyDeviceToDevice, stream_ipc[recv]);
#elif defined PORT_HIP
          hipMemcpyAsync(recvbuf[recv] + recvoffset[recv], remotebuf[recv] + remoteoffset[recv], recvcount[recv] * sizeof(T), hipMemcpyDeviceToDevice, stream_ipc[recv]);
#elif defined PORT_SYCL
          q_ipc[recv].memcpy(recvbuf[recv] + recvoffset[recv], remotebuf[recv] + remoteoffset[recv], recvcount[recv] * sizeof(T));
#endif
        }
        break;
      default:
        break;
    }
  }

  template <typename T>
  void Comm<T>::wait() {
    switch(lib) {
      case MPI:
        MPI_Waitall(numsend, sendrequest.data(), MPI_STATUSES_IGNORE);
        MPI_Waitall(numrecv, recvrequest.data(), MPI_STATUSES_IGNORE);
        break;
      case XCCL:
#if defined CAP_NCCL && defined PORT_CUDA
        cudaStreamSynchronize(stream_nccl);
#elif defined CAP_NCCL && defined PORT_HIP
        hipStreamSynchronize(stream_nccl);
#elif defined CAP_ONECCL
        q.wait();
#endif
        break;
      case IPC:
        for(int send = 0; send < numsend; send++) {
#ifdef PORT_CUDA
          cudaStreamSynchronize(stream_ipc[send]);
#elif defined PORT_HIP
          hipStreamSynchronize(stream_ipc[send]);
#elif defined PORT_SYCL
	  q_ipc[send].wait();
#endif
          MPI_Send(&ack_sender[send], 1, MPI_INT, sendproc[send], 0, comm_mpi);
        }
        for(int recv = 0; recv < numrecv; recv++)
          MPI_Recv(&ack_recver[recv], 1, MPI_INT, recvproc[recv], 0, comm_mpi, MPI_STATUS_IGNORE);
        break;
      case IPC_get:
        for(int recv = 0; recv < numrecv; recv++) {
#ifdef PORT_CUDA
          cudaStreamSynchronize(stream_ipc[recv]);
#elif defined PORT_HIP
          hipStreamSynchronize(stream_ipc[recv]);
#elif defined PORT_SYCL
          q_ipc[recv].wait();
#endif
        }
        break;
      default:
        break;
    }
  }
