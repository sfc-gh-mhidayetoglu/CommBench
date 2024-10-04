#!/bin/bash -l

date

export OPAL_PREFIX=/opt/amazon/openmpi


ARG_TCP="--mca pml ^cm --mca btl tcp,self --mca btl_tcp_if_exclude lo,docker0"
ARG_OFI="-x LD_LIBRARY_PATH=/opt/aws-ofi-nccl/lib:$LD_LIBRARY_PATH"
# ARG_MPI="--display-map --display-allocation"
# rm debugfiles/*
# ARG_NCCL_DEBUG="-x NCCL_DEBUG=INFO -x NCCL_DEBUG_FILE=debugfiles/filename.%h.%p -x NCCL_DEBUG_SUBSYS=ALL"
# ARG_NCCL="-x NCCL_CROSS_NIC=0"

#export NCCL_DEBUG=INFO
#NCCL_CROSS_NIC=1
#NCCL_BUFFSIZE=8388608
#NCCL_P2P_NET_CHUNKSIZE=524288
#NCCL_CUMEM_ENABLE=0
#NCCL_SOCKET_IFNAME=eth0

for library in 2
do
for direction in 0 3
do
for pattern in 1
do
for n in 2
do
for g in 8
do
for k in 1
do
#for count in 1 2 4 8 16 32 64 128 256 512 1024 2048 4096 8192 16384 32768 65536 131072 262144 524288 1048576 2097152 4194304 8388608 16777216 33554432 67108864 134217728 268435456
for i in $(seq 30 30);
do
  count=$((2**i))
  warmup=5
  numiter=20
  window=1
  mpirun $ARG_TCP $ARG_OFI $ARG_MPI $ARG_NCCL_DEBUG $ARG_NCCL -np 16 -hostfile hostfile.txt ./CommBench $library $pattern $direction $count $warmup $numiter $window $n $g $k
done
done
done
done
done
done
done

exit


for n in $(seq 27 27);
#for n in $(seq 0 30);
do
  i=$((2**n))
  mpirun $ARG_TCP $ARG_OFI --display-map --display-allocation -np 32 -hostfile hostfile.txt ./CommBench $i
done

exit





date
