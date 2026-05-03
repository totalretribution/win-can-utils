rmdir /s /q build
mkdir build
cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_C_COMPILER=gcc -DWINCAN_BUILD_TOOLS=ON -DWINCAN_BUILD_EXAMPLES=ON
mingw32-make
