rmdir /s /q build
mkdir build
cd build
cmake .. -G "MinGW Makefiles" -DWINCAN_BUILD_TOOLS=ON -DWINCAN_BUILD_EXAMPLES=ON
mingw32-make