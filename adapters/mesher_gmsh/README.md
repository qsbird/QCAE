# Gmsh planar mesher

`qcae_mesher_gmsh` implements `IPlanarMesher` (`modules/contracts/include/qcae/planar_mesher.hpp`)
with the Gmsh library, inside the calling process. It builds the region from straight-edged loops
in Gmsh's built-in geometry kernel, meshes it with three-node triangles and returns plain data:
node positions, triangles turned counter-clockwise about the region normal, and the boundary
segments along every input edge. Decisions and trade-offs are in
[ADR-10](../../docs/architecture/gmsh-mesher.md).

Gmsh keeps its models and options in process-wide state. The adapter therefore:

- runs every Gmsh call under one lock, so concurrent tasks mesh one at a time;
- starts Gmsh once, in its constructor, without user configuration files and without terminal
  output. Gmsh's start-up adds its executable's directory to `PATH` and `PYTHONPATH` and switches
  the process locale to `C.UTF-8`; the constructor restores all three. Gmsh 4.15.2 documents
  `-noenv` and `-nolocale` for this, but its option parser rejects both and exits the process.
  Construct the mesher during start-up, before other threads run;
- removes its Gmsh model on every exit path, because after a failure Gmsh keeps the failed model
  loaded and current;
- refuses a request whose estimated triangle count exceeds the caller's limit before meshing, since
  a running mesh cannot be interrupted, and checks the actual count afterwards;
- checks that the triangles cover exactly the region area, which catches crossing loops and holes
  outside the outer loop.

The adapter is optional and off by default. Core builds and tests do not need Gmsh.

## Building Gmsh

The tested library is Gmsh 4.15.2 (`gmsh_4_15_2`, commit `657c8e9`), built from source with every
optional component off except the mesh module:

```sh
git clone --depth 1 --branch gmsh_4_15_2 https://gitlab.onelab.info/gmsh/gmsh.git gmsh-src
cmake -S gmsh-src -B gmsh-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/opt/gmsh/4.15.2 -DDEFAULT=0 -DENABLE_MESH=1 -DENABLE_EIGEN=1 \
  -DENABLE_OPENMP=0 -DENABLE_GMP=0 -DENABLE_TINYOBJLOADER=1 \
  -DENABLE_BUILD_SHARED=1 -DENABLE_BUILD_DYNAMIC=1
cmake --build gmsh-build && cmake --install gmsh-build
```

The resulting library links only the C and C++ runtime. TinyObjLoader stays on because
`src/geo/GModelIO_OBJ.cpp` includes it unconditionally in 4.15.2. Without OpenMP, meshing is
single-threaded and the same input gives the same mesh.

## Building QCAE with it

```sh
cmake -S . -B build -DQCAE_WITH_GMSH=ON -DQCAE_GMSH_ROOT=/opt/gmsh/4.15.2
cmake --build build
ctest --test-dir build -R gmsh_planar_mesher
```

`QCAE_GMSH_ROOT` is local machine configuration; CMake only searches that prefix. Gmsh is
GPL-2.0-or-later with a linking exception: a distributed binary that links it is a combined work
under the GPL, while the QCAE sources stay under the WTFPL.
