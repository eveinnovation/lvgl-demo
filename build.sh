LD_LIBRARY_PATH=/home/ovidiu/c++/ffmpeg/lib/:/home/ovidiu/c++/ffmpeg/lib64/:/usr/local/lib
rm -rf build
mkdir build && cd build
cmake .. -DCMAKE_CXX_COMPILER=g++ -DCMAKE_CC_COMPILER=gcc
cmake --build .