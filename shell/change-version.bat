@echo off

echo change to v2025.829.135256
cd ../third-party

cd Simple-Web-Server
git checkout master
git pull
git checkout 187f798

cd..
cd TPCircularBuffer
git checkout master
git pull
git checkout cc52039

cd..
cd ViGEmClient
git checkout master
git pull
git checkout 8d71f67

cd..
cd build-deps
git checkout dist
git pull
git checkout 2450694

cd..
cd googletest
git checkout master
git pull
git checkout 52eb810

cd..
cd inputtino
git checkout master
git pull
git checkout 504f0ab

cd..
cd nanors
git checkout master
git pull
git checkout 19f07b5

cd..
cd nv-codec-headers
git checkout master
git pull
git checkout 22441b5

cd..
cd nvapi-open-source-sdk
git checkout master
git pull
git checkout cce4e90

cd..
cd tray
git checkout master
git pull
git checkout 0309a7c

cd..
cd wayland-protocols
git checkout master
git pull
git checkout 0091197

cd..
cd wlr-protocols
git checkout master
git pull
git checkout a741f0a

cd..
cd moonlight-common-c
git checkout master
git pull
#我们自己的分支，未来只需要拉取主干代码接口
#git checkout 5f22801

cd enet
git checkout master
git pull
git checkout 115a10b

cd..
cd..
cd doxyconfig
git checkout master
git pull
git checkout a73f908

cd doxygen-awesome-css
git checkout master
git pull
git checkout 98dd024

cd..
cd..
cd libdisplaydevice
git checkout master
git pull
git checkout f31e46d

cd third-party
cd googletest
git checkout master
git pull
git checkout f8d7d77

cd ..
cd doxyconfig
git checkout master
git pull
git checkout a73f908

cd doxygen-awesome-css
git checkout master
git pull
git checkout 98dd024

pause