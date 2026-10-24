# SDL 3.4.16 with a Direct3D 12 fix

NeoTS2 needs SDL 3.4.16 with [d3d12-descriptor-heap.patch](d3d12-descriptor-heap.patch) applied. Builds made with
the stock SDL release, and the SDL3.dll shipped with them, are affected by this bug.

## Why

In SDL 3.4.14 to 3.4.16, the Direct3D 12 backend runs one descriptor past the end of its sampler heap when a frame
binds textures on more than about a thousand draws. A large lot does that. The driver then writes outside the heap
without any warning. With the D3D12 debug layer on, the game stops with exception 0x87D in
`ID3D12Device::CopyDescriptorsSimple`. This is SDL issue
[#16369](https://github.com/libsdl-org/SDL/issues/16369), which is still open; upstream plans a fix for SDL 3.6.0.

SDL starts a new heap only once the current one is completely full. It doesn't check whether the next batch of
descriptors fits. The patch:

- makes that check cover the whole batch;
- before each draw, starts new heaps if the draw's descriptors might not fit in the current ones, and writes all of
  that draw's bindings into the new heaps. Without this, a heap could change halfway through a draw.

Drop the patch once an SDL release includes a fix.

## Building

1. Download and unpack the official source release
   [SDL3-3.4.16.tar.gz](https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz).
2. From the unpacked `SDL3-3.4.16` folder:

   ```
   git apply <NeoTS2>/third_party/sdl/d3d12-descriptor-heap.patch
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX=<SDL install folder>
   cmake --build build --config Release
   cmake --install build --config Release
   ```

3. Configure NeoTS2 with `-DSDL3_DIR=<SDL install folder>/cmake`. The build copies that SDL3.dll next to neots2.exe.
