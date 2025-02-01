# bsdmon

Minimal tool to print out CPU, Memory, Disk usage and network interface information in Linux/FreeBSD

## Build

gcc src/main.c -o bsdmon

## Usage

./bsdmon

### Output

```bash
bsdmon - System Monitor
=======================
CPU Usage: 0.09%
Memory Usage: 1.12 GB / 23.47 GB (4.76% used)
Disk Usage ("/"): 97.42 GB / 1006.85 GB (9.68% used)
Network interfaces:
  eth0: 192.168.1.10 (mask: 255.255.255.0)
  docker0: 172.17.0.1 (mask: 255.255.0.0)
```