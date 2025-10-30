### 安装说明
目前vscode、clion在配置cmake时需选择gnu的x64环境，注意事项如下：
1. 官方在cmake内并没有对vs studio进行支持的配置，使用vs studio会出现指令错误的问题
2. clion 编译好后，可以运行sunshine，但是webui不正常，需要从vscode的编译成果中拷贝assets目录才能正确显示界面
3. 主要编译使用gnu 13以上即可，如果使用msys2安装环境，存在缺失zlib的问题，需要配置vcpkg内的libzlib.a进行适配
4. 为了配合gfw，我们修改了部分cmake内容，需要下载的插件，从本地目录直接安装。
5. 使用gnu开发，因为不是使用msvc，我们还需要配置一下cppwinrt否则会因为mingw默认缺少winrt的api而出现编译问题

初始安装教程（msys2搭建基础环境）：https://zhuanlan.zhihu.com/p/719319869
```shell
pacman -Syu # 第一次运行会提示关闭窗口，照做 

pacman -Su # 重新打开 MSYS2 终端后运行  升级到最新版本

#安装需要的组件
pacman -S \
  base-devel \
  cmake \
  diffutils \
  doxygen \
  gcc \
  git \
  make \
  mingw-w64-x86_64-binutils \
  mingw-w64-x86_64-boost \
  mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-curl \
  mingw-w64-x86_64-graphviz \
  mingw-w64-x86_64-miniupnpc \
  mingw-w64-x86_64-nlohmann-json \
  mingw-w64-x86_64-nodejs \
  mingw-w64-x86_64-onevpl \
  mingw-w64-x86_64-openssl \
  mingw-w64-x86_64-opus \
  mingw-w64-x86_64-rust \
  mingw-w64-x86_64-toolchain \
  python \
  python-pip
  
```
配置一下环境变量到安装目录，如:
```text
D:\msys64\mingw64\bin
```

```shell
#到代码下的build目录内，执行一下代码
cmake -G "MinGW Makefiles" ..
#执行编译
mingw32-make -j10
```

#### build的时候出现乱码
```text
ctrl+,  打开设置  在 CMake: Output Log Encoding 中设置成utf-8 即可
```

### 特殊说明
```text
v2025.628.4510 使用vs studio 2022进行编译，配合msys2安装组件，并配置mingw64到环境变量即可 尝试编译也有问题
v2025.819.140525 使用vscode基本没有错误了，配合msys2安装组件，并配置mingw64到环境变量即可 需要修改一下window.rc的逻辑
```


#### 下载安装 vcpkg 
```shell
# ① 克隆 vcpkg（已有可跳过）
git clone https://github.com/microsoft/vcpkg

cd vcpkg

.\bootstrap-vcpkg.bat
 
# ② 安装 libcurl + openssl
.\vcpkg install curl[core,openssl]        # 默认动态库 /MD
.\vcpkg install miniupnpc
.\vcpkg install minhook

# ③ 将 vcpkg 集成到 VS/MSBuild（只需一次）
.\vcpkg integrate install
```


### msys2 下安装minhook
```shell
#从https://github.com/msys2/MINGW-packages下载获取源码包

#拷贝mingw-w64-MinHook 到 msys64/home/aministrator/目录内

#在msys2 minggw64的环境内，定位到目录 
cd ~/mingw-w64-MinHook
#使用命令生成.pkg.tar.zst
makepkg -s
#使用命令安装
pacman -U ./mingw-w64-x86_64-MinHook-1.3.4-2-any.pkg.tar.zst
[PKGBUILD](../../../msys64/home/Administrator/mingw-w64-MinHook/PKGBUILD)
```


#### 比对基地版源码
1. confighttp.cpp存在修改
2. audio.cpp存在修改
3. rtsp.cpp 存在会话和mic的调用 还涉及到了session的修改
4. stream.cpp 存在mic的枚举定义 和动态码率的支持，主要逻辑 都在这
5. system_tray.cpp 系统托盘逻辑
6. upnp.cpp 需要注册一下mic的通道
7. video.cpp 主要的动态码率支持逻辑  在这里