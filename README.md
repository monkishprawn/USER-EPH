# USER-EPH
LAMMPS extension to capture electron-ion interaction

Artur Tamm and Alfredo A. Correa (LLNL)

Installation Instructions

Get LAMMPS (source code)

`git clone https://github.com/lammps/lammps.git lammps
Get USER-EPH (this plugin)

```
git clone https://github.com/artuuuro/USER-EPH.git lammps/src/USER-EPH`
cd lammps/src
```

Edit `Makefile` add string ` user-eph ` to the end of `PACKUSER` variable, near line 66.
Edit `MAKE/Makefile.mpi` and `MAKE/Makefile.serial` to read `CCFLAGS = -g -O3 -std=c++11` near line 10.

Execute
```
make yes-manybody
make yes-user-eph
make serial
make mpi
```

