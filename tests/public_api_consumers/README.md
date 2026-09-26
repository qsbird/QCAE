# Public API consumers

This standalone CMake project builds one executable per active public header in
`modules/targets.json`, plus the generated operation catalog. Each translation unit includes exactly that header and
links its declared target. It adds no product include directories; missing public
usage requirements and private-header dependencies therefore fail compilation.
The count is written to `public-consumer-count.txt` in the consumer build tree.

From the repository root:

```sh
cmake -S tests/public_api_consumers -B /tmp/qcae-api-core -G Ninja \
  -DQCAE_BUILD_IPC=OFF -DQCAE_BUILD_STORAGE=OFF -DQCAE_BUILD_DESKTOP=OFF
cmake --build /tmp/qcae-api-core --target qcae_public_consumers
cmake -S tests/public_api_consumers -B /tmp/qcae-api-desktop -G Ninja \
  -DQCAE_BUILD_IPC=ON -DQCAE_BUILD_STORAGE=ON -DQCAE_BUILD_DESKTOP=ON \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt -DVTK_DIR=/path/to/vtk/cmake
cmake --build /tmp/qcae-api-desktop --target qcae_public_consumers
```

The desktop configuration needs an existing Qt/VTK installation but does not open
windows or render graphics. This checks public header consumers, not AP/BP
functional scenario coverage or full skeleton acceptance.
