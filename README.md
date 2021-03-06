# nmsynproxy

nmsynproxy is a Linux user space TCP SYN proxy for IPv4 and IPv6 using netmap,
OpenDataPlane (ODP) or L Data Plane (LDP). The recommened variant today is the
LDP variant.

The SYN proxy makes hybrid use of SYN cookies and SYN cache. It also makes
hybrid use of TCP sequence numbers and TCP timestamp option. The SYN cookie
implementation is particularly modern, and is expected to lead to better
performance, security and compatibility than most SYN cookie implementations
out there. That's assuming SYN cookies are needed, as the SYN proxy uses also
SYN cache, in contrast to the Linux iptables in-kernel SYN proxy that merely
uses SYN cookies.

Multithreading is supported, provided that the network interface card (NIC) has
multiple queues. Actually, the NIC queue count needs to be adjusted to the same
number as the number of threads before starting up nmsynproxy or ldpsynproxy.
The ODP version, odpsynproxy, has somewhat more tolerance for different input
queue counts, but even then performance can suffer if the NIC input queue count
is not adjusted properly.

Performance should be great. With any recent good CPU, even as few as three
threads should achieve 40 Gbps link saturation when transferring large files.

There are two main supported variants of SYN proxy: nmsynproxy and ldpsynproxy.
nmsynproxy supports only netmap, ldpsynproxy can be used with netmap (using the
`netmap:` prefix on inteface names) or without netmap (by leaving out the
prefix). For highest performance, either nmsynproxy or the `netmap:` prefixed
use of ldpsynproxy is required. This requires netmap to compile the sources and
also the netmap kernel module must be installed.

An experimental ODP version, odpsynproxy, is also provided.

# Prerequisites

You need to have flex and bison installed in order to compile this project.
Also, needless to say, compiler tools and GNU make must be available. To
actually communicate with real network interfaces, you also need netmap for the
highest performance possible, but more on that later. If low performance is
enough, ldpsynproxy can be used instead of nmsynproxy in the socket mode.

Also, pptk submodule must be initialized and updated.

```
git submodule init
git submodule update
```

# Git history

The git history has been edited to remove some proprietary code. Thus,
historical versions of the repository may not compile correctly.

# Compilation

To compile, type `make -j4` where the number after `-j` is the number of cores.

To run unit tests, run `make unit`. Note that using `-j` with `make unit` is
not recommended.

# Netmap support

To compile with netmap support, edit the file `opts.mk` (generated as empty
file automatically after successful `make`), and add the lines:

```
WITH_NETMAP=yes
NETMAP_INCDIR=/home/YOURUSERNAME/netmap/sys
```

But before this, you need to clone netmap:

```
cd /home/YOURUSERNAME
git clone https://github.com/luigirizzo/netmap
cd netmap
./configure --no-drivers
make
insmod ./netmap.ko
```

Successfully compiling netmap requires that you have your kernel headers
installed.

# Netmap drivers

If you want higher performance, you can compile netmap with drivers:

```
cd /home/YOURUSERNAME
rm -rf netmap
git clone https://github.com/luigirizzo/netmap
cd netmap
./configure
make
rmmod netmap
rmmod ixgbe
rmmod i40e
insmod ./netmap.ko
insmod ./ixgbe-5.0.4/src/ixgbe.ko
insmod ./i40e-2.0.19/src/i40e.ko
```

Adjust paths as needed to have the correct version of the driver.

# Netmap testing

Then, after netmap is installed, compile with `make -j4` as usual.

You can try netmap with the following commands to be run in two terminal
windows:

```
./synproxy/nmsynproxy vale0:1 vale1:1
taskset -c 3 ./synproxy/netmapsend vale0:0
```

# Netmap with full kernel sources

Some netmap drivers require full kernel sources. On Ubuntu 16.04 LTS, they
can be installed in the following way: first, uncomment deb-src lines in
`/etc/apt/sources.list`. Then, type these commands:

```
cd /home/YOURUSERNAME
apt-get update
apt-get source linux-image-$(uname -r)
rm -rf netmap
git clone https://github.com/luigirizzo/netmap
cd netmap
./configure --kernel-sources=/home/WHATEVER/linux-hwe-4.8.0
rmmod netmap
insmod ./netmap.ko
```

Then, you may load for example netmap specific veth driver:

