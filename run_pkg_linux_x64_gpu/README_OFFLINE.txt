OSGB to 3D Tiles 1.1 - Linux x64 GPU offline package

Requirements:
  - Linux x86-64 with glibc 2.38 or newer (Ubuntu 24.04 compatible)
  - NVIDIA GPU driver already installed
  - nvidia-smi works
  - libnvidia-opencl.so.1 is provided by the installed NVIDIA driver

Run:
  chmod +x run.sh osgb_converter_1_1
  ./run.sh -i /absolute/input -o /absolute/output \
    --enable-simplify --enable-draco --enable-top_reconstruct \
    --gpu-texture-compress --ktx2-quality 64 --threads 16 --split-json

GPU success log:
  [INFO] KTX2 encoder: requested=OpenCL GPU, active=OpenCL GPU

If the log says active=CPU, verify:
  nvidia-smi
  ldconfig -p | grep libnvidia-opencl
