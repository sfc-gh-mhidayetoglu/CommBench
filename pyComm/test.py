import pyComm
#from mpi4py import MPI
import numpy as np


#MPI.Init()
#mpi_rank = MPI.COMM_WORLD.Get_rank()
#mpi_size = MPI.COMM_WORLD.Get_size()
#print(mpi_rank, mpi_size)
#if(MPI.Is_initialized()):
#    mpi_rank = MPI.COMM_WORLD.Get_rank()
#    mpi_size = MPI.COMM_WORLD.Get_size()
#    print(mpi_rank, mpi_size)
pyComm.Comm.mpi_init()
#c = Comm(pyComm.library.MPI)
pyComm.Comm.mpi_fin()

#c.add(a, 0, b, 0, a.size, 0, 1)
#c.start()
#c.wait()
#MPI.Finalize()
