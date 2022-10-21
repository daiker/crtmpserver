# crtmpserver

#### 介绍
crtmpserver支持openssl-0.9.8。高版本openssl会不兼容。  
使用https://www.openssl.org/source/old/0.9.x/openssl-0.9.8x.tar.gz  
老版本不支持c++11，修改宏定义（加空格）之后可以正常编译成功。

#### 软件架构
软件架构说明


#### openssl0-9.8 安装教程

1.  下载地址 https://www.openssl.org/source/old/0.9.x/openssl-0.9.8x.tar.gz
2.  cd crtmpserver/openssl-0.9.8/  
    tar -xzf openssl-0.9.8x.tar.gz  
    ./config shared --prefix=/usr/local/openssl  
3.  make
4.  make install

#### crtmpserver 安装

1.  cd crtmpserver/builders/cmake
2.  cmake ./
3.  make 
4.  ./crtmpserver/crtmpserver ./crtmpserver/crtmpserver.lua

#### ffmpeg 推流测试
1.  ./ffmpeg.exe -i rtmp.mp4 -c copy -f flv rtmp://192.168.5.236/live/stream1
2.  使用vlc播放：rtmp://192.168.5.236/live/stream1
#### 参与贡献

1.  Fork 本仓库
2.  新建 Feat_xxx 分支
3.  提交代码
4.  新建 Pull Request