```
cd /home/YOURUSERNAME/netmap
rmmod veth
insmod ./veth.ko
```

# Testing with veth

Veth interfaces have poor performance. The netmap-specific veth driver is
supposed to help this, but actually on the test laptop where it was tested, it
made things even slower. Nevertheless, if you want to use veth, you can do it
in this way:

```
ip link add veth0 type veth peer name veth1
ip link add veth2 type veth peer name veth3
ifconfig veth0 up
ifconfig veth1 up
ifconfig veth2 up
ifconfig veth3 up
```

Then run these two commands in two terminal windows:
```
./synproxy/nmsynproxy netmap:veth1 netmap:veth2
taskset -c 3 ./synproxy/netmapsend netmap:veth0
```

# Testing with real network interfaces

Let's assume you have eth0 and eth1 inserted as an inline pair to an Ethernet
network. You want to bridge traffic between eth0 and eth1 and SYN proxy
incoming connections from eth1 into eth0. You must first set both interfaces to
promiscuous mode:

```
ip link set eth0 promisc on
ip link set eth1 promisc on
```

Then you must set the interfaces to have one queue each (or if using multiple
threads, you must use multiple queues with the same number of queues as the
number of threads configured in conf.txt):

```
ethtool -L eth0 combined 1
ethtool -L eth1 combined 1
```

If the NIC has separate RX and TX queues, you must configure them separately:

```
ethtool -L eth0 rx 1 tx 1
ethtool -L eth1 rx 1 tx 1
```

It is also recommended to turn off offloads:

```
ethtool -K eth0 rx off tx off tso off gso off gro off lro off
ethtool -K eth1 rx off tx off tso off gso off gro off lro off
```

If the `ethtool` reports an error about being unable to change certain offload,
remove the offload setting from the command line and try with the other
settings.

It is also recommended to turn off flow control:

```
ethtool -A eth0 rx off tx off autoneg off
ethtool -A eth1 rx off tx off autoneg off
```

Then you must start nmsynproxy:
```
./synproxy/nmsynproxy netmap:eth0 netmap:eth1
```

Note that the order interfaces are specified matters. The first is the LAN
interface. The second is the WAN interface. Only connections from WAN to LAN
are SYN proxied.

If you don't have netmap installed or the netmap kernel module loaded, you may
do instead:
```
./synproxy/ldpsynproxy eth0 eth1
```

...but note that in this variant, you must before remove any assigned addresses
from the eth0 and eth1 interfaces.

# Testing with network namespaces

Execute:

```
ip link add veth0 type veth peer name veth1
ip link add veth2 type veth peer name veth3
ifconfig veth0 up
ifconfig veth1 up
ifconfig veth2 up
ifconfig veth3 up
ethtool -K veth0 rx off tx off tso off gso off gro off lro off
ethtool -K veth1 rx off tx off tso off gso off gro off lro off
ethtool -K veth2 rx off tx off tso off gso off gro off lro off
ethtool -K veth3 rx off tx off tso off gso off gro off lro off
ip netns add ns1
ip netns add ns2
ip link set veth0 netns ns1
ip link set veth3 netns ns2
ip netns exec ns1 ip addr add 10.200.1.1/24 dev veth0
ip netns exec ns2 ip addr add 10.200.1.2/24 dev veth3
ip netns exec ns1 ip link set veth0 up
ip netns exec ns2 ip link set veth3 up
ip netns exec ns1 ip link set lo up
ip netns exec ns2 ip link set lo up
```

Then run in one terminal window and leave it running:
```
./synproxy/nmsynproxy netmap:veth1 netmap:veth2
```

Or you may alternatively run:
```
./synproxy/ldpsynproxy veth1 veth2
```

Verify that ping works to both directions:

```
ip netns exec ns1 ping 10.200.1.2
ip netns exec ns2 ping 10.200.1.1
```

Then, execute netcat in two terminal windows:
```
ip netns exec ns1 nc -v -v -v -l -p 1234
ip netns exec ns2 nc -v -v -v 10.200.1.1 1234
```

Type something to both windows and see that the counterparty gets the same
text.

Try also in other direction in two terminal windows:
```
ip netns exec ns2 nc -v -v -v -l -p 1234
ip netns exec ns1 nc -v -v -v 10.200.1.2 1234
```
