#!/bin/bash -l

date

export OPAL_PREFIX=/opt/amazon/openmpi

ARG_TCP="--mca pml ^cm --mca btl tcp,self --mca btl_tcp_if_exclude lo,docker0"
ARG_OFI="-x LD_LIBRARY_PATH=/opt/aws-ofi-nccl/lib:$LD_LIBRARY_PATH"
# ARG_MPI="--display-map --display-allocation"
# rm debugfiles/*
# ARG_NCCL_DEBUG="-x NCCL_DEBUG=TRACE -x NCCL_DEBUG_FILE=filename.%h.%p -x NCCL_DEBUG_SUBSYS=ALL"
# ARG_NCCL="-x NCCL_NVLS_ENABLE=1"  #"-x NCCL_LOCAL_REGISTER=1"

# count = 2^i, i_min <= i <= i_max
i_min=0
i_max=30
mpirun $ARG_TCP $ARG_OFI $ARG_MPI $ARG_NCCL $ARG_NCCL_DEBUG -np 16 -hostfile hostfile.txt ./CommBench $i_min $i_max

#export NCCL_DEBUG=INFO
#NCCL_CROSS_NIC=1
#NCCL_BUFFSIZE=8388608
#NCCL_P2P_NET_CHUNKSIZE=524288
#NCCL_CUMEM_ENABLE=0
#NCCL_SOCKET_IFNAME=eth0

date
