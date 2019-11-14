# GoldenDoge node with block explorer
## About

Welcome to the repository of GoldenDoge node with block explorer.

## Contents
* Building on Linux 64-bit

## Building on Linux 64-bit

Command here are adapted for ubuntu linux ,you might write different commands on other distributions!!!

Create directory `GoldenDoge-explorer` and go there:
```
$> mkdir GoldenDoge-explorer
$> cd GoldenDoge-explorer
```

To go futher you have to have a number of packages and utilities. You need at least gcc 5.4.
* `build-essential` package:
    ```
    $GoldenDoge-explorer> sudo apt-get install build-essential
    ```
* `git` package:
    ```
    $GoldenDoge-explorer> sudo apt-get install git
    ```    
* CMake (3.5 or newer):
    ```
    $GoldenDoge-explorer> sudo apt-get install cmake
    $GoldenDoge-explorer> cmake --version
    ```
    If version is too old, follow instructions on [the official site](https://cmake.org/download/).
* Boost (1.62 or newer):
    You need boost in `GoldenDoge` folder. We do not configure to use boost installed by `apt-get`, because it is sometimes updated without your control by installing some unrelated packages. Also some users reported crashes after `find_package` finds headers from one version of boost and libraries from different version, or if installed boost uses dynamic linking.
    ```
    $GoldenDoge-explorer> wget -c 'http://sourceforge.net/projects/boost/files/boost/1.67.0/boost_1_67_0.tar.bz2/download'
    $GoldenDoge-explorer> tar xf download
    $GoldenDoge-explorer> rm download
    $GoldenDoge-explorer> mv boost_1_67_0 boost
    $GoldenDoge-explorer> cd boost
    $GoldenDoge-explorer/boost> ./bootstrap.sh
    $GoldenDoge-explorer/boost> ./b2 link=static -j 2 --build-dir=build64 --stagedir=stage
    cd ..
    ```
* OpenSSL (1.1.1 or newer):
    Install OpenSSL to `GoldenDoge/openssl` folder. (In below commands use switch `linux-x86_64-clang` instead of `linux-x86_64` if using clang.)
    ```
    $GoldenDoge-explorer> git clone https://github.com/openssl/openssl.git
    $GoldenDoge-explorer> cd openssl
    $GoldenDoge-explorer/openssl> ./Configure linux-x86_64 no-shared
    $GoldenDoge-explorer/openssl> time make -j2
    $GoldenDoge-explorer/openssl> cd ..
    ```
    
For editing the code i recommend to download visual studio code
here: [https://code.visualstudio.com/Download](https://code.visualstudio.com/Download)
    
* Clone GoldenDoge source code from github in that folder:
     ```
     $GoldenDoge> git clone https://github.com/GoldenDoge/GoldenDoge-explorer
     ```
Create build directory inside `GoldenDoge`, go there and run CMake and Make:
```
$GoldenDoge-explorer> cd GoldenDoge-explorer
```

Open node.cpp file in visual studio code
```
$GoldenDoge-explorer/GoldenDoge-explorer> code src/Core/Node.cpp
```
Go to line 2881 an change api = 'http://nbr.m2pool.eu:4041' to your hostname + port on what will your node and blockexplorer run, for example ```api = 'http://your.hostname.com:4041'```

And also on line 2882 you need to change var apiList = ["http://nbr.m2pool.eu:4041"]; to your hostname + port on what will your node and blockexplorer run, for example ```var apiList = ["http://your.hostname.com:4041"];```

Now you can build your block explorer
```
$GoldenDoge-explorer/GoldenDoge-explorer> mkdir build
$GoldenDoge-explorer/GoldenDoge-explorer> cd build
$GoldenDoge-explorer/GoldenDoge-explorer/build> cmake ..
$GoldenDoge-explorer/GoldenDoge-explorer/build> time make -j2 (2 is an example, it is the number of CPU threads)
```
     
Now you can run your node with block explorer from `../bin` folder
Make sure you write the same port in `--GoldenDoged-bind-address=` like in node.cpp file
```
$GoldenDoge/GoldenDoge/build> ../bin/./GoldenDoged --GoldenDoged-bind-address=0.0.0.0:4041
```

And now you can go to your website and check out the block explorer.
Running example of block explorer: http://nbr.m2pool.eu:4041/
