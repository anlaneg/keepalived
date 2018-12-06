#! /bin/bash
#To install from a git repo, execute:
#  1. Ensure you have autoconf and automake installed
#  2. git clone http://github.com/acassen/keepalived
#  3. cd keepalived
#  4. ./build_setup      # generate the autoconf and automake environment
#  5. Follow the instructions below for Installation, omitting the first two steps.
#
#Installation
#============
#
#  1. tar -xf TARFILE
#  2. cd into the directory
#  3. './configure'
#  4. 'make'
#  5. 'make install'. This will install keepalived on your system,
#     binaries and configuration file :
#     * keepalived : The keepalived daemon program.
#     * genhash : The MD5 url digest generator. You need it to
#                 configure HTTP GET check and SSL GET check in
#                 order to compute MD5SUM digest etalon.
#     * /etc/keepalived/keepalived.conf
#  6. link keepalived.init into your runlevel directory. On Red Hat systems :
#     ln -s /etc/rc.d/init.d/keepalived.init /etc/rc.d/rc3.d/S99keepalived
#
#  By default the configure script uses /usr/local as base directory. You can
#  change this value to your own by passing it the --prefix value.
#
#  eg: './configure --prefix=/usr/'
# keepalived实现了vrrp协议，vrrp协议只有一种格式的报文，即通告报文，可访问rfc2338
# 获取vrrp报文的处理规范，非常简单
# vrrp程序中应用了事件模型，通过此模型来驱动进程的通行。
# 理解thread_t结构的实现可更快的理解本程序
sudo apt -y install libnftnl-dev libmnl-dev libnl-3-dev libnl-genl-3-dev
./build_setup
./configure
make

